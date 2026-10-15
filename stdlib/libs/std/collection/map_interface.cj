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
 * This file defines the collection interface.
 */
package std.collection

public interface MapEntryView<K, V> {
    prop key: K
    mut prop value: ?V

    // Local-mode adaptations (doc §3.4.4). Declared abstract (like ReadOnlyList's
    // first/last): a default body would make the @local? prop a concrete candidate
    // that beats the plain prop for plain receivers (list_interface.cj probe_f),
    // breaking the plain view.value reads in Map.addIfAbsent/replace.
    prop key: K @local?
    mut prop value: ?V @local!
}

public interface ReadOnlyMap<K, V> <: Collection<(K, V)> {
    func get(key: K): ?V
    func contains(key: K): Bool
    func contains(all!: Collection<K>): Bool
    func keys(): EquatableCollection<K>
    func values(): Collection<V>

    operator func [](key: K): V

    // Local-mode adaptations (doc §3.4.4). Keys and key collections are read-only
    // (@local?), results derived from the map's storage lock to the receiver locality.
    // Throw-default bodies per std.core Collection: conformers without a local-mode
    // implementation are not obliged to add one, and plain-receiver overload
    // resolution still prefers the plain member.
    func get(this @local!, key: K @local?): ?V @local! {
        throw UnimplementedException("Current Type does not implement 'get' method for its @local! instance")
    }
    func get(this @local?, key: K @local?): ?V @local? {
        throw UnimplementedException("Current Type does not implement 'get' method for its @local? instance")
    }
    operator func [](this @local!, key: K @local?): V @local! {
        throw UnimplementedException("Current Type does not implement '[]' method for its @local! instance")
    }
    operator func [](this @local?, key: K @local?): V @local? {
        throw UnimplementedException("Current Type does not implement '[]' method for its @local? instance")
    }
    func contains(this @local?, key: K @local?): Bool {
        throw UnimplementedException("Current Type does not implement 'contains' method for its @local? instance")
    }
    func contains(this @local?, all!: Collection<K> @local?): Bool {
        throw UnimplementedException("Current Type does not implement 'contains' method for its @local? instance")
    }
    func keys(this @local!): EquatableCollection<K> @local! {
        throw UnimplementedException("Current Type does not implement 'keys' method for its @local! instance")
    }
    func keys(this @local?): EquatableCollection<K> @local? {
        throw UnimplementedException("Current Type does not implement 'keys' method for its @local? instance")
    }
    func values(this @local!): Collection<V> @local! {
        throw UnimplementedException("Current Type does not implement 'values' method for its @local! instance")
    }
    func values(this @local?): Collection<V> @local? {
        throw UnimplementedException("Current Type does not implement 'values' method for its @local? instance")
    }
}

/**
 * This interface is a key-value pair that does not guarantee the sequence of elements.
 * That is, the sequence of adding elements is different from the sequence of obtaining elements.
 *
 * @since 0.24.2
 */
public interface Map<K, V> <: ReadOnlyMap<K, V> {
    /**
     * Add a new key value to the map. If the key already exists,
     * the value will be overwritten and the overwritten value will be returned.
     *
     * @return @p value If the key does exist, Return the old value. Otherwise, none is returned.
     *
     * @since 0.18.4
     */
    func add(key: K, value: V): ?V

    /**
     * Transfer specified elements for traversal and assign values in sequence.
     * If you map a mapping that previously contained a key, the old value is replaced.
     *
     * @param elements the element passing in for traversal assignment.
     *
     * @since 0.18.4
     */
    func add(all!: Collection<(K, V)>): Unit

    /**
     * Removes the key-value pair corresponding to the key based on the specified key from this mapping, if one exists.
     *
     * @param key pass in the key to be deleted.
     * @return removed element
     *
     * @since 0.18.4
     */
    func remove(key: K): Option<V>

    /**
     * Traverse the set of transferred keys and delete them based on the traversal result.
     *
     * @param keys pass in the collection to traverse.
     *
     * @since 0.18.4
     */
    func remove(all!: Collection<K>): Unit

    /**
     * Transfer a lambda expression and delete the corresponding key value if the condition is met.
     *
     * @param predicate transfer a lambda expression for judgment.
     *
     * @since 0.18.4
     */
    func removeIf(predicate: (K, V) -> Bool): Unit

    /**
     * Clear all key-value pairs.
     *
     * @since 0.18.4
     */
    func clear(): Unit

    /**
     * The operator overloads the set. If the key does not exist, an exception is reported.
     *
     * @param key transfer the value for judgment.
     * @param value transfer the value to be set.
     *
     * @since 0.18.4
     */
    operator func [](key: K, value!: V): Unit

