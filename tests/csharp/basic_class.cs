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
        TestCppOperators();
        TestNativeHandleEquality();
        TestArithmeticOperators();
        TestSmartInterfaceDefaults();
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

    private static void TestCppOperators() {
        Console.WriteLine("TestCppOperators: C++ operator methods");
        using var a = new Animal("match");
        using var b = new Animal("match");
        using var c = new Animal("other");

        Check(a.OpEq(b), "operator== maps to OpEq");
        Check(!a.OpNe(b), "operator!= maps to OpNe false for equal values");
        Check(!a.OpEq(c), "operator== maps to OpEq false for different values");
        Check(a.OpNe(c), "operator!= maps to OpNe true for different values");
        Check(a.Equals(b), "Equals maps to native operator==");
        Check(!a.Equals(c), "Equals maps to native operator== false for different values");
        Check(a.GetHashCode() == b.GetHashCode(), "value-equal wrappers have matching hash codes");
        Check(a == b, "C# operator == delegates to Equals");
        Check(a != c, "C# operator != delegates to Equals");

        IAnimal ia = a;
        IAnimal ib = b;
        IAnimal ic = c;
        Check(ia.Equals(ib), "interface-typed Equals dispatches to native operator==");
        Check(!ia.Equals(ic), "interface-typed Equals is false for different values");
    }

    private static void TestNativeHandleEquality() {
        Console.WriteLine("TestNativeHandleEquality: managed wrapper identity");
        using var item = new IdentityOnly(7);
        using var alias = item.CastTo<IdentityOnly>()!;
        using var other = new IdentityOnly(7);

        Check(!ReferenceEquals(item, alias), "borrowed alias is a distinct managed wrapper");
        Check(item == alias, "default operator == compares native handles");
        Check(!(item != alias), "default operator != compares native handles");
        Check(item.Equals(alias), "Equals compares native handles");
        Check(alias.Equals(item), "Equals is symmetric for native handles");
        Check(item.GetHashCode() == alias.GetHashCode(), "same native handle has same hash");
        Check(item != other, "different native handles are not ==");
        Check(!item.Equals(other), "different native handles are not Equals");
    }

    private static void TestArithmeticOperators() {
        Console.WriteLine("TestArithmeticOperators: C# operator aliases");
        using var two = new NumberBox(2);
        using var three = new NumberBox(3);

        Check(two + three == 5, "operator+ maps to native operator+");
        Check(-three == -3, "unary operator- maps to native operator-");
        Check(two < three, "operator< maps to native operator<");
        Check(three > two, "operator> maps to native operator>");

        INumberBox interfaceTwo = two;
        INumberBox interfaceThree = three;
        Check(interfaceTwo + interfaceThree == 5, "interface operator+ maps to native operator+");
        Check(-interfaceThree == -3, "interface unary operator- maps to native operator-");
        Check(interfaceTwo < interfaceThree, "interface operator< maps to native operator<");
        Check(interfaceThree > interfaceTwo, "interface operator> maps to native operator>");

        INumberBox offset = two.make_offset(4);
        try {
            Check(offset + three == 9, "operators work on interface-typed method returns");
        } finally {
            ((IDisposable)offset).Dispose();
        }
    }

    private static void TestSmartInterfaceDefaults() {
        Console.WriteLine("TestSmartInterfaceDefaults: generated interfaces are demand-driven");
        using var box = new SimpleBox(5);
        using var offset = box.make_offset(7);

        Check(box.get_value() == 5, "simple default concrete type constructs");
        Check(offset.get_value() == 12, "simple default method returns concrete type");
        Check(box.add(offset) == 17, "simple default method accepts concrete type");
        Check(BasicClassGlobals.add_simple_boxes(box, offset) == 17,
              "simple default global accepts concrete type");

        Check(!HasInterface(typeof(SimpleBox), "ISimpleBox"),
              "simple default type does not implement ISimpleBox");
        Check(!HasGeneratedType("Basic.Class.ISimpleBox"),
              "simple default interface type is not emitted");

        using var forced = new ForcedInterfaceBox(4);
        IForcedInterfaceBox forcedOffset = forced.make_offset(6);
        try {
            Check(forcedOffset.get_value() == 10, "forcecomplexinheritance method returns interface type");
            Check(forced.add(forcedOffset) == 14, "forcecomplexinheritance method accepts interface type");
        } finally {
            ((IDisposable)forcedOffset).Dispose();
        }
        Check(HasInterface(typeof(ForcedInterfaceBox), "IForcedInterfaceBox"),
              "forcecomplexinheritance type implements forced interface");
        Check(HasGeneratedType("Basic.Class.IForcedInterfaceBox"),
              "forcecomplexinheritance interface type is emitted");
    }

    private static bool HasInterface(Type type, string name) {
        foreach (Type iface in type.GetInterfaces()) {
            if (iface.Name == name) {
                return true;
            }
        }
        return false;
    }

    private static bool HasGeneratedType(string fullName) {
        return typeof(SimpleBox).Assembly.GetType(fullName, false) != null;
    }

    private static void TestEnum() {
        Console.WriteLine("TestEnum: enum param + return in free function");
        Check(BasicClassGlobals.brighter_than(Color.Red) == Color.Green,
              "brighter_than(RED) == GREEN");
        Check(BasicClassGlobals.brighter_than(Color.Green) == Color.Blue,
              "brighter_than(GREEN) == BLUE");
        Check(BasicClassGlobals.brighter_than(Color.Blue) == Color.Red,
              "brighter_than(BLUE) == RED");
    }
}
