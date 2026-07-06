using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Test {
  public interface IFoo {
  }

  public partial class Foo : NativeObject, IFoo, IDisposable {
    internal static Foo __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new Foo(ptr, own);
    }

    internal Foo(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public Foo() : this(NativeMethods.Foo_Foo_cCue(), NativeOwnership.Owned) {
    }

    public Foo(Foo param0) : this(NativeMethods.Foo_Foo_6CqQ(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }


    public static int bar() {
      return NativeMethods.Foo_bar_SW3D();
    }

    public static int baz() {
      return NativeMethods.Foo_baz_OWT8();
    }

    protected override void ReleaseNative() {
      NativeMethods.Destroy_Foo(NativeHandle);
    }
  }
}
