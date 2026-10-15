/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

/**
 * @file
 *
 * This file defines HashMap and related classes.
 */
package std.collection

struct HashMapEntry<K, V> {
    public let hash: Int64
    public let next: Int64
    public let key: K
    public let value: V

    @Frozen
    init(h: Int64, n: Int64, k: K, v: V) {
        hash = h
        next = n
        key = k
        value = v
    }
    @Frozen
    exclave init(this @local!, h: Int64, n: Int64, k: K @local!, v: V @local!) {
        hash = h
        next = n
        key = k
        value = v
    }

    @Frozen
    init() {
        hash = -1
        next = -1
        key = unsafe { zeroValue<K>() }
        value = unsafe { zeroValue<V>() }
    }
    @Frozen
    exclave init(this @local!) {
        hash = -1
        next = -1
        key = unsafe { zeroValue2<K>() }
        value = unsafe { zeroValue2<V>() }
    }
}
/**
 * This class is a collection of K types of hashmap and is suitable for stripping key-value pair types.
 *
 * @since 0.24.2
 */
class HashMapKeys<K, V> <: EquatableCollection<K> where K <: Hashable & Equatable<K> {
    private let map: HashMap<K, V>

    @Frozen
    init(m: HashMap<K, V>) {
        map = m
    }

    /**
     * Returns sizes of key-value.
     *
     * @return sizes of key-value.
     *
     * @since 0.24.2
     */
    @Frozen
    public prop size: Int64 {
        get() {
            return map.size
        }
    }

    /**
     * Check whether the size is empty. If yes, true is returned. Otherwise, false is returned.
     *
     * @return bool if yes, true is returned. Otherwise, false is returned.
     *
     * @since 0.24.2
     */
    @Frozen
    public func isEmpty(): Bool {
        return map.isEmpty()
    }

    /**
     * Checks whether the mapping relationship corresponding to the specified key exists in this mapping.
     *
     * @param element transfer the key to be judged.
     * @return bool returns true if exists; otherwise, false.
     *
     * @since 0.24.2
     */
    @Frozen
    public func contains(element: K): Bool {
        return map.contains(element)
    }

    /**
     * Checks whether the mapping relationship corresponding to the collection key exists in this mapping.
     *
     * @param elements transfer the collection key to be judged.
     * @return bool returns true if exists; otherwise, false.
     *
     * @since 0.24.2
     */
    @Frozen
    public func contains(all!: Collection<K>): Bool {
        return map.contains(all: all)
    }

    /**
     * Returns iterator of hashmap.
     *
     * @return iterator of hashmap.
     *
     * @since 0.24.2
     */
    @Frozen
    public func iterator(): Iterator<K> {
        return map.iterator().map<K> {i: (K, V) => i[0]}
    }

    /**
     * Returns the keys in this Map as an Array.
     */
    @Frozen
    public func toArray(): Array<K> {
        let keys = Array<K>(size, repeat: unsafe { zeroValue<K>() })
        var index = 0
        var pos = 0
        while (index < size) {
            if (map.entries[pos].hash >= 0) {
                keys[index] = map.entries[pos].key
                index++
            }
            pos++
        }
        return keys
    }
}

/**
 * This class is a collection of V types of hashmaps and is suitable for stripping key-value pair types.
 *
 * @since 0.24.2
 */
class HashMapValues<K, V> <: Collection<V> where K <: Hashable & Equatable<K> {
    private let map: HashMap<K, V>

    @Frozen
    init(m: HashMap<K, V>) {
        map = m
    }

    /**
     * Returns sizes of values.
     *
     * @return sizes of values.
     *
     * @since 0.24.2
     */
    @Frozen
    public prop size: Int64 {
        get() {
            return map.size
        }
    }

    /**
     * Check whether the size is empty. If yes, true is returned. Otherwise, false is returned.
     *
     * @return bool if yes, true is returned. Otherwise, false is returned.
     *
     * @since 0.24.2
     */
    @Frozen
    public func isEmpty(): Bool {
        return map.isEmpty()
    }

    /**
     * Returns value Iterator<T> - Iterator<T> type, which can be traversed using an iterator.
     *
     * @return the value corresponding to the return key is encapsulated with option.
     *
     * @since 0.24.2
     */
    @Frozen
    public func iterator(): Iterator<V> {
        return map.iterator().map<V> {i: (K, V) => i[1]}
    }

    /**
     * Returns the values in this Map as an Array.
     */
    @Frozen
    public func toArray(): Array<V> {
        let values = Array<V>(size, repeat: unsafe { zeroValue<V>() })
        var index = 0
        var pos = 0
        while (index < size) {
            if (map.entries[pos].hash >= 0) {
                values[index] = map.entries[pos].value
                index++
            }
            pos++
        }
        return values
    }
}

/**
 * Class HashMapEntryView is a reference view of a key in a hashmap.
 * Thread safety is not guaranteed.
 */
class HashMapEntryView<K, V> <: MapEntryView<K, V> where K <: Hashable & Equatable<K> {
    // Reference to a HashMap.
    private let map: HashMap<K, V>

    // The specified key.
    private let _key: K

    // Hash of a specified key.
    private let hash: Int64

    // Index of a specified key.
    private var index: Int64

    private var _isAbsent: Bool

    // The HashMap's version.
    private var lockVersion: Int64

    @Frozen
    init(isAbsent: Bool, key: K, hash: Int64, index: Int64, map: HashMap<K, V>) {
        this._isAbsent = isAbsent
        this._key = key
        this.hash = hash
        this.map = map
        this.index = index
        this.lockVersion = map.version()
    }

    @Frozen
    public prop key: K {
        get() {
            getKey()
        }
    }
    @Frozen
    public prop key: K @local! {
        get() {
            exclave { getKey() }
        }
    }
    @Frozen
    public prop key: K @local? {
        get() {
            exclave { getKey() }
        }
    }

    @Frozen
    public mut prop value: ?V {
        get() {
            getValue()
        }
        set(value) {
            return if (value.isNone()) {
                map.remove(_key)
                lockVersion = map.version()
                _isAbsent = true
            } else {
                setValue(value.getOrThrow())
            }
        }
    }
    @Frozen
    public mut prop value: ?V @local! {
        get() {
            exclave { getValue() }
        }
        set(_) {
            throw Exception()
        }
    }
    @Frozen
    public mut prop value: ?V @local? {
        get() {
            exclave { getValue() }
        }
        set(_) {
            throw Exception()
        }
    }
    /**
     * Get the key of HashMapEntryView with time complexity O(1).
     *
     * Returns key.
     *
     * @since 0.45.1
     */
    @Frozen
    func getKey(): K {
        if (lockVersion != map.version()) {
            throw ConcurrentModificationException()
        }
        return _key
    }
    @Frozen
    exclave func getKey(this @local!): K @local! {
        if (lockVersion != map.version()) {
            throw ConcurrentModificationException()
        }
        return _key
    }
    @Frozen
    func getKey(this @local?): K @local? {
        if (lockVersion != map.version()) {
            throw ConcurrentModificationException()
        }
        return _key
    }

    /**
     * Get the value of HashMapEntryView with time complexity O(1).
     *
     * Returns value.
     *
     * @since 0.45.1
     */
    @Frozen
    func getValue(): ?V {
        if (lockVersion != map.version()) {
            throw ConcurrentModificationException()
        }
        return if (_isAbsent) {
            None
        } else {
            map.entries[index].value
        }
    }
    @Frozen
    exclave func getValue(this @local!): ?V @local! {
        if (lockVersion != map.version()) {
            throw ConcurrentModificationException()
        }
        if (_isAbsent) {
            None
        } else {
            map.entries[index].value
        }
    }
    @Frozen
    exclave func getValue(this @local?): ?V @local? {
        if (lockVersion != map.version()) {
            throw ConcurrentModificationException()
        }
        if (_isAbsent) {
            None
        } else {
            map.entries[index].value
        }
    }

    /**
     * Update the value of HashMapEntryView with time complexity O(1).
     *
     * If the view is empty, the key-value pair is inserted, and the view is converted into a non-empty view and the value is returned.
     * Otherwise, the value is modified.
     *
     * @since 0.45.1
     */
    @Frozen
    private func setValue(v: V) {
        if (lockVersion != map.version()) {
            throw ConcurrentModificationException()
        }
        if (_isAbsent) {
            var bucketIndex: Int64 = map.getIndex(hash, map.buckets.size)
            if (map.freeSize > 0) {
                index = map.freeIndex
                map.freeIndex = map.entries[index].next
                map.freeSize--
            } else {
                if (map.resize(map.size + 1)) {
                    bucketIndex = map.getIndex(hash, map.buckets.size)
                }
                index = map.appendIndex
                map.appendIndex++
            }
            map.entries[index] = HashMapEntry<K, V>(hash, map.buckets[bucketIndex], key, v)
            map.modCount++
            map.buckets[bucketIndex] = index
            lockVersion = map.modCount
            _isAbsent = false
        } else {
            let temp = map.entries[index]
            map.entries[index] = HashMapEntry<K, V>(temp.hash, temp.next, temp.key, v)
        }
    }
}

