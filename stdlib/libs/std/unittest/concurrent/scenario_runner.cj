/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.sync.*
import std.time.DateTime
import std.collection.ArrayList
import std.random.Random
import std.runtime.blackBox

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
        let firstResult = runIteration(0, verbose: verbose)
        updateBackoffSigma(firstResult.avgDuration.toNanoseconds())

        if (!callback(firstResult, 0)) {
            return
        }

        // todo: consider running iterations in parallel for better performance.
        // With per-iteration state isolation (stateFactory), this is now safe to
        // implement as a follow-up: each iteration creates its own fresh state
        // instance and never touches shared mutable variables.
        for (iter in 1..iterations) {
            let iterResult = runIteration(iter, verbose: verbose)
            if (!callback(iterResult, iter)) {
                return
            }
        }
    }

    protected func runIteration(iteration: Int64, verbose!: Bool = false): ScenarioIterationResult {
        executeReset()

        // Per-iteration state isolation: each iteration calls the state factory
        // to produce a fresh state instance. For legacy ScenarioBuilder<Unit>
        // scenarios, the scenario(name) factory pre-registers { => () } so this
        // call returns the () placeholder. For typed scenarios, this returns a
        // fresh instance of the user's state type S.
        // stateFactory is guaranteed Some by ScenarioBuilder.build().
        let state: S = scenario.stateFactory.getOrThrow()()

        let barrier = Barrier(scenario.threadCount)
        let startSignal = HBClockCounter()
        let history = ConcurrentHistory()
        let futures = ArrayList<Future<ArrayList<OperationInvocation>>>()

        let counters: ?Array<Int64> = if (let Linearizability <- scenario.consistencyModel) {
            Some(Array<Int64>(scenario.threadCount, { _ => 0 }))
        } else {
            None
        }

        for (actor in scenario.actors) {
            let backoff = backoffGenerator.next()

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
