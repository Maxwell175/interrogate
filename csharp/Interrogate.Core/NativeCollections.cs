#nullable enable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;

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

    /// <summary>
    /// A live view over a native sequence that is exposed as a length function plus
    /// an element accessor -- interrogate's MAKE_SEQ_PROPERTY
    /// (<c>MAKE_SEQ_PROPERTY(axes, get_num_axes, get_axis)</c>).
    /// </summary>
    /// <remarks>
    /// Unlike <see cref="NativeReadOnlyList{T}"/>, there is no native container object
    /// to wrap: the pair of accessors *is* the sequence.  So this holds the accessors
    /// and reads through to the owner on every call -- it tracks the owner rather than
    /// snapshotting it, which is what the C++ (and Python) semantics are.  A sequence
    /// with no setter throws on assignment rather than pretending to be mutable.
    /// </remarks>
    /// <typeparam name="T">Managed element type.</typeparam>
    public sealed class NativeSeq<T> : IReadOnlyList<T> {
        private readonly Func<int> _count;
        private readonly Func<int, T> _get;
        private readonly Action<int, T>? _set;

        public NativeSeq(Func<int> count, Func<int, T> get, Action<int, T>? set = null) {
            _count = count ?? throw new ArgumentNullException(nameof(count));
            _get = get ?? throw new ArgumentNullException(nameof(get));
            _set = set;
        }

        public int Count => _count();

        /// <summary>True when the underlying property exposed no setter.</summary>
        public bool IsReadOnly => _set is null;

        public T this[int index] {
            get {
                if ((uint)index >= (uint)_count()) {
                    throw new ArgumentOutOfRangeException(nameof(index));
                }
                return _get(index);
            }
            set {
                if (_set is null) {
                    throw new NotSupportedException("This native sequence is read-only.");
                }
                if ((uint)index >= (uint)_count()) {
                    throw new ArgumentOutOfRangeException(nameof(index));
                }
                _set(index, value);
            }
        }

        public T[] ToArray() {
            int n = _count();
            var result = new T[n];
            for (int i = 0; i < n; ++i) {
                result[i] = _get(i);
            }
            return result;
        }

        public IEnumerator<T> GetEnumerator() {
            int n = _count();
            for (int i = 0; i < n; ++i) {
                yield return _get(i);
            }
        }

        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
    }

    /// <summary>
    /// A keyed view over a native property exposed as a has/get accessor pair --
    /// interrogate's MAKE_MAP_PROPERTY (<c>MAKE_MAP_PROPERTY(attribs, has_attrib,
    /// get_attrib)</c>).
    /// </summary>
    /// <remarks>
    /// Lookup only, deliberately.  A C++ map property does not have to be
    /// enumerable: RenderState::attribs declares only has_attrib and get_attrib, and
    /// there is no way to ask it for its keys.  Only when the class also declares a
    /// MAKE_MAP_KEYS_SEQ do the keys become reachable, and then the generator emits a
    /// <see cref="NativeMap{TKey, TValue}"/> instead -- which is a real
    /// IReadOnlyDictionary.  Reporting a Count that cannot be computed, or an empty
    /// key list, would be worse than not offering them.
    /// </remarks>
    public class NativeLookup<TKey, TValue> where TKey : notnull {
        private protected readonly Func<TKey, bool> _has;
        private protected readonly Func<TKey, TValue> _get;
        private protected readonly Action<TKey, TValue>? _set;
        private protected readonly Action<TKey>? _remove;

        public NativeLookup(Func<TKey, bool> has, Func<TKey, TValue> get,
                            Action<TKey, TValue>? set = null,
                            Action<TKey>? remove = null) {
            _has = has ?? throw new ArgumentNullException(nameof(has));
            _get = get ?? throw new ArgumentNullException(nameof(get));
            _set = set;
            _remove = remove;
        }

        /// <summary>True when the underlying property exposed no setter.</summary>
        public bool IsReadOnly => _set is null;

        public bool ContainsKey(TKey key) {
            if (key is null) throw new ArgumentNullException(nameof(key));
            return _has(key);
        }

        public bool TryGetValue(TKey key, out TValue value) {
            if (key is null) throw new ArgumentNullException(nameof(key));
            if (!_has(key)) {
                value = default!;
                return false;
            }
            value = _get(key);
            return true;
        }

        public TValue this[TKey key] {
            get {
                if (key is null) throw new ArgumentNullException(nameof(key));
                if (!_has(key)) {
                    throw new KeyNotFoundException($"No such key: {key}");
                }
                return _get(key);
            }
            set {
                if (key is null) throw new ArgumentNullException(nameof(key));
                if (_set is null) {
                    throw new NotSupportedException("This native map is read-only.");
                }
                _set(key, value);
            }
        }

        /// <summary>Removes a key. Throws if the property declared no clear function.</summary>
        public bool Remove(TKey key) {
            if (key is null) throw new ArgumentNullException(nameof(key));
            if (_remove is null) {
                throw new NotSupportedException("This native map cannot remove keys.");
            }
            if (!_has(key)) {
                return false;
            }
            _remove(key);
            return true;
        }
    }

    /// <summary>
    /// A live, editable dictionary over a native map property whose keys are also
    /// reachable, because the C++ class declared a MAKE_MAP_KEYS_SEQ alongside it
    /// (<c>MAKE_MAP_PROPERTY(tags, has_tag, get_tag, set_tag, clear_tag)</c> +
    /// <c>MAKE_MAP_KEYS_SEQ(tags, get_num_tags, get_tag_key)</c>).
    /// </summary>
    /// <remarks>
    /// <para>
    /// It is a real <see cref="IDictionary{TKey, TValue}"/>, not a read-only view:
    /// PandaNode's tags have a setter and a per-key deleter, so
    /// <c>node.Tags["team"] = "red"</c> and <c>node.Tags.Remove("team")</c> do exactly
    /// what set_tag and clear_tag do.  A property that declared no setter reports
    /// <see cref="IsReadOnly"/> and throws from the mutators -- which is how .NET has
    /// always modelled a read-only collection, and is why ICollection has IsReadOnly
    /// at all.
    /// </para>
    /// <para>
    /// Everything reads through to the owner on every call: Keys, Values and
    /// enumeration track the object rather than snapshotting it.
    /// </para>
    /// </remarks>
    public sealed class NativeMap<TKey, TValue> :
        NativeLookup<TKey, TValue>,
        IDictionary<TKey, TValue>,
        IReadOnlyDictionary<TKey, TValue> where TKey : notnull {

        private readonly Func<int> _count;
        private readonly Func<int, TKey> _key;
        private readonly Action? _clear;

        public NativeMap(Func<TKey, bool> has, Func<TKey, TValue> get,
                         Func<int> count, Func<int, TKey> key,
                         Action<TKey, TValue>? set = null,
                         Action<TKey>? remove = null,
                         Action? clear = null)
            : base(has, get, set, remove) {
            _count = count ?? throw new ArgumentNullException(nameof(count));
            _key = key ?? throw new ArgumentNullException(nameof(key));
            _clear = clear;
        }

        public int Count => _count();

        /// <summary>Live view of the keys; reads through on every enumeration.</summary>
        public ICollection<TKey> Keys => new KeyView(this);

        /// <summary>Live view of the values; reads through on every enumeration.</summary>
        public ICollection<TValue> Values => new ValueView(this);

        IEnumerable<TKey> IReadOnlyDictionary<TKey, TValue>.Keys => Keys;
        IEnumerable<TValue> IReadOnlyDictionary<TKey, TValue>.Values => Values;

        private IEnumerable<TKey> EnumerateKeys() {
            int n = _count();
            for (int i = 0; i < n; ++i) {
                yield return _key(i);
            }
        }

        public void Add(TKey key, TValue value) {
            if (key is null) throw new ArgumentNullException(nameof(key));
            if (_set is null) {
                throw new NotSupportedException("This native map is read-only.");
            }
            if (_has(key)) {
                throw new ArgumentException($"An entry with the key '{key}' already exists.", nameof(key));
            }
            _set(key, value);
        }

        public void Clear() {
            if (_clear is not null) {
                _clear();
                return;
            }
            if (_remove is null) {
                throw new NotSupportedException("This native map cannot remove keys.");
            }
            // No clear-all in C++; take a snapshot of the keys, since removing while
            // enumerating would read through into a shifting collection.
            foreach (TKey key in EnumerateKeys().ToArray()) {
                _remove(key);
            }
        }

        public IEnumerator<KeyValuePair<TKey, TValue>> GetEnumerator() {
            foreach (TKey key in EnumerateKeys()) {
                yield return new KeyValuePair<TKey, TValue>(key, _get(key));
            }
        }

        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

        void ICollection<KeyValuePair<TKey, TValue>>.Add(KeyValuePair<TKey, TValue> item)
            => Add(item.Key, item.Value);

        bool ICollection<KeyValuePair<TKey, TValue>>.Contains(KeyValuePair<TKey, TValue> item)
            => TryGetValue(item.Key, out TValue? found) &&
               EqualityComparer<TValue>.Default.Equals(found, item.Value);

        bool ICollection<KeyValuePair<TKey, TValue>>.Remove(KeyValuePair<TKey, TValue> item)
            => ((ICollection<KeyValuePair<TKey, TValue>>)this).Contains(item) && Remove(item.Key);

        void ICollection<KeyValuePair<TKey, TValue>>.CopyTo(KeyValuePair<TKey, TValue>[] array, int arrayIndex) {
            ArgumentNullException.ThrowIfNull(array);
            if (arrayIndex < 0 || arrayIndex + Count > array.Length) {
                throw new ArgumentOutOfRangeException(nameof(arrayIndex));
            }
            foreach (KeyValuePair<TKey, TValue> pair in this) {
                array[arrayIndex++] = pair;
            }
        }

        private sealed class KeyView : ICollection<TKey> {
            private readonly NativeMap<TKey, TValue> _map;
            internal KeyView(NativeMap<TKey, TValue> map) { _map = map; }

            public int Count => _map.Count;
            public bool IsReadOnly => true;
            public bool Contains(TKey item) => _map.ContainsKey(item);
            public IEnumerator<TKey> GetEnumerator() => _map.EnumerateKeys().GetEnumerator();
            IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

            public void CopyTo(TKey[] array, int arrayIndex) {
                ArgumentNullException.ThrowIfNull(array);
                foreach (TKey key in this) {
                    array[arrayIndex++] = key;
                }
            }

            public void Add(TKey item) => throw new NotSupportedException();
            public void Clear() => throw new NotSupportedException();
            public bool Remove(TKey item) => throw new NotSupportedException();
        }

        private sealed class ValueView : ICollection<TValue> {
            private readonly NativeMap<TKey, TValue> _map;
            internal ValueView(NativeMap<TKey, TValue> map) { _map = map; }

            public int Count => _map.Count;
            public bool IsReadOnly => true;
            public bool Contains(TValue item) {
                var comparer = EqualityComparer<TValue>.Default;
                foreach (TValue value in this) {
                    if (comparer.Equals(value, item)) return true;
                }
                return false;
            }
            public IEnumerator<TValue> GetEnumerator() {
                foreach (TKey key in _map.EnumerateKeys()) {
                    yield return _map._get(key);
                }
            }
            IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

            public void CopyTo(TValue[] array, int arrayIndex) {
                ArgumentNullException.ThrowIfNull(array);
                foreach (TValue value in this) {
                    array[arrayIndex++] = value;
                }
            }

            public void Add(TValue item) => throw new NotSupportedException();
            public void Clear() => throw new NotSupportedException();
            public bool Remove(TValue item) => throw new NotSupportedException();
        }
    }
}
