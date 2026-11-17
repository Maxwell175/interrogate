// Handle-type tests.  TypeHandle and ButtonHandle cross the C ABI as their
// index, so their wrappers hold an index, own nothing, and hand the index on
// wherever the C++ takes a handle by value or by const reference.
//
// Before this was so, a wrapper made from an index or a name deleted the index
// when it was released -- "free(): invalid pointer", disposed or finalized --
// and a const-reference parameter was given the index as an address.  The
// driver is written to compile against those bindings too, so that they are
// seen to crash rather than fail to build.

using System;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using Interrogate;
using Handles;

internal static class Driver {
    private static int _failed;

    private static void Check(bool ok, string msg) {
        Console.WriteLine((ok ? "  PASS " : "  FAIL ") + msg);
        if (!ok) _failed++;
    }

    public static int Main() {
        TestNothingNativeToDestroy();
        TestPointersToHandlesAreNotBound();
        TestReleasedWrappersFreeNothing();
        TestFinalizedWrappersFreeNothing();
        TestAnOwnedWrapperFreesNothing();
        TestAConstReferenceReturnIsTheIndex();
        TestAConstReferenceParameterTakesTheIndex();
        TestTypeHandleIsPassedTheSameWay();

        Console.WriteLine();
        if (_failed == 0) {
            Console.WriteLine("All handle tests passed.");
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

    // Before the fix a const-reference parameter took a wrapper, whose handle
    // is the index; after it, the index itself.  Calling through these keeps
    // one driver for both bindings.
    private static ButtonHandle Wrap(int index) =>
        ButtonHandle.__CreateFromNative((IntPtr)index, NativeOwnership.Borrowed)!;
    private static TypeHandle WrapType(int index) =>
        TypeHandle.__CreateFromNative((IntPtr)index, NativeOwnership.Borrowed)!;
    private static bool Pass(Func<ButtonHandle, bool> call, int index) => call(Wrap(index));
    private static bool Pass(Func<int, bool> call, int index) => call(index);
    private static int Pass(Func<ButtonHandle, int> call, int index) => call(Wrap(index));
    private static int Pass(Func<int, int> call, int index) => call(index);
    private static bool PassType(Func<TypeHandle, bool> call, int index) => call(WrapType(index));
    private static bool PassType(Func<int, bool> call, int index) => call(index);
    private static int IndexOf(ButtonHandle handle) => handle.GetIndex();
    private static int IndexOf(int index) => index;

    // Neither the bindings nor the native library carry a destructor for a
    // handle: there is nothing to destroy.
    private static void TestNothingNativeToDestroy() {
        Console.WriteLine("TestNothingNativeToDestroy");
        var native = typeof(ButtonList).Assembly.GetTypes()
            .Where(t => t.Name.StartsWith("NativeMethods"))
            .SelectMany(t => t.GetMethods(BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public))
            .Select(m => m.Name)
            .ToList();
        Check(native.Count > 0, $"NativeMethods found ({native.Count} entry points)");
        Check(!native.Any(n => n.StartsWith("Destroy_ButtonHandle")), "no Destroy_ButtonHandle");
        Check(!native.Any(n => n.StartsWith("Destroy_TypeHandle")), "no Destroy_TypeHandle");
        Check(native.Any(n => n.StartsWith("Destroy_ButtonList")), "control: Destroy_ButtonList is bound");

        // The native helpers are named _inCSDestr_<library hash>_<type>; take
        // the hash from the one the control binds.
        var control = typeof(ButtonList).Assembly.GetTypes()
            .Where(t => t.Name.StartsWith("NativeMethods"))
            .SelectMany(t => t.GetMethods(BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public))
            .First(m => m.Name.StartsWith("Destroy_ButtonList"))
            .GetCustomAttribute<LibraryImportAttribute>()!.EntryPoint!;
        IntPtr library = NativeLibrary.Load("handles", typeof(ButtonList).Assembly, null);
        Check(NativeLibrary.TryGetExport(library, control, out _), $"control: the library exports {control}");
        foreach (string type in new[] { "ButtonHandle", "TypeHandle" }) {
            string entry = control.Replace("ButtonList", type);
            Check(!NativeLibrary.TryGetExport(library, entry, out _), $"the library exports no {entry}");
        }

        using var fromIndex = new ButtonHandle(1);
        using var fromName = new ButtonHandle("enter");
        Check(fromIndex.Ownership == NativeOwnership.Borrowed,
              $"a handle made from an index owns nothing (got {fromIndex.Ownership})");
        Check(fromName.Ownership == NativeOwnership.Borrowed,
              $"a handle made from a name owns nothing (got {fromName.Ownership})");
        using var list = new ButtonList();
        Check(list.Ownership == NativeOwnership.Owned, "control: a ButtonList owns its object");
    }

    // A non-const reference or a pointer to a handle would take the index as an
    // address, so those methods are left unbound.
    private static void TestPointersToHandlesAreNotBound() {
        Console.WriteLine("TestPointersToHandlesAreNotBound");
        var names = typeof(ButtonList).GetMethods().Select(m => m.Name).ToHashSet();
        Check(!names.Contains("GetFirstButton"), "get_first_button(ButtonHandle &) is not bound");
        Check(!names.Contains("AddButtonPtr"), "add_button_ptr(ButtonHandle const *) is not bound");
        Check(names.Contains("AddButtonRef"), "control: add_button_ref(ButtonHandle const &) is bound");
        var parameter = typeof(ButtonList).GetMethod("HasButton")!.GetParameters()[0].ParameterType;
        Check(parameter == typeof(int), $"a const-reference parameter is the index (got {parameter.Name})");
    }

    // The reported abort: every handle released by Dispose, from an index or a
    // name.  The wrapper's handle is the index, and keeps working until then.
    private static void TestReleasedWrappersFreeNothing() {
        Console.WriteLine("TestReleasedWrappersFreeNothing");
        var fromIndex = new ButtonHandle(1);
        Check(fromIndex.GetName() == "mouse1", $"index 1 is mouse1 (got {fromIndex.GetName()})");
        Check(fromIndex.NativeHandle == (IntPtr)1, "the wrapper holds the index");
        fromIndex.Dispose();
        Check(true, "disposed a handle made from an index");

        var fromName = new ButtonHandle("shift");
        Check(fromName.GetIndex() == 3, $"shift is index 3 (got {fromName.GetIndex()})");
        fromName.Dispose();
        Check(true, "disposed a handle made from a name");

        for (int i = 0; i < 1000; i++) {
            using var h = new ButtonHandle(i % HandlesGlobals.handles_button_count());
        }
        Check(true, "disposed 1000 more");
    }

    private static void AbandonHandles() {
        for (int i = 0; i < 1000; i++) {
            _ = new ButtonHandle(1 + i % 3);
            _ = new ButtonHandle("enter");
        }
    }

    // The same, released by the finalizer thread, where the crash dump caught it.
    private static void TestFinalizedWrappersFreeNothing() {
        Console.WriteLine("TestFinalizedWrappersFreeNothing");
        AbandonHandles();
        CollectFinalizeCollect();
        Check(true, "finalized 2000 handles made from indices and names");
    }

    // A wrapper made by a path that marks it owned (a cast that takes
    // ownership, a collection element) still has nothing to delete.
    private static void TestAnOwnedWrapperFreesNothing() {
        Console.WriteLine("TestAnOwnedWrapperFreesNothing");
        var owned = ButtonHandle.__CreateFromNative((IntPtr)2, NativeOwnership.Owned)!;
        Check(owned.GetName() == "enter", $"index 2 is enter (got {owned.GetName()})");
        owned.Dispose();
        Check(true, "disposed a handle marked owned");
    }

    // A handle returned by const reference arrives as its index, as one
    // returned by value does.  Before the fix it was a pointer into the list,
    // which the wrapper's methods then passed on as an index.
    private static void TestAConstReferenceReturnIsTheIndex() {
        Console.WriteLine("TestAConstReferenceReturnIsTheIndex");
        using var list = new ButtonList();
        list.AddButton(1);
        list.AddButton(3);
        Check(list.GetButton(0) == 1, "by value: button 0 is 1");
        Check(IndexOf(list.GetLastButton()) == 3,
              $"by const reference: the last button is 3 (got {IndexOf(list.GetLastButton())})");
        Check(new ButtonHandle(2).GetAlias() == 1, "by value: enter's alias is mouse1");
    }

    // The second half of the fault: the index given where a const reference
    // was wanted was read as an address.
    private static void TestAConstReferenceParameterTakesTheIndex() {
        Console.WriteLine("TestAConstReferenceParameterTakesTheIndex");
        using var list = new ButtonList();
        Check(Pass(list.AddButtonRef, 2), "added enter by const reference");
        Check(!Pass(list.AddButtonRef, 2), "enter is not added twice");
        Check(Pass(list.HasButton, 2), "the list has enter");
        Check(!Pass(list.HasButton, 1), "the list has no mouse1");
        Check(list.GetNumButtons() == 1, $"one button (got {list.GetNumButtons()})");

        using var shift = new ButtonHandle("shift");
        Check(Pass(shift.Matches, 3), "shift matches index 3");
        Check(!Pass(shift.Matches, 1), "shift does not match mouse1");
        Check(Pass(shift.CompareTo, 1) == 2, "shift compares 2 above mouse1");
    }

    private static void TestTypeHandleIsPassedTheSameWay() {
        Console.WriteLine("TestTypeHandleIsPassedTheSameWay");
        using var list = new ButtonList();
        list.SetKind(7);
        Check(list.GetKind() == 7, $"by value: the kind is 7 (got {list.GetKind()})");
        Check(PassType(list.IsKind, 7), "by const reference: the kind is 7");
        Check(!PassType(list.IsKind, 6), "by const reference: the kind is not 6");

        var kind = WrapType(7);
        Check(kind.GetName() == "type7", $"a TypeHandle's methods read its index (got {kind.GetName()})");
        var owned = TypeHandle.__CreateFromNative((IntPtr)7, NativeOwnership.Owned)!;
        owned.Dispose();
        Check(true, "disposed a TypeHandle marked owned");
    }
}
