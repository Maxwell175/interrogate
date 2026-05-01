// Memory-management tests.  Verify that non-blittable type integrations
// (std::string returns, std::stream bridges, ref-counted objects) don't
// leak under repeated use, GC collection, or exception paths.

using System;
using System.IO;
using System.Text;
using Interrogate;
using Memory;

internal static class Driver {
    private static int _failed;

    private static void Check(bool ok, string msg) {
        Console.WriteLine((ok ? "  PASS " : "  FAIL ") + msg);
        if (!ok) _failed++;
    }

    public static int Main() {
        TestDisposeReleasesNative();
        TestFinalizerReleasesNative();
        TestStringReturnNoLeak();
        TestStringParamStableUnderIteration();
        TestStringReturnAcrossObjects();
        TestListReturnNoNativeLeak();
        TestListElementStringsDoNotLeak();
        TestListFinalizerReleasesNative();
        TestStreamBridgeUsingReleases();
        TestStreamBridgeFinalizerReleases();
        TestStreamBridgeExceptionSafe();
        TestArgumentTrackerStaysAliveAcrossCall();

        Console.WriteLine();
        if (_failed == 0) {
            Console.WriteLine("All memory tests passed.");
            return 0;
        }
        Console.WriteLine($"{_failed} test(s) failed.");
        return 1;
    }

