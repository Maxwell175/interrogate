#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Advanced.Features {
  public interface IPrintable : INativeObject {
    string to_string();
  }

  public partial class Printable : NativeObject, IPrintable, INativeType<Printable>, IDisposable {
    public static Printable? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new Printable(ptr, own);
    }

    static Printable? INativeType<Printable>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal Printable(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public Printable() : this(NativeMethods.Printable_Printable_H2S6(), NativeOwnership.Owned) {
    }

    public Printable(IPrintable param0) : this(NativeMethods.Printable_Printable_jkyB(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }


    public virtual string to_string() {
      return NativeMethods.Printable_to_string_md1D(NativeHandle) ?? throw new InvalidOperationException("Native method returned null.");
    }

    protected override void ReleaseNative() {
      NativeMethods.Destroy_Printable(NativeHandle);
    }
  }
}
