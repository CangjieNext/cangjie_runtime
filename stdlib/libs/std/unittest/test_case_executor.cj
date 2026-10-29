/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */
package std.unittest

import std.unittest.common.*
import std.unittest.mock.*
import std.collection.ArrayList
import std.random.Random

/**
 * None of these configuration parameters are intended as public API for now.
 */
extend Configuration {
    // enable the coverage-guided algorithm
    prop coverageGuided: Bool {
        get(){ get(KeyCoverageGuided.coverageGuided) ?? false }
    }
    // number of initial seeds generated without coverage information
    prop coverageGuidedInitialSeeds: Int64 {
        get(){ get(KeyCoverageGuidedInitialSeeds.coverageGuidedInitialSeeds) ?? 20 }
    }
    // maximum number of candidates in queue
    prop coverageGuidedMaxCandidates: Int64 {
        get(){ get(KeyCoverageGuidedMaxCandidates.coverageGuidedMaxCandidates) ?? 200 }
    }
    // score given to any new trace in queue
    prop coverageGuidedBaselineScore: Int64 {
        get(){ get(KeyCoverageGuidedBaselineScore.coverageGuidedBaselineScore) ?? 2000 }
    }
    // score given to any "good" new trace in queue
    prop coverageGuidedNewCoverageScore: Int64 {
        get(){ get(KeyCoverageGuidedNewCoverageScore.coverageGuidedNewCoverageScore) ?? 4000 }
    }
    // bonus to score given to traces that spawn new coverage through mutation
    prop coverageGuidedNewCoverageBonus: Int64 {
        get(){ get(KeyCoverageGuidedNewCoverageBonus.coverageGuidedNewCoverageBonus) ?? 500 }
    }
}

interface Executor {
    func execute(suiteInfo: TestSuiteReportInfo, caseResult: TestCaseResult, configuration: Configuration): Unit
}

class TestCaseExecutor<T> <: Executor {
    var generationStep = 0
    var randomSeed = None<Int64>
    var hasMoreParameters: Bool = true

    TestCaseExecutor(
        private let strategy: DataStrategyProcessor<T>,
        private let body: (T) -> Unit
    ) {}

    /*
     * start new strategy iteration or stop
     * @returns false to stop, true to continue
     */
    private func nextIterationOrBreak(
        currentResult: TestCaseResult,
        configuration: Configuration
    ): Bool {
        if (currentResult.hasFailures() || !hasMoreParameters) {
            return false
        }

        let exceptionHandler = { e: Exception =>
            currentResult.add(strategyErrorAsResult(e, argDescription([])))
            false
        }

        try {
            strategy.nextIteration(configuration)
            hasMoreParameters = strategy.isInfinite
        } catch (e: Exception) {
            return exceptionHandler(e)
        } catch (e: Error) {
            return exceptionHandler(ErrorWrapperException(e))
        }

        return true
    }

    // Classic binary max-heap: elements stored compactly in data[0..count).
    // O(log n) insert + O(log n) popMax via siftUp/siftDown.
    // Fixed capacity with replace-min semantics when full (preserves highest-score candidates).
    // Replaces the previous O(n log n) sortBy() approach and the buggy segment-tree slot-reuse design.
    private class MaxHeap<T> {
        let capacity: Int64
        var data: Array<Option<(T, Int64)>>
        var count: Int64 = 0

        // Clamp capacity to safe range to prevent overflow/OOM from misconfiguration.
        // Default 200 if <= 0; hard cap at 1,000,000 to avoid gigabyte-level preallocation.
        init(capacity: Int64) {
            let clamped = if (capacity <= 0) {
                200
            } else if (capacity > 1000000) {
                1000000
            } else {
                capacity
            }
            this.capacity = clamped
            data = Array<Option<(T, Int64)>>(clamped, { _ => None })
        }

        var isEmpty: Bool { count == 0 }
        var currentSize: Int64 { count }

