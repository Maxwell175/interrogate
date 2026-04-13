#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Secondary.Base.Methods {
  public interface IPrimaryBase : INativeObject {
    int get_id();
    int GetId();
  }

  public partial class PrimaryBase : NativeObject, IPrimaryBase, INativeType<PrimaryBase>, IDisposable {
    public static PrimaryBase? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new PrimaryBase(ptr, own);
    }

    static PrimaryBase? INativeType<PrimaryBase>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal PrimaryBase(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public PrimaryBase() : this(NativeMethods.PrimaryBase_PrimaryBase_B2pm(), NativeOwnership.Owned) {
    }

    public PrimaryBase(IPrimaryBase param0) : this(NativeMethods.PrimaryBase_PrimaryBase_RQha(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }


    public int get_id() {
      return NativeMethods.PrimaryBase_get_id_kOzt(NativeHandle);
    }
    /// <inheritdoc cref="get_id"/>
    public int GetId() => this.get_id();

    protected override void ReleaseNative() {
      NativeMethods.Destroy_PrimaryBase(NativeHandle);
    }
  }
}
