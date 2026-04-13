#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Advanced.Features {
  public interface ISerializable : INativeObject {
    string serialize();
    string Serialize();
  }

  public partial class Serializable : NativeObject, ISerializable, INativeType<Serializable>, IDisposable {
    public static Serializable? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new Serializable(ptr, own);
    }

    static Serializable? INativeType<Serializable>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal Serializable(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public Serializable() : this(NativeMethods.Serializable_Serializable_TSCe(), NativeOwnership.Owned) {
    }

    public Serializable(ISerializable param0) : this(NativeMethods.Serializable_Serializable_1dfC(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }


    public virtual string serialize() {
      return NativeMethods.Serializable_serialize_ME_f(NativeHandle) ?? throw new InvalidOperationException("Native method returned null.");
    }
    /// <inheritdoc cref="serialize"/>
    public string Serialize() => this.serialize();

    protected override void ReleaseNative() {
      NativeMethods.Destroy_Serializable(NativeHandle);
    }
  }
}
