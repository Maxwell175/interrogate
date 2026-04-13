#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Advanced.Features {
  public interface ICircle : INativeObject, IShape {
    float get_radius();
    float GetRadius();
    void set_radius(float r);
    void SetRadius(float r);
    float Radius { get; set; }
  }

  public partial class Circle : Shape, ICircle, INativeType<Circle> {
    public static new Circle? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new Circle(ptr, own);
    }

    static Circle? INativeType<Circle>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal Circle(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public Circle(ICircle param0) : this(NativeMethods.Circle_Circle_hOSs(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }

    public Circle(float radius) : this(NativeMethods.Circle_Circle_X2F7(radius), NativeOwnership.Owned) {
    }


    public float get_radius() {
      return NativeMethods.Circle_get_radius_AiYO(NativeHandle);
    }
    /// <inheritdoc cref="get_radius"/>
    public float GetRadius() => this.get_radius();

    public void set_radius(float r) {
      NativeMethods.Circle_set_radius_Kk_F(NativeHandle, r);
    }
    /// <inheritdoc cref="set_radius"/>
    public void SetRadius(float r) => this.set_radius(r);

    public float Radius {
      get {
        return NativeMethods.Circle_get_radius_AiYO(NativeHandle);
      }
      set {
        NativeMethods.Circle_set_radius_Kk_F(NativeHandle, value);
      }
    }

    protected override void ReleaseNative() {
      NativeMethods.Destroy_Circle(NativeHandle);
    }
  }
}