/**
 * The hashmap is implemented based on the hash algorithm.
 * You can use put key-value to obtain the value and get key to obtain the value.
 *
 * @since 0.18.4
 */
public class HashMap<K, V> <: Map<K, V> where K <: Hashable & Equatable<K> {

    /* Max size of buckets. */
    private static const MAX_BUCKET_SIZE: Int64 = 4611686018427387904

    /*
     * Load factor of the HashMap.
     * Capacity expansion when the usage more than 75%
     */
    private static const LOAD_FACTOR: Float32 = 0.75

    /*
     * default Initialized Capacity.
     * Default array initialization size is 16
     */
    private static const DEFAULT_CAPACITY: Int64 = 16

    /* Maximum number of int64 types. */
    /* Subscripts that can be directly inserted*/
    var appendIndex: Int64 = 0

    /* Subscript of the currently available deleted entry.*/
    var freeIndex: Int64 = -1

    /* Total number of deleted entries that are available.*/
    var freeSize: Int64 = 0

    /* Modified hashmap version.*/
    var modCount: Int64 = 0b00000000

    /* Index Array.*/
    var buckets: Array<Int64>

    /* Element Array.*/
    var entries: Array<HashMapEntry<K, V>>

    /**
     * Initializes an empty HashMap with a default initial DEFAULT_CAPACITY (16) and a default load factor.
     *
     * @since 0.18.4
     */
    @Frozen
    public init() {
        buckets = Array<Int64>(DEFAULT_CAPACITY, repeat: -1)
        entries = Array<HashMapEntry<K, V>>(
            DEFAULT_CAPACITY,
            repeat: HashMapEntry<K, V>()
        )
    }
    @Frozen
    public exclave init(this @local!) {
        buckets = Array<Int64>(DEFAULT_CAPACITY, repeat: -1)
        entries = Array<HashMapEntry<K, V>>(
            DEFAULT_CAPACITY,
            repeat: HashMapEntry<K, V>() @local!
        )
    }

    /**
     * Initializes a HashMap with an incoming iterator for initialization.
     *
     * @param elements an incoming iterator is initialized.
     *
     * @since 0.18.4
     */
    @Frozen
    public init(elements: Collection<(K, V)>) {
        let bucketSize: Int64 = elements.size + (elements.size >> 1)
        buckets = Array<Int64>(tableSizeFor(bucketSize), repeat: -1)
        entries = Array<HashMapEntry<K, V>>(elements.size, repeat: HashMapEntry<K, V>())
        for (i in elements) {
            putWithoutResize(i[0], i[1])
        }
    }
    @Frozen
    public exclave init(this @local!, elements: Collection<(K, V)> @local!) {
        // Collection 的 size 无 local 形态（文档 §9 只列 isEmpty(@local?)/toArray），
        // 故不做 size 预分配：以默认容量起步，由 add 按需扩容（resize）。
        buckets = Array<Int64>(DEFAULT_CAPACITY, repeat: -1)
        entries = Array<HashMapEntry<K, V>>(DEFAULT_CAPACITY, repeat: HashMapEntry<K, V>() @local!)
        // 不用 `for ((k, v) in elements)`：这会经 Iterable.iterator(this @local!) 的接口
        // 默认体，对未自适配该重载的类型报 IllegalStateException（运行期才发现）。
        // 改走 toArray(this @local!)（实测可正常分派），对任意实现了它的 Collection 生效。
        let src: Array<(K, V)> @local! = elements.toArray()
        var i: Int64 = 0
        while (i < src.size) {
            let pair: (K, V) @local! = src[i]
            add(pair[0], pair[1])
            i++
        }
    }

    @Frozen
    public exclave init(this @local?, elements: Collection<(K, V)> @local?) {
        // 与 ArrayList init(this @local?, Collection)（array_list.cj:290）同手法：
        // @local? 上下文无元素写入手段——Array 的 `[]=(this @local?, ...)` 写入器位于
        // `extend<T> Array<T> where T <: Copyable`（array.cj:1714），K/V 无该约束故不可用
        // （注意 arraySet3 本身已无 Copyable 约束，限制在公开写入器与 std.collection
        // 不可达的内建之间；arraySetUnchecked3/rawArrayInitByValue3 仍带约束）。
        // 故仅对本包可识别的 HashMap 共享内部结构，其余显式拒绝，避免静默丢失元素。
        match (elements) {
            case map: HashMap<K, V> =>
                buckets = map.buckets
                entries = map.entries
                appendIndex = map.appendIndex
                freeIndex = map.freeIndex
                freeSize = map.freeSize
                modCount = map.modCount
            case _ =>
                throw IllegalArgumentException("Unsupported collection type for @local? HashMap init.")
        }
    }


    /**
     * Initializes a hashmap with an incoming list for initialization.
     *
     * @param elements an incoming list is initialized.
     *
     * @since 0.18.4
     */
    @Frozen
    public init(elements: Array<(K, V)>) {
        let bucketSize: Int64 = elements.size + (elements.size >> 1)
        buckets = Array<Int64>(tableSizeFor(bucketSize), repeat: -1)
        entries = Array<HashMapEntry<K, V>>(elements.size, repeat: HashMapEntry<K, V>())
        for (i in elements) {
            putWithoutResize(i[0], i[1])
        }
    }
    @Frozen
    public exclave init(this @local!, elements: Array<(K, V)> @local!) {
        let bucketSize: Int64 = elements.size + (elements.size >> 1)
        buckets = Array<Int64>(tableSizeFor(bucketSize), repeat: -1)
        entries = Array<HashMapEntry<K, V>>(elements.size, repeat: HashMapEntry<K, V>() @local!)
        for ((k, v) in elements) {
            add(k, v)
        }
    }

    @Frozen
    public exclave init(this @local?, elements: Array<(K, V)> @local?) {
        // @local? 源的元素为 (K, V) @local?，无法写入本地哈希表：Array 的
        // `[]=(this @local?, ...)` 写入器在 Copyable 扩展内（array.cj:1714），K/V 无该
        // 约束；`Collection<T> @local?` 的 iterator()/size 分派亦不受编译器支持。
        // 按既定回退规则显式拒绝，不静默丢弃元素；需要该构造时请改用
        // init(this @local!, elements:)。
        throw IllegalArgumentException("Unsupported element source for @local? HashMap init: use the @local! overload.")
    }

    /**
     * Initializes a HashMap with an incoming @p DEFAULT_CAPACITY for initialization.
     *
     * @param capacity an incoming capacity is initialized.
     *
     * @throws IllegalArgumentException if capacity is less than zero
     *
     * @since 0.18.4
     */
    @Frozen
    public init(capacity: Int64) {
        if (capacity < 0) {
            throw IllegalArgumentException("Invalid size of HashMap: ${capacity}.")
        }
        let bucketSize = capacity + (capacity >> 1)
        buckets = Array<Int64>(tableSizeFor(bucketSize), repeat: -1)
        entries = Array<HashMapEntry<K, V>>(capacity, repeat: HashMapEntry<K, V>())
    }
    @Frozen
    public exclave init(this @local!, capacity: Int64) {
        if (capacity < 0) {
            throw IllegalArgumentException("Invalid size of HashMap: ${capacity}.")
        }
        let bucketSize = capacity + (capacity >> 1)
        buckets = Array<Int64>(tableSizeFor(bucketSize), repeat: -1)
        entries = Array<HashMapEntry<K, V>>(capacity, repeat: HashMapEntry<K, V>() @local!)
    }

    /**
     * Initializes a hash map with an incoming size and an initial element for initialization.
     *
     * @param size the size of the incoming initial element.
     * @param initElement an incoming initElement is initialized.
     *
     * @throws IllegalArgumentException if size is less than zero
     *
     * @since 0.18.4
     */
    @Frozen
    public init(size: Int64, initElement: (Int64) -> (K, V)) {
        if (size < 0) {
            throw IllegalArgumentException("Invalid size of HashMap: ${size}.")
        } else {
            buckets = Array<Int64>(tableSizeFor(size), repeat: -1)
            entries = Array<HashMapEntry<K, V>>(size, repeat: HashMapEntry<K, V>())
            for (i in 0..size) {
                let element: (K, V) = initElement(i)
                add(element[0], element[1])
            }
        }
    }
    @Frozen
    public exclave init(this @local!, size: Int64, initElement: (Int64) -> (K, V) @local!) {
        if (size < 0) {
            throw IllegalArgumentException("Invalid size of HashMap: ${size}.")
        } else {
            buckets = Array<Int64>(tableSizeFor(size), repeat: -1)
            entries = Array<HashMapEntry<K, V>>(size, repeat: HashMapEntry<K, V>() @local!)
            for (i in 0..size) {
                let element: (K, V) @local! = initElement(i)
                add(element[0], element[1])
            }
        }
    }

