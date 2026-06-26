/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.sort

import std.math.*

/*
 * Stable ascending sort
 *
 * @param data Array to be sorted.
 *
 * @since 0.27.3
 */
@Deprecated[message: "Use global function `public func sort<T>(data: Array<T>, stable!: Bool = false, descending!: Bool = false): Unit where T <: Comparable<T>` instead."]
public func stableSort<T>(data: Array<T>): Unit where T <: Comparable<T> {
    if (data.size < 2) {
        return
    }
    data.timSort()
}

/*
 * Stable sort
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
public func stableSort<T>(data: Array<T>, comparator: (T, T) -> Ordering): Unit {
    if (data.size < 2) {
        return
    }
    data.timSort(comparator)
}

extend<T> Array<T> where T <: Comparable<T> {
    @OverflowWrapping
    private func shortSort(begin: Int64, end: Int64): Unit {
        let len = this.size
        if (len <= 8) {
            if (len <= 1) {
                return
            } else if (len == 2) {
                if (this[begin] > this[end]) {
                    var temp = this[begin]
                    this[begin] = this[end]
                    this[end] = temp
                }
            } else if (len == 3) {
                this.compareSwap3(begin, begin + 1, end)
            } else {
                insertionSort(begin, end)
            }
        } else {
            var temp = TempArray(len, this[begin])
            let n = getSequenceLength(begin, end, temp)
            if (n == len) {
                return
            }
            binaryInsertionSort(begin, end, n)
        }
    }

    @OverflowWrapping
    private func maybeMerge(
        startArr: Array<Int64>,
        lengthArr: Array<Int64>,
        endIndex: Int64,
        temp: TempArray<T>
    ): Int64 {
        var index: Int64 = endIndex
        while (index > 0) {
            var n: Int64 = index - 1
            let sn = startArr[n]
            let sn1 = startArr[n + 1]
            let ln = lengthArr[n]
            let ln1 = lengthArr[n + 1]
            if (n == 0) {
                if (ln <= ln1) {
                    this.merge(sn, ln, sn1, ln1, temp)
                    lengthArr[n] = ln + ln1
                    index--
                }
                break
            }
            let s = startArr[n - 1]
            let l = lengthArr[n - 1]
            if (l <= ln + ln1) {
                if (l < ln1) {
                    this.merge(s, l, sn, ln, temp)
                    recordMergedLeftRunAndKeepRightRun(startArr, lengthArr, n, sn1, ln1, l + ln)
                    index--
                } else {
                    this.merge(sn, ln, sn1, ln1, temp)
                    lengthArr[n] = ln + ln1
                    index--
                }
            } else if (ln <= ln1) {
                this.merge(sn, ln, sn1, ln1, temp)
                lengthArr[n] = ln + ln1
                index--
            } else {
                break
            }
        }
        return index
    }

    @OverflowWrapping
    private func mergeAll(
        startArr: Array<Int64>,
        lengthArr: Array<Int64>,
        endIndex: Int64,
        temp: TempArray<T>
    ): Unit {
        var index: Int64 = endIndex
        while (index > 0) {
            var n: Int64 = index - 1
            let sn = startArr[n]
            let sn1 = startArr[n + 1]
            let ln = lengthArr[n]
            let ln1 = lengthArr[n + 1]
            if (n == 0) {
                this.merge(sn, ln, sn1, ln1, temp)
                break
            }
            let s = startArr[n - 1]
            let l = lengthArr[n - 1]
            if (l < ln1) {
                this.merge(s, l, sn, ln, temp)
                recordMergedLeftRunAndKeepRightRun(startArr, lengthArr, n, sn1, ln1, l + ln)
                index--
            } else {
                this.merge(sn, ln, sn1, ln1, temp)
                lengthArr[n] = ln + ln1
                index--
            }
        }
    }

    @OverflowWrapping
    private func merge(
        start1: Int64,
        len1: Int64,
        start2: Int64,
        len2: Int64,
        temp: TempArray<T>
    ): Unit {
        var s1 = start1
        var l1 = len1
        var s2 = start2
        var l2 = len2
        var lt = this.queryLT(this[s1 + l1 - 1], s2, l2, true)
        l2 = lt - s2
        if (l2 == 0) {
            return
        }
        var gt = this.queryGT(this[s2], s1, l1, false)
        l1 = l1 - (gt - s1)
        s1 = gt
        if (l1 == 0) {
            return
        }
        if (l1 == 1) {
            let t0 = this[s1]
            this.copyTo(this, s2, s2 - 1, l2)
            this[s1 + l2] = t0
        } else if (l2 == 1) {
            let t0 = this[s2]
            this.copyTo(this, s1, s1 + 1, l1)
            this[s1] = t0
        } else if (l1 <= l2) {
            mergeFromLeft(s1, l1, s2, l2, temp)
        } else {
            mergeFromRight(s1, l1, s2, l2, temp)
        }
    }

    @OverflowWrapping
    private func mergeFromLeftOneByOne(arr: Array<T>, state: MergeFromLeftState): Unit {
        state.resetCounts()
        var t1 = arr[state.leftCursor]
        var t2 = this[state.rightCursor]
        while (true) {
            if (t1 > t2) {
                this[state.target] = t2
                state.takeRight()
                if (state.rightRest == 0) {
                    state.shouldStop = true
                    break
                }
                if (state.rightCount > 4) {
                    break
                }
                t2 = this[state.rightCursor]
            } else {
                this[state.target] = t1
                state.takeLeft()
                if (state.leftRest == 1) {
                    state.shouldStop = true
                    break
                }
                if (state.leftCount > 4) {
                    break
                }
                t1 = arr[state.leftCursor]
            }
        }
    }

    @OverflowWrapping
    private func mergeFromLeftByRange(arr: Array<T>, state: MergeFromLeftState): Unit {
        while (true) {
            var point = this[state.rightCursor]
            state.leftCount = arr.queryGT(point, state.leftCursor, state.leftRest, false) - state.leftCursor
            moveLeftBufferPrefix(this, arr, state, state.leftCount, point)
            state.target = state.target + state.leftCount + 1
            state.leftCursor += state.leftCount
            state.leftRest -= state.leftCount
            state.rightCursor++
            state.rightRest--
            if (state.leftRest == 1 || state.rightRest == 0) {
                state.shouldStop = true
                break
            }
            point = arr[state.leftCursor]
            state.rightCount = this.queryLT(point, state.rightCursor, state.rightRest, false) - state.rightCursor
            moveRightRunPrefix(this, state, state.rightCount, point)
            state.target = state.target + state.rightCount + 1
            state.rightCursor += state.rightCount
            state.rightRest -= state.rightCount
            state.leftCursor++
            state.leftRest--
            if (state.rightRest == 0 || state.leftRest == 1) {
                state.shouldStop = true
                break
            }
            if (state.leftCount < 4 || state.rightCount < 4) {
                break
            }
        }
    }

    @OverflowWrapping
    private func mergeFromLeft(
        start1: Int64,
        len1: Int64,
        start2: Int64,
        len2: Int64,
        temp: TempArray<T>
    ) {
        let arr = temp.getArray(len1)
        this.copyTo(arr, start1, 0, len1)
        let state = MergeFromLeftState(start1, 0, len1, start2, len2)
        this[state.target] = this[state.rightCursor]
        state.takeRight()
        while (true) {
            this.mergeFromLeftOneByOne(arr, state)
            if (state.shouldStop) {
                break
            }
            state.shouldStop = false
            this.mergeFromLeftByRange(arr, state)
            if (state.shouldStop) {
                break
            }
        }
        finishMergeFromLeft(this, arr, state)
    }

    @OverflowWrapping
    private func mergeFromRightOneByOne(rightBuffer: Array<T>, state: MergeFromRightState): Unit {
        state.resetCounts()
        var leftTailValue = this[state.leftTail]
        var rightTailValue = rightBuffer[state.rightTail]
        while (true) {
            if (leftTailValue > rightTailValue) {
                this[state.target] = leftTailValue
                state.takeLeftTail()
                if (state.leftRest == 0) {
                    state.shouldStop = true
                    break
                }
                if (state.leftBatch > 4) {
                    break
                }
                leftTailValue = this[state.leftTail]
            } else {
                this[state.target] = rightTailValue
                state.takeRightTail()
                if (state.rightRest == 1) {
                    state.shouldStop = true
                    break
                }
                if (state.rightBatch > 4) {
                    break
                }
                rightTailValue = rightBuffer[state.rightTail]
            }
        }
    }

    @OverflowWrapping
    private func mergeFromRightByRange(start1: Int64, rightBuffer: Array<T>, state: MergeFromRightState): Unit {
        while (true) {
            var tailProbe = this[state.leftTail]
            let ltIndex = rightBuffer.queryLT(tailProbe, 0, state.rightRest, true)
            state.rightBatch = state.rightRest - ltIndex
            moveRightBufferSuffix(this, rightBuffer, state, state.rightBatch, tailProbe)
            state.target = state.target - state.rightBatch - 1
            state.rightTail -= state.rightBatch
            state.rightRest -= state.rightBatch
            state.leftTail--
            state.leftRest--
            if (state.rightRest == 1 || state.leftRest == 0) {
                state.shouldStop = true
                break
            }
            tailProbe = rightBuffer[state.rightTail]
            let gtIndex = this.queryGT(tailProbe, start1, state.leftRest, true)
            state.leftBatch = state.leftTail - gtIndex + 1
            moveLeftRunSuffix(this, state, state.leftBatch, tailProbe)
            state.target = state.target - state.leftBatch - 1
            state.leftTail -= state.leftBatch
            state.leftRest -= state.leftBatch
            state.rightTail--
            state.rightRest--
            if (state.leftRest == 0 || state.rightRest == 1) {
                state.shouldStop = true
                break
            }
            if (state.leftBatch < 4 || state.rightBatch < 4) {
                break
            }
        }
    }

    @OverflowWrapping
    private func mergeFromRight(
        start1: Int64,
        len1: Int64,
        start2: Int64,
        len2: Int64,
        temp: TempArray<T>
    ) {
        let rightBuffer = temp.getArray(len2)
        this.copyTo(rightBuffer, start2, 0, len2)
        let state = MergeFromRightState(start2 + len2 - 1, start1 + len1 - 1, len1, len2 - 1, len2)
        this[state.target] = this[state.leftTail]
        state.takeLeftTail()
        while (true) {
            this.mergeFromRightOneByOne(rightBuffer, state)
            if (state.shouldStop) {
                break
            }
            state.shouldStop = false
            this.mergeFromRightByRange(start1, rightBuffer, state)
            if (state.shouldStop) {
                break
            }
        }
        finishMergeFromRight(this, start1, rightBuffer, state)
    }

    @OverflowWrapping
    func timSort(): Unit {
        if (this.size <= 64) {
            shortSort(0, this.size - 1)
            return
        }

        // init temp data
        let mrl = this.getMinRunLength()
        var temp = TempArray(this.size, this[0])
        let stackLen = Int64(log2(Float64(this.size) / Float64(mrl) + 2.0)) + 1
        let startArr = Array<Int64>(stackLen, repeat: 0)
        let lengthArr = Array<Int64>(stackLen, repeat: 0)
        var endIndex = -1

        var begin: Int64 = 0
        while (begin < this.size) {
            let length = this.size - begin
            var seqLen = length
            if (length > 1) {
                seqLen = getSequenceLength(begin, this.size - 1, temp)
                if (seqLen < mrl) {
                    if (length < mrl) {
                        binaryInsertionSort(begin, begin + length - 1, seqLen)
                        seqLen = length
                    } else {
                        binaryInsertionSort(begin, begin + mrl - 1, seqLen)
                        seqLen = mrl
                    }
                }
            }
            endIndex++
            startArr[endIndex] = begin
            lengthArr[endIndex] = seqLen
            begin = begin + seqLen
            endIndex = this.maybeMerge(startArr, lengthArr, endIndex, temp)
        }
        this.mergeAll(startArr, lengthArr, endIndex, temp)
    }
}

extend<T> Array<T> {
    // the value range of the parameter: 0 <= begin < end <= array.size - 1
    @OverflowWrapping
    private func shortSort(begin: Int64, end: Int64, compareBy: (T, T) -> Ordering): Unit {
        let len = this.size
        if (len <= 8) {
            if (len <= 1) {
                return
            } else if (len == 2) {
                this.compareSwap2(begin, end, compareBy)
            } else if (len == 3) {
                this.compareSwap3(begin, begin + 1, end, compareBy)
            } else {
                insertionSort(begin, end, compareBy)
            }
        } else {
            var cmpTemp = TempArray(len, this[begin])
            let cmpSlot = getSequenceLength(begin, end, cmpTemp, compareBy)
            if (cmpSlot == len) {
                return
            }
            binaryInsertionSort(begin, end, compareBy, cmpSlot)
        }
    }

    @OverflowWrapping
    private func maybeMergeOneRun(
        cmpRunStartArr: Array<Int64>,
        cmpRunLengthArr: Array<Int64>,
        cmpIndex: Int64,
        cmpTemp: TempArray<T>,
        compareBy: (T, T) -> Ordering
    ): (Int64, Bool) {
        let cmpSlot: Int64 = cmpIndex - 1
        let cmpCurrStart = cmpRunStartArr[cmpSlot]
        let cmpNextStart = cmpRunStartArr[cmpSlot + 1]
        let cmpCurrLen = cmpRunLengthArr[cmpSlot]
        let cmpNextLen = cmpRunLengthArr[cmpSlot + 1]
        if (cmpSlot == 0) {
            if (cmpCurrLen <= cmpNextLen) {
                this.merge(cmpCurrStart, cmpCurrLen, cmpNextStart, cmpNextLen, cmpTemp, compareBy)
                cmpRunLengthArr[cmpSlot] = cmpCurrLen + cmpNextLen
                return (cmpIndex - 1, true)
            }
            return (cmpIndex, true)
        }
        let cmpPrevStart = cmpRunStartArr[cmpSlot - 1]
        let cmpPrevLen = cmpRunLengthArr[cmpSlot - 1]
        if (cmpPrevLen <= cmpCurrLen + cmpNextLen) {
            if (cmpPrevLen < cmpNextLen) {
                this.merge(cmpPrevStart, cmpPrevLen, cmpCurrStart, cmpCurrLen, cmpTemp, compareBy)
                recordMergedLeftRunAndKeepRightRun(cmpRunStartArr, cmpRunLengthArr, cmpSlot, cmpNextStart, cmpNextLen,
                    cmpPrevLen + cmpCurrLen)
                return (cmpIndex - 1, false)
            }
            this.merge(cmpCurrStart, cmpCurrLen, cmpNextStart, cmpNextLen, cmpTemp, compareBy)
            cmpRunLengthArr[cmpSlot] = cmpCurrLen + cmpNextLen
            return (cmpIndex - 1, false)
        }
        if (cmpCurrLen <= cmpNextLen) {
            this.merge(cmpCurrStart, cmpCurrLen, cmpNextStart, cmpNextLen, cmpTemp, compareBy)
            cmpRunLengthArr[cmpSlot] = cmpCurrLen + cmpNextLen
            return (cmpIndex - 1, false)
        }
        return (cmpIndex, true)
    }

    @OverflowWrapping
    private func maybeMerge(
        cmpRunStartArr: Array<Int64>,
        cmpRunLengthArr: Array<Int64>,
        cmpStackTop: Int64,
        cmpTemp: TempArray<T>,
        compareBy: (T, T) -> Ordering
    ): Int64 {
        var cmpIndex: Int64 = cmpStackTop
        while (cmpIndex > 0) {
            let (newIndex, shouldBreak) = maybeMergeOneRun(cmpRunStartArr, cmpRunLengthArr, cmpIndex, cmpTemp, compareBy)
            cmpIndex = newIndex
            if (shouldBreak) {
                break
            }
        }
        return cmpIndex
    }

    @OverflowWrapping
    private func mergeAll(
        cmpRunStartArr: Array<Int64>,
        cmpRunLengthArr: Array<Int64>,
        cmpStackTop: Int64,
        cmpTemp: TempArray<T>,
        compareBy: (T, T) -> Ordering
    ): Unit {
        var cmpIndex: Int64 = cmpStackTop
        while (cmpIndex > 0) {
            var cmpSlot: Int64 = cmpIndex - 1
            let cmpCurrStart = cmpRunStartArr[cmpSlot]
            let cmpNextStart = cmpRunStartArr[cmpSlot + 1]
            let cmpCurrLen = cmpRunLengthArr[cmpSlot]
            let cmpNextLen = cmpRunLengthArr[cmpSlot + 1]
            if (cmpSlot == 0) {
                this.merge(cmpCurrStart, cmpCurrLen, cmpNextStart, cmpNextLen, cmpTemp, compareBy)
                break
            }
            let cmpPrevStart = cmpRunStartArr[cmpSlot - 1]
            let cmpPrevLen = cmpRunLengthArr[cmpSlot - 1]
            if (cmpPrevLen < cmpNextLen) {
                this.merge(cmpPrevStart, cmpPrevLen, cmpCurrStart, cmpCurrLen, cmpTemp, compareBy)
                recordMergedLeftRunAndKeepRightRun(
                    cmpRunStartArr,
                    cmpRunLengthArr,
                    cmpSlot,
                    cmpNextStart,
                    cmpNextLen,
                    cmpPrevLen + cmpCurrLen
                )
                cmpIndex--
            } else {
                this.merge(cmpCurrStart, cmpCurrLen, cmpNextStart, cmpNextLen, cmpTemp, compareBy)
                cmpRunLengthArr[cmpSlot] = cmpCurrLen + cmpNextLen
                cmpIndex--
            }
        }
    }

    @OverflowWrapping
    private func merge(
        start1: Int64,
        len1: Int64,
        start2: Int64,
        len2: Int64,
        cmpTemp: TempArray<T>,
        compareBy: (T, T) -> Ordering
    ): Unit {
        var leftCursor = start1
        var leftRemain = len1
        var rightCursor = start2
        var rightRemain = len2
        var lessThan = this.queryLT(this[leftCursor + leftRemain - 1], rightCursor, rightRemain, compareBy, true)
        rightRemain = lessThan - rightCursor
        if (rightRemain == 0) {
            return
        }
        var greaterThan = this.queryGT(this[rightCursor], leftCursor, leftRemain, compareBy, false)
        leftRemain = leftRemain - (greaterThan - leftCursor)
        leftCursor = greaterThan
        if (leftRemain == 0) {
            return
        }
        if (leftRemain == 1) {
            let savedValue = this[leftCursor]
            this.copyTo(this, rightCursor, rightCursor - 1, rightRemain)
            this[leftCursor + rightRemain] = savedValue
        } else if (rightRemain == 1) {
            let savedValue = this[rightCursor]
            this.copyTo(this, leftCursor, leftCursor + 1, leftRemain)
            this[leftCursor] = savedValue
        } else if (leftRemain <= rightRemain) {
            mergeFromLeft(leftCursor, leftRemain, rightCursor, rightRemain, cmpTemp, compareBy)
        } else {
            mergeFromRight(leftCursor, leftRemain, rightCursor, rightRemain, cmpTemp, compareBy)
        }
    }

    @OverflowWrapping
    private func mergeFromLeftOneByOne(
        arr: Array<T>,
        state: MergeFromLeftState,
        compareBy: (T, T) -> Ordering
    ): Unit {
        state.resetCounts()
        var leftValue = arr[state.leftCursor]
        var rightValue = this[state.rightCursor]
        while (true) {
            match (compareBy(leftValue, rightValue)) {
                case GT =>
                    this[state.target] = rightValue
                    state.takeRight()
                    if (state.rightRest == 0) {
                        state.shouldStop = true
                        break
                    }
                    if (state.rightCount > 4) {
                        break
                    }
                    rightValue = this[state.rightCursor]
                case _ =>
                    this[state.target] = leftValue
                    state.takeLeft()
                    if (state.leftRest == 1) {
                        state.shouldStop = true
                        break
                    }
                    if (state.leftCount > 4) {
                        break
                    }
                    leftValue = arr[state.leftCursor]
            }
        }
    }

    @OverflowWrapping
    private func mergeFromLeftByRange(
        arr: Array<T>,
        state: MergeFromLeftState,
        compareBy: (T, T) -> Ordering
    ): Unit {
        while (true) {
            var probe = this[state.rightCursor]
            state.leftCount = arr.queryGT(probe, state.leftCursor, state.leftRest, compareBy, false) - state.leftCursor
            moveLeftBufferPrefix(this, arr, state, state.leftCount, probe)
            state.target = state.target + state.leftCount + 1
            state.leftCursor += state.leftCount
            state.leftRest -= state.leftCount
            state.rightCursor++
            state.rightRest--
            if (state.leftRest == 1 || state.rightRest == 0) {
                state.shouldStop = true
                break
            }
            probe = arr[state.leftCursor]
            state.rightCount = this.queryLT(probe, state.rightCursor, state.rightRest, compareBy, false) - state
                .rightCursor
            moveRightRunPrefix(this, state, state.rightCount, probe)
            state.target = state.target + state.rightCount + 1
            state.rightCursor += state.rightCount
            state.rightRest -= state.rightCount
            state.leftCursor++
            state.leftRest--
            if (state.rightRest == 0 || state.leftRest == 1) {
                state.shouldStop = true
                break
            }
            if (state.leftCount < 4 || state.rightCount < 4) {
                break
            }
        }
    }

    @OverflowWrapping
    private func mergeFromRightOneByOne(
        rightBuffer: Array<T>,
        state: MergeFromRightState,
        compareBy: (T, T) -> Ordering
    ): Unit {
        state.resetCounts()
        var leftTailValue = this[state.leftTail]
        var rightTailValue = rightBuffer[state.rightTail]
        while (true) {
            match (compareBy(leftTailValue, rightTailValue)) {
                case GT =>
                    this[state.target] = leftTailValue
                    state.takeLeftTail()
                    if (state.leftRest == 0) {
                        state.shouldStop = true
                        break
                    }
                    if (state.leftBatch > 4) {
                        break
                    }
                    leftTailValue = this[state.leftTail]
                case _ =>
                    this[state.target] = rightTailValue
                    state.takeRightTail()
                    if (state.rightRest == 1) {
                        state.shouldStop = true
                        break
                    }
                    if (state.rightBatch > 4) {
                        break
                    }
                    rightTailValue = rightBuffer[state.rightTail]
            }
        }
    }

    @OverflowWrapping
    private func mergeFromRightByRange(
        start1: Int64,
        rightBuffer: Array<T>,
        state: MergeFromRightState,
        compareBy: (T, T) -> Ordering
    ): Unit {
        while (true) {
            var tailProbe = this[state.leftTail]
            let ltIndex = rightBuffer.queryLT(tailProbe, 0, state.rightRest, compareBy, true)
            state.rightBatch = state.rightRest - ltIndex
            moveRightBufferSuffix(this, rightBuffer, state, state.rightBatch, tailProbe)
            state.target = state.target - state.rightBatch - 1
            state.rightTail -= state.rightBatch
            state.rightRest -= state.rightBatch
            state.leftTail--
            state.leftRest--
            if (state.rightRest == 1 || state.leftRest == 0) {
                state.shouldStop = true
                break
            }
            tailProbe = rightBuffer[state.rightTail]
            let gtIndex = this.queryGT(tailProbe, start1, state.leftRest, compareBy, true)
            state.leftBatch = state.leftTail - gtIndex + 1
            moveLeftRunSuffix(this, state, state.leftBatch, tailProbe)
            state.target = state.target - state.leftBatch - 1
            state.leftTail -= state.leftBatch
            state.leftRest -= state.leftBatch
            state.rightTail--
            state.rightRest--
            if (state.leftRest == 0 || state.rightRest == 1) {
                state.shouldStop = true
                break
            }
            if (state.leftBatch < 4 || state.rightBatch < 4) {
                break
            }
        }
    }

    @OverflowWrapping
    private func mergeFromLeft(
        start1: Int64,
        len1: Int64,
        start2: Int64,
        len2: Int64,
        cmpTemp: TempArray<T>,
        compareBy: (T, T) -> Ordering
    ) {
        let cmpBuffer = cmpTemp.getArray(len1)
        this.copyTo(cmpBuffer, start1, 0, len1)
        let state = MergeFromLeftState(start1, 0, len1, start2, len2)
        this[state.target] = this[state.rightCursor]
        state.takeRight()
        while (true) {
            this.mergeFromLeftOneByOne(cmpBuffer, state, compareBy)
            if (state.shouldStop) {
                break
            }
            state.shouldStop = false
            this.mergeFromLeftByRange(cmpBuffer, state, compareBy)
            if (state.shouldStop) {
                break
            }
        }
        finishMergeFromLeft(this, cmpBuffer, state)
    }

    @OverflowWrapping
    private func mergeFromRight(
        start1: Int64,
        len1: Int64,
        start2: Int64,
        len2: Int64,
        cmpTemp: TempArray<T>,
        compareBy: (T, T) -> Ordering
    ) {
        let rightCmpBuffer = cmpTemp.getArray(len2)
        this.copyTo(rightCmpBuffer, start2, 0, len2)
        let state = MergeFromRightState(start2 + len2 - 1, start1 + len1 - 1, len1, len2 - 1, len2)
        this[state.target] = this[state.leftTail]
        state.takeLeftTail()
        while (true) {
            this.mergeFromRightOneByOne(rightCmpBuffer, state, compareBy)
            if (state.shouldStop) {
                break
            }
            state.shouldStop = false
            this.mergeFromRightByRange(start1, rightCmpBuffer, state, compareBy)
            if (state.shouldStop) {
                break
            }
        }
        finishMergeFromRight(this, start1, rightCmpBuffer, state)
    }

    @OverflowWrapping
    func timSort(compareBy: (T, T) -> Ordering): Unit {
        if (this.size <= 64) {
            shortSort(0, this.size - 1, compareBy)
            return
        }

        // init cmpTemp data
        let cmpMinRunLength = this.getMinRunLength()
        var cmpTemp = TempArray(this.size, this[0])
        let cmpRunStackLen = Int64(log2(Float64(this.size) / Float64(cmpMinRunLength) + 2.0)) + 1
        let cmpRunStartArr = Array<Int64>(cmpRunStackLen, repeat: 0)
        let cmpRunLengthArr = Array<Int64>(cmpRunStackLen, repeat: 0)
        var cmpStackTop = -1

        var begin: Int64 = 0
        while (begin < this.size) {
            let length = this.size - begin
            var seqLen = length
            if (length > 1) {
                seqLen = getSequenceLength(begin, this.size - 1, cmpTemp, compareBy)
                if (seqLen < cmpMinRunLength) {
                    if (length < cmpMinRunLength) {
                        binaryInsertionSort(begin, begin + length - 1, compareBy, seqLen)
                        seqLen = length
                    } else {
                        binaryInsertionSort(begin, begin + cmpMinRunLength - 1, compareBy, seqLen)
                        seqLen = cmpMinRunLength
                    }
                }
            }
            cmpStackTop++
            cmpRunStartArr[cmpStackTop] = begin
            cmpRunLengthArr[cmpStackTop] = seqLen
            begin = begin + seqLen
            cmpStackTop = this.maybeMerge(cmpRunStartArr, cmpRunLengthArr, cmpStackTop, cmpTemp, compareBy)
        }
        this.mergeAll(cmpRunStartArr, cmpRunLengthArr, cmpStackTop, cmpTemp, compareBy)
    }
}

class TempArray<T> {
    private var data: Array<T>
    private let zero: T
    private let max: Int64

    init(n: Int64, z: T) {
        this.data = Array<T>()
        this.max = n >> 1
        this.zero = z
    }

    @OverflowWrapping
    func getArray(n: Int64): Array<T> {
        if (data.size >= n) {
            return this.data
        }
        var newSize = data.size * 2
        if (newSize < n) {
            newSize = n
        }
        if (newSize > this.max) {
            newSize = this.max
        }
        this.data = Array<T>(newSize, repeat: this.zero)
        return this.data
    }
}

// MergeFromLeftState is scoped to a single mergeFromLeft call; not shared across
// threads or recursive calls. Its mutable fields replace former locals only to
// reduce stack traffic and are not observed outside that one invocation.
class MergeFromLeftState {
    var target: Int64
    var leftCursor: Int64
    var leftRest: Int64
    var rightCursor: Int64
    var rightRest: Int64
    var leftCount: Int64 = 0
    var rightCount: Int64 = 0
    var shouldStop: Bool = false

    init(target: Int64, leftCursor: Int64, leftRest: Int64, rightCursor: Int64, rightRest: Int64) {
        this.target = target
        this.leftCursor = leftCursor
        this.leftRest = leftRest
        this.rightCursor = rightCursor
        this.rightRest = rightRest
    }

    func resetCounts(): Unit {
        leftCount = 0
        rightCount = 0
        shouldStop = false
    }

    func takeLeft(): Unit {
        target++
        leftCursor++
        leftRest--
        leftCount++
        rightCount = 0
    }

    func takeRight(): Unit {
        target++
        rightCursor++
        rightRest--
        rightCount++
        leftCount = 0
    }
}

// MergeFromRightState is scoped to a single mergeFromRight call; not shared
// across threads or recursive calls. Its mutable fields replace former locals
// only to reduce stack traffic and are not observed outside that one invocation.
class MergeFromRightState {
    var target: Int64
    var leftTail: Int64
    var leftRest: Int64
    var rightTail: Int64
    var rightRest: Int64
    var leftBatch: Int64 = 0
    var rightBatch: Int64 = 0
    var shouldStop: Bool = false

    init(target: Int64, leftTail: Int64, leftRest: Int64, rightTail: Int64, rightRest: Int64) {
        this.target = target
        this.leftTail = leftTail
        this.leftRest = leftRest
        this.rightTail = rightTail
        this.rightRest = rightRest
    }

    func resetCounts(): Unit {
        leftBatch = 0
        rightBatch = 0
        shouldStop = false
    }

    func takeLeftTail(): Unit {
        target--
        leftTail--
        leftRest--
        leftBatch++
        rightBatch = 0
    }

    func takeRightTail(): Unit {
        target--
        rightTail--
        rightRest--
        rightBatch++
        leftBatch = 0
    }
}

// Package-level merge helpers shared by both Array<T> extend blocks.
// Extracted as free functions to avoid cross-extend-block private method visibility limits.

@OverflowWrapping
private func moveLeftBufferPrefix<T>(dest: Array<T>, arr: Array<T>, state: MergeFromLeftState, count: Int64, point: T): Unit {
    match (count) {
        case 0 => dest[state.target] = point
        case 1 =>
            dest[state.target] = arr[state.leftCursor]
            dest[state.target + 1] = point
        case 2 =>
            dest[state.target] = arr[state.leftCursor]
            dest[state.target + 1] = arr[state.leftCursor + 1]
            dest[state.target + 2] = point
        case _ =>
            arr.copyTo(dest, state.leftCursor, state.target, count)
            dest[state.target + count] = point
    }
}

@OverflowWrapping
private func moveRightRunPrefix<T>(dest: Array<T>, state: MergeFromLeftState, count: Int64, point: T): Unit {
    match (count) {
        case 0 => dest[state.target] = point
        case 1 =>
            dest[state.target] = dest[state.rightCursor]
            dest[state.target + 1] = point
        case 2 =>
            dest[state.target] = dest[state.rightCursor]
            dest[state.target + 1] = dest[state.rightCursor + 1]
            dest[state.target + 2] = point
        case _ =>
            dest.copyTo(dest, state.rightCursor, state.target, count)
            dest[state.target + count] = point
    }
}

@OverflowWrapping
private func finishMergeFromLeft<T>(dest: Array<T>, arr: Array<T>, state: MergeFromLeftState): Unit {
    if (state.rightRest == 0) {
        arr.copyTo(dest, state.leftCursor, state.target, state.leftRest)
    }
    if (state.leftRest == 1) {
        dest.copyTo(dest, state.rightCursor, state.target, state.rightRest)
        dest[state.target + state.rightRest] = arr[state.leftCursor]
    }
}

@OverflowWrapping
private func moveRightBufferSuffix<T>(dest: Array<T>, rightBuffer: Array<T>, state: MergeFromRightState, count: Int64, point: T): Unit {
    match (count) {
        case 0 => dest[state.target] = point
        case 1 =>
            dest[state.target] = rightBuffer[state.rightTail]
            dest[state.target - 1] = point
        case 2 =>
            dest[state.target] = rightBuffer[state.rightTail]
            dest[state.target - 1] = rightBuffer[state.rightTail - 1]
            dest[state.target - 2] = point
        case _ =>
            rightBuffer.copyTo(dest, state.rightTail - count + 1, state.target - count + 1, count)
            dest[state.target - count] = point
    }
}

@OverflowWrapping
private func moveLeftRunSuffix<T>(dest: Array<T>, state: MergeFromRightState, count: Int64, point: T): Unit {
    match (count) {
        case 0 => dest[state.target] = point
        case 1 =>
            dest[state.target] = dest[state.leftTail]
            dest[state.target - 1] = point
        case 2 =>
            dest[state.target] = dest[state.leftTail]
            dest[state.target - 1] = dest[state.leftTail - 1]
            dest[state.target - 2] = point
        case _ =>
            dest.copyTo(dest, state.leftTail - count + 1, state.target - count + 1, count)
            dest[state.target - count] = point
    }
}

@OverflowWrapping
private func finishMergeFromRight<T>(dest: Array<T>, start1: Int64, rightBuffer: Array<T>, state: MergeFromRightState): Unit {
    if (state.leftRest == 0) {
        rightBuffer.copyTo(dest, 0, state.target - state.rightRest + 1, state.rightRest)
    }
    if (state.rightRest == 1) {
        dest.copyTo(dest, start1, start1 + 1, state.leftRest)
        dest[start1] = rightBuffer[state.rightTail]
    }
}
