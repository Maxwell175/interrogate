#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Basic.Class {
  public interface IAnimal : INativeObject {
    string get_name();
    string GetName();
    void set_name(string name);
    void SetName(string name);
    int get_age();
    int GetAge();
    void set_age(int age);
    void SetAge(int age);
    void speak();
    void Speak();
  }

  public partial class Animal : NativeObject, IAnimal, INativeType<Animal>, IDisposable {
    public static Animal? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new Animal(ptr, own);
    }

    static Animal? INativeType<Animal>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal Animal(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public Animal(IAnimal param0) : this(NativeMethods.Animal_Animal_ryQg(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }

    public Animal(string name) : this(NativeMethods.Animal_Animal_q6nS(name), NativeOwnership.Owned) {
    }


    public string get_name() {
      return NativeMethods.Animal_get_name_ZzYm(NativeHandle) ?? throw new InvalidOperationException("Native method returned null.");
    }
    /// <inheritdoc cref="get_name"/>
    public string GetName() => this.get_name();

    public void set_name(string name) {
      NativeMethods.Animal_set_name_pTJQ(NativeHandle, name);
    }
    /// <inheritdoc cref="set_name"/>
    public void SetName(string name) => this.set_name(name);

    public int get_age() {
      return NativeMethods.Animal_get_age_Kokn(NativeHandle);
    }
    /// <inheritdoc cref="get_age"/>
    public int GetAge() => this.get_age();

    public void set_age(int age) {
      NativeMethods.Animal_set_age_MRQt(NativeHandle, age);
    }
    /// <inheritdoc cref="set_age"/>
    public void SetAge(int age) => this.set_age(age);

    public void speak() {
      NativeMethods.Animal_speak_qkLW(NativeHandle);
    }
    /// <inheritdoc cref="speak"/>
    public void Speak() => this.speak();

    protected override void ReleaseNative() {
      NativeMethods.Destroy_Animal(NativeHandle);
    }
  }
}
