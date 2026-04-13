#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;

namespace Interrogate {
    /// <summary>
    /// Base class for read-only native-backed collections that implement
    /// <see cref="IReadOnlyList{T}"/>.  Generated collection wrappers inherit
    /// from this class so that users get standard .NET collection semantics
    /// over native (C++) memory.
    /// </summary>
    /// <typeparam name="T">Managed element type.</typeparam>
    public abstract class NativeReadOnlyList<T> : NativeObject, IReadOnlyList<T> {
        protected NativeReadOnlyList(IntPtr ptr, NativeOwnership ownership)
            : base(ptr, ownership) {
        }

        public abstract int Count { get; }

        protected abstract T GetItem(int index);

        public T this[int index] {
            get {
                if ((uint)index >= (uint)Count)
                    throw new ArgumentOutOfRangeException(nameof(index));
                return GetItem(index);
            }
        }

        public IEnumerator<T> GetEnumerator() {
            int count = Count;
            for (int i = 0; i < count; ++i) {
                yield return GetItem(i);
            }
        }

        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
    }

    /// <summary>
    /// Base class for mutable native-backed collections that implement
    /// <see cref="IList{T}"/>.  Generated collection wrappers inherit from
    /// this class, giving users full read-write list semantics over native memory.
    /// For primitive element types, generated subclasses also expose zero-copy
    /// <c>AsSpan()</c> / <c>AsReadOnlySpan()</c> accessors for high-performance
    /// bulk access.
    /// </summary>
    /// <typeparam name="T">Managed element type.</typeparam>
    public abstract class NativeList<T> : NativeReadOnlyList<T>, IList<T> {
        protected NativeList(IntPtr ptr, NativeOwnership ownership)
            : base(ptr, ownership) {
        }

        protected abstract void SetItem(int index, T value);

        public new T this[int index] {
            get {
                if ((uint)index >= (uint)Count)
                    throw new ArgumentOutOfRangeException(nameof(index));
                return GetItem(index);
            }
            set {
                if ((uint)index >= (uint)Count)
                    throw new ArgumentOutOfRangeException(nameof(index));
                SetItem(index, value);
            }
        }

        public abstract bool IsReadOnly { get; }

        public abstract void Add(T item);

        public abstract void Clear();

        public virtual bool Contains(T item) => IndexOf(item) >= 0;

        public virtual void CopyTo(T[] array, int arrayIndex) {
            ArgumentNullException.ThrowIfNull(array);
            int count = Count;
            if (arrayIndex < 0 || arrayIndex + count > array.Length)
                throw new ArgumentOutOfRangeException(nameof(arrayIndex));
            for (int i = 0; i < count; ++i) {
                array[arrayIndex + i] = GetItem(i);
            }
        }

        public virtual int IndexOf(T item) {
            var comparer = EqualityComparer<T>.Default;
            int count = Count;
            for (int i = 0; i < count; ++i) {
                if (comparer.Equals(GetItem(i), item))
                    return i;
            }
            return -1;
        }

        public abstract void Insert(int index, T item);

        public virtual bool Remove(T item) {
            int index = IndexOf(item);
            if (index < 0) return false;
            RemoveAt(index);
            return true;
        }

        public abstract void RemoveAt(int index);
    }
}
