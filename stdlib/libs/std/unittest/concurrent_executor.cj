/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest

import std.unittest.common.*
import std.unittest.concurrent.ScenarioRunner
import std.unittest.concurrent.ScenarioIterationResult
import std.unittest.concurrent.ConcurrentScenario
import std.collection.ArrayList
import std.time.DateTime

class ConcurrentTestFailure <: Exception {
    public let iterationResult: ScenarioIterationResult
    
    ConcurrentTestFailure(result!: ScenarioIterationResult) {
        super("Concurrent test iteration failed")
        iterationResult = result
    }
}

class ConcurrentExecutor <: Executor {
    private let executor: ScenarioRunner
    
    ConcurrentExecutor(
        private let scenario!: ConcurrentScenario
    ) {
        executor = ScenarioRunner(scenario)
    }
    
    public func execute(
        suiteInfo: TestSuiteReportInfo,
        tcase: TestCaseResult,
        configuration: Configuration
    ): Unit {
        let progress = ProgressOutput(configuration)
        
        let iterations = scenario.iterations
        
        progress.verbose {
            "Starting concurrent scenario '${tcase.caseId.caseName}' " +
            "with ${scenario.threadCount} threads, ${iterations} iterations."
        }
        
        let verbose = configuration.verbose
        executor.executeIterationsWithCallback(iterations, verbose: verbose) { iterResult, iterIndex =>
            handleIterationResult(tcase, configuration, iterResult, iterIndex)
        }
        
        if (!tcase.hasFailures()) {
            progress.verbose {
                "Concurrent scenario '${tcase.caseId.caseName}' completed successfully."
            }
        }
    }
    
    private func handleIterationResult(
        tcase: TestCaseResult,
        configuration: Configuration,
        iterResult: ScenarioIterationResult,
        iterIndex: Int64
    ): Bool {
        let stepKind = CaseStep(ArgumentDescription(
            PrettyText("iteration ${iterIndex}"),
            iterIndex,
            0,
            configuration.get(KeyRandomSeed.randomSeed)
        ))
        
        if (!iterResult.allSucceeded) {
            let stepResult = RunStepResult.simpleError(stepKind, ConcurrentTestCheckResult(iterResult))
            tcase.add(stepResult)
            return false
        }
        
        let passedChecks = iterResult.successCount
        let stepResult = RunStepResult(passedChecks, DateTime.now(), stepKind, Test(1))
        
        if (tcase.subResults.isEmpty()) {
            tcase.add(stepResult)
        } else {
            tcase.addSuccessfullStep(stepResult)
        }
        
        return true
    }
}
