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

        /// <summary>
        /// Whether <typeparamref name="TSelf"/>'s C++ type participates in the project's
        /// reference-counting system. <c>false</c> by default; the generator overrides it to
        /// <c>true</c> for ref-counted classes so an owning <see cref="NativeObject.CastTo{T}(bool)"/>
        /// can bump the count and hand back a wrapper that is safe to keep past the source's lifetime.
        /// </summary>
        static virtual bool IsReferenceCounted => false;
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
        /// See <see cref="NativeObject.CastTo{T}(bool)"/> for the checked/fail-closed semantics
        /// and the meaning of <paramref name="own"/>.
        /// </summary>
        /// <typeparam name="T">The target generated binding class type.</typeparam>
        /// <param name="obj">The interface-typed wrapper to cast, or <c>null</c>.</param>
        /// <param name="own">
        /// When <c>true</c> and <typeparamref name="T"/> is reference-counted, the result owns a
        /// reference and is safe to keep past <paramref name="obj"/>'s lifetime.
        /// </param>
        /// <returns>
        /// A wrapper of type <typeparamref name="T"/>, or <c>null</c> if <paramref name="obj"/> is
        /// null, wraps a null pointer, or cannot be verified to be a <typeparamref name="T"/>.
        /// </returns>
        public static T? CastTo<T>(this INativeObject? obj, bool own = false)
                where T : NativeObject, INativeType<T> {
            return NativeObject.Cast<T>(obj, own);
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
        /// Optional hooks a binding runtime can install to bracket every native
        /// release in a runtime-specific scope. <see cref="BeforeRelease"/> runs
        /// immediately before <see cref="ReleaseNative"/> and
        /// <see cref="AfterRelease"/> immediately after — on the same thread, for
        /// every owned / ref-counted disposal, including those driven by the
        /// finalizer thread.
        /// <para>
        /// Interrogate.Core attaches no meaning to these; they exist so a library
        /// binding can satisfy an invariant of its own native runtime (for
        /// example, entering a thread-scoped memory-reclamation critical section
        /// so a destructor that mutates shared state is properly covered) without
        /// that concept leaking into the generator or this base class.
        /// </para>
        /// <para>
        /// Install once at startup, before any wrapper is disposed; they are read
        /// on every disposal and are not synchronized.
        /// </para>
        /// </summary>
        public static Action? BeforeRelease;

        /// <inheritdoc cref="BeforeRelease"/>
        public static Action? AfterRelease;

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
        /// Safely casts this wrapper to another type in the C++ inheritance hierarchy, returning a
        /// new wrapper of type <typeparamref name="T"/> pointing at the same native object.
        /// <para>
        /// <b>Checked / fail-closed.</b> When <typeparamref name="T"/>'s C++ type participates in a
        /// runtime type system (dtool's <c>TypedObject</c>), the cast verifies this object's dynamic
        /// type via <c>is_of_type</c> and returns <c>null</c> on a mismatch — and also <c>null</c> if
        /// the dynamic type cannot be queried, rather than reinterpreting blindly. When
        /// <typeparamref name="T"/> is <i>not</i> runtime-typed there is nothing to verify against, so
        /// the cast succeeds only if this wrapper's managed type already is a <typeparamref name="T"/>
        /// (an identity or upcast, whose pointer is known valid) and returns <c>null</c> otherwise.
        /// This never hands back a wrapper over a wrongly-offset or unverified pointer.
        /// </para>
        /// <para>
        /// <b>Reaching a C++ secondary base</b> (e.g. a <c>ReferenceCount</c> mixin) is done through
        /// the directly-implemented interface (<c>obj as IReferenceCount</c>), which already carries
        /// the correct pointer offset — not through this method.
        /// </para>
        /// <para>
        /// <b>Ownership.</b> By default the result is a lightweight <see cref="NativeOwnership.Borrowed"/>
        /// view: the caller must keep the source alive for as long as the result is used. Pass
        /// <paramref name="own"/> <c>true</c> to get a result that owns its own reference when
        /// <typeparamref name="T"/> is reference-counted, making it safe to keep after the source is
        /// disposed or collected. (For non-reference-counted types <paramref name="own"/> has no
        /// effect — there is no count to hold — and the borrowed-lifetime rule still applies.)
        /// </para>
        /// </summary>
        /// <typeparam name="T">
        /// The target type. Must be a generated binding class implementing <see cref="INativeType{T}"/>.
        /// </typeparam>
        /// <param name="own">
        /// Request an owning result for reference-counted <typeparamref name="T"/> (see remarks).
        /// </param>
        /// <returns>
        /// A wrapper of type <typeparamref name="T"/>, or <c>null</c> if this wrapper holds a null
        /// pointer or cannot be verified to be a <typeparamref name="T"/>.
        /// </returns>
        /// <example>
        /// <code>
        /// IBaseType obj = GetSomething();
        /// DerivedType derived = obj.CastTo&lt;DerivedType&gt;()
        ///     ?? throw new InvalidOperationException("Not the expected type");
        /// </code>
        /// </example>
        public T? CastTo<T>(bool own = false) where T : NativeObject, INativeType<T> {
            return Cast<T>(this, own);
        }

        /// <summary>
        /// Shared implementation behind the instance and extension <c>CastTo</c> overloads.
        /// See <see cref="CastTo{T}(bool)"/> for the semantics.
        /// </summary>
        internal static T? Cast<T>(INativeObject? source, bool own)
                where T : NativeObject, INativeType<T> {
            if (source == null) return null;
            IntPtr handle = source.NativeHandle;
            if (handle == IntPtr.Zero) return null;

            if (T.TypeHandle != 0) {
                // T is runtime-typed: only proceed on a positive dynamic-type check.  If the
                // source cannot be queried (not runtime-typed, or its own type is unregistered)
                // we cannot verify the cast, so fail closed instead of reinterpreting.
                if (source is not IRuntimeTyped typed || typed.GetTypeIndex() == 0
                        || !typed.IsOfType(T.TypeHandle)) {
                    return null;
                }
                // Verified: the object is dynamically a T.  Its TypedObject subobject is at
                // offset 0, and `handle` is the object's primary pointer, so it is a valid T*.
            } else if (source is not T) {
                // Nothing to verify against and this is not an identity/upcast whose pointer we
                // already know is valid, so we cannot produce a correct T* — fail closed.
                return null;
            }

            bool wantOwn = own && T.IsReferenceCounted;
            T? result = T.CreateFromNative(handle,
                wantOwn ? NativeOwnership.RefCounted : NativeOwnership.Borrowed);
            if (wantOwn) {
                result?.AddNativeRef();
            }
            return result;
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
        /// Increments the native reference count of this object. No-op by default; the generator
        /// overrides it for reference-counted classes so an owning <see cref="CastTo{T}(bool)"/> can
        /// take a reference it will release via <see cref="ReleaseNative"/> on disposal. Only called
        /// on wrappers whose type reports <see cref="INativeType{T}.IsReferenceCounted"/>.
        /// </summary>
        protected virtual void AddNativeRef() { }

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
                    BeforeRelease?.Invoke();
                    try {
                        ReleaseNative();
                    } finally {
                        AfterRelease?.Invoke();
                    }
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
