#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Advanced.Features {
  public interface IDocument : INativeObject, ISerializable, IPrintable {
    string get_title();
    string GetTitle();
    string Title { get; }
  }

  public partial class Document : Serializable, IDocument, INativeType<Document>, IPrintable {
    public static new Document? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new Document(ptr, own);
    }

    static Document? INativeType<Document>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal Document(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public Document(IDocument param0) : this(NativeMethods.Document_Document_VzBg(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }

    public Document(string title) : this(NativeMethods.Document_Document_pA2S(title), NativeOwnership.Owned) {
    }


    public string get_title() {
      return NativeMethods.Document_get_title_K0CU(NativeHandle) ?? throw new InvalidOperationException("Native method returned null.");
    }
    /// <inheritdoc cref="get_title"/>
    public string GetTitle() => this.get_title();

    public override string serialize() {
      return NativeMethods.Document_serialize_obch(NativeHandle) ?? throw new InvalidOperationException("Native method returned null.");
    }

    public virtual string to_string() {
      return NativeMethods.Document_to_string_q8Qd(NativeHandle) ?? throw new InvalidOperationException("Native method returned null.");
    }

    public string Title {
      get {
        IntPtr result = NativeMethods.Document_get_title_K0CU(NativeHandle);
        return result == IntPtr.Zero ? throw new InvalidOperationException("Native getter returned null.") : Marshal.PtrToStringUTF8(result)!;
      }
    }

    protected override void ReleaseNative() {
      NativeMethods.Destroy_Document(NativeHandle);
    }
  }
}