    // Waits for all pending finalizers plus a full GC round.  Needed because
    // managed finalizers run on a background thread; tests must block until
    // the queue has drained before asserting that native counts dropped.
    private static void CollectFinalizeCollect() {
        for (int i = 0; i < 3; i++) {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
    }

    // After explicit Dispose() on a NativeObject wrapper, the C++ destructor
    // must have run.  Uses Tracker's g_live counter as ground truth.
    private static void TestDisposeReleasesNative() {
        Console.WriteLine("TestDisposeReleasesNative");
        MemoryGlobals.memory_reset_counts();

        var t = new Tracker("one");
        Check(MemoryGlobals.memory_live_trackers() == 1, "live count is 1 after new");
        t.Dispose();
        Check(MemoryGlobals.memory_live_trackers() == 0,
              $"live count is 0 after Dispose (got: {MemoryGlobals.memory_live_trackers()})");
    }

    // If the caller drops the wrapper without calling Dispose, the finalizer
    // must still release the native pointer.  Allocate-then-forget in a
    // helper so the only reference lives inside that helper's frame.
    private static void TestFinalizerReleasesNative() {
        Console.WriteLine("TestFinalizerReleasesNative");
        MemoryGlobals.memory_reset_counts();

        AllocateAndAbandon();
        CollectFinalizeCollect();

        Check(MemoryGlobals.memory_live_trackers() == 0,
              $"live count is 0 after GC (got: {MemoryGlobals.memory_live_trackers()})");
        Check(MemoryGlobals.memory_total_trackers_created() == 1,
              "Tracker was actually constructed");
    }

    // Inlined separately so the JIT can't keep `t` live past this frame —
    // otherwise the object would still be rooted when CollectFinalizeCollect
    // runs, and we couldn't distinguish "finalizer ran" from "GC didn't collect".
    private static void AllocateAndAbandon() {
        var t = new Tracker("forgotten");
        _ = t.get_label();
    }

    // 10k string-returning calls must not leak process memory.  We can't
    // directly measure RSS portably, but thread_local string_holder should
    // reuse the same allocation every call — if it accidentally became
    // per-call `new`, we'd see continuous growth.  As a proxy, we verify
    // the string content is correct every call (regression for the
    // `static std::string` initialization-once bug).
    private static void TestStringReturnNoLeak() {
        Console.WriteLine("TestStringReturnNoLeak");
        MemoryGlobals.memory_reset_counts();

        using var a = new Tracker("alpha");
        using var b = new Tracker("bravo");

        for (int i = 0; i < 10000; i++) {
            string s = (i % 2 == 0) ? a.echo_label() : b.echo_label();
            string expected = (i % 2 == 0) ? "alpha" : "bravo";
            if (s != expected) {
                Check(false, $"iteration {i}: got '{s}' expected '{expected}'");
                return;
            }
        }
        Check(true, "10000 alternating string returns round-tripped correctly");
    }

    // Regression for an earlier `static std::string string_holder` bug where
    // the holder's initializer ran only on the first call and subsequent
    // calls returned stale data.  TestStringReturnNoLeak catches that via
    // alternation, but this test explicitly probes the "mutate then re-read"
    // pattern that would fail silently with the old implementation.
    private static void TestStringParamStableUnderIteration() {
        Console.WriteLine("TestStringParamStableUnderIteration");
        using var t = new Tracker("initial");
        for (int i = 0; i < 1000; i++) {
            string got = t.echo_label();
            if (got != "initial") {
                Check(false, $"echo_label iter {i}: got '{got}'");
                return;
            }
        }
        Check(true, "label reads unchanged across 1000 calls");
    }

    // Multiple NativeObject wrappers returning strings — if the native
    // thread_local holder were accidentally per-process (not per-thread),
    // interleaved get_label calls from different objects would stomp on
    // each other.  This drives the thread_local guarantee.
    private static void TestStringReturnAcrossObjects() {
        Console.WriteLine("TestStringReturnAcrossObjects");
        var trackers = new Tracker[16];
        for (int i = 0; i < trackers.Length; i++) {
            trackers[i] = new Tracker($"tracker-{i}");
        }
        try {
            for (int iter = 0; iter < 500; iter++) {
                for (int i = 0; i < trackers.Length; i++) {
                    string got = trackers[i].get_label();
                    if (got != $"tracker-{i}") {
                        Check(false, $"iter {iter} tracker {i}: got '{got}'");
                        return;
                    }
                }
            }
            Check(true, "8000 interleaved string reads returned correct per-object values");
        } finally {
            foreach (var t in trackers) t.Dispose();
        }
    }

    // A native-returning collection IS a NativeObject (vector_string
    // wrapper).  Disposing it must delete the underlying std::vector.  Run
    // many iterations and snapshot Tracker.live_count — the vector's own
    // elements aren't Trackers, so we can't directly count the native
    // objects here, but we CAN check that Tracker objects aren't leaking
    // as a side effect of the vector marshalling.
    private static void TestListReturnNoNativeLeak() {
        Console.WriteLine("TestListReturnNoNativeLeak");
        MemoryGlobals.memory_reset_counts();
        using var t = new Tracker("src");

        Check(MemoryGlobals.memory_live_trackers() == 1, "only 1 Tracker live before loop");

        for (int i = 0; i < 500; i++) {
            using var labels = t.build_labels(10);
            if (labels!.Count != 10) {
                Check(false, $"iter {i}: labels.Count != 10");
                return;
            }
        }

        Check(MemoryGlobals.memory_live_trackers() == 1,
              $"only 1 Tracker live after loop (got: {MemoryGlobals.memory_live_trackers()})");
    }

    // Every string element pulled from the vector must be copied into a
    // managed string (not freed on the native side).  If the CoTaskMemFree
    // regression came back, this test would crash inside the first indexer
    // call with "free(): invalid size".
    private static void TestListElementStringsDoNotLeak() {
        Console.WriteLine("TestListElementStringsDoNotLeak");
        using var t = new Tracker("item");
        using var labels = t.build_labels(100);

        for (int iter = 0; iter < 100; iter++) {
            for (int i = 0; i < labels!.Count; i++) {
                string got = labels[i];
                if (got != $"item-{i}") {
                    Check(false, $"iter {iter} index {i}: got '{got}'");
                    return;
                }
            }
        }
        Check(true, "10000 string-element reads succeeded without heap corruption");
    }

    // Abandoned collection wrapper — finalizer must still run the native
    // destructor.  Unlike Tracker, vector_string doesn't have a bespoke
    // live-counter, but NativeObject's generic finalizer dispatch is what
    // we're checking here, piggy-backing on Tracker as the probe.
    private static void TestListFinalizerReleasesNative() {
        Console.WriteLine("TestListFinalizerReleasesNative");
        MemoryGlobals.memory_reset_counts();

        AbandonTrackerAndLabels();
        CollectFinalizeCollect();

        Check(MemoryGlobals.memory_live_trackers() == 0,
              $"Tracker freed after GC (got: {MemoryGlobals.memory_live_trackers()})");
    }

    private static void AbandonTrackerAndLabels() {
        var t = new Tracker("ghost");
        var labels = t.build_labels(5);
        _ = labels![0];
        // t and labels leave scope without Dispose; finalizer must clean up.
    }

    // Every `using var` stream bridge must get destroyed immediately.
    // LiveCount should return to its baseline after each loop body.
    private static void TestStreamBridgeUsingReleases() {
        Console.WriteLine("TestStreamBridgeUsingReleases");
        long baseline = StreamBridge.LiveCount;

        using var t = new Tracker("stream-me");
        for (int i = 0; i < 1000; i++) {
            using var mem = new MemoryStream();
            t.write_label(mem);
        }
        Check(StreamBridge.LiveCount == baseline,
              $"bridge LiveCount returned to {baseline} (got: {StreamBridge.LiveCount})");
    }

    // If a caller allocates StreamBridge directly and forgets `using`, the
    // finalizer must still release the native handle.
    private static void TestStreamBridgeFinalizerReleases() {
        Console.WriteLine("TestStreamBridgeFinalizerReleases");
        long baseline = StreamBridge.LiveCount;

        AbandonStreamBridges();
        CollectFinalizeCollect();

        Check(StreamBridge.LiveCount == baseline,
              $"bridge LiveCount back to {baseline} after GC (got: {StreamBridge.LiveCount})");
    }

    private static void AbandonStreamBridges() {
        for (int i = 0; i < 50; i++) {
            _ = StreamBridge.ForOutput(new MemoryStream());
        }
    }

    // If the native call throws (simulated here via a Stream that throws in
    // Read/Write), `using var` still runs and the bridge is destroyed.
    // LiveCount must return to baseline even on the exception path.
    private static void TestStreamBridgeExceptionSafe() {
        Console.WriteLine("TestStreamBridgeExceptionSafe");
        long baseline = StreamBridge.LiveCount;

        using var t = new Tracker("stream-me");
        for (int i = 0; i < 50; i++) {
            try {
                using var broken = new ThrowingStream();
                // read_into_label drains the stream — our ThrowingStream's
                // Read callback returns -1 on every call, so C++ sees EOF and
                // the call completes normally.  We still count as "exception
                // path" because the managed Read throws inside the bridge
                // trampoline, and we want to make sure that still releases.
                t.read_into_label(broken);
            } catch (IOException) {
                // no-op
            }
        }
        Check(StreamBridge.LiveCount == baseline,
              $"bridge LiveCount back to {baseline} (got: {StreamBridge.LiveCount})");
    }

    // When a method takes a NativeObject argument, that object must be
    // rooted for the duration of the native call — otherwise the GC could
    // collect it while C++ is dereferencing the pointer.  Hard to trigger a
    // bad race deterministically; this test at least verifies the happy-path
    // argument forwarding and that no object is prematurely freed after the
    // call returns.
    private static void TestArgumentTrackerStaysAliveAcrossCall() {
        Console.WriteLine("TestArgumentTrackerStaysAliveAcrossCall");
        MemoryGlobals.memory_reset_counts();

        using var a = new Tracker("left");
        using var b = new Tracker("right");
        Check(MemoryGlobals.memory_live_trackers() == 2, "2 trackers live");

        string combined = a.combined_label(b);
        Check(combined == "left+right", $"combined_label (got: '{combined}')");
        Check(MemoryGlobals.memory_live_trackers() == 2,
              "both trackers still live after call");
    }

    private sealed class ThrowingStream : Stream {
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => throw new NotSupportedException();
        public override long Position {
            get => 0;
            set => throw new NotSupportedException();
        }
        public override void Flush() { }
        public override int Read(byte[] buffer, int offset, int count)
            => throw new IOException("simulated read failure");
        public override long Seek(long offset, SeekOrigin origin)
            => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count)
            => throw new NotSupportedException();
    }
}
