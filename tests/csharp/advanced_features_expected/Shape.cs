#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Advanced.Features {
  public interface IShape : INativeObject {
    float area();
    float Area();
    string type_name();
    string TypeName();
    int get_id();
    int GetId();
    int Id { get; }
  }

  public partial class Shape : NativeObject, IShape, INativeType<Shape>, IDisposable {
    public static Shape? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new Shape(ptr, own);
    }

    static Shape? INativeType<Shape>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal Shape(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public Shape() : this(NativeMethods.Shape_Shape_veB5(), NativeOwnership.Owned) {
    }

    public Shape(IShape param0) : this(NativeMethods.Shape_Shape_TL(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }


    public virtual float area() {
      return NativeMethods.Shape_area_HTb5(NativeHandle);
    }
    /// <inheritdoc cref="area"/>
    public float Area() => this.area();

    public virtual string type_name() {
      return NativeMethods.Shape_type_name_TvJn(NativeHandle) ?? throw new InvalidOperationException("Native method returned null.");
    }
    /// <inheritdoc cref="type_name"/>
    public string TypeName() => this.type_name();

    public int get_id() {
      return NativeMethods.Shape_get_id_Dam3(NativeHandle);
    }
    /// <inheritdoc cref="get_id"/>
    public int GetId() => this.get_id();

    public int Id {
      get {
        return NativeMethods.Shape_get_id_Dam3(NativeHandle);
      }
    }

    protected override void ReleaseNative() {
      NativeMethods.Destroy_Shape(NativeHandle);
    }
  }
}
