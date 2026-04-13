#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Secondary.Base.Methods {
  public interface ITextLike : INativeObject {
    void set_text(string text);
    void SetText(string text);
    string get_text();
    string GetText();
  }

  public partial class TextLike : NativeObject, ITextLike, INativeType<TextLike>, IDisposable {
    public static TextLike? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new TextLike(ptr, own);
    }

    static TextLike? INativeType<TextLike>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal TextLike(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public TextLike() : this(NativeMethods.TextLike_TextLike_ADHG(), NativeOwnership.Owned) {
    }

    public TextLike(ITextLike param0) : this(NativeMethods.TextLike_TextLike_xOf(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }


    public void set_text(string text) {
      NativeMethods.TextLike_set_text_mJyb(NativeHandle, text);
    }
    /// <inheritdoc cref="set_text"/>
    public void SetText(string text) => this.set_text(text);

    public string get_text() {
      return NativeMethods.TextLike_get_text_yH6B(NativeHandle) ?? throw new InvalidOperationException("Native method returned null.");
    }
    /// <inheritdoc cref="get_text"/>
    public string GetText() => this.get_text();

    protected override void ReleaseNative() {
      NativeMethods.Destroy_TextLike(NativeHandle);
    }
  }
}
