// Reference-counting lifetime tests.  These would all have failed before the
// upcast thunks stopped ref()-ing their result: construction and inherited
// secondary-base calls each leaked a reference, so a ref-counted object could
// never reach zero.

using System;
using Interrogate;
using Refcounting;

internal static class Driver {
    private static int _failed;

    private static void Check(bool ok, string msg) {
        Console.WriteLine((ok ? "  PASS " : "  FAIL ") + msg);
        if (!ok) _failed++;
    }

    public static int Main() {
        TestConstructDisposeBalance();
        TestFinalizerBalance();
        TestInheritedMethodNoRefLeak();
        TestBorrowedCastDoesNotPin();
        TestOwningCastKeepsAlivePastSource();
        TestFailClosedCast();

        Console.WriteLine();
        if (_failed == 0) {
            Console.WriteLine("All refcounting tests passed.");
            return 0;
        }
        Console.WriteLine($"{_failed} test(s) failed.");
        return 1;
    }

    private static void CollectFinalizeCollect() {
        for (int i = 0; i < 3; i++) {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
    }

    // A single ref-counted object, constructed then disposed, must be freed and
    // must report a reference count of exactly 1 while live.  Before the fix the
    // ctor's cached upcast-to-RefBase leaked a reference, so the count was 2 and
    // the object was never freed.
    private static void TestConstructDisposeBalance() {
        Console.WriteLine("TestConstructDisposeBalance");
        RefcountingGlobals.rc_reset_counts();

        var g = new Gadget();
        Check(RefcountingGlobals.rc_live_gadgets() == 1, "1 live after new");
        Check(g.get_ref_count() == 1,
              $"ref count is 1 after new (got: {g.get_ref_count()})");
        g.Dispose();
        Check(RefcountingGlobals.rc_live_gadgets() == 0,
              $"0 live after Dispose (got: {RefcountingGlobals.rc_live_gadgets()})");
    }

    // Same balance on the finalizer path: abandon without Dispose, then GC.
    private static void TestFinalizerBalance() {
        Console.WriteLine("TestFinalizerBalance");
        RefcountingGlobals.rc_reset_counts();

        AllocateAndAbandon();
        CollectFinalizeCollect();

        Check(RefcountingGlobals.rc_live_gadgets() == 0,
              $"0 live after GC (got: {RefcountingGlobals.rc_live_gadgets()})");
        Check(RefcountingGlobals.rc_total_gadgets_created() == 1,
              "Gadget was actually constructed");
    }

    private static void AllocateAndAbandon() {
        var g = new Gadget();
        _ = g.describe();
    }

    // get_ref_count is inherited from the ref-counted *secondary* base, so each
    // call re-upcasts `this`.  Before the fix each of those upcasts leaked a
    // reference; the count must stay put no matter how many times we call.
    private static void TestInheritedMethodNoRefLeak() {
        Console.WriteLine("TestInheritedMethodNoRefLeak");
        RefcountingGlobals.rc_reset_counts();

        var g = new Gadget();
        for (int i = 0; i < 1000; i++) {
            _ = g.get_ref_count();
        }
        Check(g.get_ref_count() == 1,
              $"ref count stable at 1 after 1000 inherited-method calls (got: {g.get_ref_count()})");
        g.Dispose();
        Check(RefcountingGlobals.rc_live_gadgets() == 0, "freed after loop");
    }

    // A default (borrowed) cast must not take a reference — proven by the object
    // being freed as soon as the source is disposed, even with the view still
    // referenced.  (Also a leak regression: before the fix, creating the wrapper
    // leaked the ctor's upcast reference and the object would survive here.)
    private static void TestBorrowedCastDoesNotPin() {
        Console.WriteLine("TestBorrowedCastDoesNotPin");
        RefcountingGlobals.rc_reset_counts();

        var g = new Gadget();
        var view = g.CastTo<Gadget>();
        Check(view != null, "borrowed cast returned a wrapper");
        Check(g.get_ref_count() == 1,
              $"borrowed cast did not bump the count (got: {g.get_ref_count()})");
        g.Dispose();
        Check(RefcountingGlobals.rc_live_gadgets() == 0,
              $"freed despite an outstanding borrowed view (got: {RefcountingGlobals.rc_live_gadgets()})");
        GC.KeepAlive(view);
    }

    // An owning cast (own: true) of a ref-counted type takes a reference, so the
    // result stays valid after the source is gone.
    private static void TestOwningCastKeepsAlivePastSource() {
        Console.WriteLine("TestOwningCastKeepsAlivePastSource");
        RefcountingGlobals.rc_reset_counts();

        var g = new Gadget();
        var owned = g.CastTo<Gadget>(own: true);
        Check(owned != null, "owning cast returned a wrapper");
        Check(g.get_ref_count() == 2,
              $"owning cast bumped the count to 2 (got: {g.get_ref_count()})");

        g.Dispose();
        Check(RefcountingGlobals.rc_live_gadgets() == 1,
              "object still live after the source was disposed");
        Check(owned!.get_ref_count() == 1,
              $"count dropped to 1 after source disposed (got: {owned.get_ref_count()})");
        Check(owned.describe() == "gadget#0",
              $"owned cast is still usable (got: '{owned.describe()}')");

        owned.Dispose();
        Check(RefcountingGlobals.rc_live_gadgets() == 0,
              $"freed after the owning cast was disposed (got: {RefcountingGlobals.rc_live_gadgets()})");
    }

    // A cast that cannot be verified must fail closed (null), not reinterpret the
    // pointer.  Gadget and Sprocket are unrelated; before fail-closed this handed
    // back a Sprocket wrapper over a Gadget pointer.
    private static void TestFailClosedCast() {
        Console.WriteLine("TestFailClosedCast");
        RefcountingGlobals.rc_reset_counts();

        using var g = new Gadget();
        Check(g.CastTo<Sprocket>() == null, "cast to an unrelated type returns null");
        Check(g.CastTo<Gadget>() != null, "identity cast still succeeds");
    }
}
