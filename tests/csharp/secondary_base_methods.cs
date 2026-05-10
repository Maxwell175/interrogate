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
        TestSecondaryBasePointerOffset();
        TestDeepChainToBaseA();
        TestDeepChainToBaseB();
        TestDeepChainToBaseC();
        TestDeepChainToBaseD();
        TestDiamondInheritance();

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

    // When a method takes a pointer to a secondary base, the C# binding must
    // pass the correctly-offset pointer. This test verifies the pointer offset
    // is applied (the critical bug fix for multiple inheritance).
    private static void TestSecondaryBasePointerOffset() {
        Console.WriteLine("TestSecondaryBasePointerOffset: passing derived as secondary base");
        using var container = new Container();
        using var node = new DerivedTextNode();
        node.set_id(99);

        // Container::attach_child takes PrimaryBase*, which is the secondary base
        // of DerivedTextNode (TextLike is primary). The binding must apply the
        // pointer offset when passing node to attach_child.
        container.attach_child(node);

        int lastId = container.get_last_child_id();
        Check(lastId == 99, $"Container received correct pointer (got ID: {lastId}, expected: 99)");
    }

    // Deep chain tests - these test recursive upcast chain finding
    // Level4 -> Level3 -> Level2 -> Level1 -> BaseA (4 hops!)
    private static void TestDeepChainToBaseA() {
        Console.WriteLine("TestDeepChainToBaseA: 4-hop chain Level4->Level3->Level2->Level1->BaseA");
        using var obj = new Level4();
        obj.set_a(111);
        obj.set_id(42);  // Also test that PrimaryBase is accessible

        using var tester = new DeepTester();
        tester.test_base_a(obj);

        Check(tester.get_last_a() == 111, $"BaseA via 4-hop chain (got: {tester.get_last_a()}, expected: 111)");
        Check(obj.get_id() == 42, $"PrimaryBase methods work (got: {obj.get_id()}, expected: 42)");
    }

    // Level4 -> Level3 -> Level2 -> BaseB (3 hops)
    private static void TestDeepChainToBaseB() {
        Console.WriteLine("TestDeepChainToBaseB: 3-hop chain Level4->Level3->Level2->BaseB");
        using var obj = new Level4();
        obj.set_b(222);

        using var tester = new DeepTester();
        tester.test_base_b(obj);

        Check(tester.get_last_b() == 222, $"BaseB via 3-hop chain (got: {tester.get_last_b()}, expected: 222)");
    }

    // Level4 -> Level3 -> BaseC (2 hops)
    private static void TestDeepChainToBaseC() {
        Console.WriteLine("TestDeepChainToBaseC: 2-hop chain Level4->Level3->BaseC");
        using var obj = new Level4();
        obj.set_c(333);

        using var tester = new DeepTester();
        tester.test_base_c(obj);

        Check(tester.get_last_c() == 333, $"BaseC via 2-hop chain (got: {tester.get_last_c()}, expected: 333)");
    }

    // Level4 -> BaseD (1 hop, direct secondary base)
    private static void TestDeepChainToBaseD() {
        Console.WriteLine("TestDeepChainToBaseD: 1-hop chain Level4->BaseD");
        using var obj = new Level4();
        obj.set_d(444);

        using var tester = new DeepTester();
        tester.test_base_d(obj);

        Check(tester.get_last_d() == 444, $"BaseD via 1-hop chain (got: {tester.get_last_d()}, expected: 444)");
    }

    // Diamond inheritance: DiamondBottom -> DiamondLeft (primary), DiamondRight (secondary)
    // Both DiamondLeft and DiamondRight inherit from DiamondBase (two separate subobjects).
    // Passing DiamondBottom as DiamondRight* must use the correct offset.
    private static void TestDiamondInheritance() {
        Console.WriteLine("TestDiamondInheritance: diamond pattern with secondary base offset");
        using var obj = new DiamondBottom();
        obj.set_left(55);
        obj.set_right(77);
        obj.set_bottom(99);

        using var tester = new DiamondTester();

        // Passing as DiamondLeft* (primary base, offset 0) should work trivially
        tester.test_left(obj);
        Check(tester.get_last_left() == 55, $"DiamondLeft via primary base (got: {tester.get_last_left()}, expected: 55)");

        // Passing as DiamondRight* (secondary base, non-zero offset) - this is the real test
        tester.test_right(obj);
        Check(tester.get_last_right() == 77, $"DiamondRight via secondary base (got: {tester.get_last_right()}, expected: 77)");
    }
}