    // Local-mode adaptations for write members (doc §3.4.4). Elements are stored,
    // so they are delivered as @local!; keys used for lookup are read-only (@local?).
    func add(this @local!, key: K @local!, value: V @local!): ?V @local! {
        throw UnimplementedException("Current Type does not implement 'add' method for its @local! instance")
    }
    func add(this @local!, all!: Collection<(K, V)> @local!): Unit {
        throw UnimplementedException("Current Type does not implement 'add' method for its @local! instance")
    }
    func remove(this @local!, key: K @local?): Option<V> @local! {
        throw UnimplementedException("Current Type does not implement 'remove' method for its @local! instance")
    }
    func remove(this @local!, all!: Collection<K> @local?): Unit {
        throw UnimplementedException("Current Type does not implement 'remove' method for its @local! instance")
    }
    func removeIf(this @local!, predicate: ((K @local?, V @local?) -> Bool) @local?): Unit {
        throw UnimplementedException("Current Type does not implement 'removeIf' method for its @local! instance")
    }
    func clear(this @local!): Unit {
        throw UnimplementedException("Current Type does not implement 'clear' method for its @local! instance")
    }
    operator func [](this @local!, key: K @local!, value!: V @local!): Unit {
        throw UnimplementedException("Current Type does not implement '[]' method for its @local! instance")
    }

    func entryView(k: K): MapEntryView<K, V>

    func entryView(this @local!, k: K @local!): MapEntryView<K, V> @local! {
        throw UnimplementedException("Current Type does not implement 'entryView' method for its @local! instance")
    }
    func entryView(this @local?, k: K @local!): MapEntryView<K, V> @local? {
        throw UnimplementedException("Current Type does not implement 'entryView' method for its @local? instance")
    }

    /**
     * Associates the specified @p value with the specified @p key in this map if the key is absent.
     *
     * @param key the key to put.
     * @param value the value to set if absent.
     *
     * @return:
     * - Some(v): if the @p key exists in map before invoking this method, return the old value.
     * - None: if @p key is not in the map, set the @p key and @p value pair and return None.
     *
     * @since 0.59.3
     */
    @Frozen
    func addIfAbsent(key: K, value: V): ?V {
        var view = entryView(key)
        return match (view.value) {
            case None =>
                view.value = value
                None
            case _ => view.value
        }
    }

    @Frozen
    func addIfAbsent(this @local!, key: K @local!, value: V @local!): ?V @local! {
        throw UnimplementedException("Current Type does not implement 'addIfAbsent' method for its @local! instance")
    }

    /**
     * Replaces the value associated with @p key to @p value,
     * if there exists a pair of @p key and some value v in the map.
     * Otherwise, just returns 'None' and does nothing.
     *
     * @param key: the key of the key-value pair, whose value needs to be replaced.
     * @param value: the value to be set.
     *
     * @return:
     * - Some(v): if the pair of @p key and v exists in map before invoking this method.
     * - None: if @p key is not in the map.
     *
     * @since 0.59.3
     */
    @Frozen
    func replace(key: K, value: V): ?V {
        var view = entryView(key)
        return match (view.value) {
            case Some(v) =>
                view.value = value
                v
            case _ => None
        }
    }

    @Frozen
    func replace(this @local!, key: K @local!, value: V @local!): ?V @local! {
        throw UnimplementedException("Current Type does not implement 'replace' method for its @local! instance")
    }
}

/**
 * This interface is a collection of keys returned by a key-value pair.
 *
 * @since 0.24.2
 */
public interface EquatableCollection<T> <: Collection<T> {
    /**
     * Checks whether the mapping relationship corresponding to the specified key exists in this mapping.
     *
     * @param element specified element
     * @return bool returns true if exists; otherwise, false.
     *
     * @since 0.24.2
     */
    func contains(element: T): Bool

    /**
     * Checks whether the mapping relationship corresponding to the collection key exists in this mapping.
     *
     * @param elements specified collection of elements
     * @return bool returns true if exists; otherwise, false.
     *
     * @since 0.24.2
     */
    func contains(all!: Collection<T>): Bool

    // Local-mode adaptations (doc §3.4.4). Bool is copyable and the element is
    // read-only, so a single @local? wide gate suffices.
    func contains(this @local?, element: T @local?): Bool {
        throw UnimplementedException("Current Type does not implement 'contains' method for its @local? instance")
    }
    func contains(this @local?, all!: Collection<T> @local?): Bool {
        throw UnimplementedException("Current Type does not implement 'contains' method for its @local? instance")
    }
}

public interface OrderedMap<K, V> <: Map<K, V> {
    prop first: ?(K, V)
    prop last: ?(K, V)
    func removeFirst(): ?(K, V)
    func removeLast(): ?(K, V)

    func backward(mark: K, inclusive!: Bool): Iterator<(K, V)>
    func forward(mark: K, inclusive!: Bool): Iterator<(K, V)>
}
