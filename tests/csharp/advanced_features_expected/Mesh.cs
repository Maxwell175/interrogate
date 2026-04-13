#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Advanced.Features {
  public interface IMesh : INativeObject, IRenderable {
  }

  public partial class Mesh : Renderable, IMesh, INativeType<Mesh> {
    public static new Mesh? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new Mesh(ptr, own);
    }

    static Mesh? INativeType<Mesh>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal Mesh(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public Mesh(int num_verts) : this(NativeMethods.Mesh_Mesh_YolD(num_verts), NativeOwnership.Owned) {
    }


    protected override void ReleaseNative() {
      NativeMethods.Destroy_Mesh(NativeHandle);
    }
  }
}