    @Frozen
    public exclave init(this @local?, size: Int64, initElement: (Int64) -> (K, V) @local?) {
        // 闭包产出 (K, V) @local?，而本地哈希表的元素写入要求 @local!（Array 的
        // @local? 写入器在 Copyable 扩展内，array.cj:1714；K/V 无该约束）。
        // 按既定回退规则显式拒绝，不静默丢弃元素。
        throw IllegalArgumentException("Unsupported element source for @local? HashMap init: use the @local! overload.")
    }


    @Frozen
    private init(other: HashMap<K, V>) {
        buckets = other.buckets.clone()
        entries = other.entries.clone()
        appendIndex = other.appendIndex
        freeIndex = other.freeIndex
        freeSize = other.freeSize
    }

    /**
     * Returns value Iterator<T> - Iterator<T> type, which can be traversed using an iterator.
     *
     * @param key transfer key to obtain the value.
     * @return the value corresponding to the return key is encapsulated with option.
     *
     * @since 0.18.4
     */
    @Frozen
    public func get(key: K): ?V {
        let hash: Int64 = getHash(key.hashCode())
        let index: Int64 = getIndex(hash, buckets.size)
        var bucket: Int64 = buckets[index]
        while (bucket >= 0) {
            if (hash == entries[bucket].hash && key == entries[bucket].key) {
                return entries[bucket].value
            }
            bucket = entries[bucket].next
        }
        return None
    }
    // 审计 2026-09-24 ⚠️#17 处置：@local! 版 get 的键参数放宽为 K @local?（对齐文档
    // 《总览》§18 与文档先行写法）；K 的 hashCode/== 均有 @local? 形态，@local! 实参
    // 经隐式拓宽传入；@local? 键 + @local? 接收者的场景用下方 get(this @local?, key: K @local?)。
    @Frozen
    public exclave func get(this @local!, key: K @local?): ?V @local! {
        let hash: Int64 = getHash(key.hashCode())
        let index: Int64 = getIndex(hash, buckets.size)
        var bucket: Int64 = buckets[index]
        while (bucket >= 0) {
            if (hash == entries[bucket].hash && key == entries[bucket].key) {
                return entries[bucket].value
            }
            bucket = entries[bucket].next
        }
        return None
    }
    // gap audit 2026-09-24 #19 / §9.1(c)（= #4）：@local? 版 get。与
    // contains(this @local?, key: K @local?)（:940）同一探测链，hashCode/== 走类型的
    // @local? 覆写；命中返回 value，未命中返回 None。语义与 get(this @local!)（:591）
    // 一致（键参数同为 K @local?），仅接收者 locality 放宽。
    @Frozen
    public exclave func get(this @local?, key: K @local?): Option<V> @local? {
        let hash: Int64 = getHash(key.hashCode())
        let index: Int64 = getIndex(hash, buckets.size)
        var bucket: Int64 = buckets[index]
        while (bucket >= 0) {
            if (hash == entries[bucket].hash && key == entries[bucket].key) {
                return Some(entries[bucket].value)
            }
            bucket = entries[bucket].next
        }
        return None
    }

    /**
     * Associates the specified @p value with the specified @p key in this map.
     * If you map a mapping that previously contained a key, the old value is replaced.
     *
     * @param key the key to put.
     * @param value the value to assign.
     * @return If the key exists before the assignment, the value before the assignment is encapsulated with Option.
     * Otherwise, return to Option<V>.None
     *
     * @since 0.18.4
     */
    @Frozen
    @OverflowWrapping
    public func add(key: K, value: V): Option<V> {
        let hash: Int64 = getHash(key.hashCode())
        var bucketIndex: Int64 = getIndex(hash, buckets.size)
        var entryIndex: Int64 = buckets[bucketIndex]
        while (entryIndex >= 0) {
            if (hash == entries[entryIndex].hash && key == entries[entryIndex].key) {
                let oldValue: Option<V> = Some(entries[entryIndex].value)
                entries[entryIndex] = HashMapEntry<K, V>(entries[entryIndex].hash, entries[entryIndex].next,
                    entries[entryIndex].key, value)
                return oldValue
            }
            entryIndex = entries[entryIndex].next
        }
        if (freeSize > 0) {
            entryIndex = freeIndex
            freeIndex = entries[entryIndex].next
            freeSize--
        } else {
            if (resize(size + 1)) {
                bucketIndex = getIndex(hash, buckets.size)
            }
            entryIndex = appendIndex
            appendIndex++
        }

        entries[entryIndex] = HashMapEntry<K, V>(hash, buckets[bucketIndex], key, value)
        modCount++
        buckets[bucketIndex] = entryIndex
        return Option<V>.None
    }
    @Frozen
    @OverflowWrapping
    public exclave func add(this @local!, key: K @local!, value: V @local!): Option<V> @local! {
        let hash: Int64 = getHash(key.hashCode())
        var bucketIndex: Int64 = getIndex(hash, buckets.size)
        var entryIndex: Int64 = buckets[bucketIndex]
        while (entryIndex >= 0) {
            if (hash == entries[entryIndex].hash && key == entries[entryIndex].key) {
                let oldValue: Option<V> @local! = Option<V>.Some(entries[entryIndex].value) @local!
                entries[entryIndex] = HashMapEntry<K, V>(entries[entryIndex].hash, entries[entryIndex].next,
                    entries[entryIndex].key, value) @local!
                return oldValue
            }
            entryIndex = entries[entryIndex].next
        }
        if (freeSize > 0) {
            entryIndex = freeIndex
            freeIndex = entries[entryIndex].next
            freeSize--
        } else {
            if (resize(size + 1)) {
                bucketIndex = getIndex(hash, buckets.size)
            }
            entryIndex = appendIndex
            appendIndex++
        }

        entries[entryIndex] = HashMapEntry<K, V>(hash, buckets[bucketIndex], key, value) @local!
        modCount++
        buckets[bucketIndex] = entryIndex
        return Option<V>.None
    }

    @Frozen
    @OverflowWrapping
    protected func putWithoutResize(key: K, value: V): Unit {
        let hash: Int64 = getHash(key.hashCode())
        var bucketIndex: Int64 = getIndex(hash, buckets.size)
        var entryIndex: Int64 = buckets[bucketIndex]
        while (entryIndex >= 0) {
            if (hash == entries[entryIndex].hash && key == entries[entryIndex].key) {
                entries[entryIndex] = HashMapEntry<K, V>(entries[entryIndex].hash, entries[entryIndex].next,
                    entries[entryIndex].key, value)
                return
            }
            entryIndex = entries[entryIndex].next
        }

        entryIndex = appendIndex
        appendIndex++

        entries[entryIndex] = HashMapEntry<K, V>(hash, buckets[bucketIndex], key, value)
        modCount++
        buckets[bucketIndex] = entryIndex
    }

    @Frozen
    @OverflowWrapping
    protected exclave func putWithoutResize(this @local!, key: K @local!, value: V @local!): Unit {
        let hash: Int64 = getHash(key.hashCode())
        var bucketIndex: Int64 = getIndex(hash, buckets.size)
        var entryIndex: Int64 = buckets[bucketIndex]
        while (entryIndex >= 0) {
            if (hash == entries[entryIndex].hash && key == entries[entryIndex].key) {
                entries[entryIndex] = HashMapEntry<K, V>(entries[entryIndex].hash, entries[entryIndex].next,
                    entries[entryIndex].key, value) @local!
                return
            }
            entryIndex = entries[entryIndex].next
        }

        entryIndex = appendIndex
        appendIndex++

        entries[entryIndex] = HashMapEntry<K, V>(hash, buckets[bucketIndex], key, value) @local!
        modCount++
        buckets[bucketIndex] = entryIndex
    }

    /**
     * A view of a single entry in a hashmap, which can be empty or has a value.
     *
     * @param key the key to put.
     * @return If the hashmap has this key, a view with a value is returned. Otherwise, an empty view is returned.
     *
     * @since 0.45.1
     */
    @Frozen
    public func entryView(key: K): MapEntryView<K, V> {
        let hash: Int64 = getHash(key.hashCode())
        var index: Int64 = getIndex(hash, buckets.size)
        var entryIndex: Int64 = buckets[index]
        while (entryIndex >= 0) {
            if (hash == entries[entryIndex].hash && key == entries[entryIndex].key) {
                return HashMapEntryView<K, V>(false, key, hash, entryIndex, this)
            }
            entryIndex = entries[entryIndex].next
        }
        return HashMapEntryView<K, V>(true, key, hash, entryIndex, this)
    }
    @Frozen
    public exclave func entryView(this @local!, key: K @local!): MapEntryView<K, V> @local! {
        // 已知编译器限制：MapEntryView 接口要求 plain 的 key/value 属性，而返回的本地视图
        // 必须持有 `@local!` 的 map 引用；编译器禁止把局部 map 引用存入普通字段
        // （字段不能标注 locality，接收者带 locality 时字段写入又要求同局部性的值）。
        // 按既定回退规则显式拒绝，而非返回语义错误的视图。
        throw UnimplementedException("HashMap.entryView() is unavailable in local mode: " +
            "MapEntryView requires plain key/value properties, which cannot hold a local map reference.")
    }

