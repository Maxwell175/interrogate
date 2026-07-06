using System;

namespace Interrogate {
    public enum NativeOwnership { Borrowed, Owned, RefCounted }

    public abstract class NativeObject : IDisposable {
        private System.Runtime.InteropServices.HandleRef _handle;
        internal NativeOwnership Ownership;
        private bool _disposed;

        protected NativeObject(System.IntPtr ptr, NativeOwnership ownership) {
            _handle = new System.Runtime.InteropServices.HandleRef(this, ptr);
            Ownership = ownership;
        }

        internal System.IntPtr NativeHandle => _handle.Handle;

        internal static System.IntPtr Unwrap(NativeObject obj) {
            return obj != null ? obj._handle.Handle : System.IntPtr.Zero;
        }

        public override bool Equals(object obj) {
            return obj is NativeObject n && _handle.Handle == n._handle.Handle;
        }
        public override int GetHashCode() {
            return _handle.Handle.GetHashCode();
        }
        public static bool operator ==(NativeObject a, NativeObject b) {
            if (ReferenceEquals(a, b)) return true;
            if (a is null || b is null) return false;
            return a._handle.Handle == b._handle.Handle;
        }
        public static bool operator !=(NativeObject a, NativeObject b) {
            return !(a == b);
        }

        protected abstract void ReleaseNative();

        protected virtual void Dispose(bool disposing) {
            if (!_disposed) {
                if (disposing && Ownership != NativeOwnership.Borrowed && _handle.Handle != System.IntPtr.Zero) {
                    ReleaseNative();
                }
                _handle = new System.Runtime.InteropServices.HandleRef(null, System.IntPtr.Zero);
                _disposed = true;
            }
        }

        ~NativeObject() { Dispose(false); }
        public void Dispose() { Dispose(true); System.GC.SuppressFinalize(this); }
    }
}
