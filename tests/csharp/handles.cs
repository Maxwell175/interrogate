// Handle-type tests.  TypeHandle and ButtonHandle cross the C ABI as their
// index, so their wrappers hold an index, own nothing, and hand the index on
// wherever the C++ takes a handle by value or by const reference.  A const
// reference keeps the handle in the C# signature, as a parameter and as a
// return value (null for index 0); a pointer or a non-const reference goes by
// ref, through a temporary the C wrapper builds from the index, and a null
// handle reaches a pointer as a null pointer; and two handles are equal, and
// hash alike, when their indices are.
//
// Before this was so, a wrapper made from an index or a name deleted the index
// when it was released -- "free(): invalid pointer", disposed or finalized --
// and a const-reference parameter was given the index as an address.  The
// driver is written to compile against those bindings too, and against each
// later one (a const reference as an int; pointers unbound; a const reference
// returned as an int; a null handle given to a pointer as index 0), so that they are
// seen to crash or fail rather than fail to build: what one of them lacks is
// reached by reflection.

using System;
using System.Collections.Generic;
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
        TestReleasedWrappersFreeNothing();
        TestFinalizedWrappersFreeNothing();
        TestAnOwnedWrapperFreesNothing();
        TestAConstReferenceReturnIsAHandle();
        TestAConstReferenceElementIsAHandle();
        TestAConstReferenceParameterKeepsTheHandle();
        TestTypeHandleIsPassedTheSameWay();
        TestAConstReferenceConstructorParameterKeepsTheHandle();
        TestAPointerOrReferenceGoesByRef();
        TestANullHandleIsANullPointer();
        TestOnlyMethodsTakeAHandleByRef();
        TestHandlesCompareByIndex();

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

    // A const-reference parameter takes the handle, or (in the older binding)
    // its index.  Calling through these keeps one driver for both.
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
    // The index a returned value stands for: the handle's, or (in the older
    // binding) the int itself.
    private static int IndexOf(object? handle) =>
        handle is int index ? index : (int)NativeObject.Unwrap(handle as INativeObject);
    private static bool IsNull(object? value) => value is null;

    private static readonly NullabilityInfoContext Nullability = new();

    // "ButtonHandle?" for a nullable handle, else the type's name.
    private static string Describe(Type type, NullabilityInfo info) =>
        type.Name + (!type.IsValueType && info.ReadState == NullabilityState.Nullable ? "?" : "");
    private static string ReturnOf(Type type, string method) {
        MethodInfo info = type.GetMethod(method)!;
        return Describe(info.ReturnType, Nullability.Create(info.ReturnParameter));
    }
    private static string PropertyOf(Type type, string property) {
        PropertyInfo? info = type.GetProperty(property);
        return info == null ? "no such property" : Describe(info.PropertyType, Nullability.Create(info));
    }
    // The type argument n of a generic property's type, as Describe gives it.
    private static string TypeArgumentOf(Type type, string property, int n) {
        PropertyInfo? info = type.GetProperty(property);
        if (info == null) return "no such property";
        if (!info.PropertyType.IsGenericType) return info.PropertyType.Name;
        return Describe(info.PropertyType.GetGenericArguments()[n],
                        Nullability.Create(info).GenericTypeArguments[n]);
    }

    // The P/Invoke a binding calls for a C wrapper, by its name's prefix.
    private static MethodInfo NativeMethod(string prefix) =>
        typeof(ButtonList).Assembly.GetTypes()
            .Where(t => t.Name.StartsWith("NativeMethods"))
            .SelectMany(t => t.GetMethods(BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public))
            .First(m => m.Name.StartsWith(prefix + "_"));

    private static Type ParameterType(Type type, string method) =>
        type.GetMethod(method)!.GetParameters()[0].ParameterType;

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

    // A handle returned by const reference keeps its handle type, as a
    // parameter does, and index 0 comes back as null; one returned by value is
    // its index.  Before V2 it was a pointer into the list, which the
    // wrapper's methods then passed on as an index; after V2 it was the index.
    private static void TestAConstReferenceReturnIsAHandle() {
        Console.WriteLine("TestAConstReferenceReturnIsAHandle");
        foreach (var (type, method, handle) in new[] {
                     (typeof(ButtonList), "GetLastButton", "ButtonHandle?"),
                     (typeof(ButtonList), "GetNoButton", "ButtonHandle?"),
                     (typeof(ButtonList), "GetButtonRef", "ButtonHandle?"),
                     (typeof(ButtonList), "GetKindRef", "TypeHandle?"),
                     (typeof(ButtonRow), "GetNamed", "ButtonHandle?") }) {
            string returned = ReturnOf(type, method);
            Check(returned == handle, $"{type.Name}.{method} returns a {handle} (got {returned})");
        }
        foreach (var (type, method) in new[] {
                     (typeof(ButtonList), "GetButton"), (typeof(ButtonList), "GetKind"),
                     (typeof(ButtonHandle), "GetAlias") }) {
            string returned = ReturnOf(type, method);
            Check(returned == "Int32", $"control: by value, {type.Name}.{method} returns the index (got {returned})");
        }
        foreach (string native in new[] { "ButtonList_get_last_button", "ButtonList_get_kind_ref" }) {
            Type returned = NativeMethod(native).ReturnType;
            Check(returned == typeof(int), $"the P/Invoke {native} returns the index (got {returned.Name})");
        }

        using var list = new ButtonList();
        list.AddButton(1);
        list.AddButton(3);
        list.SetKind(7);
        Check(list.GetButton(0) == 1, "control: by value, button 0 is 1");
        Check(new ButtonHandle(2).GetAlias() == 1, "control: by value, enter's alias is mouse1");
        Check(IndexOf(list.GetLastButton()) == 3,
              $"by const reference: the last button is 3 (got {IndexOf(list.GetLastButton())})");
        Check(IndexOf(list.GetButtonRef(0)) == 1,
              $"by const reference: button 0 is 1 (got {IndexOf(list.GetButtonRef(0))})");
        Check(IndexOf(list.GetKindRef()) == 7, $"by const reference: the kind is 7 (got {IndexOf(list.GetKindRef())})");
        Check(IsNull(list.GetNoButton()), $"index 0 by const reference is null (got {list.GetNoButton()})");
        var none = new ButtonList();
        Check(IsNull(none.GetKindRef()), "a TypeHandle of index 0 by const reference is null");

        string property = PropertyOf(typeof(ButtonList), "LastButton");
        Check(property == "ButtonHandle?", $"the property LastButton is a ButtonHandle? (got {property})");
        object? last = typeof(ButtonList).GetProperty("LastButton")?.GetValue(list);
        Check(IndexOf(last) == 3, $"the property LastButton is shift (got {IndexOf(last)})");
    }

    // The same for every other way a binding returns one: the elements of a
    // sequence property, of an indexer and of a map property.
    private static void TestAConstReferenceElementIsAHandle() {
        Console.WriteLine("TestAConstReferenceElementIsAHandle");
        foreach (var (type, property, n) in new[] {
                     (typeof(ButtonList), "Buttons", 0), (typeof(ButtonRow), "Named", 1) }) {
            string element = TypeArgumentOf(type, property, n);
            Check(element == "ButtonHandle?", $"{type.Name}.{property} holds ButtonHandle? (got {element})");
        }
        string indexer = PropertyOf(typeof(ButtonRow), "Item");
        Check(indexer == "ButtonHandle?", $"ButtonRow's indexer gives a ButtonHandle? (got {indexer})");

        using var list = new ButtonList();
        list.AddButton(0);
        list.AddButton(2);
        var buttons = (System.Collections.IEnumerable?)typeof(ButtonList).GetProperty("Buttons")?.GetValue(list);
        var held = buttons?.Cast<object?>().ToList() ?? new List<object?>();
        Check(held.Count == 2 && IsNull(held[0]) && IndexOf(held[1]) == 2,
              $"Buttons holds null and enter (got {string.Join(", ", held.Select(h => h?.ToString() ?? "null"))})");

        using var row = new ButtonRow();
        PropertyInfo? item = typeof(ButtonRow).GetProperty("Item");
        var byPosition = Enumerable.Range(0, 3).Select(i => item?.GetValue(row, new object[] { i })).ToList();
        Check(IsNull(byPosition[0]) && IndexOf(byPosition[1]) == 1 && IndexOf(byPosition[2]) == 2,
              $"the indexer gives null, mouse1 and enter (got {string.Join(", ", byPosition.Select(h => h?.ToString() ?? "null"))})");
        object? named = typeof(ButtonRow).GetProperty("Named")?.GetValue(row);
        PropertyInfo? lookup = named?.GetType().GetProperty("Item");
        object? enter = lookup?.GetValue(named, new object[] { "enter" });
        object? nothing = lookup?.GetValue(named, new object[] { "none" });
        Check(IndexOf(enter) == 2 && lookup != null, $"Named[\"enter\"] is enter (got {IndexOf(enter)})");
        Check(IsNull(nothing) && lookup != null, "Named[\"none\"], index 0, is null");
    }

    // A const-reference parameter keeps the handle in C#, and the wrapper's
    // index is what the C wrapper is given.  Given as an address, as it once
    // was, the C++ read the index as one.
    private static void TestAConstReferenceParameterKeepsTheHandle() {
        Console.WriteLine("TestAConstReferenceParameterKeepsTheHandle");
        foreach (var (type, method) in new[] {
                     (typeof(ButtonList), "HasButton"), (typeof(ButtonList), "AddButtonRef"),
                     (typeof(ButtonHandle), "Matches"), (typeof(ButtonHandle), "CompareTo") }) {
            Type parameter = ParameterType(type, method);
            Check(parameter == typeof(ButtonHandle),
                  $"{type.Name}.{method} takes a ButtonHandle (got {parameter.Name})");
        }
        Type byValue = ParameterType(typeof(ButtonList), "AddButton");
        Check(byValue == typeof(int), $"control: a handle by value is its index (got {byValue.Name})");

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
        Type parameter = ParameterType(typeof(ButtonList), "IsKind");
        Check(parameter == typeof(TypeHandle), $"ButtonList.IsKind takes a TypeHandle (got {parameter.Name})");

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

    // The constructors take a const-reference handle the same way: the
    // implicit copy constructor of a handle, and ButtonList's.
    private static void TestAConstReferenceConstructorParameterKeepsTheHandle() {
        Console.WriteLine("TestAConstReferenceConstructorParameterKeepsTheHandle");
        ConstructorInfo? copy = typeof(ButtonHandle).GetConstructor(new[] { typeof(ButtonHandle) });
        int copied = copy == null ? -1 : ((ButtonHandle)copy.Invoke(new object[] { Wrap(3) })).GetIndex();
        Check(copied == 3, $"ButtonHandle(ButtonHandle) copies shift (got {(copy == null ? "no such constructor" : copied.ToString())})");
        ConstructorInfo? first = typeof(ButtonList).GetConstructor(new[] { typeof(ButtonHandle) });
        using var list = (ButtonList?)first?.Invoke(new object[] { Wrap(2) });
        Check(list != null && list.GetNumButtons() == 1 && list.GetButton(0) == 2,
              $"ButtonList(ButtonHandle) holds enter (got {(list == null ? "no such constructor" : list.GetNumButtons() + " buttons")})");
    }

    // Invokes a method taking one handle by ref; null if it is not bound so.
    private static (object? result, object? handle)? CallByRef(object target, string method, Type type, object? handle) {
        MethodInfo? info = target.GetType().GetMethod(method);
        ParameterInfo[]? parameters = info?.GetParameters();
        if (info == null || parameters!.Length != 1 || !parameters[0].ParameterType.IsByRef ||
            parameters[0].ParameterType.GetElementType() != type) {
            Check(false, $"{target.GetType().Name}.{method} takes a handle by ref (got " +
                  (info == null ? "nothing" : string.Join(", ", parameters!.Select(p => p.ParameterType.Name))) + ")");
            return null;
        }
        var args = new[] { handle };
        object? result = info.Invoke(target, args);
        return (result, args[0]);
    }

    // A pointer or a non-const reference to a handle goes by ref: the C wrapper
    // builds a local handle from the index it is pointed to, calls with it, and
    // writes its index back, and the C# variable then holds that index.
    private static void TestAPointerOrReferenceGoesByRef() {
        Console.WriteLine("TestAPointerOrReferenceGoesByRef");
        foreach (var (method, handle) in new[] {
                     ("NextButton", typeof(ButtonHandle)), ("GetFirstButton", typeof(ButtonHandle)),
                     ("ClearButton", typeof(ButtonHandle)), ("AddButtonPtr", typeof(ButtonHandle)),
                     ("NextKind", typeof(TypeHandle)) }) {
            MethodInfo? info = typeof(ButtonList).GetMethod(method);
            Type? parameter = info?.GetParameters()[0].ParameterType;
            Check(parameter != null && parameter.IsByRef && parameter.GetElementType() == handle,
                  $"ButtonList.{method} takes a {handle.Name} by ref (got {parameter?.Name ?? "nothing"})");
        }

        using var list = new ButtonList();
        list.AddButton(1);
        list.AddButton(2);
        list.AddButton(3);

        ButtonHandle enter = Wrap(2);
        if (CallByRef(list, "NextButton", typeof(ButtonHandle), enter) is var (found, next)) {
            Check(found is true, "next_button read enter and found the button after it");
            Check(IndexOf(next) == 3, $"next_button wrote shift back (got {IndexOf(next)})");
            Check(enter.GetIndex() == 2, "control: the wrapper given is unchanged: it is a value");
        }
        ButtonHandle shift = Wrap(3);
        if (CallByRef(list, "NextButton", typeof(ButtonHandle), shift) is var (more, same)) {
            Check(more is false, "control: next_button found nothing after shift");
            Check(ReferenceEquals(same, shift), "an unchanged index leaves the same wrapper");
        }
        if (CallByRef(list, "GetFirstButton", typeof(ButtonHandle), null) is var (_, firstButton)) {
            Check(IndexOf(firstButton) == 1, $"get_first_button wrote mouse1 over null (got {IndexOf(firstButton)})");
        }
        if (CallByRef(list, "ClearButton", typeof(ButtonHandle), Wrap(3)) is var (_, cleared)) {
            Check(cleared == null, $"clear_button left index 0, which comes back as null (got {IndexOf(cleared)})");
        }
        if (CallByRef(list, "AddButtonPtr", typeof(ButtonHandle), enter) is var (_, added)) {
            Check(list.GetNumButtons() == 4 && list.GetButton(3) == 2,
                  $"add_button_ptr read enter through the pointer (got {list.GetNumButtons()} buttons)");
            Check(ReferenceEquals(added, enter), "a const pointer leaves the same wrapper");
        }
        if (CallByRef(list, "NextKind", typeof(TypeHandle), WrapType(7)) is var (_, kind)) {
            Check(IndexOf(kind) == 8, $"next_kind read kind 7 and wrote 8 (got {IndexOf(kind)})");
        }
    }

    // A pointer may be null: a null handle reaches the C++ as a null pointer,
    // which it may test, and nothing is written back to it.  A wrapper of
    // index 0 is a handle, not a null pointer.  (A reference cannot be null,
    // and there null is index 0: see get_first_button above.)
    private static void TestANullHandleIsANullPointer() {
        Console.WriteLine("TestANullHandleIsANullPointer");
        foreach (string native in new[] {
                     "ButtonList_is_null_button", "ButtonList_fill_button", "ButtonList_fill_kind",
                     "ButtonList_clear_button", "ButtonList_add_button_ptr" }) {
            Type parameter = NativeMethod(native).GetParameters()[1].ParameterType;
            Check(parameter == typeof(IntPtr), $"the P/Invoke {native} takes a pointer (got {parameter.Name})");
        }
        foreach (string native in new[] {
                     "ButtonList_next_button", "ButtonList_get_first_button", "ButtonList_next_kind" }) {
            Type parameter = NativeMethod(native).GetParameters()[1].ParameterType;
            Check(parameter == typeof(int).MakeByRefType(),
                  $"control: the P/Invoke {native}, a reference, takes the index by ref (got {parameter.Name})");
        }

        using var list = new ButtonList();
        list.AddButton(1);
        list.SetKind(7);
        if (CallByRef(list, "IsNullButton", typeof(ButtonHandle), null) is var (isNull, stillNull)) {
            Check(isNull is true, "a null handle reaches is_null_button as a null pointer");
            Check(stillNull == null, "control: and it stays null");
        }
        // Wrap(0) is null, as __CreateFromNative gives; a constructor makes a wrapper.
        if (CallByRef(list, "IsNullButton", typeof(ButtonHandle), new ButtonHandle(0)) is var (zeroIsNull, _)) {
            Check(zeroIsNull is false, "a wrapper of index 0 reaches it as a pointer to index 0");
        }
        if (CallByRef(list, "IsNullButton", typeof(ButtonHandle), Wrap(2)) is var (enterIsNull, _)) {
            Check(enterIsNull is false, "control: a wrapper of enter reaches it as a pointer");
        }
        if (CallByRef(list, "FillButton", typeof(ButtonHandle), null) is var (filled, notFilled)) {
            Check(filled is false, "fill_button was given a null pointer and wrote nothing");
            Check(notFilled == null, $"a null handle stays null (got {IndexOf(notFilled)})");
        }
        if (CallByRef(list, "FillButton", typeof(ButtonHandle), new ButtonHandle(0)) is var (filledZero, first)) {
            Check(filledZero is true && IndexOf(first) == 1,
                  $"control: fill_button wrote mouse1 over index 0 (got {IndexOf(first)})");
        }
        if (CallByRef(list, "FillKind", typeof(TypeHandle), null) is var (filledKind, noKind)) {
            Check(filledKind is false && noKind == null, "a null TypeHandle reaches fill_kind as a null pointer");
        }
        if (CallByRef(list, "FillKind", typeof(TypeHandle), new TypeHandle()) is var (filledKindZero, kind)) {
            Check(filledKindZero is true && IndexOf(kind) == 7,
                  $"control: fill_kind wrote kind 7 over index 0 (got {IndexOf(kind)})");
        }
    }

    // A constructor or an operator would have to read the handle back too;
    // they are not written, and the method an operator names still is.
    private static void TestOnlyMethodsTakeAHandleByRef() {
        Console.WriteLine("TestOnlyMethodsTakeAHandleByRef");
        bool byRefCtor = typeof(ButtonList).GetConstructors()
            .Any(c => c.GetParameters().Length == 2);
        Check(!byRefCtor, "ButtonList(ButtonHandle &, int) is not bound");
        bool byPointerCtor = typeof(ButtonList).GetConstructors()
            .Any(c => c.GetParameters().Length == 3);
        Check(!byPointerCtor, "ButtonList(const ButtonHandle *, int, int) is not bound");
        Check(typeof(ButtonList).GetMethod("op_Addition") == null, "ButtonList has no C# operator +");
        Check(typeof(ButtonList).GetMethod("op_Subtraction") == null, "ButtonList has no C# operator -");

        MethodInfo? OperatorMethod(string name) => typeof(ButtonList).GetMethods()
            .FirstOrDefault(m => m.Name.Contains(name) && m.ReturnType == typeof(ButtonList) &&
                                 m.GetParameters().Length == 1 &&
                                 m.GetParameters()[0].ParameterType == typeof(ButtonHandle).MakeByRefType());
        MethodInfo? plus = OperatorMethod("add");
        Check(plus != null, $"operator + is bound as a method taking the handle by ref ({plus?.Name ?? "none"})");
        if (plus != null) {
            using var list = new ButtonList();
            var args = new object?[] { Wrap(1) };
            using var sum = (ButtonList)plus.Invoke(list, args)!;
            Check(sum.GetNumButtons() == 1 && sum.GetButton(0) == 1,
                  $"the sum holds mouse1 (got {sum.GetNumButtons()} buttons)");
        }
        MethodInfo? minus = OperatorMethod("sub");
        Check(minus != null, $"operator - is bound as a method taking the handle by ref ({minus?.Name ?? "none"})");
        if (minus != null) {
            using var list = new ButtonList();
            list.AddButton(1);
            list.AddButton(2);
            list.AddButton(0);
            using var less = (ButtonList)minus.Invoke(list, new object?[] { Wrap(1) })!;
            Check(less.GetNumButtons() == 2 && less.GetButton(0) == 2,
                  $"control: the difference leaves out mouse1 (got {less.GetNumButtons()} buttons)");
            // Given index 0 instead of a null pointer, it would leave out index 0.
            using var same = (ButtonList)minus.Invoke(list, new object?[] { null })!;
            Check(same.GetNumButtons() == 3,
                  $"a null handle reaches operator - as a null pointer, which leaves out nothing (got {same.GetNumButtons()} buttons)");
        }
    }

    // Two wrappers of one index are one handle: equal, with the same hash, and
    // compared in C# without asking the C++ == or !=.
    private static void TestHandlesCompareByIndex() {
        Console.WriteLine("TestHandlesCompareByIndex");
        var enter = new ButtonHandle(2);
        var named = new ButtonHandle("enter");
        var shift = new ButtonHandle(3);
        int calls = HandlesGlobals.handles_equality_calls();

        Check(enter.Equals(named), "two wrappers of enter are equal");
        Check(!enter.Equals(shift), "control: enter is not shift");
        Check(enter == named && !(enter != named), "control: == and != agree");
        Check(enter != shift && !(enter == shift), "control: == and != tell enter from shift");
        Check(HandlesGlobals.handles_equality_calls() == calls,
              $"compared without the C++ == or != (it ran {HandlesGlobals.handles_equality_calls() - calls} times)");

        Check(enter.GetHashCode() == 2 && shift.GetHashCode() == 3,
              $"the hash is the index (got {enter.GetHashCode()} and {shift.GetHashCode()})");
        var set = new HashSet<ButtonHandle> { enter, named, shift };
        Check(set.Count == 2, $"a set holds enter once (got {set.Count})");
        var names = new Dictionary<ButtonHandle, string> { [enter] = "enter" };
        Check(names.TryGetValue(named, out string? name) && name == "enter",
              "another wrapper of enter finds it in a dictionary");

        ButtonHandle? none = null;
        Check(none == new ButtonHandle(0), "null is index 0 to ==");

        var kind = WrapType(7);
        calls = HandlesGlobals.handles_equality_calls();
        Check(kind.Equals(WrapType(7)) && kind == WrapType(7) && kind != WrapType(8),
              "two TypeHandle wrappers of 7 are equal, and not 8");
        Check(kind.GetHashCode() == 7, $"a TypeHandle's hash is its index (got {kind.GetHashCode()})");
        Check(HandlesGlobals.handles_equality_calls() == calls,
              "a TypeHandle is compared without the C++ == or !=");
    }
}
