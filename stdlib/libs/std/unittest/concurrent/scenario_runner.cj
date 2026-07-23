/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.sync.*
import std.sync.AtomicInt64
import std.time.DateTime
import std.collection.ArrayList
import std.collection.concurrent.ArrayBlockingQueue
import std.random.Random
import std.runtime.blackBox
import std.runtime.getProcessorCount

protected class ScenarioRunner<S> where S <: Any {
    private let backoffGenerator: BackoffGenerator

    protected ScenarioRunner(private let scenario: ConcurrentScenario<S>) {
        backoffGenerator = BackoffGenerator(random: Random())
    }

    protected init(scenario: ConcurrentScenario<S>, backoffSeed: Int64) {
        this.scenario = scenario
        backoffGenerator = BackoffGenerator(random: Random(UInt64(backoffSeed)))
    }

    protected func executeIterationsWithCallback(
        iterations: Int64,
        verbose!: Bool = false,
        callback!: (ScenarioIterationResult, Int64) -> Bool,
    ): Unit {
        // Iteration 0 always runs first, sequentially, to calibrate the backoff
        // sigma from the observed actor duration. This preserves the existing
        // calibration step and guarantees iteration 0 is the first result
        // delivered to the callback (existing tests rely on this).
        let firstResult = runIteration(0, backoffGenerator, verbose: verbose)
        updateBackoffSigma(firstResult.avgDuration.toNanoseconds())

        if (!callback(firstResult, 0)) {
            return
        }

        if (iterations <= 1) {
            return
        }

        if (scenario.parallelIterations) {
            executeIterationsParallel(iterations, verbose: verbose, callback: callback)
        } else {
            for (iter in 1..iterations) {
                let iterResult = runIteration(iter, backoffGenerator, verbose: verbose)
                if (!callback(iterResult, iter)) {
                    return
                }
            }
        }
    }

    // Parallel dispatch of iterations 1..iterations-1.
    //
    // Strategy (ArrayBlockingQueue + spawn-on-completion): the amount of work
    // is fixed upfront (iterations-1 tasks), so instead of a persistent worker
    // pool we keep at most poolSize concurrent in-flight iterations. Each time
    // a completed result is received from the queue, a replacement iteration
    // is spawned (if any remain and we have not been cancelled). Results are
    // delivered to the callback in completion order (out-of-order relative to
    // the iteration index); the callback is only ever invoked from this single
    // drain thread, so callers do not need to make their callback state
    // thread-safe.
    //
    // Cancellation (callback returned false): once the callback signals
    // false, no further iterations are dispatched and the callback is not
    // invoked again; in-flight iterations are allowed to run to completion
    // (their results are discarded) so that workers do not block forever on
    // resultQueue.add.
    private func executeIterationsParallel(
        iterations: Int64,
        verbose!: Bool = false,
        callback!: (ScenarioIterationResult, Int64) -> Bool,
    ): Unit {
        let totalParallel = iterations - 1
        let procCount = getProcessorCount()
        // Pool size is derived from the per-iteration actor count so that the
        // total concurrent actor threads (poolSize * threadCount) is roughly
        // equal to the processor count.
        //
        // ROOT CAUSE NOTE: Cangjie's spawn places new cjthreads on the
        // spawning thread's processor's LOCAL queue (not the global queue),
        // and work-stealing is reactive — only IDLE processors steal. When
        // poolSize * threadCount == procCount, all processors are busy, no
        // stealing occurs, and the actors of a single iteration stay on the
        // spawning processor's local queue → run sequentially on the same
        // core → no hardware-level data race can manifest. In sequential
        // mode, idle processors steal the second actor, placing the two
        // actors on different cores → race detected. This is why parallel
        // mode has lower per-iteration race-detection probability than
        // sequential mode. Leaving processors idle would help, but under
        // full-suite contention (-j20) other test executables' threads
        // claim the idle processors, negating the benefit. The practical
        // workaround is to use more iterations in race-detection tests to
        // compensate for the lower per-iteration probability.
        let threadCount = scenario.threadCount
        let poolSize = if (procCount < 1 || threadCount < 1) {
            Int64(1)
        } else if (threadCount >= procCount) {
            Int64(1)
        } else {
            procCount / threadCount
        }

        let resultQueue = ArrayBlockingQueue<ScenarioIterationResult>(poolSize)
        let cancelled = AtomicInt64(0)

        // Per-iteration backoff. Iteration 0 already used the shared
        // backoffGenerator (and calibrated sigma). Each parallel iteration
        // gets its own BackoffGenerator with the calibrated sigma and a fresh
        // RNG seeded from the shared one (drawn sequentially here on the
        // caller thread, so the shared RNG is never touched concurrently by
        // workers).
        let calibratedSigma = backoffGenerator.sigma
        let sharedRandom = backoffGenerator.random
        let seeds = Array<UInt64>(totalParallel, { _ => sharedRandom.nextUInt64() })

        var nextSeedIdx: Int64 = 0
        var dispatched: Int64 = 0
        var completed: Int64 = 0

        while (completed < totalParallel) {
            if (cancelled.load() == 0 &&
                nextSeedIdx < totalParallel &&
                dispatched - completed < poolSize)
            {
                let iterNum = nextSeedIdx + 1
                let seed = seeds[nextSeedIdx]
                let iterBackoff = BackoffGenerator(random: Random(seed), sigma: calibratedSigma)
                spawn {
                    let r = runIteration(iterNum, iterBackoff, verbose: verbose)
                    resultQueue.add(r)
                }
                dispatched++
                nextSeedIdx++
                continue
            }

            if (completed >= dispatched) {
                break                     
            }

            let result = resultQueue.remove()
            completed++
            if (cancelled.load() == 0) {
                if (!callback(result, result.iteration)) {
                    cancelled.store(1)
                }
            }
        }
    }

