/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * Implementing this interface allows an object to be the target of the "for-in loop" expression.
 */
public interface Iterable<E> {
    /**
     * Returns an iterator over elements of type E.
     *
     * @returns Iterator<E> an iterator over the elements
     */
    func iterator(): Iterator<E>
}

/**
 * An iterator over a collection.
 */
public abstract class Iterator<T> <: Iterable<T> {
    /**
     * Returns the next element in the iteration, or None if there are no more elements.
     *
     * @returns Option<T> Some containing the next element, or None if the iteration is exhausted
     */
    public func next(): Option<T>

    @Frozen
    public init() {}

    /**
     * Returns this iterator itself as an Iterable.
     *
     * @returns Iterator<T> this iterator
     */
    @Frozen
    public func iterator(): Iterator<T> { this }
}

class FilterIterator<T> <: Iterator<T> {
    private let predicate: (T) -> Bool
    private let it: Iterator<T>

    /**
     * @param it the source iterator to filter
     * @param predicate the predicate closure used to filter elements
     */
    init(it: Iterator<T>, predicate: (T) -> Bool) {
        this.it = it
        this.predicate = predicate
    }

    /**
     * Returns the next element that satisfies the predicate, or None if exhausted.
     *
     * @returns Option<T> the next element satisfying the predicate, or None
     */
    public func next(): Option<T> {
        while (let Some(value) <- it.next()) {
            if (predicate(value)) {
                return Some(value)
            }
        }
        return None
    }
}

class MapIterator<T, R> <: Iterator<R> {
    private let transform: (T) -> R
    private let it: Iterator<T>

    /**
     * @param it the source iterator to map over
     * @param transform the transformation closure applied to each element
     */
    @Frozen
    init(it: Iterator<T>, transform: (T) -> R) {
        this.it = it
        this.transform = transform
    }

    /**
     * Returns the next transformed element, or None if exhausted.
     *
     * @returns Option<R> the transformed next element, or None
     */
    @Frozen
    public func next(): Option<R> {
        return match (it.next()) {
            case Some(value) => Some(transform(value))
            case _ => None
        }
    }
}

class StepIterator<T> <: Iterator<T> {
    private let _count: Int64
    private let it: Iterator<T>
    private var index: Int64 = 0

    /**
     * @param it the source iterator to step over
     * @param count the number of elements to skip between yielded elements
     * @throws IllegalArgumentException if count is less than or equal to zero
     */
    init(it: Iterator<T>, count: Int64) {
        if (count <= 0) {
            throw IllegalArgumentException("Step count must be greater than 0!")
        }
        this.it = it
        this._count = count
        this.index = count
    }

    /**
     * Returns the next element after stepping, or None if exhausted.
     *
     * @returns Option<T> the next stepped element, or None
     */
    public func next(): Option<T> {
        while (let Some(v) <- it.next()) {
            if (index < _count) {
                index++
                continue
            }
            index = 1
            return Some(v)
        }
        return None
    }
}

class SkipIterator<T> <: Iterator<T> {
    private var _count: Int64
    private let it: Iterator<T>

    /**
     * @param it the source iterator to skip elements from
     * @param count the number of elements to skip
     * @throws IllegalArgumentException if count is less than zero
     */
    init(it: Iterator<T>, count: Int64) {
        if (count < 0) {
            throw IllegalArgumentException("Skip count cannot less than 0!")
        }
        this.it = it
        this._count = count
    }

    /**
     * Returns the next element after skipping the specified count, or None if exhausted.
     *
     * @returns Option<T> the next element after skipping, or None
     */
    public func next(): Option<T> {
        while (_count > 0) {
            match (it.next()) {
                case None => return None
                case _ => _count--
            }
        }
        return it.next()
    }
}

/**
 * An iterator that yields the first `count` elements from the source iterator.
 */
class TakeIterator<T> <: Iterator<T> {
    private var _count: Int64
    private let it: Iterator<T>

    /**
     * @param it the source iterator to take elements from
     * @param count the maximum number of elements to take
     * @throws IllegalArgumentException if count is less than zero
     */
    init(it: Iterator<T>, count: Int64) {
        if (count < 0) {
            throw IllegalArgumentException("Take count cannot less than 0!")
        }
        this.it = it
        this._count = count
    }

    /**
     * Returns the next element if the take limit has not been reached, otherwise returns None.
     *
     * @returns Option<T> the next element within the take limit, or None
     */
    public func next(): Option<T> {
        if (_count == 0) {
            return None
        }
        _count--
        return it.next()
    }
}

class ConcatIterator<T> <: Iterator<T> {
    private let it: Iterator<T>
    private let other: Iterator<T>

