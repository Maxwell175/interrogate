#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Advanced.Features {
  public interface IIntArray : INativeObject {
    int get_num_elements();
    int GetNumElements();
    int get_element(int n);
    int GetElement(int n);
    void add_element(int val);
    void AddElement(int val);
  }

  public partial class IntArray : NativeObject, IIntArray, INativeType<IntArray>, IReadOnlyList<int>, IDisposable {
    public static IntArray? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new IntArray(ptr, own);
    }

    static IntArray? INativeType<IntArray>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal IntArray(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public IntArray() : this(NativeMethods.IntArray_IntArray_bJR5(), NativeOwnership.Owned) {
    }

    public IntArray(IIntArray param0) : this(NativeMethods.IntArray_IntArray_BIPk(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }


    public int get_num_elements() {
      return NativeMethods.IntArray_get_num_elements_zXDN(NativeHandle);
    }
    /// <inheritdoc cref="get_num_elements"/>
    public int GetNumElements() => this.get_num_elements();

    public int get_element(int n) {
      return NativeMethods.IntArray_get_element_RD3X(NativeHandle, n);
    }
    /// <inheritdoc cref="get_element"/>
    public int GetElement(int n) => this.get_element(n);

    public void add_element(int val) {
      NativeMethods.IntArray_add_element_ZAs0(NativeHandle, val);
    }
    /// <inheritdoc cref="add_element"/>
    public void AddElement(int val) => this.add_element(val);

    int IReadOnlyCollection<int>.Count {
      get {
        return (int)NativeMethods.IntArray_get_num_elements_zXDN(NativeHandle);
      }
    }

    int IReadOnlyList<int>.this[int index] {
      get {
        return NativeMethods.IntArray_get_element_RD3X(NativeHandle, index);
      }
    }

    IEnumerator<int> IEnumerable<int>.GetEnumerator() {
      int count = ((IReadOnlyCollection<int>)this).Count;
      for (int i = 0; i < count; i++) {
        yield return ((IReadOnlyList<int>)this)[i];
      }
    }

    IEnumerator IEnumerable.GetEnumerator() {
      return ((IEnumerable<int>)this).GetEnumerator();
    }

    protected override void ReleaseNative() {
      NativeMethods.Destroy_IntArray(NativeHandle);
    }
  }
}