    protected func runIteration(
        iteration: Int64,
        backoffGen: BackoffGenerator,
        verbose!: Bool = false
    ): ScenarioIterationResult {
        executeReset()

        // Per-iteration state isolation: each iteration calls the state factory
        // to produce a fresh state instance. For simple ScenarioBuilder<Unit>
        // scenarios, the scenario(name) factory pre-registers { => () } so this
        // call returns the () placeholder. For typed scenarios, this returns a
        // fresh instance of the user's state type S.
        // stateFactory is guaranteed Some by ScenarioBuilder.build().
        let state: S = scenario.stateFactory.getOrThrow()()

        let barrier = Barrier(scenario.threadCount + 1)
        let startSignal = HBClockCounter()
        let history = ConcurrentHistory()
        let futures = ArrayList<Future<ArrayList<OperationInvocation>>>()

        let counters: ?Array<Int64> = if (let Linearizability <- scenario.consistencyModel) {
            Some(Array<Int64>(scenario.threadCount, { _ => 0 }))
        } else {
            None
        }

        for (actor in scenario.actors) {
            let backoff = backoffGen.next()

            futures.add(spawn {
                executeActorWithHistory(actor, state, barrier, startSignal, backoff, counters, scenario.threadCount)
            })
        }

        barrier.wait()
        startSignal.wait(0)

        for (future in futures) {
            let invocations = future.get()
            if (invocations.size > 0) {
                let actorId = invocations[0].actorId
                history.recordActorInvocations(actorId, invocations)
            }
        }

        var verifyFailed = false
        var consistencyResult: ?ConsistencyValidationResult = None

        try {
            executeAfter(state)
            verifyFailed = !executeVerify(state)

            if (let Some(validator) <- scenario.consistencyValidator) {
                let result = validator.validate(history, verbose: verbose)
                consistencyResult = Some(result)
                if (!result.isConsistent) {
                    verifyFailed = true
                }
            }
        } catch (e: Exception) {
            consistencyResult = Some(ConsistencyValidationResult(
                consistencyModel: scenario.consistencyModel,
                matchingPermutation: None,
                operationChecks: 0,
                prunedBranches: 0,
                cacheHits: 0,
                history: history,
                partialOrder: PartialOrder(),
                exception: Some(e)
            ))
            verifyFailed = true
        }

        let actorResults = ArrayList<ActorResult>()
        for (i in 0..scenario.threadCount) {
            actorResults.add(ActorResult(
                actorId: i,
                actorName: "actor_${i}",
                exception: None,
                startTime: DateTime.now(),
                endTime: DateTime.now()
            ))
        }

        ScenarioIterationResult(
            iteration: iteration,
            failedAfter: verifyFailed,
            actorResults: actorResults.toArray(),
            consistencyValidation: consistencyResult
        )
    }

    protected func updateBackoffSigma(avgDurationNanos: Int64): Unit {
        backoffGenerator.sigma = Float64(avgDurationNanos / 10)
    }

    private func executeReset(): Unit {
        if (let Some(block) <- scenario.resetBlock) {
            block()
        }
    }

    private func executeAfter(state: S): Unit {
        if (let Some(block) <- scenario.afterBlock) {
            block(state)
        }
    }

    private func executeVerify(state: S): Bool {
        match (scenario.verifyBlock) {
            case Some(block) => block(state)
            case None => true
        }
    }

    private func executeActorWithHistory(
        actor: Actor<S>,
        state: S,
        barrier: Barrier,
        startSignal: HBClockCounter,
        backoff: Int64,
        counters: ?Array<Int64>,
        actorCount: Int64
    ): ArrayList<OperationInvocation> {
        barrier.wait()
        startSignal.waitSpin(1)

        performBackoff(backoff)

        let invocations = ArrayList<OperationInvocation>()
        var localCounter: Int64 = 0
        var previousResponseVC: ?Array<Int64> = None

        if (let Some(sharedCounters) <- counters) {
            localCounter++
            sharedCounters[actor.id] = localCounter
            let startVC = Array<Int64>(actorCount, { j => sharedCounters[j] })
            previousResponseVC = Some(startVC)
        }

        for (i in 0..actor.operations.size) {
            if (let Some(sharedCounters) <- counters) {
                let invocationVC = previousResponseVC

                let result = actor.operations[i](state)

                localCounter++
                sharedCounters[actor.id] = localCounter
                let responseVC = Array<Int64>(actorCount, { j => sharedCounters[j] })
                previousResponseVC = Some(responseVC)

                invocations.add(OperationInvocation(
                    actorId: actor.id,
                    operationIndex: i,
                    operationName: actor.operationNames[i],
                    result: result,
                    invocationVC: invocationVC,
                    responseVC: Some(responseVC)
                ))
            } else {
                let result = actor.operations[i](state)

                invocations.add(OperationInvocation(
                    actorId: actor.id,
                    operationIndex: i,
                    operationName: actor.operationNames[i],
                    result: result
                ))
            }
        }

        invocations
    }

    private func performBackoff(iterations: Int64): Unit {
        var counter: Int64 = 0
        while (counter < iterations) {
            counter++
        }
        blackBox(counter)
    }

}