    private var itOver: Bool = false

    /**
     * @param it the first iterator to concatenate
     * @param other the second iterator to concatenate
     */
    init(it: Iterator<T>, other: Iterator<T>) {
        this.it = it
        this.other = other
    }

    /**
     * Returns the next element from the first iterator, or from the second if the first is exhausted.
     *
     * @returns Option<T> the next element from the concatenated iterators, or None
     */
    public func next(): Option<T> {
        if (itOver) {
            return other.next()
        }

        match (it.next()) {
            case Some(value) => return Some(value)
            case _ =>
                itOver = true
                return other.next()
        }
    }
}

class FlatMapIterator<T, R> <: Iterator<R> {
    private var subIt: Option<Iterator<R>> = None
    private let transform: (T) -> Iterable<R>
    private let it: Iterator<T>

    /**
     * @param it the source iterable whose elements are transformed to Iterable<R>
     * @param transform the transformation closure that maps each element to an Iterable<R>
     */
    init(it: Iterable<T>, transform: (T) -> Iterable<R>) {
        this.it = it.iterator()
        this.transform = transform
    }

    /**
     * Returns the next element from the flattened result of the transform, or None if exhausted.
     *
     * @returns Option<R> the next flattened element, or None
     */
    public func next(): Option<R> {
        while (true) {
            match (subIt) {
                case Some(result) => match (result.next()) {
                    case Some(v) => return Some(v)
                    case _ => ()
                }
                case _ => ()
            }

            match (it.next()) {
                case Some(sub) => subIt = Some(transform(sub).iterator())
                case _ => return None
            }
        }
        return None
    }
}

class ZipIterator<T, R> <: Iterator<(T, R)> {
    private let itt: Iterator<T>
    private let itr: Iterator<R>

    /**
     * @param itt the first iterator to zip
     * @param itr the second iterator to zip
     */
    init(itt: Iterator<T>, itr: Iterator<R>) {
        this.itt = itt
        this.itr = itr
    }

    /**
     * Returns the next pair of elements from both iterators, or None if either is exhausted.
     *
     * @returns Option<(T, R)> the next paired elements, or None
     */
    public func next(): Option<(T, R)> {
        match ((itt.next(), itr.next())) {
            case (Some(t), Some(r)) => return Some((t, r))
            case _ => return None
        }
    }
}

class EnumerateIterator<T> <: Iterator<(Int64, T)> {
    private var index: Int64 = -1
    private let it: Iterator<T>

    /**
     * @param it the source iterator to enumerate
     */
    init(it: Iterator<T>) {
        this.it = it
    }

    /**
     * Returns the next element paired with its index, or None if exhausted.
     *
     * @returns Option<(Int64, T)> the next element with its index, or None
     */
    public func next(): Option<(Int64, T)> {
        return match (it.next()) {
            case Some(value) =>
                index++
                Some((index, value))
            case _ => return None
        }
    }
}

class FilterMapIterator<T, R> <: Iterator<R> {
    private let transform: (T) -> Option<R>
    private let it: Iterator<T>

    /**
     * @param it the source iterator to filter and map
     * @param transform the transformation closure that returns Option<R> for each element
     */
    init(it: Iterator<T>, transform: (T) -> Option<R>) {
        this.it = it
        this.transform = transform
    }

    /**
     * Returns the next transformed element that is not None, or None if exhausted.
     *
     * @returns Option<R> the next non-None transformed element, or None
     */
    public func next(): Option<R> {
        while (let Some(t) <- it.next()) {
            match (transform(t)) {
                case Some(r) => return Some(r)
                case _ => continue
            }
        }
        return None
    }
}

class InspectIterator<T> <: Iterator<T> {
    private let action: (T) -> Unit
    private let it: Iterator<T>

    /**
     * @param it the source iterator to inspect
     * @param action the closure to execute on each element before yielding
     */
    init(it: Iterator<T>, action: (T) -> Unit) {
        this.it = it
        this.action = action
    }

    /**
     * Returns the next element after executing the action on it, or None if exhausted.
     *
     * @returns Option<T> the next element after inspection, or None
     */
    public func next(): Option<T> {
        match (it.next()) {
            case Some(value) =>
                action(value)
                return Some(value)
            case _ => ()
        }
        return None
    }
}

class IntersperseIterator<T> <: Iterator<T> {
    private let separator: T
    private let it: Iterator<T>
    private var item: ?T
    private var isInsert = false

    /**
     * @param it the source iterator to intersperse
     * @param separator the element to insert between each pair of elements
     */
    init(it: Iterator<T>, separator: T) {
        this.it = it
        this.separator = separator
        this.item = it.next()
    }