    @Frozen
    public exclave func entryView(this @local?, key: K @local!): MapEntryView<K, V> @local? {
        throw UnimplementedException("HashMap.entryView() is unavailable in local mode: " +
            "MapEntryView requires plain key/value properties, which cannot hold a local map reference.")
    }

    /**
     * Transfer specified elements for traversal and assign values in sequence.
     * If you map a mapping that previously contained a key, the old value is replaced.
     *
     * @param elements the element passing in for traversal assignment.
     *
     * @since 0.18.4
     */
    @Frozen
    public func add(all!: Collection<(K, V)>): Unit {
        if (all.size > entries.size) {
            resize(all.size)
        }
        for ((k, v) in all) {
            add(k, v)
        }
    }
    @Frozen
    public exclave func add(this @local!, all!: Collection<(K, V)> @local!) {
        for ((k, v) in all) {
            add(k, v)
        }
    }

    /**
     * Removes the key-value pair corresponding to the key based on the specified key from this mapping, if one exists.
     *
     * @param key pass in the key to be deleted.
     * @return removed element
     *
     * @since 0.18.4
     */
    @Frozen
    @OverflowWrapping
    public func remove(key: K): Option<V> {
        let NULL_KEY: K = unsafe { zeroValue<K>() }
        let NULL_VALUE: V = unsafe { zeroValue<V>() }
        let hash: Int64 = getHash(key.hashCode())
        let index: Int64 = getIndex(hash, buckets.size)
        var bucket: Int64 = buckets[index]
        var pre: Int64 = -1
        while (bucket >= 0) {
            if (hash == entries[bucket].hash && key == entries[bucket].key) {
                if (pre < 0) {
                    buckets[index] = entries[bucket].next
                } else {
                    let temp = entries[pre]
                    entries[pre] = HashMapEntry<K, V>(temp.hash, entries[bucket].next, temp.key, temp.value)
                }
                let removeEnt: V = entries[bucket].value
                entries[bucket] = HashMapEntry<K, V>(-1, freeIndex, NULL_KEY, NULL_VALUE)
                freeIndex = bucket
                freeSize++
                modCount++
                return Some(removeEnt)
            }
            pre = bucket
            bucket = entries[bucket].next
        }
        return None
    }
    @Frozen
    @OverflowWrapping
    public exclave func remove(this @local!, key: K @local?): Option<V> @local! {
        let NULL_KEY: K @local! = unsafe { zeroValue2<K>() }
        let NULL_VALUE: V @local! = unsafe { zeroValue2<V>() }
        let hash: Int64 = getHash(key.hashCode())
        let index: Int64 = getIndex(hash, buckets.size)
        var bucket: Int64 = buckets[index]
        var pre: Int64 = -1
        while (bucket >= 0) {
            if (hash == entries[bucket].hash && key == entries[bucket].key) {
                if (pre < 0) {
                    buckets[index] = entries[bucket].next
                } else {
                    let temp = entries[pre]
                    entries[pre] = HashMapEntry<K, V>(temp.hash, entries[bucket].next, temp.key, temp.value) @local!
                }
                let removeEnt: V @local! = entries[bucket].value
                entries[bucket] = HashMapEntry<K, V>(-1, freeIndex, NULL_KEY, NULL_VALUE) @local!
                freeIndex = bucket
                freeSize++
                modCount++
                return Option<V>.Some(removeEnt) @local!
            }
            pre = bucket
            bucket = entries[bucket].next
        }
        return Option<V>.None
    }

    /**
     * Traverse the set of transferred keys and delete them based on the traversal result.
     *
     * @param keys pass in the collection to traverse.
     *
     * @since 0.18.4
     */
    @Frozen
    public func remove(all!: Collection<K>): Unit {
        for (key in all) {
            remove(key)
        }
    }
    @Frozen
    public exclave func remove(this @local!, all!: Collection<K> @local?) {
        for (key in all) {
            remove(key)
        }
    }

    /**
     * Transfer a lambda expression and delete the corresponding key value if the condition is met.
     *
     * @param predicate transfer a lambda expression for judgment.
     *
     * @since 0.18.4
     */
    @Frozen
    @OverflowWrapping
    public func removeIf(predicate: (K, V) -> Bool): Unit {
        let NULL_KEY: K = unsafe { zeroValue<K>() }
        let NULL_VALUE: V = unsafe { zeroValue<V>() }
        var lockVersion = modCount
        for (i in 0..buckets.size where buckets[i] != -1) {
            var next: Int64 = buckets[i]
            var pre: Int64 = -1
            while (next >= 0) {
                let item = entries[next]
                if (predicate(item.key, item.value)) {
                    if (lockVersion != modCount) {
                        throw ConcurrentModificationException("The predicate cannot contain a modify operation.")
                    }
                    if (pre < 0) {
                        buckets[i] = item.next
                    } else {
                        let temp = entries[pre]
                        entries[pre] = HashMapEntry<K, V>(temp.hash, item.next, temp.key, temp.value)
                    }
                    entries[next] = HashMapEntry<K, V>(-1, freeIndex, NULL_KEY, NULL_VALUE)
                    freeIndex = next
                    freeSize++
                    modCount++
                    lockVersion++
                } else {
                    if (lockVersion != modCount) {
                        throw ConcurrentModificationException("The predicate cannot contain a modify operation.")
                    }
                    pre = next
                }
                next = item.next
            }
        }
    }
    @Frozen
    @OverflowWrapping
    public exclave func removeIf(this @local!, predicate: ((K @local?, V @local?) -> Bool) @local?) {
        let NULL_KEY: K @local! = unsafe { zeroValue2<K>() }
        let NULL_VALUE: V @local! = unsafe { zeroValue2<V>() }
        var lockVersion = modCount
        for (i in 0..buckets.size where buckets[i] != -1) {
            var next: Int64 = buckets[i]
            var pre: Int64 = -1
            while (next >= 0) {
                let item = entries[next]
                if (predicate(item.key, item.value)) {
                    if (lockVersion != modCount) {
                        throw ConcurrentModificationException("The predicate cannot contain a modify operation.")
                    }
                    if (pre < 0) {
                        buckets[i] = item.next
                    } else {
                        let temp = entries[pre]
                        entries[pre] = HashMapEntry<K, V>(temp.hash, item.next, temp.key, temp.value) @local!
                    }
                    entries[next] = HashMapEntry<K, V>(-1, freeIndex, NULL_KEY, NULL_VALUE) @local!
                    freeIndex = next
                    freeSize++
                    modCount++
                    lockVersion++
                } else {
                    if (lockVersion != modCount) {
                        throw ConcurrentModificationException("The predicate cannot contain a modify operation.")
                    }
                    pre = next
                }
                next = item.next
            }
        }
    }

    /**
     * Clear all key-value pairs.
     *
     * @since 0.18.4
     */
    @Frozen
    @OverflowWrapping
    public func clear(): Unit {
        for (i in 0..entries.size where entries[i].hash >= 0) {
            entries[i] = HashMapEntry<K, V>()
        }

        for (i in 0..buckets.size where buckets[i] != -1) {
            buckets[i] = -1
        }

        modCount++
        freeIndex = -1
        appendIndex = 0
        freeSize = 0
    }
    @Frozen
    @OverflowWrapping
    public exclave func clear(this @local!) {
        for (i in 0..entries.size where entries[i].hash >= 0) {
            entries[i] = HashMapEntry<K, V>() @local!
        }
        for (i in 0..buckets.size where buckets[i] != -1) {
            buckets[i] = -1
        }
        modCount++
        freeIndex = -1
        appendIndex = 0
        freeSize = 0
    }

    /**
     * Reserves capacity for at least additional more elements to be inserted in this hashmap.
     * If the additional parameter is negative, the hash map does not change.
     * If the additional parameter plus the size of the hashmap is smaller than the capacity of the  array, the hashmap does not change.
     *
     * @param additional ensure that there is sufficient capacity. additional indicates the quantity to be added.
     *
     * @since 0.18.4
     */
    @Frozen
    public func reserve(additional: Int64): Unit {
        if (additional <= 0 || size + additional < capacity) {
            return
        }

        resize(size + additional)
        modCount++
    }
    @Frozen
    public exclave func reserve(this @local!, additional: Int64) {
        if (additional <= 0 || size + additional < capacity) {
            return
        }
        resize(size + additional)
        modCount++
    }

    /**
     * Returns the capacity of the hashmap.
     * The return value is the size of the array, not the size of the elements that can be accommodated.
     *
     * @return the capacity of the hashmap.
     *
     * @since 0.18.4
     */
    @Frozen
    public prop capacity: Int64 @local? {
        get() {
            return entries.size
        }
    }

    /**
     * Checks whether the mapping relationship corresponding to the collection key exists in this mapping.
     *
     * @param keys transfer the collection key to be judged.
     * @return bool returns true if exists; otherwise, false.
     *
     * @since 0.18.4
     */
    @Frozen
    public func contains(all!: Collection<K>): Bool {
        for (key in all where !contains(key)) {
            return false
        }
        return true
    }
    @Frozen
    public func contains(this @local?, all!: Collection<K> @local?): Bool {
        for (key in all) {
            if (!contains(key)) {
                return false
            }
        }
        return true
    }

