/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.collection.ArrayList
import std.collection.collectArrayList
import std.core.Duration
import std.time.DateTime
import std.unittest.common.PrettyText

protected class ActorResult {
    protected ActorResult(
        protected let actorId!: Int64,
        protected let actorName!: String,
        protected let exception!: ?Exception,
        protected let startTime!: DateTime,
        protected let endTime!: DateTime
    ) {}
    
    protected static func success<S>(actor: Actor<S>, startTime: DateTime, endTime: DateTime): ActorResult {
        ActorResult(
            actorId: actor.id,
            actorName: actor.name,
            exception: None,
            startTime: startTime,
            endTime: endTime
        )
    }

    protected static func failure<S>(actor: Actor<S>, e: Exception, startTime: DateTime, endTime: DateTime): ActorResult {
        ActorResult(
            actorId: actor.id,
            actorName: actor.name,
            exception: Some(e),
            startTime: startTime,
            endTime: endTime
        )
    }
    
    protected prop isSuccess: Bool {
        get() { this.exception.isNone() }
    }
    
    protected prop duration: Duration {
        get() { endTime - startTime }
    }
    
    protected func hasException(): Bool {
        match (exception) {
            case Some(_) => true
            case None => false
        }
    }
    
    protected func getExceptionMessage(): String {
        match (exception) {
            case Some(e) => e.toString()
            case None => ""
        }
    }
}

protected class ScenarioIterationResult {
    protected ScenarioIterationResult(
        protected let iteration!: Int64, 
        protected let failedAfter!: Bool,
        protected let actorResults!: Array<ActorResult>,
        protected let consistencyValidation!: ?ConsistencyValidationResult
    ) {}
    
    protected prop avgDuration: Duration {
        get() {
            var total: Duration = Duration.Zero
            for (r in actorResults) {
                total += r.duration
            }
            total / actorResults.size
        }
    }

    protected prop allSucceeded: Bool {
        get() {
            if (failedAfter) {
                return false
            }

            for (r in actorResults) {
                if (!r.isSuccess) {
                    return false
                }
            }
            
            if (let Some(consResult) <- consistencyValidation) {
                if (!consResult.isConsistent) {
                    return false
                }
            }
            
            true
        }
    }
    
    protected prop successCount: Int64 {
        get() {
            var count: Int64 = 0
            for (r in actorResults) {
                if (r.isSuccess) {
                    count++
                }
            }
            count
        }
    }
    
    protected prop failureCount: Int64 {
        get() {
            var count: Int64 = 0
            if (failedAfter) {
                count += 1
            }
            for (r in actorResults) {
                if (!r.isSuccess) {
                    count++
                }
            }
            count
        }
    }
}

/**
 * Top-level result of running a `ConcurrentScenario`: aggregates the
 * per-iteration outcomes, the overall verdict, and a printable report.
 *
 * A `ScenarioResult` is produced by `ConcurrentScenario.run()` (or fed into a
 * `TestSuite` via the `ConcurrentExecutor` adapter, which converts each
 * iteration into a `RunStepResult`). It exposes:
 * - the scenario name, the number of iterations actually executed, and the
 *   total wall-clock duration;
 * - a `passed` boolean that is `true` iff every executed iteration succeeded
 *   (all actors ran without throwing *and* consistency validation, if
 *   enabled, found the recorded history consistent);
 * - the list of per-iteration `ConsistencyValidationResult`s (empty if no
 *   consistency validator was attached to the scenario);
 * - aggregate actor success / failure counts across all iterations;
 * - a multi-line `report()` renderer returning a `PrettyText`.
 *
 * On success the report is a one-line summary; on failure it is verbose by
 * default, embedding the per-failed-iteration consistency verdict and
 * mismatch table.
 */
public class ScenarioResult {
    protected ScenarioResult(
        /** Human-readable scenario name (set via `scenario(name)` / `ScenarioBuilder.name`). */
        public let scenarioName!: String,
        /** Number of iterations actually executed (may be less than the configured count due to early-exit on first failure). */
        public let iterationsExecuted!: Int64,
        protected let iterationResults!: ArrayList<ScenarioIterationResult>,
        /** Total wall-clock duration of the `ConcurrentScenario.run()` call. */
        public let totalDuration!: Duration
    ) {}
    
