#nullable enable

using System;
using System.Runtime.InteropServices;

namespace Interrogate {
    /// <summary>
    /// Controls how a <see cref="NativeObject"/> manages the lifetime of its underlying C++ pointer.
    /// </summary>
    public enum NativeOwnership {
        /// <summary>
        /// The C++ side owns the object. The C# wrapper will not free it on disposal.
        /// Use for pointers returned by getters, accessors, or any method that does not
        /// transfer ownership. The caller must ensure the native object outlives this wrapper.
        /// </summary>
        Borrowed,

        /// <summary>
        /// The C# side owns the object and will call the C++ destructor on disposal.
        /// Use for pointers returned by constructors or factory methods that transfer ownership.
        /// </summary>
        Owned,

        /// <summary>
        /// The object participates in the C++ project's reference-counting system.
        /// The wrapper calls <c>ref()</c> on construction and <c>unref_delete()</c> on disposal,
        /// integrating C# garbage collection with C++ reference counting.
        /// </summary>
        RefCounted
    }

    /// <summary>
    /// Implemented by all generated binding classes to enable <see cref="NativeObject.CastTo{T}"/>
    /// without reflection. This uses C# static abstract interface members for NativeAOT compatibility.
    /// </summary>
    /// <typeparam name="TSelf">The concrete binding class type (CRTP pattern).</typeparam>
    public interface INativeObject {
        IntPtr NativeHandle { get; }
    }

    public interface INativeType<TSelf> where TSelf : NativeObject {
        /// <summary>
        /// Creates a managed wrapper around an existing native pointer.
        /// </summary>
        /// <param name="ptr">The native C++ pointer. Must not be <see cref="IntPtr.Zero"/>.</param>
        /// <param name="ownership">How the wrapper should manage the pointer's lifetime.</param>
        /// <returns>A new wrapper instance, or <c>null</c> if <paramref name="ptr"/> is zero.</returns>
        static abstract TSelf? CreateFromNative(IntPtr ptr, NativeOwnership ownership);

        /// <summary>
        /// The registered runtime type handle for <typeparamref name="TSelf"/>, when its C++ type
        /// participates in a runtime type system (i.e. derives from a type exposing <c>is_of_type</c>).
        /// <c>0</c> by default (not runtime-checkable); the generator overrides it for type-system
        /// classes so <see cref="NativeObjectExtensions.CastTo{T}"/> can verify the dynamic type.
        /// </summary>
        static virtual int TypeHandle => 0;
    }

    /// <summary>
    /// Implemented by wrappers whose underlying C++ type participates in a runtime type system
    /// (interrogate/dtool's <c>TypedObject</c>). The generator adds this to the type that declares
    /// <c>is_of_type</c>, so every derived wrapper inherits it. It lets
    /// <see cref="NativeObjectExtensions.CastTo{T}"/> perform a checked downcast without reflection and
    /// without a library-specific dependency.
    /// </summary>
    public interface IRuntimeTyped {
        /// <summary>
        /// Whether the underlying C++ object is dynamically an instance of the given registered type
        /// handle (see <see cref="INativeType{TSelf}.TypeHandle"/>). Maps to C++ <c>is_of_type</c>.
        /// <para>
        /// <b>Precondition:</b> only valid when <see cref="GetTypeIndex"/> is non-zero. The C++
        /// <c>is_of_type</c> looks the object's own type up in the type registry; if that type is
        /// unregistered (index 0 / <c>TypeHandle::none()</c>) the lookup dereferences a null registry
        /// node and crashes. Callers (e.g. <see cref="NativeObjectExtensions.CastTo{T}"/>) must gate
        /// this call on <see cref="GetTypeIndex"/> being non-zero.
        /// </para>
        /// </summary>
        bool IsOfType(int typeHandle);

        /// <summary>
        /// The registered type-registry index of the object's own dynamic type, or <c>0</c> if its
        /// type is unregistered (<c>TypeHandle::none()</c>). Maps to C++ <c>get_type_index</c> — a
        /// plain field read that never touches the registry, so it is always safe to call. Used to
        /// decide whether <see cref="IsOfType"/> can be called safely for a checked downcast.
        /// </summary>
        int GetTypeIndex();
    }