    /**
     * Checks whether the mapping relationship corresponding to the specified key exists in this mapping.
     *
     * @param key transfer the key to be judged.
     * @return bool returns true if exists; otherwise, false.
     *
     * @since 0.18.4
     */
    @Frozen
    public func contains(key: K): Bool {
        if (appendIndex - freeSize == 0) {
            return false
        }
        let hash: Int64 = getHash(key.hashCode())
        let index: Int64 = getIndex(hash, buckets.size)
        var bucket: Int64 = buckets[index]
        while (bucket >= 0) {
            if (hash == entries[bucket].hash && key == entries[bucket].key) {
                return true
            }
            bucket = entries[bucket].next
        }
        return false
    }

    // local-mode @local? contains（C1 修复后恢复；2026-09-14）
    // 与 plain contains 同一探测链；差异仅在 key 以 @local? 参与比较，
    // hashCode/== 均走类型的 @local? 覆写（Hashable/Equatable 适配层已提供）。
    @Frozen
    public func contains(this @local?, key: K @local?): Bool {
        if (appendIndex - freeSize == 0) {
            return false
        }
        let hash: Int64 = getHash(key.hashCode())
        let index: Int64 = getIndex(hash, buckets.size)
        var bucket: Int64 = buckets[index]
        while (bucket >= 0) {
            if (hash == entries[bucket].hash && key == entries[bucket].key) {
                return true
            }
            bucket = entries[bucket].next
        }
        return false
    }

    /**
     * Copy a HashMap.
     *
     * @return a clone value of HashMap.
     *
     * @since 0.18.4
     */
    @Frozen
    public func clone(): HashMap<K, V> {
        return HashMap<K, V>(this)
    }
    @Frozen
    public exclave func clone(this @local!): HashMap<K, V> @local! {
        // 编译器限制：HashMapEntry<K, V> 含无约束 K/V，无法整数组拷贝——Array 的
        // copyTo/cloneToLocal 位于 `extend<T> Array<T> where T <: Copyable`
        // （array.cj:1709）。改按 add 逐条复制，保证结果独立于源（写不穿透）。
        let c: HashMap<K, V> @local! = HashMap<K, V>(size) @local!
        for ((k, v) in this) {
            c.add(k, v)
        }
        c
    }

    /**
     * Returns the Set view of all keys in this HashMap.
     *
     * @return the Set view of the keys.
     *
     * @since 0.18.4
     */
    @Frozen
    public func keys(): EquatableCollection<K> {
        return HashMapKeys<K, V>(this)
    }
    @Frozen
    public exclave func keys(this @local!): EquatableCollection<K> @local! {
        HashMapKeysLocal<K, V>(this) @local!
    }

    @Frozen
    public exclave func keys(this @local?): EquatableCollection<K> @local? {
        HashMapKeysLocal<K, V>(this) @local?
    }

    /**
     * Returns the Set view of all values in this HashMap.
     *
     * @return the list view of the values.
     *
     * @since 0.18.4
     */
    @Frozen
    public func values(): Collection<V> {
        return HashMapValues<K, V>(this)
    }
    @Frozen
    public exclave func values(this @local!): Collection<V> @local! {
        HashMapValuesLocal<K, V>(this) @local!
    }

    @Frozen
    public exclave func values(this @local?): Collection<V> @local? {
        HashMapValuesLocal<K, V>(this) @local?
    }

