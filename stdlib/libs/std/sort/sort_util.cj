/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.sort

import std.math.*

extend<T> Array<T> where T <: Comparable<T> {
    func compareSwap3(i1: Int64, i2: Int64, i3: Int64) {
        var a: T = this[i1]
        var b: T = this[i2]
        var c: T = this[i3]
        if (a > b) {
            var temp = a
            a = b
            b = temp
        }
        if (c < a) {
            this[i1] = c
            this[i2] = a
            this[i3] = b
        } else if (c < b) {
            this[i1] = a
            this[i2] = c
            this[i3] = b
        } else {
            this[i1] = a
            this[i2] = b
            this[i3] = c
        }
    }

    func insertionSort(begin: Int64, end: Int64): Unit {
        var i: Int64 = begin + 1
        while (i <= end) {
            var point = this[i]
            var j = i - 1
            while (j >= begin) {
                var temp = this[j]
                if (temp > point) {
                    this[j + 1] = temp
                } else {
                    break
                }
                j--
            }
            this[j + 1] = point
            i++
        }
    }

    // the value range of the parameter: 0 <= begin < end <= array.size - 1
    @OverflowWrapping
    func heapify(begin: Int64, end: Int64, root: Int64): Unit {
        var fi: Int64 = root
        var si = (fi << 1) + 1 // fi * 2 + 1
        while (begin + si <= end) {
            // si or (si + 1) is taken only once
            let s = if (begin + si + 1 <= end) {
                let s1 = this[begin + si]
                let s2 = this[begin + si + 1]
                if (s1 < s2) {
                    si++
                    s2
                } else {
                    s1
                }
            } else {
                this[begin + si]
            }
            let f = this[begin + fi]
            if (f < s) {
                this[begin + fi] = s
                this[begin + si] = f
            } else {
                return
            }
            fi = si
            si = (fi << 1) + 1
        }
    }

    // the value range of the parameter: 0 <= begin < end <= array.size - 1
    @OverflowWrapping
    func heapSort(begin: Int64, end: Int64): Unit {
        let len = end - begin + 1
        var i = (len >> 1) - 1

        // build max or min heap
        while (i >= 0) {
            this.heapify(begin, end, i)
            i--
        }
        i = end
        while (i > begin) {
            // swap top and bottom data
            this.swap(begin, i)
            // reset max or min heap
            this.heapify(begin, i - 1, 0)
            i--
        }
    }

    // the value range of the parameter: 0 <= begin < end <= array.size - 1
    // end - begin >= 1
    @OverflowWrapping
    func getSequenceLength(begin: Int64, end: Int64, temp: TempArray<T>): Int64 {
        var b: Int64 = begin
        var equalNum: Int64 = 1
        var isAscending: Bool = true

        // Obtains the length of a sequence with the same value.
        if (this[b] < this[b + 1]) {
            isAscending = true
        } else if (this[b] > this[b + 1]) {
            isAscending = false
        } else {
            equalNum++
            b++
            while (b + 1 <= end) {
                if (this[b] < this[b + 1]) {
                    isAscending = true
                    break
                } else if (this[b] > this[b + 1]) {
                    isAscending = false
                    break
                } else {
                    equalNum++
                    b++
                }
            }
        }

        if (b >= end) {
            return equalNum
        } else if (end - b == 1) {
            if (!isAscending) {
                let te = this[end]
                this.copyTo(this, begin, begin + 1, equalNum)
                this[begin] = te
            }
            return equalNum + 1
        }
        b++
        var n: Int64 = 1
        if (isAscending) {
            while (b + 1 <= end && this[b] <= this[b + 1]) {
                n++
                b++
            }
            return equalNum + n
        } else {
            while (b + 1 <= end && this[b] > this[b + 1]) {
                n++
                b++
            }

            // reverse sequence
            if (equalNum == 1) {
                var i = begin
                var j = begin + n // begin + equalNum + n - 1
                while (i < j) {
                    this.swap(i, j)
                    i++
                    j--
                }
                return equalNum + n
            } else {
                var i = begin + equalNum
                var j = i + n - 1
                while (i < j) {
                    this.swap(i, j)
                    i++
                    j--
                }
            }

            if (equalNum == 2) {
                let t1 = this[begin]
                let t2 = this[begin + 1]
                if (n == 1) {
                    this[begin] = this[begin + 2]
                    this[begin + 1] = t1
                    this[begin + 2] = t2
                } else if (n == 2) {
                    this[begin] = this[begin + 2]
                    this[begin + 1] = this[begin + 3]
                    this[begin + 2] = t1
                    this[begin + 3] = t2
                } else {
                    this.copyTo(this, begin + 2, begin, n)
                    this[begin + equalNum + n - 2] = t1
                    this[begin + equalNum + n - 1] = t2
                }
            } else {
                if (n == 1) {
                    let t = this[begin + equalNum]
                    this.copyTo(this, begin, begin + 1, equalNum)
                    this[begin] = t
                } else if (n == 2) {
                    let t1 = this[begin + equalNum]
                    let t2 = this[begin + equalNum + 1]
                    this.copyTo(this, begin, begin + 2, equalNum)
                    this[begin] = t1
                    this[begin + 1] = t2
                } else {
                    if (n < equalNum) {
                        let arr = temp.getArray(n)
                        this.copyTo(arr, begin + equalNum, 0, n)
                        this.copyTo(this, begin, begin + n, equalNum)
                        arr.copyTo(this, 0, begin, n)
                    } else {
                        let arr = temp.getArray(equalNum)
                        this.copyTo(arr, begin, 0, equalNum)
                        this.copyTo(this, begin + equalNum, begin, n)
                        arr.copyTo(this, 0, begin + n, equalNum)
                    }
                }
            }
            return equalNum + n
        }
    }

    @OverflowWrapping
    func binaryInsertionSort(begin: Int64, end: Int64, n: Int64) {
        var i = begin + n
        while (i <= end) {
            let point = this[i]
            let l = this.binaryQueryGT(point, begin, i - begin)
            let moveN = i - l
            if (moveN == 0) {
                i++
                continue
            }
            match (moveN) {
                case 1 => this[i] = this[i - 1]
                case 2 =>
                    this[i] = this[i - 1]
                    this[i - 1] = this[i - 2]
                case _ => this.copyTo(this, l, l + 1, moveN)
            }
            this[l] = point
            i++
        }
    }

    // The queried position is just greater than and equal the previous value.
    @OverflowWrapping
    func binaryQueryGT(point: T, start: Int64, len: Int64): Int64 {
        var l = start
        var r = start + len
        while (l < r) {
            let m = l + ((r - l) >> 1)
            if (point < this[m]) {
                r = m
            } else {
                l = m + 1
            }
        }
        return l
    }

    // The queried position is just less than and equal the next value.
    @OverflowWrapping
    func binaryQueryLT(point: T, start: Int64, len: Int64): Int64 {
        var l = start
        var r = start + len
        while (l < r) {
            let m = l + ((r - l) >> 1)
            if (point <= this[m]) {
                r = m
            } else {
                l = m + 1
            }
        }
        return l
    }

    // The queried position is just greater than and equal the previous value.
    @OverflowWrapping
    func queryGT(point: T, start: Int64, len: Int64, r: Bool) {
        var gallopMax = len
        if (r) {
            let end = start + len - 1
            if (len > 64) {
                let gallopMid = Int64(sqrt(Float64(len)))
                if (point < this[end - gallopMid]) {
                    return this.binaryQueryGT(point, start, len - gallopMid)
                } else {
                    gallopMax = gallopMid
                }
            }
            var ofs = 1
            var lastOfs = 0
            while (ofs < gallopMax) {
                if (point < this[end - ofs]) {
                    lastOfs = ofs
                    ofs = (ofs << 1) + 1
                } else {
                    break
                }
            }
            if (ofs > gallopMax) {
                ofs = gallopMax
            }
            return this.binaryQueryGT(point, start + len - ofs, ofs - lastOfs)
        } else {
            if (len > 64) {
                let gallopMid = Int64(sqrt(Float64(len)))
                if (point < this[start + gallopMid]) {
                    gallopMax = gallopMid
                } else {
                    return this.binaryQueryGT(point, start + gallopMid, len - gallopMid)
                }
            }
            var ofs = 1
            var lastOfs = 0
            while (ofs < gallopMax) {
                if (point < this[start + ofs]) {
                    break
                } else {
                    lastOfs = ofs
                    ofs = (ofs << 1) + 1
                }
            }
            if (ofs > gallopMax) {
                ofs = gallopMax
            }
            return this.binaryQueryGT(point, start + lastOfs, ofs - lastOfs)
        }
    }

    // The queried position is just less than and equal the next value.
    @OverflowWrapping
    func queryLT(point: T, start: Int64, len: Int64, r: Bool) {
        var gallopMax = len
        if (r) {
            let end = start + len - 1
            if (len > 64) {
                let gallopMid = Int64(sqrt(Float64(len)))
                if (point > this[end - gallopMid]) {
                    gallopMax = gallopMid
                } else {
                    return this.binaryQueryLT(point, start, len - gallopMid)
                }
            }
            var ofs = 1
            var lastOfs = 0
            while (ofs < gallopMax) {
                if (point > this[end - ofs]) {
                    break
                } else {
                    lastOfs = ofs
                    ofs = (ofs << 1) + 1
                }
            }
            if (ofs > gallopMax) {
                ofs = gallopMax
            }
            return this.binaryQueryLT(point, start + len - ofs, ofs - lastOfs)
        } else {
            if (len > 64) {
                let gallopMid = Int64(sqrt(Float64(len)))
                if (point > this[start + gallopMid]) {
                    return this.binaryQueryLT(point, start + gallopMid, len - gallopMid)
                } else {
                    gallopMax = gallopMid
                }
            }
            var ofs = 1
            var lastOfs = 0
            while (ofs < gallopMax) {
                if (point > this[start + ofs]) {
                    lastOfs = ofs
                    ofs = (ofs << 1) + 1
                } else {
                    break
                }
            }
            if (ofs > gallopMax) {
                ofs = gallopMax
            }
            return this.binaryQueryLT(point, start + lastOfs, ofs - lastOfs)
        }
    }
}