    /// <summary>
    /// Extension helpers for interface-typed native wrappers.
    /// </summary>
    public static class NativeObjectExtensions {
        /// <summary>
        /// Safely casts an interface-typed wrapper to a concrete generated binding class.
        /// Creates a borrowed wrapper of type <typeparamref name="T"/> pointing to the
        /// same native object.
        /// </summary>
        /// <typeparam name="T">The target generated binding class type.</typeparam>
        /// <param name="obj">The interface-typed wrapper to cast, or <c>null</c>.</param>
        /// <returns>
        /// A borrowed wrapper of type <typeparamref name="T"/>, or <c>null</c> if <paramref name="obj"/>
        /// is null, wraps a null pointer, or is not dynamically a <typeparamref name="T"/>.
        /// </returns>
        public static T? CastTo<T>(this INativeObject? obj) where T : NativeObject, INativeType<T> {
            if (obj == null || obj.NativeHandle == IntPtr.Zero) {
                return null;
            }

            int typeHandle = T.TypeHandle;
            if (typeHandle != 0 && obj is IRuntimeTyped typed && typed.GetTypeIndex() != 0
                    && !typed.IsOfType(typeHandle)) {
                return null;
            }

            return T.CreateFromNative(obj.NativeHandle, NativeOwnership.Borrowed);
        }
    }

    /// <summary>
    /// Base class for all C# wrappers around C++ objects generated by interrogate.
    /// Provides pointer-based identity, safe casting between types in the inheritance
    /// hierarchy, and deterministic disposal integrated with C++ reference counting.
    /// <para>
    /// <b>Ownership model:</b> Each wrapper holds a native pointer with an associated
    /// <see cref="NativeOwnership"/> policy. <see cref="NativeOwnership.Borrowed"/> wrappers
    /// are lightweight views — safe to create and discard, but the caller must ensure the
    /// underlying C++ object outlives them. <see cref="NativeOwnership.RefCounted"/> wrappers
    /// participate in the native project's reference counting and are safe to hold indefinitely.
    /// </para>
    /// <para>
    /// <b>Casting:</b> Use <see cref="CastTo{T}"/> to safely downcast between
    /// wrapper types.  For C++ multiple inheritance, secondary base interfaces
    /// are implemented directly on the derived class — no casting needed.
    /// </para>
    /// <para>
    /// <b>Equality:</b> Two wrappers are equal if they point to the same native address,
    /// regardless of wrapper type or ownership. This matches C++ pointer identity semantics.
    /// </para>
    /// </summary>
    public abstract class NativeObject : INativeObject, IDisposable {
        private HandleRef _handle;

        /// <summary>
        /// How this wrapper manages the native pointer's lifetime.
        /// </summary>
        public NativeOwnership Ownership;

        private bool _disposed;

        // Conservative estimate of the minimum unmanaged allocation backing a
        // native object.  Even a small value is enough to inform the GC that
        // non-borrowed wrappers represent real unmanaged memory so it schedules
        // collection/finalization more aggressively in allocation-heavy loops.
        private const long NativeMemoryPressureHint = 512;

        /// <summary>
        /// Initializes a new wrapper around a native C++ pointer.
        /// </summary>
        /// <param name="ptr">The native pointer. May be <see cref="IntPtr.Zero"/> for null objects.</param>
        /// <param name="ownership">The lifetime policy for this pointer.</param>
        protected NativeObject(IntPtr ptr, NativeOwnership ownership) {
            _handle = new HandleRef(this, ptr);
            Ownership = ownership;
            if (ownership != NativeOwnership.Borrowed && ptr != IntPtr.Zero) {
                GC.AddMemoryPressure(NativeMemoryPressureHint);
            }
        }

        /// <summary>
        /// The raw native pointer. Exposed for advanced interop scenarios and P/Invoke calls.
        /// Avoid storing this value — use the wrapper object instead to maintain GC safety.
        /// </summary>
        public IntPtr NativeHandle => _handle.Handle;

        /// <summary>
        /// Extracts the native pointer from a wrapper, returning <see cref="IntPtr.Zero"/> for null.
        /// Used internally by generated bindings for parameter marshalling.
        /// </summary>
        /// <param name="obj">The wrapper to unwrap, or <c>null</c>.</param>
        /// <returns>The native pointer, or <see cref="IntPtr.Zero"/> if <paramref name="obj"/> is null.</returns>
        public static IntPtr Unwrap(INativeObject? obj) {
            return obj?.NativeHandle ?? IntPtr.Zero;
        }


