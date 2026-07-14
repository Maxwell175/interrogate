#nullable enable

using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Interrogate {
    /// <summary>
    /// Which C++ stream a <see cref="NativeStream"/> wraps.
    /// </summary>
    public enum NativeStreamKind {
        Input,
        Output,
        InputOutput,
    }

    /// <summary>
    /// A <see cref="Stream"/> over a C++ stream that the *native* side owns --
    /// the counterpart of <see cref="StreamBridge"/>, which goes the other way.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Generated bindings return one of these wherever a C++ function hands back an
    /// <c>std::istream *</c> (<c>VirtualFile::open_read_file</c>,
    /// <c>Multifile::open_read_subfile</c>, ...).  Before it existed, such a return
    /// was mapped to an opaque <see cref="IntPtr"/> and the bound <c>istream</c>
    /// exposed only <c>get()</c> -- one P/Invoke per byte -- so the managed side
    /// could open a file and then do nothing useful with it.
    /// </para>
    /// <para>
    /// <b>It does not own the stream.</b>  Disposing it does not free anything:
    /// freeing stays with whoever produced it, which in C++ means passing it back to
    /// the matching <c>close_*</c> method.  That is the C++ contract, and pretending
    /// otherwise would double-free.  <see cref="StreamBridge"/> recognises a
    /// NativeStream and passes its handle straight back through, so
    /// <c>CloseReadFile(OpenReadFile(...))</c> now closes the stream it was given
    /// rather than a freshly bridged one.
    /// </para>
    /// </remarks>
    public sealed partial class NativeStream : Stream {
        private readonly IntPtr _handle;
        private readonly NativeStreamKind _kind;
        private readonly bool _canSeek;

        private NativeStream(IntPtr handle, NativeStreamKind kind) {
            _handle = handle;
            _kind = kind;
            _canSeek = ProbeSeekable(handle, kind != NativeStreamKind.Output ? 1 : 0);
        }

        /// <summary>
        /// Not every C++ stream is seekable: a compressed multifile subfile is read
        /// through a decompressing stream, which refuses to seek to the end.  Probe
        /// once, here, rather than claim seekability and throw from Length -- which
        /// matters because Stream.CopyTo asks CanSeek and then Length to size its
        /// buffer.  The stream is fresh at this point, so the probe restores position.
        /// </summary>
        private static bool ProbeSeekable(IntPtr handle, int isInput) {
            long start = NativeMethods.Tell(handle, isInput);
            if (start < 0) {
                return false;
            }
            if (NativeMethods.Seek(handle, 0, 2, isInput) < 0) {
                return false;
            }
            return NativeMethods.Seek(handle, start, 0, isInput) >= 0;
        }

        /// <summary>
        /// Wraps a native stream pointer.  Returns null for <see cref="IntPtr.Zero"/>,
        /// which is how the C++ open_* methods report failure.
        /// </summary>
        public static NativeStream? Wrap(IntPtr handle, NativeStreamKind kind) =>
            handle == IntPtr.Zero ? null : new NativeStream(handle, kind);

        /// <summary>The underlying C++ stream pointer.</summary>
        public IntPtr Handle => _handle;

        /// <summary>Which C++ stream type this wraps.</summary>
        public NativeStreamKind Kind => _kind;

        public override bool CanRead =>
            _kind is NativeStreamKind.Input or NativeStreamKind.InputOutput;

        public override bool CanWrite =>
            _kind is NativeStreamKind.Output or NativeStreamKind.InputOutput;

        public override bool CanSeek => _canSeek;

        private bool IsInput => _kind != NativeStreamKind.Output;

        public override long Length {
            get {
                long saved = NativeMethods.Tell(_handle, IsInput ? 1 : 0);
                long end = NativeMethods.Seek(_handle, 0, 2, IsInput ? 1 : 0);
                if (end < 0 || saved < 0) {
                    throw new NotSupportedException(
                        "This native stream is not seekable, so its length is not known " +
                        "up front (a compressed multifile subfile reads through a " +
                        "decompressing stream).  Copy it out instead.");
                }
                NativeMethods.Seek(_handle, saved, 0, IsInput ? 1 : 0);
                return end;
            }
        }

        public override long Position {
            get {
                long position = NativeMethods.Tell(_handle, IsInput ? 1 : 0);
                if (position < 0) {
                    throw new IOException("The native stream reported no position.");
                }
                return position;
            }
            set => Seek(value, SeekOrigin.Begin);
        }

        public override int Read(byte[] buffer, int offset, int count) {
            ValidateBufferArguments(buffer, offset, count);
            return Read(buffer.AsSpan(offset, count));
        }

        public override unsafe int Read(Span<byte> buffer) {
            if (!CanRead) {
                throw new NotSupportedException("The native stream is not readable.");
            }
            if (buffer.IsEmpty) {
                return 0;
            }
            fixed (byte *destination = buffer) {
                long read = NativeMethods.Read(_handle, (IntPtr)destination, buffer.Length);
                if (read < 0) {
                    throw new IOException("Reading from the native stream failed.");
                }
                return (int)read;
            }
        }

        public override void Write(byte[] buffer, int offset, int count) {
            ValidateBufferArguments(buffer, offset, count);
            Write(buffer.AsSpan(offset, count));
        }

        public override unsafe void Write(ReadOnlySpan<byte> buffer) {
            if (!CanWrite) {
                throw new NotSupportedException("The native stream is not writable.");
            }
            if (buffer.IsEmpty) {
                return;
            }
            fixed (byte *source = buffer) {
                long written = NativeMethods.Write(_handle, (IntPtr)source, buffer.Length);
                if (written < 0) {
                    throw new IOException("Writing to the native stream failed.");
                }
            }
        }

        public override long Seek(long offset, SeekOrigin origin) {
            int nativeOrigin = origin switch {
                SeekOrigin.Begin => 0,
                SeekOrigin.Current => 1,
                SeekOrigin.End => 2,
                _ => throw new ArgumentOutOfRangeException(nameof(origin)),
            };
            long position = NativeMethods.Seek(_handle, offset, nativeOrigin, IsInput ? 1 : 0);
            if (position < 0) {
                throw new IOException("Seeking the native stream failed.");
            }
            return position;
        }

        public override void Flush() {
            if (CanWrite) {
                NativeMethods.Flush(_handle);
            }
        }

        public override void SetLength(long value) =>
            throw new NotSupportedException("A native stream cannot be resized.");

        /// <summary>
        /// Does not free the native stream -- see the class remarks.  Hand it back to
        /// the C++ close_* method that matches the open_* that produced it.
        /// </summary>
        protected override void Dispose(bool disposing) {
            base.Dispose(disposing);
        }

        private static partial class NativeMethods {
            private const string LibName = "interrogate_support";

            [LibraryImport(LibName, EntryPoint = "igStream_Read")]
            internal static partial long Read(IntPtr stream, IntPtr buffer, long length);

            [LibraryImport(LibName, EntryPoint = "igStream_Write")]
            internal static partial long Write(IntPtr stream, IntPtr buffer, long length);

            [LibraryImport(LibName, EntryPoint = "igStream_Seek")]
            internal static partial long Seek(IntPtr stream, long offset, int origin, int isInput);

            [LibraryImport(LibName, EntryPoint = "igStream_Tell")]
            internal static partial long Tell(IntPtr stream, int isInput);

            [LibraryImport(LibName, EntryPoint = "igStream_Flush")]
            internal static partial void Flush(IntPtr stream);
        }
    }
}
