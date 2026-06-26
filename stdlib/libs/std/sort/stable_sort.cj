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
    private func mergeFromLeftOneByOne(
        arr: Array<T>,
        ti: Int64,
        s1: Int64,
        l1: Int64,
        s2: Int64,
        l2: Int64
    ): (Int64, Int64, Int64, Int64, Int64, Bool) {
        var vti = ti; var vs1 = s1; var vl1 = l1; var vs2 = s2; var vl2 = l2
        var c1 = 0
        var c2 = 0
        var t1 = arr[vs1]
        var t2 = this[vs2]
        while (true) {
            if (t1 > t2) {
                this[vti] = t2
                vti++
                vs2++
                vl2--
                c2++
                c1 = 0
                if (vl2 == 0) {
                    return (vti, vs1, vl1, vs2, vl2, true)
                }
                if (c2 > 4) {
                    break
                }
                t2 = this[vs2]
            } else {
                this[vti] = t1
                vti++
                vs1++
                vl1--
                c1++
                c2 = 0
                if (vl1 == 1) {
                    return (vti, vs1, vl1, vs2, vl2, true)
                }
                if (c1 > 4) {
                    break
                }
                t1 = arr[vs1]
            }
        }
        return (vti, vs1, vl1, vs2, vl2, false)
    }

    @OverflowWrapping
    private func mergeFromLeftByRange(
        arr: Array<T>,
        ti: Int64,
        s1: Int64,
        l1: Int64,
        s2: Int64,
        l2: Int64
    ): (Int64, Int64, Int64, Int64, Int64, Bool) {
        var vti = ti; var vs1 = s1; var vl1 = l1; var vs2 = s2; var vl2 = l2
        var c1 = 0
        var c2 = 0
        while (true) {
            var point = this[vs2]
            c1 = arr.queryGT(point, vs1, vl1, false) - vs1
            moveLeftBufferPrefix(this, arr, vti, vs1, c1, point)
            vti = vti + c1 + 1
            vs1 += c1
            vl1 -= c1
            vs2++
            vl2--
            if (vl1 == 1 || vl2 == 0) {
                return (vti, vs1, vl1, vs2, vl2, true)
            }
            point = arr[vs1]
            c2 = this.queryLT(point, vs2, vl2, false) - vs2
            moveRightRunPrefix(this, vti, vs2, c2, point)
            vti = vti + c2 + 1
            vs2 += c2
            vl2 -= c2
            vs1++
            vl1--
            if (vl2 == 0 || vl1 == 1) {
                return (vti, vs1, vl1, vs2, vl2, true)
            }
            if (c1 < 4 || c2 < 4) {
                break
            }
        }
        return (vti, vs1, vl1, vs2, vl2, false)
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
        var ti = start1
        var s1 = 0
        var l1 = len1
        var s2 = start2
        var l2 = len2
        this[ti] = this[s2]
        ti++
        s2++
        l2--
        while (true) {
            let (nTi, nS1, nL1, nS2, nL2, shouldStop) = mergeFromLeftOneByOne(arr, ti, s1, l1, s2, l2)
            ti = nTi
            s1 = nS1
            l1 = nL1
            s2 = nS2
            l2 = nL2
            if (shouldStop) {
                break
            }
            let (mTi, mS1, mL1, mS2, mL2, stop) = mergeFromLeftByRange(arr, ti, s1, l1, s2, l2)
            ti = mTi
            s1 = mS1
            l1 = mL1
            s2 = mS2
            l2 = mL2
            if (stop) {
                break
            }
        }
        finishMergeFromLeft(this, arr, ti, s1, l1, s2, l2)
    }

    @OverflowWrapping
    private func mergeFromRightOneByOne(
        rightBuffer: Array<T>,
        rightTarget: Int64,
        leftTail: Int64,
        leftRest: Int64,
        rightTail: Int64,
        rightRest: Int64
    ): (Int64, Int64, Int64, Int64, Int64, Bool) {
        var vrt = rightTarget; var vlt = leftTail; var vlr = leftRest; var vrt2 = rightTail; var vrr = rightRest
        var leftBatch = 0
        var rightBatch = 0
        var leftTailValue = this[vlt]
        var rightTailValue = rightBuffer[vrt2]
        while (true) {
            if (leftTailValue > rightTailValue) {
                this[vrt] = leftTailValue
                vrt--
                vlt--
                vlr--
                leftBatch++
                rightBatch = 0
                if (vlr == 0) {
                    return (vrt, vlt, vlr, vrt2, vrr, true)
                }
                if (leftBatch > 4) {
                    break
                }
                leftTailValue = this[vlt]
            } else {
                this[vrt] = rightTailValue
                vrt--
                vrt2--
                vrr--
                rightBatch++
                leftBatch = 0
                if (vrr == 1) {
                    return (vrt, vlt, vlr, vrt2, vrr, true)
                }
                if (rightBatch > 4) {
                    break
                }
                rightTailValue = rightBuffer[vrt2]
            }
        }
        return (vrt, vlt, vlr, vrt2, vrr, false)
    }

    @OverflowWrapping
    private func mergeFromRightByRange(
        start1: Int64,
        rightBuffer: Array<T>,
        rightTarget: Int64,
        leftTail: Int64,
        leftRest: Int64,
        rightTail: Int64,
        rightRest: Int64
    ): (Int64, Int64, Int64, Int64, Int64, Bool) {
        var vrt = rightTarget; var vlt = leftTail; var vlr = leftRest; var vrt2 = rightTail; var vrr = rightRest
        var leftBatch = 0
        var rightBatch = 0
        while (true) {
            var tailProbe = this[vlt]
            let ltIndex = rightBuffer.queryLT(tailProbe, 0, vrr, true)
            rightBatch = vrr - ltIndex
            moveRightBufferSuffix(this, rightBuffer, vrt, vrt2, rightBatch, tailProbe)
            vrt = vrt - rightBatch - 1
            vrt2 -= rightBatch
            vrr -= rightBatch
            vlt--
            vlr--
            if (vrr == 1 || vlr == 0) {
                return (vrt, vlt, vlr, vrt2, vrr, true)
            }
            tailProbe = rightBuffer[vrt2]
            let gtIndex = this.queryGT(tailProbe, start1, vlr, true)
            leftBatch = vlt - gtIndex + 1
            moveLeftRunSuffix(this, vrt, vlt, leftBatch, tailProbe)
            vrt = vrt - leftBatch - 1
            vlt -= leftBatch
            vlr -= leftBatch
            vrt2--
            vrr--
            if (vlr == 0 || vrr == 1) {
                return (vrt, vlt, vlr, vrt2, vrr, true)
            }
            if (leftBatch < 4 || rightBatch < 4) {
                break
            }
        }
        return (vrt, vlt, vlr, vrt2, vrr, false)
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
        var rightTarget = start2 + len2 - 1
        var leftTail = start1 + len1 - 1
        var leftRest = len1
        var rightTail = len2 - 1
        var rightRest = len2
        this[rightTarget] = this[leftTail]
        rightTarget--
        leftTail--
        leftRest--
        while (true) {
            let (nTgt, nLTail, nLRest, nRTail, nRRest, shouldStop) =
                mergeFromRightOneByOne(rightBuffer, rightTarget, leftTail, leftRest, rightTail, rightRest)
            rightTarget = nTgt
            leftTail = nLTail
            leftRest = nLRest
            rightTail = nRTail
            rightRest = nRRest
            if (shouldStop) {
                break
            }
            let (mTgt, mLTail, mLRest, mRTail, mRRest, stop) =
                mergeFromRightByRange(start1, rightBuffer, rightTarget, leftTail, leftRest, rightTail, rightRest)
            rightTarget = mTgt
            leftTail = mLTail
            leftRest = mLRest
            rightTail = mRTail
            rightRest = mRRest
            if (stop) {
                break
            }
        }
        finishMergeFromRight(this, start1, rightBuffer, rightTarget, leftRest, rightTail, rightRest)
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
        ti: Int64,
        s1: Int64,
        l1: Int64,
        s2: Int64,
        l2: Int64,
        compareBy: (T, T) -> Ordering
    ): (Int64, Int64, Int64, Int64, Int64, Bool) {
        var vti = ti; var vs1 = s1; var vl1 = l1; var vs2 = s2; var vl2 = l2
        var c1 = 0
        var c2 = 0
        var leftValue = arr[vs1]
        var rightValue = this[vs2]
        while (true) {
            match (compareBy(leftValue, rightValue)) {
                case GT =>
                    this[vti] = rightValue
                    vti++
                    vs2++
                    vl2--
                    c2++
                    c1 = 0
                    if (vl2 == 0) {
                        return (vti, vs1, vl1, vs2, vl2, true)
                    }
                    if (c2 > 4) {
                        break
                    }
                    rightValue = this[vs2]
                case _ =>
                    this[vti] = leftValue
                    vti++
                    vs1++
                    vl1--
                    c1++
                    c2 = 0
                    if (vl1 == 1) {
                        return (vti, vs1, vl1, vs2, vl2, true)
                    }
                    if (c1 > 4) {
                        break
                    }
                    leftValue = arr[vs1]
            }
        }
        return (vti, vs1, vl1, vs2, vl2, false)
    }

    @OverflowWrapping
    private func mergeFromLeftByRange(
        arr: Array<T>,
        ti: Int64,
        s1: Int64,
        l1: Int64,
        s2: Int64,
        l2: Int64,
        compareBy: (T, T) -> Ordering
    ): (Int64, Int64, Int64, Int64, Int64, Bool) {
        var vti = ti; var vs1 = s1; var vl1 = l1; var vs2 = s2; var vl2 = l2
        var c1 = 0
        var c2 = 0
        while (true) {
            var probe = this[vs2]
            c1 = arr.queryGT(probe, vs1, vl1, compareBy, false) - vs1
            moveLeftBufferPrefix(this, arr, vti, vs1, c1, probe)
            vti = vti + c1 + 1
            vs1 += c1
            vl1 -= c1
            vs2++
            vl2--
            if (vl1 == 1 || vl2 == 0) {
                return (vti, vs1, vl1, vs2, vl2, true)
            }
            probe = arr[vs1]
            c2 = this.queryLT(probe, vs2, vl2, compareBy, false) - vs2
            moveRightRunPrefix(this, vti, vs2, c2, probe)
            vti = vti + c2 + 1
            vs2 += c2
            vl2 -= c2
            vs1++
            vl1--
            if (vl2 == 0 || vl1 == 1) {
                return (vti, vs1, vl1, vs2, vl2, true)
            }
            if (c1 < 4 || c2 < 4) {
                break
            }
        }
        return (vti, vs1, vl1, vs2, vl2, false)
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
        var ti = start1
        var s1 = 0
        var l1 = len1
        var s2 = start2
        var l2 = len2
        this[ti] = this[s2]
        ti++
        s2++
        l2--
        while (true) {
            let (nTi, nS1, nL1, nS2, nL2, shouldStop) =
                mergeFromLeftOneByOne(cmpBuffer, ti, s1, l1, s2, l2, compareBy)
            ti = nTi
            s1 = nS1
            l1 = nL1
            s2 = nS2
            l2 = nL2
            if (shouldStop) {
                break
            }
            let (mTi, mS1, mL1, mS2, mL2, stop) =
                mergeFromLeftByRange(cmpBuffer, ti, s1, l1, s2, l2, compareBy)
            ti = mTi
            s1 = mS1
            l1 = mL1
            s2 = mS2
            l2 = mL2
            if (stop) {
                break
            }
        }
        finishMergeFromLeft(this, cmpBuffer, ti, s1, l1, s2, l2)
    }

    @OverflowWrapping
    private func mergeFromRightOneByOne(
        rightBuffer: Array<T>,
        rightTarget: Int64,
        leftTail: Int64,
        leftRest: Int64,
        rightTail: Int64,
        rightRest: Int64,
        compareBy: (T, T) -> Ordering
    ): (Int64, Int64, Int64, Int64, Int64, Bool) {
        var vrt = rightTarget; var vlt = leftTail; var vlr = leftRest; var vrt2 = rightTail; var vrr = rightRest
        var leftBatch = 0
        var rightBatch = 0
        var leftTailValue = this[vlt]
        var rightTailValue = rightBuffer[vrt2]
        while (true) {
            match (compareBy(leftTailValue, rightTailValue)) {
                case GT =>
                    this[vrt] = leftTailValue
                    vrt--
                    vlt--
                    vlr--
                    leftBatch++
                    rightBatch = 0
                    if (vlr == 0) {
                        return (vrt, vlt, vlr, vrt2, vrr, true)
                    }
                    if (leftBatch > 4) {
                        break
                    }
                    leftTailValue = this[vlt]
                case _ =>
                    this[vrt] = rightTailValue
                    vrt--
                    vrt2--
                    vrr--
                    rightBatch++
                    leftBatch = 0
                    if (vrr == 1) {
                        return (vrt, vlt, vlr, vrt2, vrr, true)
                    }
                    if (rightBatch > 4) {
                        break
                    }
                    rightTailValue = rightBuffer[vrt2]
            }
        }
        return (vrt, vlt, vlr, vrt2, vrr, false)
    }

    @OverflowWrapping
    private func mergeFromRightByRange(
        start1: Int64,
        rightBuffer: Array<T>,
        rightTarget: Int64,
        leftTail: Int64,
        leftRest: Int64,
        rightTail: Int64,
        rightRest: Int64,
        compareBy: (T, T) -> Ordering
    ): (Int64, Int64, Int64, Int64, Int64, Bool) {
        var vrt = rightTarget; var vlt = leftTail; var vlr = leftRest; var vrt2 = rightTail; var vrr = rightRest
        var leftBatch = 0
        var rightBatch = 0
        while (true) {
            var tailProbe = this[vlt]
            let ltIndex = rightBuffer.queryLT(tailProbe, 0, vrr, compareBy, true)
            rightBatch = vrr - ltIndex
            moveRightBufferSuffix(this, rightBuffer, vrt, vrt2, rightBatch, tailProbe)
            vrt = vrt - rightBatch - 1
            vrt2 -= rightBatch
            vrr -= rightBatch
            vlt--
            vlr--
            if (vrr == 1 || vlr == 0) {
                return (vrt, vlt, vlr, vrt2, vrr, true)
            }
            tailProbe = rightBuffer[vrt2]
            let gtIndex = this.queryGT(tailProbe, start1, vlr, compareBy, true)
            leftBatch = vlt - gtIndex + 1
            moveLeftRunSuffix(this, vrt, vlt, leftBatch, tailProbe)
            vrt = vrt - leftBatch - 1
            vlt -= leftBatch
            vlr -= leftBatch
            vrt2--
            vrr--
            if (vlr == 0 || vrr == 1) {
                return (vrt, vlt, vlr, vrt2, vrr, true)
            }
            if (leftBatch < 4 || rightBatch < 4) {
                break
            }
        }
        return (vrt, vlt, vlr, vrt2, vrr, false)
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
        var rightTarget = start2 + len2 - 1
        var leftTail = start1 + len1 - 1
        var leftRest = len1
        var rightTail = len2 - 1
        var rightRest = len2
        this[rightTarget] = this[leftTail]
        rightTarget--
        leftTail--
        leftRest--
        while (true) {
            let (nTgt, nLTail, nLRest, nRTail, nRRest, shouldStop) =
                mergeFromRightOneByOne(rightCmpBuffer, rightTarget, leftTail, leftRest, rightTail, rightRest, compareBy)
            rightTarget = nTgt
            leftTail = nLTail
            leftRest = nLRest
            rightTail = nRTail
            rightRest = nRRest
            if (shouldStop) {
                break
            }
            let (mTgt, mLTail, mLRest, mRTail, mRRest, stop) =
                mergeFromRightByRange(start1, rightCmpBuffer, rightTarget, leftTail, leftRest, rightTail, rightRest,
                    compareBy)
            rightTarget = mTgt
            leftTail = mLTail
            leftRest = mLRest
            rightTail = mRTail
            rightRest = mRRest
            if (stop) {
                break
            }
        }
        finishMergeFromRight(this, start1, rightCmpBuffer, rightTarget, leftRest, rightTail, rightRest)
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

// Package-level merge helpers shared by both Array<T> extend blocks.
// Extracted as free functions to avoid cross-extend-block private method visibility limits.

@OverflowWrapping
private func moveLeftBufferPrefix<T>(dest: Array<T>, arr: Array<T>, target: Int64, leftCursor: Int64, count: Int64,
    point: T): Unit {
    match (count) {
        case 0 => dest[target] = point
        case 1 =>
            dest[target] = arr[leftCursor]
            dest[target + 1] = point
        case 2 =>
            dest[target] = arr[leftCursor]
            dest[target + 1] = arr[leftCursor + 1]
            dest[target + 2] = point
        case _ =>
            arr.copyTo(dest, leftCursor, target, count)
            dest[target + count] = point
    }
}

@OverflowWrapping
private func moveRightRunPrefix<T>(dest: Array<T>, target: Int64, rightCursor: Int64, count: Int64, point: T): Unit {
    match (count) {
        case 0 => dest[target] = point
        case 1 =>
            dest[target] = dest[rightCursor]
            dest[target + 1] = point
        case 2 =>
            dest[target] = dest[rightCursor]
            dest[target + 1] = dest[rightCursor + 1]
            dest[target + 2] = point
        case _ =>
            dest.copyTo(dest, rightCursor, target, count)
            dest[target + count] = point
    }
}

@OverflowWrapping
private func finishMergeFromLeft<T>(dest: Array<T>, arr: Array<T>, target: Int64, leftCursor: Int64, leftRest: Int64,
    rightCursor: Int64, rightRest: Int64): Unit {
    if (rightRest == 0) {
        arr.copyTo(dest, leftCursor, target, leftRest)
    }
    if (leftRest == 1) {
        dest.copyTo(dest, rightCursor, target, rightRest)
        dest[target + rightRest] = arr[leftCursor]
    }
}

@OverflowWrapping
private func moveRightBufferSuffix<T>(dest: Array<T>, rightBuffer: Array<T>, target: Int64, rightTail: Int64,
    count: Int64, point: T): Unit {
    match (count) {
        case 0 => dest[target] = point
        case 1 =>
            dest[target] = rightBuffer[rightTail]
            dest[target - 1] = point
        case 2 =>
            dest[target] = rightBuffer[rightTail]
            dest[target - 1] = rightBuffer[rightTail - 1]
            dest[target - 2] = point
        case _ =>
            rightBuffer.copyTo(dest, rightTail - count + 1, target - count + 1, count)
            dest[target - count] = point
    }
}

@OverflowWrapping
private func moveLeftRunSuffix<T>(dest: Array<T>, target: Int64, leftTail: Int64, count: Int64, point: T): Unit {
    match (count) {
        case 0 => dest[target] = point
        case 1 =>
            dest[target] = dest[leftTail]
            dest[target - 1] = point
        case 2 =>
            dest[target] = dest[leftTail]
            dest[target - 1] = dest[leftTail - 1]
            dest[target - 2] = point
        case _ =>
            dest.copyTo(dest, leftTail - count + 1, target - count + 1, count)
            dest[target - count] = point
    }
}

@OverflowWrapping
private func finishMergeFromRight<T>(dest: Array<T>, start1: Int64, rightBuffer: Array<T>, target: Int64,
    leftRest: Int64, rightTail: Int64, rightRest: Int64): Unit {
    if (leftRest == 0) {
        rightBuffer.copyTo(dest, 0, target - rightRest + 1, rightRest)
    }
    if (rightRest == 1) {
        dest.copyTo(dest, start1, start1 + 1, leftRest)
        dest[start1] = rightBuffer[rightTail]
    }
}
