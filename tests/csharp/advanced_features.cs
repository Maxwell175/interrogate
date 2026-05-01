using System;
using Advanced.Features;

internal static class Driver {
    private static int _failed;

    private static void Check(bool ok, string msg) {
        Console.WriteLine((ok ? "  PASS " : "  FAIL ") + msg);
        if (!ok) _failed++;
    }

    public static int Main() {
        TestBaseClass();
        TestDerivedClass();
        TestFloatReturn();
        TestGlobals();

        Console.WriteLine();
        if (_failed == 0) {
            Console.WriteLine("All advanced_features tests passed.");
            return 0;
        }
        Console.WriteLine($"{_failed} test(s) failed.");
        return 1;
    }

    private static void TestBaseClass() {
        Console.WriteLine("TestBaseClass: plain shape default values");
        using var shape = new Shape();
        Check(shape.area() == 0.0f, $"default area (got: {shape.area()})");
        Check(shape.type_name() == "shape", $"type_name (got: '{shape.type_name()}')");
        shape.set_id(3);
        Check(shape.get_id() == 3, $"id round-trip (got: {shape.get_id()})");
    }

    private static void TestDerivedClass() {
        Console.WriteLine("TestDerivedClass: Circle overrides");
        using var c = new Circle(2.0f);
        Check(c.get_radius() == 2.0f, $"radius (got: {c.get_radius()})");
        // Virtual dispatch — the override should run, giving π·r² ≈ 12.566.
        float a = c.area();
        Check(a > 12.5f && a < 12.6f, $"area (got: {a})");
        Check(c.type_name() == "circle", $"type_name (got: '{c.type_name()}')");
    }

    private static void TestFloatReturn() {
        Console.WriteLine("TestFloatReturn: primitive round-trip");
        float s = AdvancedFeaturesGlobals.add_floats(1.5f, 2.25f);
        Check(Math.Abs(s - 3.75f) < 1e-6f, $"sum (got: {s})");
    }

    // Free functions should be emitted on the module's globals class.
    private static void TestGlobals() {
        Console.WriteLine("TestGlobals: module globals class exposes free functions");
        Check(AdvancedFeaturesGlobals.add_numbers(2, 3) == 5, "add_numbers(2,3) == 5");
    }
}