        // Insert with replace-min policy when full.
        // If heap is full and new score exceeds current minimum, replace the minimum
        // (preserving the highest-priority candidates, matching old ArrayList "cull lowest" intent).
        // If heap is full and new score is not better than minimum, drop silently.
        func insert(element: T, score: Int64): Unit {
            if (count >= capacity) {
                // Find minimum-score element (O(n) but capacity is small, typically <= 256)
                var minIdx: Int64 = 0
                var minScore = data[0].getOrThrow()[1]
                var i: Int64 = 1
                while (i < count) {
                    let s = data[i].getOrThrow()[1]
                    if (s < minScore) {
                        minScore = s
                        minIdx = i
                    }
                    i = i + 1
                }
                if (score <= minScore) {
                    // New candidate not better than worst existing → drop it
                    return
                }
                // Replace minimum with new element, then sift up from minIdx
                data[minIdx] = Some((element, score))
                siftUp(minIdx)
            } else {
                // Normal insert at end, then sift up
                data[count] = Some((element, score))
                count = count + 1
                siftUp(count - 1)
            }
        }

        // Pop the maximum-score element (root).
        // Moves the last element to root and sifts it down to restore heap invariant.
        func popMax(): Option<(T, Int64)> {
            if (count == 0) { return None }
            let result = data[0].getOrThrow()
            count = count - 1
            if (count > 0) {
                // Move last element to root
                data[0] = data[count]
                data[count] = None
                siftDown(0)
            } else {
                data[0] = None
            }
            return Some(result)
        }

        // siftUp: bubble element at idx up to maintain max-heap invariant.
        private func siftUp(idx: Int64): Unit {
            var i = idx
            while (i > 0) {
                let parent = (i - 1) / 2
                let iScore = data[i].getOrThrow()[1]
                let pScore = data[parent].getOrThrow()[1]
                if (iScore > pScore) {
                    // Swap
                    let tmp = data[i]
                    data[i] = data[parent]
                    data[parent] = tmp
                    i = parent
                } else {
                    break
                }
            }
        }

        // siftDown: sink element at idx down to maintain max-heap invariant.
        private func siftDown(idx: Int64): Unit {
            var i = idx
            while (true) {
                let left = 2 * i + 1
                let right = 2 * i + 2
                var largest = i
                if (left < count) {
                    let lScore = data[left].getOrThrow()[1]
                    let curScore = data[largest].getOrThrow()[1]
                    if (lScore > curScore) { largest = left }
                }
                if (right < count) {
                    let rScore = data[right].getOrThrow()[1]
                    let curScore = data[largest].getOrThrow()[1]
                    if (rScore > curScore) { largest = right }
                }
                if (largest == i) { break }
                // Swap
                let tmp = data[i]
                data[i] = data[largest]
                data[largest] = tmp
                i = largest
            }
        }
    }

    private func putToQue(que: MaxHeap<CovRandomSource>,
        varg: Array<(element: CovRandomSource, score: Int64)>) {
        for (item in varg) {
            que.insert(element: item.element, score: item.score)
        }
    }

    private func popFromQue(que: MaxHeap<CovRandomSource>): (CovRandomSource, Int64) {
        match (que.popMax()) {
            case Some(entry) => entry
            case None => throw Exception("Internal error: popFromQue called on empty MaxHeap — heap invariant violated or concurrent modification")
        }
    }

