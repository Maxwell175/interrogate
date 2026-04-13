#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Advanced.Features {
  public interface IRenderable : INativeObject {
    void render();
    void Render();
    int vertex_count();
    int VertexCount();
  }

  public abstract partial class Renderable : NativeObject, IRenderable, INativeType<Renderable>, IDisposable {
    internal sealed class __Opaque_Renderable : Renderable {
      internal __Opaque_Renderable(IntPtr ptr, NativeOwnership own) : base(ptr, own) {
      }
      protected override void ReleaseNative() {
        NativeMethods.Destroy_Renderable(NativeHandle);
      }
    }

    public static Renderable? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new __Opaque_Renderable(ptr, own);
    }

    static Renderable? INativeType<Renderable>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal Renderable(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public virtual void render() {
      NativeMethods.Renderable_render_6TOC(NativeHandle);
    }
    /// <inheritdoc cref="render"/>
    public void Render() => this.render();

    public virtual int vertex_count() {
      return NativeMethods.Renderable_vertex_count_4xva(NativeHandle);
    }
    /// <inheritdoc cref="vertex_count"/>
    public int VertexCount() => this.vertex_count();

    protected override void ReleaseNative() {
      NativeMethods.Destroy_Renderable(NativeHandle);
    }
  }
}
