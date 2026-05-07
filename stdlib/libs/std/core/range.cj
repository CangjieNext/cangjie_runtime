/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * Represents a range of values with a step size.
 */
@ConstSafe
public struct Range<T> <: Iterable<T> where T <: Countable<T> & Comparable<T> & Equatable<T> {
    /**
     * The start value of the range.
     *
     * @type T
     */
    public let start: T

    /**
     * The end value of the range.
     *
     * @type T
     */
    public let end: T

    /**
     * The step size of the range.
     *
     * @type Int64
     */
    public let step: Int64

    /**
     * Whether the range has a start value.
     *
     * @type Bool
     */
    public let hasStart: Bool

    /**
     * Whether the range has an end value.
     *
     * @type Bool
     */
    public let hasEnd: Bool

    /**
     * Whether the range end is inclusive (closed).
     *
     * @type Bool
     */
    public let isClosed: Bool

    /**
     * Constructs a Range with the specified start, end, step, and boundary flags.
     *
     * @param start the start value of the range
     * @param end the end value of the range
     * @param step the step size between consecutive values
     * @param hasStart whether the range has a start bound
     * @param hasEnd whether the range has an end bound
     * @param isClosed whether the end bound is inclusive
     * @throws IllegalArgumentException if the value of the step is equal to zero
     */
    @Frozen
    public const init(start: T, end: T, step: Int64, hasStart: Bool, hasEnd: Bool, isClosed: Bool) {
        if (step == 0) {
            throw IllegalArgumentException("The value of the step should not be zero.")
        }
        this.start = start
        this.end = end
        this.step = step
        this.hasStart = hasStart
        this.hasEnd = hasEnd
        this.isClosed = isClosed
    }

    /**
     * Returns an iterator over the values in this range.
     *
     * @returns Iterator<T> an iterator over the range values
     */
    @Frozen
    public func iterator(): Iterator<T> {
        return RangeIterator<T>(start, last(), step, isEmpty())
    }

    /**
     * Checks if this range is empty (contains no values).
     *
     * @returns Bool true if the range is empty, false otherwise
     */
    public const func isEmpty(): Bool {
        if (isClosed) {
            if (step > 0) {
                return start > end
            }
            return start < end
        } else {
            if (step > 0) {
                return start >= end
            }
            return start <= end
        }
    }

    /**
     * Calculates the last value contained in this range.
     * This function has a sensible value only for non-empty ranges.
     */
    @OverflowWrapping
    private const func last() {
        if (step != 1 && step != -1) {
            let (lower, upper, absStep) = if (step > 0) {
                (start, end, step)
            } else {
                (end, start, -step)
            }
            let gap = UInt64(upper.position() - lower.position())
            let rem = Int64(gap % UInt64(absStep))
            if (rem != 0) {
                if (step > 0) {
                    return end.next(-rem)
                } else {
                    return end.next(rem)
                }
            }
        }
        if (isClosed) {
            end
        } else {
            end.next(-step)
        }
    }
}

/**
 * An iterator over the values in a Range.
 */
public class RangeIterator<T> <: Iterator<T> where T <: Countable<T> & Comparable<T> & Equatable<T> {
    private let _last: T
    private let _step: Int64

    private var cur: T
    private var hasNext: Bool

    init(first: T, last: T, step: Int64, isEmpty: Bool) {
        this._last = last
        this._step = step

        this.cur = first
        this.hasNext = !isEmpty
    }

    /**
     * Returns the next value in the range, or None if the iteration is exhausted.
     *
     * @returns Option<T> Some containing the next value, or None
     */
    @Frozen
    public func next(): Option<T> {
        var res = cur
        if (hasNext) {
            hasNext = res != _last
            cur = cur.next(_step)
            return Some<T>(res)
        }
        return None<T>
    }
}
