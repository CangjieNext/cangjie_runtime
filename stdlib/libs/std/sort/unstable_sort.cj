/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.sort

/**
 * Unstable ascending sort
 *
 * @param data Array to be sorted.
 *
 * @since 0.27.3
 */
@Deprecated[message: "Use global function `public func sort<T>(data: Array<T>, stable!: Bool = false, descending!: Bool = false): Unit where T <: Comparable<T>` instead."]
public func unstableSort<T>(data: Array<T>): Unit where T <: Comparable<T> {
    if (data.size < 2) {
        return
    }
    data.introSort(0, data.size - 1, data.depth())
}

/**
 * Unstable sort
 *
 * @param data Array to be sorted.
 * @param comparator The comparator must establish a total order:
 *        - Antisymmetry: if compare(a, b) is LT, then compare(b, a) must be GT
 *        - Transitivity: if compare(a, b) is LT and compare(b, c) is LT, then compare(a, c) must be LT
 *        - Consistency: multiple invocations of compare(a, b) must return the same result
 *        Violating these requirements may result in undefined behavior.
 *
 * @since 0.27.3
 */
@Deprecated[message: "Use global function `public func sort<T>(data: Array<T>, by!: (T, T) -> Ordering, stable!: Bool = false, descending!: Bool = false): Unit` instead."]
public func unstableSort<T>(data: Array<T>, comparator: (T, T) -> Ordering): Unit {
    if (data.size < 2) {
        return
    }
    data.introSort(0, data.size - 1, data.depth(), comparator)
}

extend<T> Array<T> where T <: Comparable<T> {
    @OverflowWrapping
    func introSort(begin: Int64, end: Int64, depth: Int64): Unit {
        var b: Int64 = begin
        var e: Int64 = end
        var d: Int64 = depth
        while (b < e) {
            let len = e - b + 1
            if (len <= 16) {
                if (len == 1) {
                    return
                } else if (len == 2) {
                    if (this[b] > this[e]) {
                        var temp = this[b]
                        this[b] = this[e]
                        this[e] = temp
                    }
                } else if (len == 3) {
                    this.compareSwap3(b, b + 1, e)
                } else {
                    this.insertionSort(b, e)
                }
                return
            }
            if (d == 0) {
                this.heapSort(b, e)
                return
            }
            d--

            let (lo, hi) = this.getPartition(b, e)

            // Let the smaller side do recursion
            // Let the bigger side do loop
            if (lo - b < e - hi) {
                this.introSort(b, lo, d)
                b = hi
            } else {
                this.introSort(hi, e, d)
                e = lo
            }
        }
    }

    @OverflowWrapping
    private func getPartition(begin: Int64, end: Int64): (Int64, Int64) {
        let len: Int64 = end - begin + 1
        let mid: Int64 = (len >> 1) + begin
        if (len >= 64) {
            let off: Int64 = len >> 2
            this.compareSwap3(begin, begin + off, mid)
            this.compareSwap3(mid, mid + off, end)
        }
        this.compareSwap3(begin, mid, end)
        let pivot: T = this[mid]

        // swap mid and end
        let temp = this[end]
        this[end] = pivot
        this[mid] = temp
        var pointNum = 0
        var b = begin
        var e = end - 1

        // query the first value equal to pivot
        var first: Int64 = b
        while (b < e) {
            let tb = this[b]
            if (tb < pivot) {
                b++
            } else if (tb == pivot) {
                pointNum++
                first = b
                b++
                break
            } else if (tb > pivot) {
                var continueBreak = false
                while (b < e) {
                    let te = this[e]
                    if (pivot < te) {
                        e--
                    } else if (pivot == te) {
                        pointNum++
                        this[b] = te
                        this[e] = tb
                        e--
                        first = b
                        b++
                        continueBreak = true
                        break
                    } else {
                        this[b] = te
                        this[e] = tb
                        b++
                        e--
                        break
                    }
                }
                if (continueBreak) {
                    break
                }
            }
        }

        // find partition pivot
        while (b < e) {
            var tb: T = pivot
            while (b < e) {
                tb = this[b]
                if (tb < pivot) {
                    b++
                } else if (tb == pivot) {
                    pointNum++
                    b++
                } else if (tb > pivot) {
                    break
                }
            }
            var te: T = pivot
            while (b < e) {
                te = this[e]
                if (te > pivot) {
                    e--
                } else if (te == pivot) {
                    pointNum++
                    break
                } else {
                    break
                }
            }
            if (b >= e) {
                break
            }
            this[b] = te
            this[e] = tb
            b++
            e--
        }

        if (this[b] <= pivot) {
            b++
        }
        this.swap(b, end)

        if (b - first >= pointNum * pointNum) {
            return (b - 1, b + 1)
        } else {
            // When the number of traversal times is less than the square of the number of pivot
            // merge a large number of values that are the same as pivot
            let hi = b
            b = first
            e = hi - 1
            while (b < e) {
                var te: T = pivot
                while (b < e) {
                    te = this[e]
                    if (te == pivot) {
                        e--
                    } else {
                        break
                    }
                }
                var tb: T = pivot
                while (b < e) {
                    tb = this[b]
                    if (tb != pivot) {
                        b++
                    } else {
                        break
                    }
                }
                if (b >= e) {
                    break
                }
                this[b] = te
                this[e] = tb
                b++
                e--
            }
            if (this[b] == pivot) {
                b--
            }
            return (b, hi + 1)
        }
    }
}

