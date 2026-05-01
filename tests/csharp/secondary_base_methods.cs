using System;
using Secondary.Base.Methods;

internal static class Driver {
    private static int _failed;

    private static void Check(bool ok, string msg) {
        Console.WriteLine((ok ? "  PASS " : "  FAIL ") + msg);
        if (!ok) _failed++;
    }

    public static int Main() {
        TestPrimaryBaseMethodsVisible();
        TestSecondaryBaseMethodsVisible();

        Console.WriteLine();
        if (_failed == 0) {
            Console.WriteLine("All secondary_base_methods tests passed.");
            return 0;
        }
        Console.WriteLine($"{_failed} test(s) failed.");
        return 1;
    }

    // The derived class should expose methods from its primary base via the
    // usual C# inheritance.
    private static void TestPrimaryBaseMethodsVisible() {
        Console.WriteLine("TestPrimaryBaseMethodsVisible: primary base methods on derived");
        using var node = new DerivedTextNode();
        node.set_id(42);
        Check(node.get_id() == 42, $"get_id (got: {node.get_id()})");
    }

    // C# has no multiple inheritance, so methods from a secondary C++ base
    // must be re-exposed on the derived class (otherwise callers lose access
    // to them entirely).  This test verifies that re-exposure happens.
    private static void TestSecondaryBaseMethodsVisible() {
        Console.WriteLine("TestSecondaryBaseMethodsVisible: secondary base methods on derived");
        using var node = new DerivedTextNode();
        node.set_text("hello");
        Check(node.get_text() == "hello", $"get_text (got: '{node.get_text()}')");
    }
}
