#nullable enable

using System;
using System.Runtime.InteropServices;
using Interrogate;

namespace Basic.Class {
  internal static partial class NativeMethods {
    [LibraryImport("basic_class", EntryPoint = "_inCkh1TryQg")]
    internal static partial IntPtr Animal_Animal_ryQg(IntPtr param0);

    [LibraryImport("basic_class", EntryPoint = "_inCkh1Tq6nS", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr Animal_Animal_q6nS(string name);

    [LibraryImport("basic_class", EntryPoint = "_inCkh1TZzYm", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial string Animal_get_name_ZzYm(IntPtr _this);

    [LibraryImport("basic_class", EntryPoint = "_inCkh1TpTJQ", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void Animal_set_name_pTJQ(IntPtr _this, string name);

    [LibraryImport("basic_class", EntryPoint = "_inCkh1TKokn")]
    internal static partial int Animal_get_age_Kokn(IntPtr _this);

    [LibraryImport("basic_class", EntryPoint = "_inCkh1TMRQt")]
    internal static partial void Animal_set_age_MRQt(IntPtr _this, int age);

    [LibraryImport("basic_class", EntryPoint = "_inCkh1TqkLW")]
    internal static partial void Animal_speak_qkLW(IntPtr _this);

    [LibraryImport("basic_class", EntryPoint = "_inCkh1TuQX4")]
    internal static partial IntPtr Dog_Dog_uQX4(IntPtr param0);

    [LibraryImport("basic_class", EntryPoint = "_inCkh1TtYoF", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr Dog_Dog_tYoF(string name);

    [LibraryImport("basic_class", EntryPoint = "_inCkh1T7NSx")]
    internal static partial void Dog_fetch_7NSx(IntPtr _this);

    [LibraryImport("basic_class", EntryPoint = "_inCkh1T0ej4")]
    [return: MarshalAs(UnmanagedType.I1)]
    internal static partial bool Dog_is_good_boy_0ej4(IntPtr _this);

    [LibraryImport("basic_class", EntryPoint = "_inCSDestr_Animal")]
    internal static partial void Destroy_Animal(IntPtr self);

    [LibraryImport("basic_class", EntryPoint = "_inCSDestr_Dog")]
    internal static partial void Destroy_Dog(IntPtr self);

  }
}