    /**
     * Returns the next element, interspersing the separator between elements.
     *
     * @returns Option<T> the next element or separator, or None if exhausted
     */
    public func next(): Option<T> {
        if (isInsert) {
            isInsert = false
            item = it.next()
            if (item.isNone()) {
                return None
            }
            return Some(separator)
        }
        isInsert = true
        return item
    }
}

extend<T> Iterator<T> where T <: Equatable<T> {
    /**
     * Checks if the iterator contains the specified element.
     *
     * @param element the element to search for
     * @returns Bool true if the element is found, false otherwise
     */
    @Frozen
    public func contains(element: T): Bool {
        for (item in this.iterator()) {
            if (item == element) {
                return true
            }
        }
        return false
    }
}

extend<T> Iterator<T> where T <: Comparable<T> {
    /**
     * Returns the maximum element in the iterator, or None if the iterator is empty.
     *
     * @returns Option<T> Some containing the maximum element, or None
     */
    public func max(): Option<T> {
        var result: Option<T> = None
        for (item in this.iterator()) {
            match (result) {
                case Some(value) =>
                    if (item > value) {
                        result = Some(item)
                    }
                case _ => result = Some(item)
            }
        }
        return result
    }

    /**
     * Returns the minimum element in the iterator, or None if the iterator is empty.
     *
     * @returns Option<T> Some containing the minimum element, or None
     */
    public func min(): Option<T> {
        var result: Option<T> = None
        for (item in this.iterator()) {
            match (result) {
                case Some(value) =>
                    if (item < value) {
                        result = Some(item)
                    }
                case _ => result = Some(item)
            }
        }
        return result
    }
}

