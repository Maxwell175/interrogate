using System;
using Basic.Class;

internal static class Driver {
    private static int _failed;

    private static void Check(bool ok, string msg) {
        Console.WriteLine((ok ? "  PASS " : "  FAIL ") + msg);
        if (!ok) _failed++;
    }

    public static int Main() {
        TestStringRoundTrip();
        TestGettersSetters();
        TestSubclass();
        TestEnum();

        Console.WriteLine();
        if (_failed == 0) {
            Console.WriteLine("All basic_class tests passed.");
            return 0;
        }
        Console.WriteLine($"{_failed} test(s) failed.");
        return 1;
    }

    // Constructor takes a std::string; getter returns std::string.  This is
    // the regression test for the CoTaskMemFree bug on string returns.
    private static void TestStringRoundTrip() {
        Console.WriteLine("TestStringRoundTrip: string param -> C++ -> string return");
        using var animal = new Animal("Luna");
        Check(animal.get_name() == "Luna",
              $"get_name() after construction (got: '{animal.get_name()}')");

        animal.set_name("Rex");
        Check(animal.get_name() == "Rex",
              $"get_name() after set_name (got: '{animal.get_name()}')");
    }

    private static void TestGettersSetters() {
        Console.WriteLine("TestGettersSetters: int fields");
        using var animal = new Animal("Bella");
        Check(animal.get_age() == 0, $"default age is 0 (got: {animal.get_age()})");
        animal.set_age(7);
        Check(animal.get_age() == 7, $"age after set (got: {animal.get_age()})");
    }

    // Dog inherits from Animal; Dog.speak overrides.  Verify both the
    // inherited getter and the override dispatch.
    private static void TestSubclass() {
        Console.WriteLine("TestSubclass: inheritance and overrides");
        using var dog = new Dog("Fido");
        Check(dog.get_name() == "Fido", "inherited get_name works");
        Check(dog.is_good_boy() == true, "Dog.is_good_boy");

        string said = dog.speak();
        Check(said.Contains("barks"), $"Dog.speak contains 'barks' (got: '{said}')");
    }

    private static void TestEnum() {
        Console.WriteLine("TestEnum: enum param + return in free function");
        Check(BasicClassGlobals.brighter_than(Color.RED) == Color.GREEN,
              "brighter_than(RED) == GREEN");
        Check(BasicClassGlobals.brighter_than(Color.GREEN) == Color.BLUE,
              "brighter_than(GREEN) == BLUE");
        Check(BasicClassGlobals.brighter_than(Color.BLUE) == Color.RED,
              "brighter_than(BLUE) == RED");
    }
}