    private func coverageGuidedLoop(
        tcase: TestCaseResult,
        progress: ProgressOutput,
        localConfiguration: Configuration,
        random: Random,
        que: MaxHeap<CovRandomSource>,
        actualBody: (T) -> Unit
    ): Unit {
        var cnt = 0
        progress.verbose("cnt = *, candidates = ${que.currentSize}, cov est = ${currentCoverageEst()}\r")
        while (!que.isEmpty) {
            let (cur, score) = popFromQue(que)
            let temp = random.nextUInt8(126) + 2
            let newS = cur.copy(temperature: temp, seed: random.nextUInt64())
            localConfiguration.random = newS
            executeFiniteSubstrategy(tcase, localConfiguration, actualBody)
            if (!nextIterationOrBreak(tcase, localConfiguration)) {
                break
            }

            if (CoverageInfo.newCoverageDiscovered()) {
                progress.verboseLine("new coverage on seed = ${newS.seed}, iter = ${cnt}, temp = ${temp}")
                putToQue(
                    que,
                    (newS.freeze(), localConfiguration.coverageGuidedNewCoverageScore),
                    (cur, score + localConfiguration.coverageGuidedNewCoverageBonus)
                )
            } else {
                putToQue(que, (cur, score - 1))
            }
            if (cnt % 500 == 0) {
                progress.verbose("cnt = ${cnt}, candidates = ${que.currentSize}, cov est = ${currentCoverageEst()}\r")
            }
            cnt++
        }
        progress.verboseLine("")
    }

    private func executeGuided(result: TestCaseResult, configuration: Configuration): Unit {
        let progress = ProgressOutput(configuration)
        unsafe { CJ_MICROFUZZ_RESET_ALL_COVERAGE_INFO() }
        progress.verboseLine("Starting guided execution....")
        let generationSteps = configuration.generationSteps
        let localConfiguration = configuration.clone()

        let random = match (localConfiguration.random) {
            case Some(randomSource: Random) =>
                randomSource
            case _ => Random()
        }

        let initialSeeds = Array(configuration.coverageGuidedInitialSeeds) { _: Int64 => random.nextUInt64() }

        let mutator = Mutator(random: random)

        func actualBody(v: T) {
            unsafe { CJ_MICROFUZZ_RESET_LOCAL_COVERAGE_INFO() }
            unsafe { CJ_MICROFUZZ_ENABLE_TRACE() }
            body(v)
            unsafe { CJ_MICROFUZZ_DISABLE_TRACE() }
        }

        // Classic binary max-heap replaces the previous ArrayList+sortBy() O(n log n) per put.
        // New implementation: O(log n) insert (siftUp) + O(log n) popMax (siftDown).
        // Fixed capacity with replace-min semantics preserves highest-score candidates.
        let que = MaxHeap<CovRandomSource>(capacity: configuration.coverageGuidedMaxCandidates)

        progress.verboseLine("Seeding initial generation")
        for (seed in initialSeeds) {
            let ss = CovRandomSource(seed: seed, mutator: mutator)
            localConfiguration.random = ss
            executeFiniteSubstrategy(result, localConfiguration, actualBody)
            if (!nextIterationOrBreak(result, localConfiguration)) {
                return
            }
            if (CoverageInfo.newCoverageDiscovered()) {
                que.insert(element: ss.freeze(), score: localConfiguration.coverageGuidedNewCoverageScore)
                progress.verboseLine("new coverage on seed = ${seed}")
            } else {
                que.insert(element: ss.freeze(), score: localConfiguration.coverageGuidedBaselineScore)
            }
        }

        coverageGuidedLoop(result, progress, localConfiguration, random, que, actualBody)
    }

    private func argDescription(param: Array<InputParameter>) {
        ArgumentDescription(formatArguments(param), generationStep - 1, strategy.currentReduction, randomSeed)
    }

