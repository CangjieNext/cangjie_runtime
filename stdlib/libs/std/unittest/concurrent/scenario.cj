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

public class ConcurrentScenario {
    public ConcurrentScenario(
        protected let name!: String,
        protected let threadCount!: Int64,
        protected let iterations!: Int64,
        protected let resetBlock!: ?() -> Unit,
        protected let actors!: Array<Actor>,
        protected let afterBlock!: ?() -> Unit,
        protected let verifyBlock!: ?() -> Bool,
        protected let configuration!: Configuration,
        protected let consistencyValidator!: ?ConsistencyValidator = None,
        protected let consistencyModel!: ConsistencyModel = SequentialConsistency
    ) {}
    
    public func run(): ScenarioResult {
        let executor = ScenarioRunner(this)
        let iterationResults = ArrayList<ScenarioIterationResult>()
        let startTime = DateTime.now()
        
        let callback = { iterResult: ScenarioIterationResult, _: Int64 =>
            iterationResults.add(iterResult)
            true
        }
        
        executor.executeIterationsWithCallback(iterations, callback)
        
        let endTime = DateTime.now()
        
        ScenarioResult(
            scenarioName: name,
            iterationsExecuted: iterations,
            iterationResults: iterationResults,
            totalDuration: endTime - startTime
        )
    }
}

public func scenario(name: String): ScenarioBuilder {
    ScenarioBuilder(name)
}

public func scenario(): ScenarioBuilder {
    ScenarioBuilder()
}


public class Actor {
    public Actor(
        public let id!: Int64,
        public let operations!: Array<() -> EquatableObj>,
        public let operationNames!: Array<String>,
        public let name!: String
    ) {}
}