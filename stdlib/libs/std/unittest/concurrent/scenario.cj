/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.unittest.common.Configuration
import std.time.DateTime
import std.collection.ArrayList

public class ConcurrentScenario<S> where S <: Any {
    protected ConcurrentScenario(
        protected let name!: String,
        protected let threadCount!: Int64,
        protected let iterations!: Int64,
        protected let resetBlock!: ?() -> Unit,
        protected let stateFactory!: ?() -> S,
        protected let actors!: Array<Actor<S>>,
        protected let afterBlock!: ?(S) -> Unit,
        protected let verifyBlock!: ?(S) -> Bool,
        protected let configuration!: Configuration,
        protected let consistencyValidator!: ?ConsistencyValidator = None,
        protected let consistencyModel!: ConsistencyModel = SequentialConsistency,
        protected let parallelIterations!: Bool = false
    ) {}

    public func run(): ScenarioResult {
        let executor = ScenarioRunner<S>(this)
        let iterationResults = ArrayList<ScenarioIterationResult>()
        let startTime = DateTime.now()

        // The callback returns false on the first failing iteration, triggering
        // early-exit: no further iterations are dispatched and in-flight results
        // are discarded. This prevents waiting through thousands of iterations
        // after a consistency violation has already been found.
        let callback = { iterResult: ScenarioIterationResult, _: Int64 =>
            iterationResults.add(iterResult)
            iterResult.allSucceeded
        }

        executor.executeIterationsWithCallback(iterations, callback: callback)

        let endTime = DateTime.now()

        ScenarioResult(
            scenarioName: name,
            iterationsExecuted: iterationResults.size,
            iterationResults: iterationResults,
            totalDuration: endTime - startTime
        )
    }
}

// Simple entry: returns ScenarioBuilder<Unit> for stateless, capture-based scenarios.
// Operations receive () (Unit) as the state placeholder and ignore it.
// A default state factory { => () } is registered automatically (without setting
// the "explicit" flag) so that the runner always has a state to pass to
// operations; simple users can still call .reset { => var = ... } to reassign
// shared mutable state per iteration.
//
// To use the typed per-iteration-state API, call .withState<S>(factory) on the
// returned builder — see ScenarioBuilder.withState.
public func scenario(name: String): ScenarioBuilder<Unit> {
    ScenarioBuilder<Unit>(name).defaultStateFactory({ => () })
}

public func scenario(): ScenarioBuilder<Unit> {
    ScenarioBuilder<Unit>().defaultStateFactory({ => () })
}


protected class Actor<S> where S <: Any {
    protected Actor(
        protected let id!: Int64,
        protected let operations!: Array<(S) -> EquatableObj>,
        protected let operationNames!: Array<String>,
        protected let name!: String
    ) {}
}
