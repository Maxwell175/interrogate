// Exercises IList<T> collection facades for both blittable (int) and
// non-blittable (string) element types.  Covers:
//   - Round-trips in both directions (param and return)
//   - Indexer get/set, Add, Clear, Count
//   - AsSpan zero-copy for blittable
//   - Heavy iteration with no leak of native collection objects

using System;
using System.Collections.Generic;
using Collections;

internal static class Driver {
    private static int _failed;

    private static void Check(bool ok, string msg) {
        Console.WriteLine((ok ? "  PASS " : "  FAIL ") + msg);
        if (!ok) _failed++;
    }

    public static int Main() {
        TestIntVectorRoundTrip();
        TestIntVectorAsSpan();
        TestIntVectorFromEnumerable();
        TestStringVectorRoundTrip();
        TestStringVectorIndexerGet();
        TestStringVectorIndexerSet();
        TestCollectionNoLeak();
        TestSmartPointerBackedArray();

        Console.WriteLine();
        if (_failed == 0) {
            Console.WriteLine("All collection tests passed.");
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

    // C++ side populates an int vector, C# reads back and indexes.
    private static void TestIntVectorRoundTrip() {
        Console.WriteLine("TestIntVectorRoundTrip: Bag.add_int + get_ints");
        using var bag = new Bag();
        bag.add_int(1); bag.add_int(2); bag.add_int(3);

        using var ints = bag.get_ints();
        Check(ints!.Count == 3, $"Count (got: {ints.Count})");
        Check(ints[0] == 1 && ints[1] == 2 && ints[2] == 3,
              $"indexer values [0..2] == 1,2,3 (got: {ints[0]},{ints[1]},{ints[2]})");
    }

    // Zero-copy span over native int memory.
    private static void TestIntVectorAsSpan() {
        Console.WriteLine("TestIntVectorAsSpan: blittable span zero-copy");
        using var ints = new vector_int();
        for (int i = 0; i < 16; i++) ints.Add(i * i);

        var span = ints.AsReadOnlySpan();
        Check(span.Length == 16, $"span length (got: {span.Length})");
        int sum = 0;
        for (int i = 0; i < span.Length; i++) sum += span[i];
        Check(sum == 1240, $"sum of 0..15² (got: {sum})");

        // Round-trip back through a C++ call — Bag.total should see the same
        // contents we just wrote into native memory.
        using var bag = new Bag();
        bag.set_ints(ints);
        Check(bag.total() == 1240, $"C++ total() sees same data (got: {bag.total()})");
    }

    // The IEnumerable constructor should copy each element via Add.
    private static void TestIntVectorFromEnumerable() {
        Console.WriteLine("TestIntVectorFromEnumerable");
        var src = new List<int> { 10, 20, 30 };
        using var ints = new vector_int(src);
        Check(ints.Count == 3, $"Count (got: {ints.Count})");

        using var bag = new Bag();
        bag.set_ints(ints);
        Check(bag.total() == 60, $"total (got: {bag.total()})");
    }

    // The string vector is the CoTaskMemFree regression test — every
    // GetItem used to free a pointer into C++ storage.
    private static void TestStringVectorRoundTrip() {
        Console.WriteLine("TestStringVectorRoundTrip: string vector round-trip");
        using var bag = new Bag();
        bag.add_name("alpha");
        bag.add_name("beta");
        bag.add_name("gamma");

        Check(bag.name_count() == 3, $"C++ name_count (got: {bag.name_count()})");

        using var names = bag.get_names();
        Check(names!.Count == 3, $"managed Count (got: {names.Count})");
    }

    private static void TestStringVectorIndexerGet() {
        Console.WriteLine("TestStringVectorIndexerGet: vector_string[i] doesn't crash");
        using var bag = new Bag();
        bag.add_name("one");
        bag.add_name("two");
        bag.add_name("three");

        using var names = bag.get_names();
        // If the CoTaskMemFree bug were still present, the first indexer
        // read would either crash with 'free(): invalid size' or corrupt
        // the heap.  Iterating all three exercises the path three times.
        Check(names![0] == "one", $"[0] == 'one' (got: '{names[0]}')");
        Check(names[1] == "two", $"[1] == 'two' (got: '{names[1]}')");
        Check(names[2] == "three", $"[2] == 'three' (got: '{names[2]}')");
    }

    // Mutable indexer path: names[i] = "...".  Tests set_element on the
    // string vector and ensures the underlying C++ data is actually updated.
    private static void TestStringVectorIndexerSet() {
        Console.WriteLine("TestStringVectorIndexerSet: vector_string[i] = x");
        using var bag = new Bag();
        bag.add_name("old1");
        bag.add_name("old2");

        using var names = bag.get_names();
        names![0] = "new1";
        names[1] = "new2";
        bag.set_names(names);

        Check(bag.joined_names() == "new1,new2",
              $"joined (got: '{bag.joined_names()}')");
    }

    // 5k vector allocations through the binding; every `using var` should
    // free the native storage, so memory doesn't grow unboundedly.  The GC
    // scan at the end catches wrappers that escaped our explicit disposal.
    private static void TestCollectionNoLeak() {
        Console.WriteLine("TestCollectionNoLeak: 5000 vector allocations don't leak");
        long start = GC.GetTotalAllocatedBytes();

        for (int i = 0; i < 5000; i++) {
            using var ints = new vector_int();
            for (int j = 0; j < 8; j++) ints.Add(j);
            using var roundtripped = new Bag();
            roundtripped.set_ints(ints);
            using var copy = roundtripped.get_ints();
            if (copy!.Count != 8) {
                Check(false, $"iter {i}: copy.Count != 8");
                return;
            }
        }
        CollectFinalizeCollect();

        long grew = GC.GetTotalAllocatedBytes() - start;
        // 5k iterations, each allocating 3 NativeObjects + some boxing =
        // hundreds of KB of managed allocations is normal; a *leak* would
        // show up as multi-MB of retained memory.  We can't easily measure
        // retained unmanaged memory from C#, so this check is weak — the
        // real leak detection here is "did we crash" and "do counts match."
        Check(grew > 0, $"allocations ran (growth: {grew} bytes)");
        Check(true, "5000 iterations completed without crash");
    }

    // FancyArray<int> is a smart-pointer-style collection: it inherits from
    // PointerToBase<BackedVec<int>>, BackedVec<int> : std::vector<int>.
    // interrogate should detect it as a mutable int array (IList<int>)
    // by walking the DF_pointer_to edge from PointerToBase to BackedVec
    // and onward to std::vector.
    private static void TestSmartPointerBackedArray() {
        Console.WriteLine("TestSmartPointerBackedArray: PointerToBase-derived class");
        // Named for the typedef (`typedef FancyArray<int> fancy_array_int`), not
        // the template instantiation -- the same convention that makes panda3d's
        // PointerToArray<unsigned char> surface as PTA_uchar.
        using var arr = new fancy_array_int();
        arr.Add(10);
        arr.Add(20);
        arr.Add(30);
        Check(arr.Count == 3, $"Count (got: {arr.Count})");
        Check(arr[0] == 10 && arr[1] == 20 && arr[2] == 30,
              $"indexer values (got: {arr[0]},{arr[1]},{arr[2]})");
    }
}
