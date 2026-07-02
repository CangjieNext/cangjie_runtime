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

class ConcurrentExecutor<S> <: Executor where S <: Any {
    private var generationStep = 0

    ConcurrentExecutor(
        private let strategy!: DataStrategyProcessor<T>,
        private let body!: (T) -> ConcurrentScenario<S>
    ) {}

    public func execute(
        suiteInfo: TestSuiteReportInfo,
        result: TestCaseResult,
        configuration: Configuration
    ): Unit {
        let lastInput = Box(Array<InputParameter>())
        let lastScenario: Box<Option<ConcurrentScenario>> = Box(None)
        let executeBody = { arg: T, input: Array<InputParameter> =>
            let scenario = body(arg)
            lastInput.value = input
            lastScenario.value = Some(scenario)
            executeScenario(suiteInfo, result, configuration, scenario, input)
        }

        strategy.nextIteration(configuration)

        // FIXME: Copypasta (almost) from TestCaseExecutor
        while (generationStep < configuration.generationSteps || !strategy.isInfinite) {
            generationStep += 1
            let processorResult = strategy.processNextWith(configuration, executeBody)
            let scenario = lastScenario.value.getOrThrow()

            var initialFailure = match (processorResult) {
                case Success(stepResult) =>
                    result.addSuccessfullStep(stepResult)
                    continue
                case NoItems =>
                    generationStep -= 1
                    return
                case StrategyError(e) =>
                    result.add(strategyErrorAsResult(e, argDescription(scenario, lastInput.value, configuration)))
                    return
                case Failure(input, stepResult) =>
                    (input, stepResult)
            }

            let (input, failure) = match (strategy.shrink(configuration, executeBody)) {
                case None => initialFailure
                case Some(Failure(input, shrunk)) => (input, shrunk)
                case Some(StrategyError(e)) =>
                    result.add(strategyErrorAsResult(e, argDescription(scenario, lastInput.value, configuration)))
                    return
                case _ =>
                    throw Exception("Unexpected shrinking result")
            }

            // It was decided that we should print full number of reduction steps
            // instead of reduction step number on which we found latest shrinked failure
            result.add(failure.withUpdatedStep(CaseStep(argDescription(scenario, lastInput.value, configuration))))

            if (strategy.isInfinite) {
                break
            }
        }
    }

    // FIXME: Copypasta (almost) from TestCaseExecutor
    // FIXME: Currently scenario argument expected to be the first one
    static func formatArguments(scenario: ConcurrentScenario, inputs: Array<InputParameter>): ?PrettyText {
        if (inputs.isNoArg()) {
            return None
        }
        let sb = PrettyText()
        sb.appendLine("Scenario: ${scenario.name}")
        for (param in inputs.iterator().skip(1)) {
            sb.append(param.name)
            sb.append(" = ")
            sb.appendLine(shieldEscapeSeq(param.repr))
        }
        return sb
    }

    private func argDescription(scenario: ConcurrentScenario, param: Array<InputParameter>, configuration: Configuration) {
        ArgumentDescription(formatArguments(scenario, param), generationStep - 1, strategy.currentReduction, configuration.randomSeed)
    }
    
    private func executeScenario(
        suiteInfo: TestSuiteReportInfo,
        tcase: TestCaseResult,
        configuration: Configuration,
        scenario: ConcurrentScenario,
        input: Array<InputParameter>,
    ): RunStepResult {
        let progress = ProgressOutput(configuration)
        let executor  = ScenarioRunner<S>(scenario)
        let iterations = scenario.iterations
        progress.verbose {
            "Starting concurrent scenario '${tcase.caseId.caseName}' " +
            "with ${scenario.threadCount} threads, ${iterations} iterations."
        }
        
        let failures = ArrayList<CheckResult>()
        let start = DateTime.now()
        let verbose = configuration.verbose
        executor.executeIterationsWithCallback(iterations, verbose: verbose) { iterResult, iterIndex =>
            if (!iterResult.allSucceeded) {
                failures.add(ConcurrentTestCheckResult(iterResult))
            }
            true
        }
        if (failures.isEmpty()) {
            progress.verbose {
                "Concurrent scenario '${tcase.caseId.caseName}' completed successfully."
            }
            return RunStepResult(iterations, start, stepKind, Test(iterations))
        }

        return RunStepResult(iterations - failures.size, start, stepKind, Failure(failures.toArray()))
    }
}
