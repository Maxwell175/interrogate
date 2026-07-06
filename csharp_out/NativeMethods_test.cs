using System;
using System.Runtime.InteropServices;
using Interrogate;

namespace Test {
  internal static partial class NativeMethods {
    [DllImport("test", EntryPoint = "_inCcE_qSW3D", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Foo_bar_SW3D();

    [DllImport("test", EntryPoint = "_inCcE_qOWT8", CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Foo_baz_OWT8();

    [DllImport("test", EntryPoint = "_inCcE_qcCue", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr Foo_Foo_cCue();

    [DllImport("test", EntryPoint = "_inCcE_q6CqQ", CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr Foo_Foo_6CqQ(IntPtr param0);

    [DllImport("test", EntryPoint = "_inCSDestr_Foo", CallingConvention = CallingConvention.Cdecl)]
    internal static extern void Destroy_Foo(IntPtr self);

  }
}
