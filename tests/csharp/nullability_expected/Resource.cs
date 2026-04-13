#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Nullability {
  public interface IResource : INativeObject {
    string get_name();
    string GetName();
  }

  public partial class Resource : NativeObject, IResource, INativeType<Resource>, IDisposable {
    public static Resource? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new Resource(ptr, own);
    }

    static Resource? INativeType<Resource>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal Resource(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public Resource(IResource param0) : this(NativeMethods.Resource_Resource_mTZc(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }

    public Resource(string name) : this(NativeMethods.Resource_Resource_387D(name), NativeOwnership.Owned) {
    }


    public string get_name() {
      return NativeMethods.Resource_get_name_Ua4g(NativeHandle) ?? throw new InvalidOperationException("Native method returned null.");
    }
    /// <inheritdoc cref="get_name"/>
    public string GetName() => this.get_name();

    protected override void ReleaseNative() {
      NativeMethods.Destroy_Resource(NativeHandle);
    }
  }
}