    /**
     * @description Performs mapping on this HashMap and returns a new HashMap.
     * 
     * @param transform the given mapping function.
     * @returns a new HashMap.
     */
    @When[env != "ohos"]
    public func mapValues<R>(transform: (K, V) -> R): HashMap<K, R> {
        let size: Int64 = entries.size
        var newMap = HashMap<K, R>()
        for (i in 0..size) {
            if (entries[i].hash >= 0) {
                newMap.add(entries[i].key, transform(entries[i].key, entries[i].value))
            }
        }
        return newMap
    }
    @Frozen
    public exclave func mapValues<R>(this @local!, transform: ((K @local?, V @local?) -> R @local!) @local?):
        HashMap<K, R> @local! {
        let newMap: HashMap<K, R> @local! = HashMap<K, R>() @local!
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0) {
                let nv: R @local! = transform(entries[i].key, entries[i].value)
                newMap.add(entries[i].key, nv)
            }
        }
        newMap
    }

    /**
     * @description Performs mapping on this HashMap and returns a new HashMap.
     * 
     * @param transform the given mapping function.
     * @returns a new HashMap.
     */
    @When[env != "ohos"]
    public func mapValues<R>(transform: (V) -> R): HashMap<K, R> {
        let size: Int64 = entries.size
        var newMap = HashMap<K, R>()
        for (i in 0..size) {
            if (entries[i].hash >= 0) {
                newMap.add(entries[i].key, transform(entries[i].value))
            }
        }
        return newMap
    }
    @Frozen
    public exclave func mapValues<R>(this @local!, transform: ((V @local?) -> R @local!) @local?):
        HashMap<K, R> @local! {
        let newMap: HashMap<K, R> @local! = HashMap<K, R>() @local!
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0) {
                let nv: R @local! = transform(entries[i].value)
                newMap.add(entries[i].key, nv)
            }
        }
        newMap
    }

    /**
     * @description Returns a new HashMap<K, V> containing key-value pairs that satisfy the filtering condition.
     * 
     * @param predicate the given condition.
     * @returns a new collection of key-value pairs that satisfy the filtering condition.
     */
    @When[env != "ohos"]
    public func filter(predicate: (K, V) -> Bool): HashMap<K, V> {
        let size: Int64 = entries.size
        var newMap = HashMap<K, V>()
        for (i in 0..size) {
            if (entries[i].hash >= 0 && predicate(entries[i].key, entries[i].value)) {
                newMap.add(entries[i].key, entries[i].value)
            }
        }
        return newMap
    }
    @Frozen
    public exclave func filter(this @local!, predicate: ((K @local?, V @local?) -> Bool) @local?):
        HashMap<K, V> @local! {
        let newMap: HashMap<K, V> @local! = HashMap<K, V>() @local!
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0 && predicate(entries[i].key, entries[i].value)) {
                newMap.add(entries[i].key, entries[i].value)
            }
        }
        newMap
    }

    /**
     * @description Iterates over all key-value pairs and performs the given operation.
     * 
     * @param action the given operation function.
     */
    @When[env != "ohos"]
    @Frozen
    public func forEach(action: (K, V) -> Unit): Unit {
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0) {
                action(entries[i].key, entries[i].value)
            }
        }
    }

    @Frozen
    public func forEach(this @local?, action: ((K @local?, V @local?) -> Unit) @local?) {
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0) {
                action(entries[i].key, entries[i].value)
            }
        }
    }

    /**
     * @description Determine whether all key-value pairs in the HashMap satisfy the condition.
     * 
     * @param predicate the given condition.
     * @returns true if all key-value pairs in the HashMap satisfy the condition, otherwise returns false.
     */
    @When[env != "ohos"]
    public func all(predicate: (K, V) -> Bool): Bool {
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0 && !predicate(entries[i].key, entries[i].value)) {
                return false
            }
        }
        return true
    }
    @Frozen
    public func all(this @local?, predicate: ((K @local?, V @local?) -> Bool) @local?): Bool {
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0 && !predicate(entries[i].key, entries[i].value)) {
                return false
            }
        }
        return true
    }

    /**
     * @description Determine whether there is any key-value pair in the HashMap that satisfies the condition.
     * 
     * @param predicate the given condition.
     * @returns Whether there is any key-value pair that satisfies the condition.
     */
    @When[env != "ohos"]
    public func any(predicate: (K, V) -> Bool): Bool {
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0 && predicate(entries[i].key, entries[i].value)) {
                return true
            }
        }
        return false
    }
    @Frozen
    public func any(this @local?, predicate: ((K @local?, V @local?) -> Bool) @local?) : Bool {
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0 && predicate(entries[i].key, entries[i].value)) {
                return true
            }
        }
        return false
    }

    /**
     * @description Determine whether all key-value pairs in the HashMap do not satisfy the condition.
     * 
     * @param predicate the given condition.
     * @returns Whether all key-value pairs in the current HashMap do not satisfy the condition.
     */
    @When[env != "ohos"]
    public func none(predicate: (K, V) -> Bool): Bool {
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0 && predicate(entries[i].key, entries[i].value)) {
                return false
            }
        }
        return true
    }
    @Frozen
    public func none(this @local?, predicate: ((K @local?, V @local?) -> Bool) @local?) : Bool {
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0 && predicate(entries[i].key, entries[i].value)) {
                return false
            }
        }
        return true
    }

    /**
     * @description Computes from left to right using the specified initial value.
     * 
     * @param initial the given initial value of type R.
     * @param operation the given computation function.
     * @returns the final computed value.
     */
    @When[env != "ohos"]
    public func fold<R>(initial: R, operation: (R, K, V) -> R): R {
        var result: R = initial
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0) {
                result = operation(result, entries[i].key, entries[i].value)
            }
        }
        return result
    }
    @Frozen
    public exclave func fold<R>(this @local!, initial: R @local!,
        operation: ((R @local!, K @local?, V @local?) -> R @local!) @local?): R @local! {
        var result: R @local! = initial
        let size: Int64 = entries.size
        for (i in 0..size) {
            if (entries[i].hash >= 0) {
                result = operation(result, entries[i].key, entries[i].value)
            }
        }
        result
    }

    /**
     * @description Compute from left to right using the first value as the initial value.
     * 
     * @param operation the given computation function.
     * @returns the final computed value.
     */
    @When[env != "ohos"]
    public func reduce(operation: (V, V) -> V): Option<V> {
        if (this.size == 0) {
            return None<V>
        }
        let len: Int64 = entries.size
        var index = 0
        var result = unsafe { zeroValue<V>() }
        while (index < len) {
            if (entries[index].hash >= 0) {
                result = entries[index].value
                index++
                break
            }
            index++
        }
        if (this.size == 1) {
            return result
        }
        while (index < len) {
            if (entries[index].hash >= 0) {
                result = operation(result, entries[index].value)
            }
            index++
        }
        return result
    }
    @Frozen
    public exclave func reduce(this @local!, operation: ((V @local!, V @local?) -> V @local!) @local?):
        Option<V> @local! {
        if (size == 0) {
            return Option<V>.None
        }
        let len: Int64 = entries.size
        var index = 0
        var result: V @local! = unsafe { zeroValue2<V>() }
        while (index < len) {
            if (entries[index].hash >= 0) {
                result = entries[index].value
                index++
                break
            }
            index++
        }
        if (size == 1) {
            return Option<V>.Some(result) @local!
        }
        while (index < len) {
            if (entries[index].hash >= 0) {
                result = operation(result, entries[index].value)
            }
            index++
        }
        Option<V>.Some(result) @local!
    }

    /**
     * An exception is reported when the get operator is overloaded and the key does not exist.
     *
     * @param key transfer the value for judgment.
     * @return the value corresponding to the key.
     *
     * @throws NoneValueException if value does not exist.
     *
     * @since 0.18.4
     */
    @Frozen
    public operator func [](key: K): V {
        return match (get(key)) {
            case None => throw NoneValueException("Value does not exist!\n")
            case Some(val) => val
        }
    }
    @Frozen
    public exclave operator func [](this @local!, key: K @local?): V @local! {
        let hash: Int64 = getHash(key.hashCode())
        let index: Int64 = getIndex(hash, buckets.size)
        var bucket: Int64 = buckets[index]
        while (bucket >= 0) {
            if (hash == entries[bucket].hash && key == entries[bucket].key) {
                return entries[bucket].value
            }
            bucket = entries[bucket].next
        }
        throw NoneValueException("Value does not exist!\n")
    }
    // gap audit 2026-09-24 #19 附带：@local? 版下标读取。与
    // get(this @local?, key: K @local?)（:611）同一探测链，hashCode/== 走类型的
    // @local? 覆写；命中返回 value，未命中抛 NoneValueException。语义与
    // @local! 版（:1224）一致，仅接收者与键的 locality 放宽。
    @Frozen
    public exclave operator func [](this @local?, key: K @local?): V @local? {
        let hash: Int64 = getHash(key.hashCode())
        let index: Int64 = getIndex(hash, buckets.size)
        var bucket: Int64 = buckets[index]
        while (bucket >= 0) {
            if (hash == entries[bucket].hash && key == entries[bucket].key) {
                return entries[bucket].value
            }
            bucket = entries[bucket].next
        }
        throw NoneValueException("Value does not exist!\n")
    }

    /**
     * The operator overloads the set. If the key does not exist, an exception is reported.
     *
     * @param key transfer the value for judgment.
     * @param value transfer the value to be set.
     *
     * @since 0.18.4
     */
    @Frozen
    public operator func [](key: K, value!: V): Unit {
        add(key, value)
    }
    @Frozen
    public exclave operator func [](this @local!, key: K @local!, value!: V @local!): Unit {
        add(key, value)
    }

    /*
     * Transfer the key to obtain the k index.
     *
     * @param hash transfer the value for judgment.
     * @param size transfer the value for judgment.
     *
     * @return the k index corresponding to the key.
     *
     * @since 0.18.4
     */
    @Frozen
    @OverflowWrapping
    func getIndex(this @local?, hash: Int64, size: Int64): Int64 {
        return hash & (size - 1)
    }

    /*
     * Transfer the key to obtain the hash value.
     *
     * @param key transfer the value for judgment.
     * @return the hash value corresponding to the key.
     *
     * @since 0.18.4
     */
    @Frozen
    @OverflowWrapping
    private func getHash(this @local?, hashCode: Int64): Int64 {
        hashCode ^ (hashCode >> 32)
    }

    /**
     * Returns sizes of key-value.
     *
     * @return sizes of key-value.
     *
     * @since 0.18.4
     */
    @Frozen
    public prop size: Int64 {
        get() {
            return appendIndex - freeSize
        }
    }
    @Frozen
    public prop size: Int64 @local? {
        get() {
            return appendIndex - freeSize
        }
    }

    /**
     * Returns iterator of hashmap.
     *
     * @return iterator of hashmap.
     *
     * @since 0.18.4
     */
    @Frozen
    public func iterator(): HashMapIterator<K, V> {
        return HashMapIterator<K, V>(this)
    }
    @Frozen
    public exclave func iterator(this @local!): HashMapIterator<K, V> @local! {
        return HashMapIterator<K, V>(this)
    }
    @Frozen
    public exclave func iterator(this @local?): HashMapIterator<K, V> @local? {
        return HashMapIterator<K, V>(this)
    }

    /**
     * Check whether the size is empty. If yes, true is returned. Otherwise, false is returned.
     *
     * @return bool if yes, true is returned. Otherwise, false is returned.
     *
     * @since 0.18.4
     */
    @Frozen
    public func isEmpty(): Bool {
        return (appendIndex - freeSize) == 0
    }
    @Frozen
    public func isEmpty(this @local?): Bool {
        return (appendIndex - freeSize) == 0
    }

    /**
     * Obtaining the Version Number.
     *
     * @return the modCount about technology.
     *
     * @since 0.18.4
     */
    @Frozen
    func version(): Int64 {
        return modCount
    }
    @Frozen
    func version(this @local?): Int64 {
        return modCount
    }

    @Frozen
    private func max(l: Int64, r: Int64) {
        return if (l >= r) {
            l
        } else {
            r
        }
    }

    /**
     * Returns the element in this Map as an Array.
     */
    @Frozen
    public func toArray(): Array<(K, V)> {
        let arr = Array<(K, V)>(size, repeat: unsafe { (zeroValue<K>(), zeroValue<V>()) })
        var index = 0
        var pos = 0
        while (index < size) {
            if (entries[pos].hash >= 0) {
                arr[index] = (entries[pos].key, entries[pos].value)
                index++
            }
            pos++
        }
        return arr
    }
    @Frozen
    public exclave func toArray(this @local!): Array<(K, V)> @local! {
        let arr: Array<(K, V)> @local! =
            Array<(K, V)>(size, repeat: unsafe { (zeroValue2<K>(), zeroValue2<V>()) }) @local!
        var index = 0
        var pos = 0
        while (index < size) {
            if (entries[pos].hash >= 0) {
                let pair: (K, V) @local! = (entries[pos].key, entries[pos].value)
                arr[index] = pair
                index++
            }
            pos++
        }
        arr
    }

    @Frozen
    public exclave func toArray(this @local?): Array<(K, V)> @local? {
        // 编译器限制：@local? 接收者读出的 key/value 为 @local?，无法写入新数组——
        // Array 的 `[]=(this @local?, ...)` 写入器在 Copyable 扩展内（array.cj:1714），
        // (K, V) 无该约束。
        // 按既定回退规则显式拒绝；需要数组时请在 @local! 形态上调用。
        throw UnimplementedException("HashMap.toArray() is unavailable for a @local? receiver; " +
            "call it on the @local! form.")
    }

    /*
     * Initializes or doubles the table size.
     * If empty, the allocation is based on the initial DEFAULT_CAPACITY target reserved in the field threshold.
     *
     * @return a TreeNode encapsulated with option after expansion.
     *
     * @since 0.18.4
     */
    @Frozen
    @OverflowWrapping
    func resize(argCap: Int64): Bool {
        var isRehash: Bool = false
        let oldBucketsSize = buckets.size
        let allowedBucketSize = Int64(Float32(oldBucketsSize) * LOAD_FACTOR)

        // Except for reserve, following two if condition would not be satisfied at the same time.
        if (argCap >= allowedBucketSize) {
            let newCapOfBuckets = max(oldBucketsSize << 1, tableSizeFor(argCap))
            let newBuckets: Array<Int64> = Array<Int64>(newCapOfBuckets, repeat: -1)
            for (i in 0..entries.size) {
                if (entries[i].hash >= 0) {
                    let bucket: Int64 = getIndex(entries[i].hash, newCapOfBuckets)
                    entries[i] = HashMapEntry<K, V>(entries[i].hash, newBuckets[bucket], entries[i].key,
                        entries[i].value)
                    newBuckets[bucket] = i
                }
            }
            buckets = newBuckets
            isRehash = true
        }
        if (argCap > entries.size) {
            let oldEntriesSize = entries.size
            let newCapOfEntries = max(oldEntriesSize + (oldEntriesSize >> 1), argCap)
            let newEntries = Array<HashMapEntry<K, V>>(
                newCapOfEntries,
                repeat: HashMapEntry<K, V>()
            )
            entries.copyTo(newEntries, 0, 0, oldEntriesSize)
            entries = newEntries
        }
        return isRehash
    }
    @Frozen
    @OverflowWrapping
    exclave func resize(this @local!, argCap: Int64): Bool {
        var isRehash: Bool = false
        let oldBucketsSize = buckets.size
        let allowedBucketSize = Int64(Float32(oldBucketsSize) * LOAD_FACTOR)

        // Except for reserve, following two if condition would not be satisfied at the same time.
        if (argCap >= allowedBucketSize) {
            let newCapOfBuckets = max(oldBucketsSize << 1, tableSizeFor(argCap))
            let newBuckets: Array<Int64> @local! = Array<Int64>(newCapOfBuckets, repeat: -1)
            for (i in 0..entries.size) {
                if (entries[i].hash >= 0) {
                    let bucket: Int64 = getIndex(entries[i].hash, newCapOfBuckets)
                    entries[i] = HashMapEntry<K, V>(entries[i].hash, newBuckets[bucket], entries[i].key,
                        entries[i].value) @local!
                    newBuckets[bucket] = i
                }
            }
            buckets = newBuckets
            isRehash = true
        }
        if (argCap > entries.size) {
            let oldEntriesSize = entries.size
            let newCapOfEntries = max(oldEntriesSize + (oldEntriesSize >> 1), argCap)
            let newEntries = Array<HashMapEntry<K, V>>(
                newCapOfEntries,
                repeat: HashMapEntry<K, V>() @local!
            ) @local!
            entries.copyTo(newEntries, 0, 0, oldEntriesSize)
            entries = newEntries
        }
        return isRehash
    }

    /*
     * Returns a power of two size for the given target capacity.
     *
     * @return target capacity.
     *
     * @since 0.18.4
     */
    @Frozen
    private static func tableSizeFor(cap: Int64): Int64 {
        if (cap <= DEFAULT_CAPACITY) {
            return DEFAULT_CAPACITY
        }
        if (cap < MAX_BUCKET_SIZE) {
            var n: Int64 = cap - 1
            n |= n >> 1
            n |= n >> 2
            n |= n >> 4
            n |= n >> 8
            n |= n >> 16
            n |= n >> 32
            return n + 1
        } else {
            return MAX_BUCKET_SIZE
        }
    }

    @Frozen
    func removePosition(hash: Int64, pos: Int64) {
        let index: Int64 = getIndex(hash, buckets.size)
        var bucket: Int64 = buckets[index]
        var pre: Int64 = -1
        while (bucket != pos) {
            pre = bucket
            bucket = entries[bucket].next
        }
        if (pre < 0) {
            buckets[index] = entries[pos].next
        } else {
            entries[pre] = HashMapEntry<K, V>(entries[pre].hash, entries[pos].next, entries[pre].key, entries[pre].value)
        }
        entries[pos] = HashMapEntry<K, V>(-1, freeIndex, unsafe { zeroValue<K>() }, unsafe { zeroValue<V>() })
        freeIndex = bucket
        freeSize++
        modCount++
    }
}