extend<T> Array<T> {
    // the element range of the parameter: 0 <= from < to <= array.size - 1
    // to - from >= 1
    @OverflowWrapping
    private func getPartition(from: Int64, to: Int64, order: (T, T) -> Ordering): (Int64, Int64) {
        let width: Int64 = to - from + 1
        let center: Int64 = (width >> 1) + from
        if (width >= 64) {
            let stride: Int64 = width >> 2
            this.compareSwap3(from, from + stride, center, order)
            this.compareSwap3(center, center + stride, to, order)
        }
        this.compareSwap3(from, center, to, order)
        let selectedPivot: T = this[center]

        // swap center and to
        let workspace = this[to]
        this[to] = selectedPivot
        this[center] = workspace
        var samePivotCount = 0
        var lo = from
        var hi = to - 1

        // query the firstSame element equal to selectedPivot
        var firstSame: Int64 = lo
        while (lo < hi) {
            let frontValue = this[lo]
            match (order(frontValue, selectedPivot)) {
                case LT => lo++
                case EQ =>
                    samePivotCount++
                    firstSame = lo
                    lo++
                    break
                case GT =>
                    var stopPartition = false
                    while (lo < hi) {
                        let backValue = this[hi]
                        match (order(selectedPivot, backValue)) {
                            case LT => hi--
                            case EQ =>
                                samePivotCount++
                                this[lo] = backValue
                                this[hi] = frontValue
                                hi--
                                firstSame = lo
                                lo++
                                stopPartition = true
                                break
                            case GT =>
                                this[lo] = backValue
                                this[hi] = frontValue
                                lo++
                                hi--
                                break
                        }
                    }
                    if (stopPartition) {
                        break
                    }
            }
        }

        // find partition selectedPivot
        while (lo < hi) {
            var frontValue: T = selectedPivot
            while (lo < hi) {
                frontValue = this[lo]
                match (order(frontValue, selectedPivot)) {
                    case LT => lo++
                    case EQ =>
                        samePivotCount++
                        lo++
                    case GT => break
                }
            }
            var backValue: T = selectedPivot
            while (lo < hi) {
                backValue = this[hi]
                match (order(selectedPivot, backValue)) {
                    case LT => hi--
                    case EQ =>
                        samePivotCount++
                        break
                    case GT => break
                }
            }
            if (lo >= hi) {
                break
            }
            this[lo] = backValue
            this[hi] = frontValue
            lo++
            hi--
        }

        match (order(this[lo], selectedPivot)) {
            case GT => ()
            case _ => lo++
        }
        this.swap(lo, to)
        if (lo - firstSame >= samePivotCount * samePivotCount) {
            return (lo - 1, lo + 1)
        } else {
            // When the number of traversal times is less than the square of the number of selectedPivot
            // merge firstItem large number of values that are the same as selectedPivot
            let highPart = lo
            lo = firstSame
            hi = highPart - 1
            while (lo < hi) {
                var backValue: T = selectedPivot
                while (lo < hi) {
                    backValue = this[hi]
                    match (order(selectedPivot, backValue)) {
                        case EQ => hi--
                        case _ => break
                    }
                }
                var frontValue: T = selectedPivot
                while (lo < hi) {
                    frontValue = this[lo]
                    match (order(frontValue, selectedPivot)) {
                        case EQ => break
                        case _ => lo++
                    }
                }
                if (lo >= hi) {
                    break
                }
                this[lo] = backValue
                this[hi] = frontValue
                lo++
                hi--
            }
            match (order(this[lo], selectedPivot)) {
                case EQ => lo--
                case _ => ()
            }
            return (lo, highPart + 1)
        }
    }

    // the element range of the parameter: 0 <= from < to <= array.size - 1
    @OverflowWrapping
    func introSort(from: Int64, to: Int64, depth: Int64, order: (T, T) -> Ordering): Unit {
        var lo: Int64 = from
        var hi: Int64 = to
        var depthLeft: Int64 = depth
        while (lo < hi) {
            let width = hi - lo + 1
            if (width <= 16) {
                if (width == 1) {
                    return
                } else if (width == 2) {
                    this.compareSwap2(lo, hi, order)
                } else if (width == 3) {
                    this.compareSwap3(lo, lo + 1, hi, order)
                } else {
                    this.insertionSort(lo, hi, order)
                }
                return
            }
            if (depthLeft == 0) {
                this.heapSort(lo, hi, order)
                return
            }
            depthLeft--
            let (lowPart, highPart) = this.getPartition(lo, hi, order)

            // Let the smaller side do recursion
            // Let the bigger side do loop
            if (lowPart - lo < hi - highPart) {
                this.introSort(lo, lowPart, depthLeft, order)
                lo = highPart
            } else {
                this.introSort(highPart, hi, depthLeft, order)
                hi = lowPart
            }
        }
    }
}
