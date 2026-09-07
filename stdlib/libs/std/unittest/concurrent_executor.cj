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
import std.unittest.concurrent.ConcurrentScenarioProducer
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
        private let body!: (T) -> ConcurrentScenarioProducer<S>
    ) {}

    public func execute(
        suiteInfo: TestSuiteReportInfo,
        tcase: TestCaseResult,
        configuration: Configuration
    ): Unit {
        var generationStep = 0

        for (arg in strategy.provide(configuration)) {
            generationStep += 1
            if (strategy.isInfinite && generationStep > configuration.generationSteps) {
                return
            }

            let input = strategy.lastItemInfo()
            let start = DateTime.now()
            let (scenarioProducer, failures) = try {
                let scenarioProducer = body(arg)
                let failures = executeScenario(suiteInfo, tcase, configuration, scenarioProducer)
                (scenarioProducer, failures)
            } catch (e: Exception) {
                tcase.add(strategyErrorAsResult(e, argDescription([], input, configuration, generationStep)))
                return
            }
            let stepKind = CaseStep(argDescription(failures, input, configuration, generationStep))
            if (failures.isEmpty()) {
                tcase.addSuccessfullStep(RunStepResult(scenarioProducer.iterations, start, stepKind, Test(1)))
            } else {
                let firstFailure = failures |> take(1) |> map<ScenarioIterationResult, CheckResult> {
                    iter => ConcurrentTestCheckResult(iter)
                } |> collectArray
                tcase.add(RunStepResult(scenarioProducer.iterations - failures.size, start, stepKind, Failure(firstFailure)))
                return
            }
        }
    }

    // FIXME: Currently scenario argument expected to be the first one
    static func formatArguments(
        failures: Array<ScenarioIterationResult>,
        inputs: Array<InputParameter>
    ): ?PrettyText {
        if (inputs.isNoArg()) {
            return None
        }
        let sb = PrettyText()
        // Do not print first parameter which is scenario
        for (param in inputs.iterator().skip(1)) {
            sb.append(param.name)
            sb.append(" = ")
            sb.appendLine(shieldEscapeSeq(param.repr))
        }
        return sb
    }

    private func argDescription(
        failures: Array<ScenarioIterationResult>,
        param: Array<InputParameter>,
        configuration: Configuration,
        generationStep: Int64
    ) {
        ArgumentDescription(
            formatArguments(failures, param),
            generationStep - 1,
            strategy.currentReduction,
            configuration.randomSeed,
        )
    }

    private func executeScenario(
        suiteInfo: TestSuiteReportInfo,
        tcase: TestCaseResult,
        configuration: Configuration,
        scenarioProducer: ConcurrentScenarioProducer<S>
    ): Array<ScenarioIterationResult> {
        let progress = ProgressOutput(configuration)
        let executor = ScenarioRunner<S>(scenarioProducer)
        let iterations = scenarioProducer.iterations
        progress.verbose {
            "Starting concurrent scenario '${tcase.caseId.caseName}' " +
                "with ${scenarioProducer.threadCount} threads, ${iterations} iterations."
        }

        let verbose = configuration.verbose
        let failures = ArrayList<ScenarioIterationResult>()
        executor.executeIterationsWithCallback(iterations, verbose: verbose) {
            iterResult, iterIndex =>
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
