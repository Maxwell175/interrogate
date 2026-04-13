#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Secondary.Base.Methods {
  public interface IDerivedTextNode : INativeObject, IPrimaryBase, ITextLike {
  }

  public partial class DerivedTextNode : PrimaryBase, IDerivedTextNode, INativeType<DerivedTextNode>, ITextLike {
    public static new DerivedTextNode? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new DerivedTextNode(ptr, own);
    }

    static DerivedTextNode? INativeType<DerivedTextNode>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal DerivedTextNode(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public DerivedTextNode() : this(NativeMethods.DerivedTextNode_DerivedTextNode_Az0(), NativeOwnership.Owned) {
    }

    public DerivedTextNode(IDerivedTextNode param0) : this(NativeMethods.DerivedTextNode_DerivedTextNode_YlfM(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }


    public void set_text(string text) {
      NativeMethods.TextLike_set_text_mJyb(NativeMethods.DerivedTextNode_upcast_to_TextLike_7f_o(NativeHandle), text);
    }
    /// <inheritdoc cref="set_text"/>
    public void SetText(string text) => this.set_text(text);

    public string get_text() {
      return NativeMethods.TextLike_get_text_yH6B(NativeMethods.DerivedTextNode_upcast_to_TextLike_7f_o(NativeHandle)) ?? throw new InvalidOperationException("Native method returned null.");
    }
    /// <inheritdoc cref="get_text"/>
    public string GetText() => this.get_text();

    protected override void ReleaseNative() {
      NativeMethods.Destroy_DerivedTextNode(NativeHandle);
    }
  }
}