/**
 *
 *
 * @since 0.18.4
 */
extend<K, V> HashMap<K, V> <: ToString where V <: ToString, K <: ToString {
    @Frozen
    public func toString(): String {
        if (size == 0) {
            return "[]"
        }
        let sb = StringBuilder("[")
        let it = iterator()
        var tmp: (K, V) = it.next().getOrThrow()
        while (let Some(next) <- it.next()) {
            //"(${k}, ${v})"
            sb.append(r'(')
            sb.append(tmp[0])
            unsafe { sb.appendFromUtf8Unchecked(", ".toArray()) }
            sb.append(tmp[1])
            unsafe { sb.appendFromUtf8Unchecked("), ".toArray()) }
            tmp = next
        }
        sb.append(r'(')
        sb.append(tmp[0])
        unsafe { sb.appendFromUtf8Unchecked(", ".toArray()) }
        sb.append(tmp[1])
        unsafe { sb.appendFromUtf8Unchecked(")]".toArray()) }
        return sb.toString()
    }

    // 提案 §19：toString(this @local?): String @local!。
    // StringBuilder 的 local 接口仅 std.core 包内可见，故此处只用公开 local API：
    // 本地迭代 + String.+(this @local!, others: String @local?) + ToString.toString(@local?)。
    @Frozen
    public exclave func toString(this @local?): String @local! {
        var result: String @local! = "["
        var first = true
        for ((k, v) in this) {
            if (!first) {
                result = result + ", "
            }
            first = false
            result = result + "(" + k.toString() + ", " + v.toString() + ")"
        }
        result = result + "]"
        return result
    }
}

/**
 * This is the function of hashmap equal and not equal. If the keys and values are same for two hasmaps, then they are equal.
 *
 * Returns true if the two hashmaps are equal for ==. Return true if the two hashmaps are not equal for !=.
 *
 * @since 0.18.4
 */
extend<K, V> HashMap<K, V> <: Equatable<HashMap<K, V>> where V <: Equatable<V> {
    @Frozen
    public operator func ==(right: HashMap<K, V>): Bool {
        if (refEq(this, right)) {
            return true
        }
        if (size != right.size) {
            return false
        }
        for ((leftKey, leftValue) in this) {
            var a = match (right.get(leftKey)) {
                case Some(v) => v
                case None => return false
            }
            if (a != leftValue) {
                return false
            }
        }
        return true
    }

    @Frozen
    public operator func ==(this @local?, right: HashMap<K, V> @local?): Bool {
        if (this.size != right.size) {
            return false
        }
        for ((k, v) in this) {
            match (right.get(k)) {
                case Some(rv) =>
                    if (v != rv) {
                        return false
                    }
                case None =>
                    return false
            }
        }
        return true
    }

    @Frozen
    public operator func !=(right: HashMap<K, V>): Bool {
        if (size != right.size) {
            return true
        }
        for ((leftKey, leftValue) in this) {
            var a = match (right.get(leftKey)) {
                case Some(v) => v
                case None => return true
            }
            if (a != leftValue) {
                return true
            }
        }
        return false
    }
}

/**
 * This is a hashmap iterator used to iterate effects.
 *
 * @since 0.18.4
 */

/**
 * HashMap keys 的 local 视图（§19 keys(@local!/@local?) 的返回类型）。
 * 与 plain HashMapKeys 同语义，仅持有 `@local!`/`@local?` 形态的 map 引用。
 */
class HashMapKeysLocal<K, V> <: EquatableCollection<K> where K <: Hashable & Equatable<K> {
    private let map: HashMap<K, V>

    @Frozen
    exclave init(this @local!, m: HashMap<K, V> @local!) {
        map = m
    }

    @Frozen
    exclave init(this @local?, m: HashMap<K, V> @local?) {
        map = m
    }

    @Frozen
    public prop size: Int64 {
        get() {
            map.size
        }
    }

    @Frozen
    public func isEmpty(): Bool {
        map.isEmpty()
    }

    @Frozen
    public func isEmpty(this @local?): Bool {
        map.isEmpty()
    }

    @Frozen
    public func contains(element: K): Bool {
        map.contains(element)
    }

    @Frozen
    public func contains(all!: Collection<K>): Bool {
        map.contains(all: all)
    }

    @Frozen
    public func iterator(): Iterator<K> {
        map.iterator().map<K> {i: (K, V) => i[0]}
    }

    @Frozen
    public exclave func iterator(this @local!): Iterator<K> @local! {
        HashMapKeysIteratorLocal<K, V>(map) @local!
    }

    @Frozen
    public exclave func iterator(this @local?): Iterator<K> @local? {
        HashMapKeysIteratorLocal<K, V>(map) @local?
    }

    @Frozen
    public exclave func toArray(this @local!): Array<K> @local! {
        let n: Int64 = map.size
        let arr: Array<K> @local! = Array<K>(n, repeat: unsafe { zeroValue2<K>() }) @local!
        var index = 0
        var pos = 0
        while (index < n) {
            if (map.entries[pos].hash >= 0) {
                let e: K @local! = map.entries[pos].key
                arr[index] = e
                index++
            }
            pos++
        }
        arr
    }