extend<T> Iterator<T> {
    /**
     * Zips this iterator with another iterator, producing pairs of elements.
     *
     * @param it the other iterator to zip with
     * @returns Iterator<(T, R)> an iterator of paired elements
     */
    @Frozen
    public func zip<R>(it: Iterator<R>): Iterator<(T, R)> {
        return ZipIterator<T, R>(this, it)
    }

    /**
     * Returns an iterator that yields elements paired with their indices.
     *
     * @returns Iterator<(Int64, T)> an iterator of (index, element) pairs
     */
    @Frozen
    public func enumerate(): Iterator<(Int64, T)> {
        return EnumerateIterator<T>(this)
    }

    /**
     * Returns an iterator that only yields elements satisfying the predicate.
     *
     * @param predicate the predicate closure used to filter elements
     * @returns Iterator<T> a filtered iterator
     */
    @Frozen
    public func filter(predicate: (T) -> Bool): Iterator<T> {
        return FilterIterator<T>(this, predicate)
    }

    /**
     * Returns an iterator that applies the transform and filters out None results.
     *
     * @param transform the transformation closure that returns Option<R>
     * @returns Iterator<R> a filtered and mapped iterator
     */
    @Frozen
    public func filterMap<R>(transform: (T) -> Option<R>): Iterator<R> {
        return FilterMapIterator<T, R>(this, transform)
    }

    /**
     * Returns an iterator that applies the transform to each element.
     *
     * @param transform the transformation closure applied to each element
     * @returns Iterator<R> a mapped iterator
     */
    @Frozen
    public func map<R>(transform: (T) -> R): Iterator<R> {
        return MapIterator<T, R>(this, transform)
    }

    /**
     * Returns an iterator that yields every `count`-th element.
     *
     * @param count the step interval for selecting elements
     * @returns Iterator<T> a stepped iterator
     * @throws IllegalArgumentException if count is less than or equal to zero
     */
    @Frozen
    public func step(count: Int64): Iterator<T> {
        return StepIterator<T>(this, count)
    }

    /**
     * Returns an iterator that skips the first `count` elements.
     *
     * @param count the number of elements to skip
     * @returns Iterator<T> an iterator with the first `count` elements skipped
     * @throws IllegalArgumentException if count is less than zero
     */
    @Frozen
    public func skip(count: Int64): Iterator<T> {
        return SkipIterator<T>(this, count)
    }

    /**
     * Returns an iterator that yields at most `count` elements.
     *
     * @param count the maximum number of elements to yield
     * @returns Iterator<T> an iterator limited to `count` elements
     * @throws IllegalArgumentException if count is less than zero
     */
    @Frozen
    public func take(count: Int64): Iterator<T> {
        return TakeIterator<T>(this, count)
    }

    /**
     * Returns an iterator that concatenates this iterator with another.
     *
     * @param other the other iterator to concatenate
     * @returns Iterator<T> a concatenated iterator
     */
    @Frozen
    public func concat(other: Iterator<T>): Iterator<T> {
        return ConcatIterator<T>(this, other)
    }

    /**
     * Returns an iterator that executes the action on each element before yielding it.
     *
     * @param action the closure to execute on each element
     * @returns Iterator<T> an inspected iterator
     */
    @Frozen
    public func inspect(action: (T) -> Unit): Iterator<T> {
        return InspectIterator<T>(this, action)
    }

    /**
     * Returns an iterator that applies the transform to each element and flattens the results.
     *
     * @param transform the transformation closure that maps each element to an Iterator<R>
     * @returns Iterator<R> a flattened mapped iterator
     */
    @Frozen
    public func flatMap<R>(transform: (T) -> Iterator<R>): Iterator<R> {
        return FlatMapIterator<T, R>(this, transform)
    }

    /**
     * Returns an iterator that inserts the separator between each pair of elements.
     *
     * @param separator the element to insert between elements
     * @returns Iterator<T> an interspersed iterator
     */
    @Frozen
    public func intersperse(separator: T): Iterator<T> {
        return IntersperseIterator<T>(this, separator)
    }

    /**
     * Checks if all elements satisfy the predicate.
     *
     * @param predicate the predicate closure to test each element
     * @returns Bool true if all elements satisfy the predicate, false otherwise
     */
    @Frozen
    public func all(predicate: (T) -> Bool): Bool {
        for (item in this) {
            if (!predicate(item)) {
                return false
            }
        }
        return true
    }

    /**
     * Checks if any element satisfies the predicate.
     *
     * @param predicate the predicate closure to test each element
     * @returns Bool true if any element satisfies the predicate, false otherwise
     */
    @Frozen
    public func any(predicate: (T) -> Bool): Bool {
        for (item in this) {
            if (predicate(item)) {
                return true
            }
        }
        return false
    }

    /**
     * Checks if no element satisfies the predicate.
     *
     * @param predicate the predicate closure to test each element
     * @returns Bool true if no element satisfies the predicate, false otherwise
     */
    @Frozen
    public func none(predicate: (T) -> Bool): Bool {
        for (item in this) {
            if (predicate(item)) {
                return false
            }
        }
        return true
    }

    /**
     * Checks if the iterator is empty (has no elements).
     *
     * @returns Bool true if the iterator is empty, false otherwise
     */
    @Frozen
    public func isEmpty(): Bool {
        return match (this.next()) {
            case None => true
            case _ => false
        }
    }

    /**
     * Counts the number of elements in the iterator.
     *
     * @returns Int64 the number of elements
     */
    @Frozen
    public func count(): Int64 {
        var count: Int64 = 0
        for (_ in this) {
            count++
        }
        return count
    }

    /**
     * Executes the given action for each element.
     *
     * @param action the closure to execute on each element
     */
    @Frozen
    public func forEach(action: (T) -> Unit): Unit {
        for (item in this) {
            action(item)
        }
    }

    /**
     * Returns the first element, or None if the iterator is empty.
     *
     * @returns Option<T> Some containing the first element, or None
     */
    @Frozen
    public func first(): Option<T> {
        return this.next()
    }

    /**
     * Returns the last element, or None if the iterator is empty.
     *
     * @returns Option<T> Some containing the last element, or None
     */
    @Frozen
    public func last(): Option<T> {
        var result: Option<T> = None
        for (item in this) {
            result = Some(item)
        }
        return result
    }

    /**
     * Returns the element at the specified position, or None if the position is out of bounds.
     *
     * @param n the index of the element to return
     * @returns Option<T> Some containing the element at position n, or None
     */
    @Frozen
    public func at(n: Int64): Option<T> {
        if (n < 0) {
            return None
        }
        var i: Int64 = 0
        for (item in this) {
            if (i == n) {
                return Some(item)
            }
            i++
        }
        return None
    }

    /**
     * Reduces the elements to a single value using the specified operation,
     * starting with the first element as the initial value.
     *
     * @param operation the binary operation to apply cumulatively
     * @returns Option<T> Some containing the reduced value, or None if the iterator is empty
     */
    @Frozen
    public func reduce(operation: (T, T) -> T): Option<T> {
        var it = this.iterator()
        var result: T = it.next() ?? return None
        while (let Some(value) <- it.next()) {
            result = operation(result, value)
        }
        return result
    }

    /**
     * Accumulates the elements starting with the given initial value,
     * applying the operation to each element.
     *
     * @param initial the initial accumulator value
     * @param operation the binary operation to apply cumulatively
     * @returns R the final accumulated value
     */
    @Frozen
    public func fold<R>(initial: R, operation: (R, T) -> R): R {
        var result: R = initial
        for (item in this) {
            result = operation(result, item)
        }
        return result
    }
}
