// Tests for "keep managed arguments (and `this`) alive across the native call".
//
// The failure this guards against is a GC race: a native-object argument is
// passed as a raw handle, and once that handle is read the wrapper has no
// further managed use, so the JIT may let it be finalized -- freeing the native
// object -- mid-call.  A pure runtime reproduction is probabilistic and only
// bites in optimized code, so the authoritative check here is a source
// assertion: the generated bindings must contain the GC.KeepAlive calls.  The
// functional checks alongside it confirm the bindings still work and that the
// finalizer really does free native objects (which is what makes the keep-alive
// necessary in the first place).

using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Keepalive;

internal static class Driver {
    private static int _failed;

    private static void Check(bool ok, string msg) {
        Console.WriteLine((ok ? "  PASS " : "  FAIL ") + msg);
        if (!ok) _failed++;
    }

    public static int Main() {
        TestFinalizerReallyFrees();
        TestArgumentBindingsWork();
        TestGeneratedSourceKeepsArgumentsAlive();

        Console.WriteLine();
        if (_failed == 0) {
            Console.WriteLine("All keepalive tests passed.");
            return 0;
        }
        Console.WriteLine($"{_failed} test(s) failed.");
        return 1;
    }

    // The wrapper for a freshly-constructed Probe is Owned: abandoning it and
    // collecting must run the destructor and free the native object.  This is
    // the finalizer that, without the keep-alive, could fire mid-call.
    private static void TestFinalizerReallyFrees() {
        Console.WriteLine("TestFinalizerReallyFrees");
        KeepaliveGlobals.kp_reset_counts();

        AllocateAndAbandon();
        for (int i = 0; i < 3; i++) {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
        Check(KeepaliveGlobals.kp_live_probes() == 0,
              $"0 live probes after GC (got {KeepaliveGlobals.kp_live_probes()})");
    }

    private static void AllocateAndAbandon() {
        _ = new Probe(1).Id();
    }

    // The bindings that carry a native-object argument must actually work.
    private static void TestArgumentBindingsWork() {
        Console.WriteLine("TestArgumentBindingsWork");

        // Static method taking a native-object argument (the reported shape).
        var p = new Probe(42);
        Check(KeepaliveGlobals.read_probe(p) == 42, "read_probe(Probe) returns the id");

        // Constructor taking a native-object argument, then an instance method
        // reading through `this`, then a setter taking a native-object argument.
        using var h = new Holder(p);
        Check(h.HolderProbeId() == 42, "Holder(Probe) + instance read returns the id");

        var p2 = new Probe(9);
        h.SetProbe(p2);
        Check(h.HolderProbeId() == 9, "set_probe(Probe) + instance read returns the new id");

        GC.KeepAlive(p);
        GC.KeepAlive(p2);
    }

    // Deterministic regression guard: the generated bindings must keep managed
    // arguments and `this` alive across the native call.  If a future change
    // drops the GC.KeepAlive emission, this fails even though the (racy)
    // runtime symptom might not.
    private static void TestGeneratedSourceKeepsArgumentsAlive() {
        Console.WriteLine("TestGeneratedSourceKeepsArgumentsAlive");

        string? csDir = FindGeneratedCsDir();
        Check(csDir != null,
              $"located the generated bindings (cwd '{Directory.GetCurrentDirectory()}')");
        if (csDir == null) return;

        // Scan code only: interrogate propagates this header's comments into the
        // generated bindings as `///` XML-doc, and those comments mention
        // GC.KeepAlive / __p3Create -- so drop full-line comments before asserting
        // or the guard matches its own documentation instead of real code.
        string src = string.Concat(
            Directory.GetFiles(csDir, "*.cs")
                .Select(File.ReadAllText)
                .SelectMany(t => t.Split('\n'))
                .Where(line => !line.TrimStart().StartsWith("//", StringComparison.Ordinal))
                .Select(line => line + "\n"));

        Check(src.Contains("GC.KeepAlive(this)"),
              "an instance member keeps `this` alive");

        // A keep-alive of something other than `this` -- i.e. a native-object
        // argument (read_probe's param, set_probe's value).
        Check(Regex.IsMatch(src, @"GC\.KeepAlive\(\s*(?!this\b)\w"),
              "a native-object argument is kept alive");

        // The constructor with a native-object argument is routed through the
        // private helper, and that helper keeps the argument alive.
        Check(src.Contains("__p3Create"),
              "a constructor with a native-object argument uses the keep-alive helper");
        Check(HelperKeepsAlive(src),
              "the constructor helper keeps its argument alive");
    }

    // True if some `private static IntPtr __p3Create...` helper body contains a
    // GC.KeepAlive before it returns.
    private static bool HelperKeepsAlive(string src) {
        foreach (Match m in Regex.Matches(src, @"private static IntPtr __p3Create")) {
            int start = m.Index;
            int ret = src.IndexOf("return __p3ret;", start, StringComparison.Ordinal);
            if (ret > start && src.IndexOf("GC.KeepAlive(", start, ret - start,
                                           StringComparison.Ordinal) >= 0) {
                return true;
            }
        }
        return false;
    }

    // The driver runs with its working directory at the per-test work dir, whose
    // `cs/` subdirectory holds the pass-2 output.  Walk up a few levels in case
    // dotnet changes the working directory.
    private static string? FindGeneratedCsDir() {
        string? dir = Directory.GetCurrentDirectory();
        for (int i = 0; i < 6 && dir != null; i++) {
            string cand = Path.Combine(dir, "cs");
            if (Directory.Exists(cand) && Directory.GetFiles(cand, "*.cs").Length > 0) {
                return cand;
            }
            dir = Path.GetDirectoryName(dir);
        }
        return null;
    }
}