        /// <summary>
        /// Safely casts this wrapper to a derived or sibling type in the C++ inheritance hierarchy.
        /// Creates a new <see cref="NativeOwnership.Borrowed"/> wrapper of type <typeparamref name="T"/>
        /// pointing to the same native object.
        /// <para>
        /// When <typeparamref name="T"/>'s C++ type participates in a runtime type system (dtool's
        /// <c>TypedObject</c>) and this object does too, the cast is <b>checked</b>: it verifies the
        /// object's dynamic type and returns <c>null</c> on a mismatch (like C# <c>as</c> or a pointer
        /// <c>dynamic_cast</c>). For types without a runtime type system it is an unchecked reinterpret.
        /// </para>
        /// </summary>
        /// <typeparam name="T">
        /// The target type. Must be a generated binding class implementing <see cref="INativeType{T}"/>.
        /// </typeparam>
        /// <returns>
        /// A borrowed wrapper of type <typeparamref name="T"/>, or <c>null</c> if this wrapper holds a
        /// null pointer or is not dynamically a <typeparamref name="T"/>.
        /// </returns>
        /// <example>
        /// <code>
        /// IBaseType obj = GetSomething();
        /// DerivedType derived = obj.CastTo&lt;DerivedType&gt;()
        ///     ?? throw new InvalidOperationException("Not the expected type");
        /// </code>
        /// </example>
        public T? CastTo<T>() where T : NativeObject, INativeType<T> {
            if (_handle.Handle == IntPtr.Zero) return null;
            int typeHandle = T.TypeHandle;
            if (typeHandle != 0 && this is IRuntimeTyped typed && typed.GetTypeIndex() != 0
                    && !typed.IsOfType(typeHandle)) {
                return null;
            }
            return T.CreateFromNative(_handle.Handle, NativeOwnership.Borrowed);
        }

        /// <summary>
        /// Determines whether two wrappers point to the same native object.
        /// </summary>
        public override bool Equals(object? obj) {
            return obj is NativeObject n && _handle.Handle == n._handle.Handle;
        }

        /// <summary>
        /// Returns a hash code based on the native pointer address.
        /// </summary>
        public override int GetHashCode() {
            return _handle.Handle.GetHashCode();
        }

        /// <summary>
        /// Determines whether two wrappers point to the same native object.
        /// Returns <c>true</c> if both are null.
        /// </summary>
        public static bool operator ==(NativeObject? a, NativeObject? b) {
            if (ReferenceEquals(a, b)) return true;
            if (a is null || b is null) return false;
            return a._handle.Handle == b._handle.Handle;
        }

        /// <summary>
        /// Determines whether two wrappers point to different native objects.
        /// </summary>
        public static bool operator !=(NativeObject? a, NativeObject? b) {
            return !(a == b);
        }

        /// <summary>
        /// Releases the native resource according to the ownership policy.
        /// Implemented by each generated class to call the correct C++ destructor or
        /// reference-count decrement.
        /// </summary>
        protected abstract void ReleaseNative();

        /// <summary>
        /// Releases native resources. Acts on both <see cref="NativeOwnership.Owned"/> and
        /// <see cref="NativeOwnership.RefCounted"/> wrappers. <see cref="NativeOwnership.Borrowed"/>
        /// wrappers are no-ops since they don't own the pointer.
        /// </summary>
        /// <param name="disposing">
        /// <c>true</c> if called from <see cref="Dispose()"/>; <c>false</c> if called from the finalizer.
        /// Both paths release non-borrowed native pointers:
        ///   • <see cref="NativeOwnership.Owned"/>: calls <c>delete ptr</c> for non-refcounted types.
        ///   • <see cref="NativeOwnership.RefCounted"/>: calls <c>unref_delete(ptr)</c>, which is
        ///     balanced because the C++ wrapper called <c>ref()</c> when returning the pointer to C#.
        /// </param>
        protected virtual void Dispose(bool disposing) {
            if (!_disposed) {
                if (Ownership != NativeOwnership.Borrowed && _handle.Handle != IntPtr.Zero) {
                    ReleaseNative();
                    GC.RemoveMemoryPressure(NativeMemoryPressureHint);
                }
                _handle = new HandleRef(null, IntPtr.Zero);
                _disposed = true;
            }
        }

        ~NativeObject() {
            Dispose(false);
        }

        /// <summary>
        /// Releases native resources held by this wrapper.
        /// For <see cref="NativeOwnership.Owned"/> objects, calls the C++ destructor.
        /// For <see cref="NativeOwnership.RefCounted"/> objects, decrements the reference count
        /// (and deletes if it reaches zero).
        /// For <see cref="NativeOwnership.Borrowed"/> objects, this is a no-op.
        /// </summary>
        public void Dispose() {
            Dispose(true);
            GC.SuppressFinalize(this);
        }
    }
}
