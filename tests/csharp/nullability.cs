using System;
using Nullability;

internal static class Driver {
    private static int _failed;

    private static void Check(bool ok, string msg) {
        Console.WriteLine((ok ? "  PASS " : "  FAIL ") + msg);
        if (!ok) _failed++;
    }

    public static int Main() {
        TestNullableReturn();
        TestNonNullableReturn();
        TestNullableParamAcceptsValue();
        TestNullableParamAcceptsNull();

        Console.WriteLine();
        if (_failed == 0) {
            Console.WriteLine("All nullability tests passed.");
            return 0;
        }
        Console.WriteLine($"{_failed} test(s) failed.");
        return 1;
    }

    // Nullable return — find_resource is annotated with [[in::nullable]], so
    // the binding returns `Resource?` and null is a legitimate value.
    private static void TestNullableReturn() {
        Console.WriteLine("TestNullableReturn: nullable return can actually be null");
        using var mgr = new ResourceManager();
        var found = mgr.find_resource("missing");
        Check(found == null, "find_resource returns null without throwing");
    }

    // Non-nullable return — get_default_resource has no attribute, so the
    // binding returns `Resource` (non-nullable).  The binding should throw
    // if the native side actually returned null, but for this test the
    // default resource always exists.
    private static void TestNonNullableReturn() {
        Console.WriteLine("TestNonNullableReturn: non-null return yields a value");
        using var mgr = new ResourceManager();
        var def = mgr.get_default_resource();
        Check(def.get_name() == "default", $"default name (got: '{def.get_name()}')");
    }

    private static void TestNullableParamAcceptsValue() {
        Console.WriteLine("TestNullableParamAcceptsValue: non-null argument forwarded");
        using var mgr = new ResourceManager();
        using var r = new Resource("alpha");
        mgr.set_active(r);
        Check(mgr.active_name() == "alpha", $"active_name (got: '{mgr.active_name()}')");
    }

    // Nullable parameter — set_active is annotated with [[in::nullable]],
    // so the managed binding accepts `null` and forwards IntPtr.Zero to C++.
    private static void TestNullableParamAcceptsNull() {
        Console.WriteLine("TestNullableParamAcceptsNull: null argument forwarded as IntPtr.Zero");
        using var mgr = new ResourceManager();
        mgr.set_active(null);
        Check(mgr.active_name() == "", "active_name is empty after set_active(null)");
    }
}
