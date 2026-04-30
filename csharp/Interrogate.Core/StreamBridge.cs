#nullable enable

using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Interrogate {
    /// <summary>
    /// Bridges a managed <see cref="System.IO.Stream"/> to a C++ std::istream,
    /// std::ostream, or std::iostream.  Generated bindings instantiate one of
    /// these for the duration of a native call that takes a stream parameter:
    /// <code>
    /// using var __bridge = StreamBridge.ForInput(stream);
    /// NativeMethods.SomeMethod(__bridge.Handle);
    /// </code>
    /// Disposal tears down the native stream and frees the GC handle that
    /// roots the managed stream for the duration of the call.
    /// </summary>
    /// <remarks>
    /// The bridge is AOT-safe: all callback trampolines are static methods
    /// marked with <see cref="UnmanagedCallersOnlyAttribute"/>, and managed
    /// identity is recovered via a <see cref="GCHandle"/> cookie rather than
    /// <c>Marshal.GetFunctionPointerForDelegate</c>.
    /// </remarks>
    public sealed partial class StreamBridge : IDisposable {
        private enum Direction { Input, Output, InputOutput }

        private GCHandle _cookie;
        private IntPtr _handle;
        private readonly Direction _direction;

        private StreamBridge(Stream? stream, Direction direction) {
            _direction = direction;
            if (stream == null) {
                _handle = IntPtr.Zero;
                return;
            }

            _cookie = GCHandle.Alloc(stream, GCHandleType.Normal);
            IntPtr cookiePtr = GCHandle.ToIntPtr(_cookie);

            unsafe {
                switch (direction) {
                    case Direction.Input:
                        _handle = NativeMethods.CreateIstream(
                            &ReadCallback, &SeekCallback, cookiePtr);
                        break;
                    case Direction.Output:
                        _handle = NativeMethods.CreateOstream(
                            &WriteCallback, &SeekCallback, cookiePtr);
                        break;
                    case Direction.InputOutput:
                        _handle = NativeMethods.CreateIostream(
                            &ReadCallback, &WriteCallback, &SeekCallback, cookiePtr);
                        break;
                }
            }

            if (_handle == IntPtr.Zero) {
                _cookie.Free();
                throw new InvalidOperationException(
                    "igStreamBridge_Create returned null");
            }
        }

        /// <summary>
        /// The native <c>std::istream*</c> / <c>std::ostream*</c> /
        /// <c>std::iostream*</c> pointer to pass to a C++ wrapper.
        /// <see cref="IntPtr.Zero"/> if this bridge wraps a null stream.
        /// </summary>
        public IntPtr Handle => _handle;

        /// <summary>
        /// Creates a bridge for C++ code that reads from the stream
        /// (<c>std::istream</c>).
        /// </summary>
        public static StreamBridge ForInput(Stream? stream) =>
            new(stream, Direction.Input);

        /// <summary>
        /// Creates a bridge for C++ code that writes to the stream
        /// (<c>std::ostream</c>).
        /// </summary>
        public static StreamBridge ForOutput(Stream? stream) =>
            new(stream, Direction.Output);

        /// <summary>
        /// Creates a bridge for C++ code that reads from and writes to the
        /// stream (<c>std::iostream</c>).
        /// </summary>
        public static StreamBridge ForInputOutput(Stream? stream) =>
            new(stream, Direction.InputOutput);

        /// <summary>
        /// Tears down the native stream and releases the <see cref="GCHandle"/>
        /// rooting the managed stream.  Does not close or dispose the wrapped
        /// <see cref="Stream"/>; ownership remains with the caller.
        /// </summary>
        public void Dispose() {
            if (_handle != IntPtr.Zero) {
                switch (_direction) {
                    case Direction.Input:       NativeMethods.DestroyIstream(_handle); break;
                    case Direction.Output:      NativeMethods.DestroyOstream(_handle); break;
                    case Direction.InputOutput: NativeMethods.DestroyIostream(_handle); break;
                }
                _handle = IntPtr.Zero;
            }
            if (_cookie.IsAllocated) {
                _cookie.Free();
            }
        }

        [UnmanagedCallersOnly]
        private static long ReadCallback(IntPtr cookie, IntPtr buf, long len) {
            try {
                var stream = GCHandle.FromIntPtr(cookie).Target as Stream;
                if (stream == null || buf == IntPtr.Zero || len < 0) return -1;
                unsafe {
                    int capped = (int)Math.Min(len, int.MaxValue);
                    return stream.Read(new Span<byte>((byte*)buf, capped));
                }
            } catch {
                return -1;
            }
        }

        [UnmanagedCallersOnly]
        private static long WriteCallback(IntPtr cookie, IntPtr buf, long len) {
            try {
                var stream = GCHandle.FromIntPtr(cookie).Target as Stream;
                if (stream == null || buf == IntPtr.Zero || len < 0) return -1;
                unsafe {
                    int capped = (int)Math.Min(len, int.MaxValue);
                    stream.Write(new ReadOnlySpan<byte>((byte*)buf, capped));
                }
                return len;
            } catch {
                return -1;
            }
        }

        [UnmanagedCallersOnly]
        private static long SeekCallback(IntPtr cookie, long offset, int origin) {
            try {
                var stream = GCHandle.FromIntPtr(cookie).Target as Stream;
                if (stream == null || !stream.CanSeek) return -1;
                SeekOrigin seekOrigin = origin switch {
                    0 => SeekOrigin.Begin,
                    1 => SeekOrigin.Current,
                    2 => SeekOrigin.End,
                    _ => SeekOrigin.Begin,
                };
                return stream.Seek(offset, seekOrigin);
            } catch {
                return -1;
            }
        }

        private static unsafe partial class NativeMethods {
            private const string LibName = "interrogate_support";

            [LibraryImport(LibName, EntryPoint = "igStreamBridge_CreateIstream")]
            internal static partial IntPtr CreateIstream(
                delegate* unmanaged<IntPtr, IntPtr, long, long> read,
                delegate* unmanaged<IntPtr, long, int, long> seek,
                IntPtr cookie);

            [LibraryImport(LibName, EntryPoint = "igStreamBridge_CreateOstream")]
            internal static partial IntPtr CreateOstream(
                delegate* unmanaged<IntPtr, IntPtr, long, long> write,
                delegate* unmanaged<IntPtr, long, int, long> seek,
                IntPtr cookie);

            [LibraryImport(LibName, EntryPoint = "igStreamBridge_CreateIostream")]
            internal static partial IntPtr CreateIostream(
                delegate* unmanaged<IntPtr, IntPtr, long, long> read,
                delegate* unmanaged<IntPtr, IntPtr, long, long> write,
                delegate* unmanaged<IntPtr, long, int, long> seek,
                IntPtr cookie);

            [LibraryImport(LibName, EntryPoint = "igStreamBridge_DestroyIstream")]
            internal static partial void DestroyIstream(IntPtr stream);

            [LibraryImport(LibName, EntryPoint = "igStreamBridge_DestroyOstream")]
            internal static partial void DestroyOstream(IntPtr stream);

            [LibraryImport(LibName, EntryPoint = "igStreamBridge_DestroyIostream")]
            internal static partial void DestroyIostream(IntPtr stream);
        }
    }
}
