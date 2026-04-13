#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Basic.Class {
  public interface IDog : INativeObject, IAnimal {
    void fetch();
    void Fetch();
    bool is_good_boy();
    bool IsGoodBoy();
  }

  public partial class Dog : Animal, IDog, INativeType<Dog> {
    public static new Dog? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new Dog(ptr, own);
    }

    static Dog? INativeType<Dog>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal Dog(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public Dog(IDog param0) : this(NativeMethods.Dog_Dog_uQX4(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }

    public Dog(string name) : this(NativeMethods.Dog_Dog_tYoF(name), NativeOwnership.Owned) {
    }


    public void fetch() {
      NativeMethods.Dog_fetch_7NSx(NativeHandle);
    }
    /// <inheritdoc cref="fetch"/>
    public void Fetch() => this.fetch();

    public bool is_good_boy() {
      return NativeMethods.Dog_is_good_boy_0ej4(NativeHandle);
    }
    /// <inheritdoc cref="is_good_boy"/>
    public bool IsGoodBoy() => this.is_good_boy();

    protected override void ReleaseNative() {
      NativeMethods.Destroy_Dog(NativeHandle);
    }
  }
}
