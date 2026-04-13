#nullable enable

using System;
using System.Runtime.InteropServices;
using Interrogate;

namespace Nullability {
  internal static partial class NativeMethods {
    [LibraryImport("nullability", EntryPoint = "_inC6SGAmTZc")]
    internal static partial IntPtr Resource_Resource_mTZc(IntPtr param0);

    [LibraryImport("nullability", EntryPoint = "_inC6SGA387D", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr Resource_Resource_387D(string name);

    [LibraryImport("nullability", EntryPoint = "_inC6SGAUa4g", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial string Resource_get_name_Ua4g(IntPtr _this);

    [LibraryImport("nullability", EntryPoint = "_inC6SGA_FY2")]
    internal static partial IntPtr ResourceManager_ResourceManager_FY2();

    [LibraryImport("nullability", EntryPoint = "_inC6SGAd9bU")]
    internal static partial IntPtr ResourceManager_ResourceManager_d9bU(IntPtr param0);

    [LibraryImport("nullability", EntryPoint = "_inC6SGAxFnx", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr ResourceManager_find_resource_xFnx(IntPtr _this, string name);

    [LibraryImport("nullability", EntryPoint = "_inC6SGAftP7")]
    internal static partial IntPtr ResourceManager_get_default_resource_ftP7(IntPtr _this);

    [LibraryImport("nullability", EntryPoint = "_inC6SGAiIY2")]
    internal static partial void ResourceManager_set_active_iIY2(IntPtr _this, IntPtr resource);

    [LibraryImport("nullability", EntryPoint = "_inC6SGAkCDc", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void ResourceManager_set_alias_kCDc(IntPtr _this, string? alias);

    [LibraryImport("nullability", EntryPoint = "_inC6SGAud6B", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial string? ResourceManager_find_label_ud6B(IntPtr _this, string name);

    [LibraryImport("nullability", EntryPoint = "_inC6SGAs5CY", StringMarshalling = StringMarshalling.Utf8)]
    internal static partial string ResourceManager_default_label_s5CY(IntPtr _this);

    [LibraryImport("nullability", EntryPoint = "_inCSDestr_Resource")]
    internal static partial void Destroy_Resource(IntPtr self);

    [LibraryImport("nullability", EntryPoint = "_inCSDestr_ResourceManager")]
    internal static partial void Destroy_ResourceManager(IntPtr self);

  }
}
