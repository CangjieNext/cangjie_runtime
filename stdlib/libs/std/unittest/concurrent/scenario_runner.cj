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

protected class ScenarioRunner {
    private let backoffGenerator: BackoffGenerator
    
    protected ScenarioRunner(private let scenario: ConcurrentScenario) {
        backoffGenerator = BackoffGenerator(random: Random())
    }
    
    protected init(scenario: ConcurrentScenario, backoffSeed: Int64) {
        this.scenario = scenario
        backoffGenerator = BackoffGenerator(random: Random(UInt64(backoffSeed)))
    }
    
    protected func executeIterationsWithCallback(
        iterations: Int64,
        callback: (ScenarioIterationResult, Int64) -> Bool
    ): Unit {
        let firstResult = runIteration(0)
        updateBackoffSigma(firstResult.avgDuration.toNanoseconds())
        
        if (!callback(firstResult, 0)) {
            return
        }
        
        for (iter in 1..iterations) {
            let iterResult = runIteration(iter)
            if (!callback(iterResult, iter)) {
                return
            }
        }
    }
    
    protected func runIteration(iteration: Int64): ScenarioIterationResult {
        executeReset()
        
        let barrier = Barrier(scenario.threadCount)
        let startSignal = HBClockCounter()
        let history = ConcurrentHistory()
        let futures = ArrayList<Future<ArrayList<OperationInvocation>>>()
        
        let needsVC = match (scenario.consistencyModel) {
            case Linearizability => true
            case _ => false
        }
        
        let counters: ?Array<Int64> = if (needsVC) {
            Some(Array<Int64>(scenario.threadCount, { _ => 0 }))
        } else {
            None
        }
        
        for (i in 0..scenario.threadCount) {
            let actor = scenario.actors[i]
            let backoff = backoffGenerator.next()
            
            futures.add(spawn {
                executeActorWithHistory(actor, barrier, startSignal, backoff, counters, scenario.threadCount)
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
            executeAfter()
            verifyFailed = !executeVerify()
            
            if (let Some(validator) <- scenario.consistencyValidator) {
                let result = validator.validate(history)
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
                cachedPermutations: 0,
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
    
    private func executeAfter(): Unit {
        if (let Some(block) <- scenario.afterBlock) {
            block()
        }
    }
    
    private func executeVerify(): Bool {
        (scenario.verifyBlock ?? return true)()
    }
    
    private func executeActorWithHistory(
        actor: Actor,
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
        
        for (i in 0..actor.operations.size) {
            let invocationVC: ?Array<Int64>
            let responseVC: ?Array<Int64>
            
            if (let Some(sharedCounters) <- counters) {
                localCounter++
                sharedCounters[actor.id] = localCounter
                invocationVC = Some(Array<Int64>(actorCount, { j => sharedCounters[j] }))
                
                let result = actor.operations[i]()
                
                localCounter++
                sharedCounters[actor.id] = localCounter
                responseVC = Some(Array<Int64>(actorCount, { j => sharedCounters[j] }))
                
                invocations.add(OperationInvocation(
                    actorId: actor.id,
                    operationIndex: i,
                    operationName: actor.operationNames[i],
                    result: result,
                    invocationVC: invocationVC,
                    responseVC: responseVC
                ))
            } else {
                let result = actor.operations[i]()
                
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