    @Frozen
    public exclave func toArray(this @local?): Array<K> @local? {
        // 编译器限制：@local? 接收者读出的元素为 K @local?，无法写入新数组——Array 的
        // `[]=(this @local?, ...)` 写入器在 Copyable 扩展内（array.cj:1714），K 无该约束
        // （arraySet3 本身已无约束，但它对 std.collection 不可达）。
        // 按既定回退规则显式拒绝而非静默丢元素；需要数组时请在 @local! 形态上调用。
        throw UnimplementedException("HashMap.keys(...).toArray() is unavailable for a @local? receiver; " +
            "call it on the @local! form.")
    }
}

/**
 * keys 视图的本地迭代器（按 entries 顺序产出 key）。
 */
class HashMapKeysIteratorLocal<K, V> <: Iterator<K> where K <: Hashable & Equatable<K> {
    private let data: HashMap<K, V>
    private var pos: Int64 = 0

    @Frozen
    exclave init(this @local!, d: HashMap<K, V> @local!) {
        data = d
        pos = 0
    }

    @Frozen
    exclave init(this @local?, d: HashMap<K, V> @local?) {
        data = d
        pos = 0
    }

    @Frozen
    public func next(): Option<K> {
        throw UnimplementedException("Use the local-mode overload of 'next'.")
    }

    @Frozen
    public exclave func next(this @local!): Option<K> @local! {
        let n: Int64 = data.entries.size
        while (pos < n && data.entries[pos].hash < 0) {
            pos++
        }
        if (pos >= n) {
            return Option<K>.None
        }
        let k: K @local! = data.entries[pos].key
        pos++
        Option<K>.Some(k) @local!
    }

    @Frozen
    public exclave func next(this @local?): Option<K> @local? {
        let n: Int64 = data.entries.size
        while (pos < n && data.entries[pos].hash < 0) {
            pos++
        }
        if (pos >= n) {
            return Option<K>.None
        }
        let k: K @local? = data.entries[pos].key
        pos++
        let r: Option<K> @local? = Option<K>.Some(k)
        r
    }
}

/**
 * HashMap values 的 local 视图（§19 values(@local!/@local?) 的返回类型）。
 */
class HashMapValuesLocal<K, V> <: Collection<V> where K <: Hashable & Equatable<K> {
    private let map: HashMap<K, V>

    @Frozen
    exclave init(this @local!, m: HashMap<K, V> @local!) {
        map = m
    }

    @Frozen
    exclave init(this @local?, m: HashMap<K, V> @local?) {
        map = m
    }

    @Frozen
    public prop size: Int64 {
        get() {
            map.size
        }
    }

    @Frozen
    public func isEmpty(): Bool {
        map.isEmpty()
    }

    @Frozen
    public func isEmpty(this @local?): Bool {
        map.isEmpty()
    }

    @Frozen
    public func iterator(): Iterator<V> {
        map.iterator().map<V> {i: (K, V) => i[1]}
    }

    @Frozen
    public exclave func iterator(this @local!): Iterator<V> @local! {
        HashMapValuesIteratorLocal<K, V>(map) @local!
    }

    @Frozen
    public exclave func iterator(this @local?): Iterator<V> @local? {
        HashMapValuesIteratorLocal<K, V>(map) @local?
    }

    @Frozen
    public exclave func toArray(this @local!): Array<V> @local! {
        let n: Int64 = map.size
        let arr: Array<V> @local! = Array<V>(n, repeat: unsafe { zeroValue2<V>() }) @local!
        var index = 0
        var pos = 0
        while (index < n) {
            if (map.entries[pos].hash >= 0) {
                let e: V @local! = map.entries[pos].value
                arr[index] = e
                index++
            }
            pos++
        }
        arr
    }

    @Frozen
    public exclave func toArray(this @local?): Array<V> @local? {
        // 同 HashMapKeysLocal.toArray(@local?)：@local? 元素无法写入新数组（Array 的
        // @local? 写入器位于 Copyable 扩展内）。
        throw UnimplementedException("HashMap.values(...).toArray() is unavailable for a @local? receiver; " +
            "call it on the @local! form.")
    }
}

/**
 * values 视图的本地迭代器（按 entries 顺序产出 value）。
 */
class HashMapValuesIteratorLocal<K, V> <: Iterator<V> where K <: Hashable & Equatable<K> {
    private let data: HashMap<K, V>
    private var pos: Int64 = 0

    @Frozen
    exclave init(this @local!, d: HashMap<K, V> @local!) {
        data = d
        pos = 0
    }

    @Frozen
    exclave init(this @local?, d: HashMap<K, V> @local?) {
        data = d
        pos = 0
    }

    @Frozen
    public func next(): Option<V> {
        throw UnimplementedException("Use the local-mode overload of 'next'.")
    }

    @Frozen
    public exclave func next(this @local!): Option<V> @local! {
        let n: Int64 = data.entries.size
        while (pos < n && data.entries[pos].hash < 0) {
            pos++
        }
        if (pos >= n) {
            return Option<V>.None
        }
        let v: V @local! = data.entries[pos].value
        pos++
        Option<V>.Some(v) @local!
    }

    @Frozen
    public exclave func next(this @local?): Option<V> @local? {
        let n: Int64 = data.entries.size
        while (pos < n && data.entries[pos].hash < 0) {
            pos++
        }
        if (pos >= n) {
            return Option<V>.None
        }
        let v: V @local? = data.entries[pos].value
        pos++
        let r: Option<V> @local? = Option<V>.Some(v)
        r
    }
}

/**
 * HashMap entryView 的 local 视图（§19 entryView(@local!/@local?) 的返回类型）。
 */





/**
 * HashMap toArray 元素写入不可行时的占位（见 toArray 注释）。
 */
public class HashMapIterator<K, V> <: Iterator<(K, V)> where K <: Hashable & Equatable<K> {
    /* Define an empty set */
    private var lockVersion: Int64
    private var index: Int64
    private let data: HashMap<K, V>
    private var isRemoved: Bool

    /**
     * Initialize the iterator and transfer the hashmap.
     *
     * @param map the hashmap to be transferred.
     *
     * @since 0.18.4
     */
    @Frozen
    public init(map: HashMap<K, V>) {
        isRemoved = true
        data = map
        lockVersion = map.version()
        index = -1
    }
    @Frozen
    public exclave init(this @local!, map: HashMap<K, V> @local!) {
        isRemoved = true
        data = map
        lockVersion = map.version()
        index = -1
    }
    @Frozen
    public exclave init(this @local?, map: HashMap<K, V> @local?) {
        isRemoved = true
        data = map
        lockVersion = map.version()
        index = -1
    }

    /**
     * Iterates the bucketIndex element. The return type is option, which contains key and value.
     *
     * @return type is option, which contains key and value.
     *
     * @throws ConcurrentModificationException if lockVersion is not equal to "data.version()".
     *
     * @since 0.18.4
     */
    @Frozen
    public func next(): ?(K, V) {
        if (lockVersion != data.version()) {
            throw ConcurrentModificationException()
        }
        let size: Int64 = data.entries.size
        do {
            index++
        } while (index < size && data.entries[index].hash < 0)
        if (index >= size) {
            return Option<(K, V)>.None
        }
        isRemoved = false
        return Some((data.entries[index].key, data.entries[index].value))
    }
    @Frozen
    public exclave func next(this @local!): ?(K, V) @local! {
        if (lockVersion != data.version()) {
            throw ConcurrentModificationException()
        }
        let size: Int64 = data.entries.size
        do {
            index++
        } while (index < size && data.entries[index].hash < 0)
        if (index >= size) {
            return Option<(K, V)>.None
        }
        isRemoved = false
        return Option<(K,V)>.Some((data.entries[index].key, data.entries[index].value))
    }
    @Frozen
    public exclave func next(this @local?): ?(K, V) @local? {
        if (lockVersion != data.version()) {
            throw ConcurrentModificationException()
        }
        let size: Int64 = data.entries.size
        do {
            index++
        } while (index < size && data.entries[index].hash < 0)
        if (index >= size) {
            return Option<(K, V)>.None
        }
        isRemoved = false
        return Some((data.entries[index].key, data.entries[index].value))
    }

    /**
     * Remove the element returned by the next function of this iterator.
     * This method can be called only once when the next function is called.
     *
     * @see Currently, the Cangjie modifier does not implement the function visible in the package,
     * and the API function modified by public is insecure. It is recommended that this method be used temporarily.
     * This method can be improved after the function visible in the package is supported in the future.
     * In the future, the  function should be used to implement the function.
     *
     * @throws ConcurrentModificationException if lockVersion is not equal to "data.version()".
     *
     * @since 0.24.1
     */
    @Frozen
    public func remove(): Option<(K, V)> {
        if (lockVersion != data.version()) {
            throw ConcurrentModificationException()
        }
        if (isRemoved || data.size <= 0 || index >= data.entries.size || data.entries[index].hash < 0) {
            return None<(K, V)>
        }
        let item = data.entries[index]
        data.removePosition(item.hash, index)
        isRemoved = true
        lockVersion = data.version()
        return Some((item.key, item.value))
    }
}
