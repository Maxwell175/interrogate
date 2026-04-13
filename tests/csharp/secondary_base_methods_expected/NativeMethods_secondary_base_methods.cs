#nullable enable

using System;
using System.Runtime.InteropServices;
using Interrogate;

namespace Secondary.Base.Methods {
  internal static partial class NativeMethods {
    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5eB2pm")]
    internal static partial IntPtr PrimaryBase_PrimaryBase_B2pm();

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5eRQha")]
    internal static partial IntPtr PrimaryBase_PrimaryBase_RQha(IntPtr param0);

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5ekOzt")]
    internal static partial int PrimaryBase_get_id_kOzt(IntPtr _this);

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5eADHG")]
    internal static partial IntPtr TextLike_TextLike_ADHG();

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5e_xOf")]
    internal static partial IntPtr TextLike_TextLike_xOf(IntPtr param0);

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5emJyb", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void TextLike_set_text_mJyb(IntPtr _this, string text);

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5eyH6B", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial string TextLike_get_text_yH6B(IntPtr _this);

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5e5q0A")]
    internal static partial IntPtr DerivedTextNode_upcast_to_PrimaryBase_5q0A(IntPtr _this);

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5eX3xk")]
    internal static partial IntPtr PrimaryBase_downcast_to_DerivedTextNode_X3xk(IntPtr _this);

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5e7f_o")]
    internal static partial IntPtr DerivedTextNode_upcast_to_TextLike_7f_o(IntPtr _this);

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5e3Byt")]
    internal static partial IntPtr TextLike_downcast_to_DerivedTextNode_3Byt(IntPtr _this);

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5e_Az0")]
    internal static partial IntPtr DerivedTextNode_DerivedTextNode_Az0();

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCcK5eYlfM")]
    internal static partial IntPtr DerivedTextNode_DerivedTextNode_YlfM(IntPtr param0);

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCSDestr_PrimaryBase")]
    internal static partial void Destroy_PrimaryBase(IntPtr self);

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCSDestr_TextLike")]
    internal static partial void Destroy_TextLike(IntPtr self);

    [LibraryImport("secondary_base_methods", EntryPoint = "_inCSDestr_DerivedTextNode")]
    internal static partial void Destroy_DerivedTextNode(IntPtr self);

  }
}