extend<T> Array<T> {
    func compareSwap2(i1: Int64, i2: Int64, order: (T, T) -> Ordering) {
        let firstItem: T = this[i1]
        let lo: T = this[i2]
        match (order(firstItem, lo)) {
            case GT =>
                this[i1] = lo
                this[i2] = firstItem
            case _ => ()
        }
    }

    func compareSwap3(i1: Int64, i2: Int64, i3: Int64, order: (T, T) -> Ordering) {
        var firstItem: T = this[i1]
        var lo: T = this[i2]
        var thirdItem: T = this[i3]
        if (order(firstItem, lo) == GT) {
            var workspace = firstItem
            firstItem = lo
            lo = workspace
        }
        if (order(thirdItem, firstItem) == LT) {
            this[i1] = thirdItem
            this[i2] = firstItem
            this[i3] = lo
        } else if (order(thirdItem, lo) == LT) {
            this[i1] = firstItem
            this[i2] = thirdItem
            this[i3] = lo
        } else {
            this[i1] = firstItem
            this[i2] = lo
            this[i3] = thirdItem
        }
    }

    @OverflowWrapping
    func getMinRunLength() {
        var runSize = this.size
        var upperBound: Int64 = 0
        while (runSize >= 64) {
            upperBound = upperBound | (runSize & 1)
            runSize = runSize >> 1
        }
        return runSize + upperBound
    }

    @OverflowWrapping
    func depth(): Int64 {
        var x: Int64 = this.size
        var res: Int64 = 0
        while (x >= 1) {
            res++
            x >>= 1
        }
        return res * 2
    }

    // the value range of the parameter: 0 <= begin < end <= array.size - 1
    @OverflowWrapping
    func insertionSort(from: Int64, to: Int64, order: (T, T) -> Ordering): Unit {
        var pos: Int64 = from + 1
        while (pos <= to) {
            var insertValue = this[pos]
            var cursor = pos - 1
            while (cursor >= from) {
                var workspace = this[cursor]
                match (order(workspace, insertValue)) {
                    case GT => this[cursor + 1] = workspace
                    case _ => break
                }
                cursor--
            }
            this[cursor + 1] = insertValue
            pos++
        }
    }

    // the value range of the parameter: 0 <= begin < end <= array.size - 1
    // end - begin >= 1
    @OverflowWrapping
    func getSequenceLength(from: Int64, to: Int64, workspace: TempArray<T>, order: (T, T) -> Ordering): Int64 {
        var lo: Int64 = from
        var sameCount: Int64 = 1
        var increasing: Bool = true

        match (order(this[lo], this[lo + 1])) {
            case LT => increasing = true
            case GT => increasing = false
            case EQ =>
                sameCount++
                lo++
                while (lo + 1 <= to) {
                    match (order(this[lo], this[lo + 1])) {
                        case LT =>
                            increasing = true
                            break
                        case GT =>
                            increasing = false
                            break
                        case EQ =>
                            sameCount++
                            lo++
                    }
                }
        }
        if (lo >= to) {
            return sameCount
        } else if (to - lo == 1) {
            if (increasing) {
                return sameCount + 1
            } else {
                let backValue = this[to]
                this.copyTo(this, from, from + 1, sameCount)
                this[from] = backValue
                return sameCount + 1
            }
        }
        lo++
        var runSize: Int64 = 1
        if (increasing) {
            while (lo + 1 <= to) {
                match (order(this[lo], this[lo + 1])) {
                    case GT => break
                    case _ =>
                        runSize++
                        lo++
                }
            }
            return sameCount + runSize
        } else {
            while (lo + 1 <= to) {
                match (order(this[lo], this[lo + 1])) {
                    case GT =>
                        runSize++
                        lo++
                    case _ => break
                }
            }

            if (sameCount == 1) {
                var pos = from
                var cursor = from + runSize // from + sameCount + runSize - 1
                while (pos < cursor) {
                    this.swap(pos, cursor)
                    pos++
                    cursor--
                }
                return sameCount + runSize
            } else {
                var pos = from + sameCount
                var cursor = pos + runSize - 1
                while (pos < cursor) {
                    this.swap(pos, cursor)
                    pos++
                    cursor--
                }
            }

            if (sameCount == 2) {
                let savedLeft = this[from]
                let savedRight = this[from + 1]
                if (runSize == 1) {
                    this[from] = this[from + 2]
                    this[from + 1] = savedLeft
                    this[from + 2] = savedRight
                } else if (runSize == 2) {
                    this[from] = this[from + 2]
                    this[from + 1] = this[from + 3]
                    this[from + 2] = savedLeft
                    this[from + 3] = savedRight
                } else {
                    this.copyTo(this, from + 2, from, runSize)
                    this[from + sameCount + runSize - 2] = savedLeft
                    this[from + sameCount + runSize - 1] = savedRight
                }
            } else {
                if (runSize == 1) {
                    let t = this[from + sameCount]
                    this.copyTo(this, from, from + 1, sameCount)
                    this[from] = t
                } else if (runSize == 2) {
                    let savedLeft = this[from + sameCount]
                    let savedRight = this[from + sameCount + 1]
                    this.copyTo(this, from, from + 2, sameCount)
                    this[from] = savedLeft
                    this[from + 1] = savedRight
                } else {
                    if (runSize < sameCount) {
                        let tmpArray = workspace.getArray(runSize)
                        this.copyTo(tmpArray, from + sameCount, 0, runSize)
                        this.copyTo(this, from, from + runSize, sameCount)
                        tmpArray.copyTo(this, 0, from, runSize)
                    } else {
                        let tmpArray = workspace.getArray(sameCount)
                        this.copyTo(tmpArray, from, 0, sameCount)
                        this.copyTo(this, from + sameCount, from, runSize)
                        tmpArray.copyTo(this, 0, from + runSize, sameCount)
                    }
                }
            }
            return sameCount + runSize
        }
    }

    // the original relative positions of equal elements are not guaranteed.
    // the value range of the parameter: 0 <= begin < end <= array.size - 1
    @OverflowWrapping
    func binaryInsertionSort(from: Int64, to: Int64, order: (T, T) -> Ordering, runSize: Int64) {
        var pos = from + runSize
        while (pos <= to) {
            let insertValue = this[pos]
            let lowerBound = this.binaryQueryGT(insertValue, from, pos - from, order)
            let shiftCount = pos - lowerBound
            if (shiftCount == 0) {
                pos++
                continue
            }
            match (shiftCount) {
                case 1 => this[pos] = this[pos - 1]
                case 2 =>
                    this[pos] = this[pos - 1]
                    this[pos - 1] = this[pos - 2]
                case _ => this.copyTo(this, lowerBound, lowerBound + 1, shiftCount)
            }
            this[lowerBound] = insertValue
            pos++
        }
    }

    // the value range of the parameter: 0 <= begin < end <= array.size - 1
    @OverflowWrapping
    func heapify(from: Int64, to: Int64, root: Int64, order: (T, T) -> Ordering): Unit {
        var rootIndex: Int64 = root
        var leafIndex = (rootIndex << 1) + 1 // rootIndex * 2 + 1
        while (from + leafIndex <= to) {
            let chosen = if (from + leafIndex + 1 <= to) {
                let s1 = this[from + leafIndex]
                let s2 = this[from + leafIndex + 1]
                match (order(s1, s2)) {
                    case LT =>
                        leafIndex++
                        s2
                    case _ => s1
                }
            } else {
                this[from + leafIndex]
            }
            let rootValue = this[from + rootIndex]
            match (order(rootValue, chosen)) {
                case LT =>
                    this[from + rootIndex] = chosen
                    this[from + leafIndex] = rootValue
                case _ => return
            }
            rootIndex = leafIndex
            leafIndex = (rootIndex << 1) + 1
        }
    }

    // the value range of the parameter: 0 <= begin < end <= array.size - 1
    @OverflowWrapping
    func heapSort(from: Int64, to: Int64, order: (T, T) -> Ordering): Unit {
        let width = to - from + 1
        var pos = (width >> 1) - 1

        while (pos >= 0) {
            this.heapify(from, to, pos, order)
            pos--
        }
        pos = to
        while (pos > from) {
            this.swap(from, pos)

            this.heapify(from, pos - 1, 0, order)
            pos--
        }
    }

    // The queried position is just greater than and equal the previous value.
    @OverflowWrapping
    func binaryQueryGT(insertValue: T, from: Int64, width: Int64, order: (T, T) -> Ordering): Int64 {
        var lowerBound = from
        var upperBound = from + width
        while (lowerBound < upperBound) {
            let midIndex = lowerBound + ((upperBound - lowerBound) >> 1)
            match (order(insertValue, this[midIndex])) {
                case LT => upperBound = midIndex
                case _ => lowerBound = midIndex + 1
            }
        }
        return lowerBound
    }

    // The queried position is just less than and equal the next value.
    @OverflowWrapping
    func binaryQueryLT(insertValue: T, from: Int64, width: Int64, order: (T, T) -> Ordering): Int64 {
        var lowerBound = from
        var upperBound = from + width
        while (lowerBound < upperBound) {
            let midIndex = lowerBound + ((upperBound - lowerBound) >> 1)
            match (order(insertValue, this[midIndex])) {
                case GT => lowerBound = midIndex + 1
                case _ => upperBound = midIndex
            }
        }
        return lowerBound
    }

    // The queried position is just greater than and equal the previous value.
    @OverflowWrapping
    func queryGT(insertValue: T, from: Int64, width: Int64, order: (T, T) -> Ordering, upperBound: Bool) {
        var searchLimit = width
        if (upperBound) {
            let to = from + width - 1
            if (width > 64) {
                let searchProbe = Int64(sqrt(Float64(width)))
                match (order(insertValue, this[to - searchProbe])) {
                    case LT => return this.binaryQueryGT(insertValue, from, width - searchProbe, order)
                    case _ => searchLimit = searchProbe
                }
            }
            var step = 1
            var prevOffset = 0
            while (step < searchLimit) {
                let t = this[to - step]
                match (order(insertValue, t)) {
                    case LT =>
                        prevOffset = step
                        step = (step << 1) + 1
                    case _ => break
                }
            }
            if (step > searchLimit) {
                step = searchLimit
            }
            return this.binaryQueryGT(insertValue, from + width - step, step - prevOffset, order)
        } else {
            if (width > 64) {
                let searchProbe = Int64(sqrt(Float64(width)))
                match (order(insertValue, this[from + searchProbe])) {
                    case LT => searchLimit = searchProbe
                    case _ => return this.binaryQueryGT(insertValue, from + searchProbe, width - searchProbe, order)
                }
            }
            var step = 1
            var prevOffset = 0
            while (step < searchLimit) {
                let t = this[from + step]
                match (order(insertValue, t)) {
                    case LT => break
                    case _ =>
                        prevOffset = step
                        step = (step << 1) + 1
                }
            }
            if (step > searchLimit) {
                step = searchLimit
            }
            return this.binaryQueryGT(insertValue, from + prevOffset, step - prevOffset, order)
        }
    }

    // The queried position is just less than and equal the next value.
    @OverflowWrapping
    func queryLT(insertValue: T, from: Int64, width: Int64, order: (T, T) -> Ordering, upperBound: Bool) {
        var searchLimit = width
        if (upperBound) {
            let to = from + width - 1
            if (width > 64) {
                let searchProbe = Int64(sqrt(Float64(width)))
                match (order(insertValue, this[to - searchProbe])) {
                    case GT => searchLimit = searchProbe
                    case _ => return this.binaryQueryLT(insertValue, from, width - searchProbe, order)
                }
            }
            var step = 1
            var prevOffset = 0
            while (step < searchLimit) {
                let t = this[to - step]
                match (order(insertValue, t)) {
                    case GT => break
                    case _ =>
                        prevOffset = step
                        step = (step << 1) + 1
                }
            }
            if (step > searchLimit) {
                step = searchLimit
            }
            return this.binaryQueryLT(insertValue, from + width - step, step - prevOffset, order)
        } else {
            if (width > 64) {
                let searchProbe = Int64(sqrt(Float64(width)))
                match (order(insertValue, this[from + searchProbe])) {
                    case GT => return this.binaryQueryLT(insertValue, from + searchProbe, width - searchProbe, order)
                    case _ => searchLimit = searchProbe
                }
            }
            var step = 1
            var prevOffset = 0
            while (step < searchLimit) {
                let t = this[from + step]
                match (order(insertValue, t)) {
                    case GT =>
                        prevOffset = step
                        step = (step << 1) + 1
                    case _ => break
                }
            }
            if (step > searchLimit) {
                step = searchLimit
            }
            return this.binaryQueryLT(insertValue, from + prevOffset, step - prevOffset, order)
        }
    }
}