    private func executeFiniteSubstrategy(
        result: TestCaseResult,
        configuration: Configuration,
        actualBody: (T) -> Unit
    ): Unit {
        let lastInput = Box(Array<InputParameter>())
        let executeBody = { arg: T, input: Array<InputParameter> =>
            let step = CaseStep(argDescription(input))
            lastInput.value = input
            Framework.runStepBody(step, Test(1), result.caseId.caseName) {
                actualBody(arg)
            }
        }

        while (generationStep < configuration.generationSteps || !strategy.isInfinite) {
            generationStep += 1
            let processorResult = strategy.processNextWith(configuration, executeBody)

            var initialFailure = match (processorResult) {
                case Success(stepResult) =>
                    result.addSuccessfullStep(stepResult)
                    continue
                case NoItems =>
                    generationStep -= 1
                    return
                case StrategyError(e) =>
                    result.add(strategyErrorAsResult(e, argDescription(lastInput.value)))
                    return
                case Failure(input, stepResult) =>
                    (input, stepResult)
            }

            let (input, failure) = match (strategy.shrink(configuration, executeBody)) {
                case None => initialFailure
                case Some(Failure(input, shrunk)) => (input, shrunk)
                case Some(StrategyError(e)) =>
                    result.add(strategyErrorAsResult(e, argDescription(lastInput.value)))
                    return
                case _ =>
                    throw Exception("Internal error. Shrinking procedure returned successful result.")
            }

            // It was decided that we should print full number of reduction steps
            // instead of reduction step number on which we found latest shrinked failure
            result.add(failure.withUpdatedStep(CaseStep(argDescription(lastInput.value))))

            if (strategy.isInfinite) {
                break
            }
        }

        hasMoreParameters = false
    }

    public func execute(_: TestSuiteReportInfo, tcase: TestCaseResult, configuration: Configuration): Unit {
        generationStep = 0
        // assume that we have data in the beginning
        hasMoreParameters = true
        randomSeed = configuration.get(KeyRandomSeed.randomSeed)

        if (!nextIterationOrBreak(tcase, configuration)) {
            return
        }

        if (configuration.coverageGuided && hasMoreParameters) {
            executeGuided(tcase, configuration)
        } else {
            do {
                executeFiniteSubstrategy(tcase, configuration, body)
            } while (nextIterationOrBreak(tcase, configuration))
        }

        // notify user that there were no steps executed
        if (generationStep == 0) {
            tcase.add(RunStepResult.simpleError(Skip, NoParametersProvidedResult()))
        }
    }

    static func formatArguments(inputs: Array<InputParameter>): ?PrettyText {
        if (inputs.isNoArg()) {
            return None
        }
        let sb = PrettyText()
        for (param in inputs) {
            sb.append(param.name)
            sb.append(" = ")
            sb.appendLine(shieldEscapeSeq(param.repr))
        }
        return sb
    }
}

extend RunStepResult {
    func withUpdatedStep(newKind: StepKind): RunStepResult {
        RunStepResult(checksPassed, startTimestamp, newKind, info, duration: duration)
    }
}

extend Exception {
    func asCheckResult(): ?CheckResult {
        match (this) {
            // relying that mock framework exception are the only ones that are pretty printable
            case mockException: PrettyException => MockFrameworkCheckResult(PrettyText.of(mockException))
            case e: AssertIntermediateException =>
                PowerAssertExceptionCheckResult(e.expression, e.getOriginalStackTrace(), e.diagram)
            case e: AssertException =>
                match (e.cause) {
                    // should have been already reported when thrown originally
                    case Some(_) => None
                    case None => ExceptionThrownCheckResult(
                        PrettyText(e.toString()), renderException(e, None)
                    )
                }
            // error will be reported by a watchdog thread
            case e: UnittestTimeoutException => None
            // should have been already reported when thrown originally
            case _: BenchmarkStoppedError => None
            case anyOther: Exception => ExceptionThrownCheckResult(
                PrettyText(anyOther.toString()), renderException(anyOther, None)
            )
        }
    }
}

func strategyErrorAsResult(e: Exception, descr: ArgumentDescription): RunStepResult {
    let existingChecks = Framework.collectUnattachedFailures()
    let errorCheckResults = e.asCheckResult() ??
        // this part can only happen if we caught exception caused by
        // @Fail / @Assert etc inside the strategy processor itself
        existingChecks.get(0) ??
        // this should never happen
        ExceptionThrownCheckResult(
        PrettyText("Unhandled exception inside strategy: ${e.toString()}"),
        renderException(e, None)
    )
    // losing information about duration here
    let kind = CaseStep(descr)
    RunStepResult.simpleError(kind, errorCheckResults)
}