    /**
     * `true` iff every executed iteration succeeded — i.e. all actors ran
     * without throwing, the `verify` predicate (if any) returned `true`, and
     * the consistency validator (if any) found the recorded history consistent.
     *
     * Note that a `true` verdict does *not* prove the absence of races — it
     * only means no consistency violation was observed within the executed
     * iterations. Stress testing is probabilistic by nature.
     */
    public prop passed: Bool {
        get() {
            for (iter in iterationResults) {
                if (!iter.allSucceeded) {
                    return false
                }
            }
            true
        }
    }

    /**
     * Per-iteration consistency validation verdicts, in execution order.
     *
     * Iterations for which no consistency validator was attached (e.g. simple
     * capture-based scenarios with no `reset` and no `withState`) are absent
     * from the list; iterations that ran a validator appear here regardless of
     * whether they passed or failed.
     */
    public prop consistencyValidations: ArrayList<ConsistencyValidationResult> {
        get() {
            iterationResults
                .iterator()
                .filterMap({ iter => iter.consistencyValidation })
                |> collectArrayList
        }
    }
    
    /** Total number of successful actor executions summed across all iterations. */
    public prop totalSuccessCount: Int64 {
        get() {
            var count: Int64 = 0
            for (iter in iterationResults) {
                count += iter.successCount
            }
            count
        }
    }
    
    /** Total number of failed actor executions summed across all iterations. */
    public prop totalFailureCount: Int64 {
        get() {
            var count: Int64 = 0
            for (iter in iterationResults) {
                count += iter.failureCount
            }
            count
        }
    }
    
    /**
     * Render a multi-line human-readable report of this scenario result.
     *
     * On success the report is a brief summary (scenario name, iteration count,
     * total duration, status `PASSED`, aggregate actor success / failure
     * counts). On failure the report additionally embeds, for each failed
     * iteration, the one-line consistency verdict and the per-actor mismatch
     * table produced by `ConsistencyValidationResult.getMismatchDetails()`.
     *
     * The report is returned as a `PrettyText` so it can be streamed to any
     * `PrettyPrinter` (e.g. a colorised terminal reporter) without losing
     * structure; call `.toString()` to flatten it to a plain `String`.
     *
     * @return a `PrettyText` rendering of this result
     */
    public func report(): PrettyText {
        let pt = PrettyText()
        pt.appendLine("Concurrent Scenario: ${scenarioName}")
          .appendLine("Iterations: ${iterationsExecuted}")
          .appendLine("Total Duration: ${totalDuration}")
          .appendLine("Status: ${if (passed) { "PASSED" } else { "FAILED" }}")
          .appendLine("Actor Results Summary:")
          .appendLine("  Total successes: ${totalSuccessCount}")
          .appendLine("  Total failures: ${totalFailureCount}")

        for (iter in iterationResults) {
            if (!iter.allSucceeded) {
                if (let Some(consResult) <- iter.consistencyValidation) {
                    if (!consResult.isConsistent) {
                        pt.appendLine("")
                          .appendLine("Iteration ${iter.iteration} failures:")
                          .append("  Consistency validation failed:")
                          .newLine()
                        if (let Some(mismatch) <- consResult.getMismatchDetails()) {
                            pt.append(mismatch)
                        }
                        pt.newLine()
                    }
                }
                for (r in iter.actorResults) {
                    if (!r.isSuccess) {
                        pt.appendLine("  Actor ${r.actorName}: ${r.getExceptionMessage()}")
                    }
                }
            }
        }

        pt
    }
    
    protected func getFirstFailure(): ?ActorResult {
        for (iter in iterationResults) {
            for (r in iter.actorResults) {
                if (!r.isSuccess) {
                    return Some(r)
                }
            }
        }
        None
    }
}