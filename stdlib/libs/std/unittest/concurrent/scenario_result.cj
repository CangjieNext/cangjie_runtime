/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.collection.ArrayList
import std.core.Duration
import std.time.DateTime

protected class ActorResult {
    protected ActorResult(
        protected let actorId!: Int64,
        protected let actorName!: String,
        protected let exception!: ?Exception,
        protected let startTime!: DateTime,
        protected let endTime!: DateTime
    ) {}
    
    protected static func success(actor: Actor, startTime: DateTime, endTime: DateTime): ActorResult {
        ActorResult(
            actorId: actor.id,
            actorName: actor.name,
            exception: None,
            startTime: startTime,
            endTime: endTime
        )
    }
    
    protected static func failure(actor: Actor, e: Exception, startTime: DateTime, endTime: DateTime): ActorResult {
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

public class ScenarioResult {
    protected ScenarioResult(
        public let scenarioName!: String,
        public let iterationsExecuted!: Int64,
        protected let iterationResults!: ArrayList<ScenarioIterationResult>,
        public let totalDuration!: Duration
    ) {}
    
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
    
    public prop totalSuccessCount: Int64 {
        get() {
            var count: Int64 = 0
            for (iter in iterationResults) {
                count += iter.successCount
            }
            count
        }
    }
    
    public prop totalFailureCount: Int64 {
        get() {
            var count: Int64 = 0
            for (iter in iterationResults) {
                count += iter.failureCount
            }
            count
        }
    }
    
    public func report(): String {
        let sb = StringBuilder()
        sb.append("Concurrent Scenario: ${scenarioName}\n")
        sb.append("Iterations: ${iterationsExecuted}\n")
        sb.append("Total Duration: ${totalDuration}\n")
        sb.append("Status: ${if (passed) { "PASSED" } else { "FAILED" }}\n")
        
        sb.append("Actor Results Summary:\n")
        sb.append("  Total successes: ${totalSuccessCount}\n")
        sb.append("  Total failures: ${totalFailureCount}\n")
        
        for (iter in iterationResults) {
            if (!iter.allSucceeded) {
                sb.append("\nIteration ${iter.iteration} failures:\n")
                for (r in iter.actorResults) {
                    if (!r.isSuccess) {
                        sb.append("  Actor ${r.actorName}: ${r.getExceptionMessage()}\n")
                    }
                }
            }
        }
        
        sb.toString()
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