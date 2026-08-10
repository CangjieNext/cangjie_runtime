/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest

import std.unittest.common.*
import std.unittest.concurrent.*
import std.collection.*
import std.time.DateTime

class ConcurrentTestFailure <: Exception {
    public let iterationResult: ScenarioIterationResult

    ConcurrentTestFailure(result!: ScenarioIterationResult) {
        super("Concurrent test iteration failed")
        iterationResult = result
    }
}

class ConcurrentExecutor<T, S> <: Executor where S <: Any {
    ConcurrentExecutor(
        private let strategy!: DataStrategyProcessor<T>,
        private let body!: (T) -> ConcurrentScenario<S>
    ) {}

    public func execute(
        suiteInfo: TestSuiteReportInfo,
        result: TestCaseResult,
        configuration: Configuration
    ): Unit {
        var generationStep = 0

        for (arg in strategy.provide(configuration)) {
            generationStep += 1
            if (generationStep >= configuration.generationSteps) {
                return
            }

            let input = strategy.lastItemInfo()
            let start = DateTime.now()
            let (scenario, failures) = try {
                let scenario = body(arg)
                let failures = executeScenario(suiteInfo, result, configuration, scenario)
                (scenario, failures)
            } catch (e: Exception) {
                result.add(strategyErrorAsResult(e, argDescription(None, [], input, configuration, generationStep)))
                return
            }
            let stepKind = CaseStep(argDescription(scenario, failures, input, configuration, generationStep))
            if (failures.isEmpty()) {
                result.add(RunStepResult(scenario.iterations, start, stepKind, Test(scenario.iterations)));
            } else {
                let failures = failures |> map<ScenarioIterationResult, CheckResult> { result => 
                    ConcurrentTestCheckResult(result)
                } |> collectArray
                result.add(RunStepResult(scenario.iterations - failures.size, start, stepKind, Failure(failures)));
                return
            }
        }
    }

    static func formatScenario(
        sb: PrettyText,
        scenario: ConcurrentScenario<S>,
        failures: Array<ScenarioIterationResult>,
    ) {
        sb.appendLine("Scenario: ${scenario.name}") 
        let mismatchTables = failures |> filterMap { result => result.consistencyValidation.flatMap({ cv => cv.getMismatchTable() }) } |> collectArray
        // if (mismatchTables.size > 1) {
        //     sb.appendLine("Displaying only one failure case of ${mismatchTables.size}")
        // }
        // if (let Some(mismatchTable) <- (mismatchTables |> first)) {
        //     sb.appendLine(mismatchTable)
        // }
    }

    // FIXME: Currently scenario argument expected to be the first one
    static func formatArguments(
        scenario: Option<ConcurrentScenario<S>>,
        failures: Array<ScenarioIterationResult>,
        inputs: Array<InputParameter>
    ): ?PrettyText {
        if (inputs.isNoArg()) {
            return None
        }
        let sb = PrettyText()
        if (let Some(scenario) <- scenario) {
            formatScenario(sb, scenario, failures)
        }
        for (param in inputs.iterator().skip(1)) {
            sb.append(param.name)
            sb.append(" = ")
            sb.appendLine(shieldEscapeSeq(param.repr))
        }
        return sb
    }

    private func argDescription(
        scenario: Option<ConcurrentScenario<S>>,
        failures: Array<ScenarioIterationResult>,
        param: Array<InputParameter>,
        configuration: Configuration,
        generationStep: Int64,
    ) {
        ArgumentDescription(
            formatArguments(scenario, failures, param),
            generationStep - 1,
            strategy.currentReduction, 
            configuration.randomSeed,
        )
    }
    
    private func executeScenario(
        suiteInfo: TestSuiteReportInfo,
        tcase: TestCaseResult,
        configuration: Configuration,
        scenario: ConcurrentScenario<S>,
    ): Array<ScenarioIterationResult> {
        let progress = ProgressOutput(configuration)
        let executor  = ScenarioRunner<S>(scenario)
        let iterations = scenario.iterations
        progress.verbose {
            "Starting concurrent scenario '${tcase.caseId.caseName}' " +
            "with ${scenario.threadCount} threads, ${iterations} iterations."
        }
        
        let verbose = configuration.verbose
        let failures = ArrayList<ScenarioIterationResult>()
        executor.executeIterationsWithCallback(iterations, verbose: verbose) { iterResult, iterIndex =>
            if (!iterResult.allSucceeded) {
                failures.add(iterResult)
            }
            true
        }
        if (failures.isEmpty()) {
            progress.verbose {
                "Concurrent scenario '${tcase.caseId.caseName}' completed successfully."
            }
        }

        return failures.toArray()
    }
}
