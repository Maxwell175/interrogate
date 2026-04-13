#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Interrogate;

namespace Nullability {
  public interface IResourceManager : INativeObject {
    IResource? find_resource(string name);
    IResource? FindResource(string name);
    IResource get_default_resource();
    IResource GetDefaultResource();
    void set_active(IResource? resource);
    void SetActive(IResource? resource);
    void set_alias(string? alias);
    void SetAlias(string? alias);
    string? find_label(string name);
    string? FindLabel(string name);
    string default_label();
    string DefaultLabel();
  }

  public partial class ResourceManager : NativeObject, IResourceManager, INativeType<ResourceManager>, IDisposable {
    public static ResourceManager? __CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return ptr == IntPtr.Zero ? null : new ResourceManager(ptr, own);
    }

    static ResourceManager? INativeType<ResourceManager>.CreateFromNative(IntPtr ptr, NativeOwnership own) {
      return __CreateFromNative(ptr, own);
    }

    internal ResourceManager(IntPtr ptr, NativeOwnership ownership) : base(ptr, ownership) {
    }

    public ResourceManager() : this(NativeMethods.ResourceManager_ResourceManager_FY2(), NativeOwnership.Owned) {
    }

    public ResourceManager(IResourceManager param0) : this(NativeMethods.ResourceManager_ResourceManager_d9bU(NativeObject.Unwrap(param0)), NativeOwnership.Owned) {
    }


    public IResource? find_resource(string name) {
      IntPtr result = NativeMethods.ResourceManager_find_resource_xFnx(NativeHandle, name);
      return Resource.__CreateFromNative(result, NativeOwnership.Borrowed);
    }
    /// <inheritdoc cref="find_resource"/>
    public IResource? FindResource(string name) => this.find_resource(name);

    public IResource get_default_resource() {
      IntPtr result = NativeMethods.ResourceManager_get_default_resource_ftP7(NativeHandle);
      return Resource.__CreateFromNative(result, NativeOwnership.Borrowed) ?? throw new InvalidOperationException("Native method returned null.");
    }
    /// <inheritdoc cref="get_default_resource"/>
    public IResource GetDefaultResource() => this.get_default_resource();

    public void set_active(IResource? resource) {
      NativeMethods.ResourceManager_set_active_iIY2(NativeHandle, NativeObject.Unwrap(resource));
    }
    /// <inheritdoc cref="set_active"/>
    public void SetActive(IResource? resource) => this.set_active(resource);

    public void set_alias(string? alias) {
      NativeMethods.ResourceManager_set_alias_kCDc(NativeHandle, alias);
    }
    /// <inheritdoc cref="set_alias"/>
    public void SetAlias(string? alias) => this.set_alias(alias);

    public string? find_label(string name) {
      return NativeMethods.ResourceManager_find_label_ud6B(NativeHandle, name);
    }
    /// <inheritdoc cref="find_label"/>
    public string? FindLabel(string name) => this.find_label(name);

    public string default_label() {
      return NativeMethods.ResourceManager_default_label_s5CY(NativeHandle) ?? throw new InvalidOperationException("Native method returned null.");
    }
    /// <inheritdoc cref="default_label"/>
    public string DefaultLabel() => this.default_label();

    protected override void ReleaseNative() {
      NativeMethods.Destroy_ResourceManager(NativeHandle);
    }
  }
}
