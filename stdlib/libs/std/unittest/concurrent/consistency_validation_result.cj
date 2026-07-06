/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *  * This source file is part of the Cangjie project, licensed under Apache-2.0
 *  * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.collection.ArrayList

protected class MismatchInfo {
    protected MismatchInfo(
        public let operation!: OperationInvocation,
        public let expectedResult!: EquatableObj
    ) {}
}

public class ConsistencyValidationResult {
    protected ConsistencyValidationResult(
        public let consistencyModel!: ConsistencyModel,
        public let matchingPermutation!: ?ArrayList<Int64>,
        public let operationChecks!: Int64,
        public let prunedBranches!: Int64,
        public let cacheHits!: Int64,
        protected let history!: ConcurrentHistory,
        protected let partialOrder!: PartialOrder,
        public let exception!: ?Exception = None,
        protected let deadEndDepth!: Int64 = -1,
        protected let deadEndPrefix!: Array<Int64> = Array<Int64>(),
        protected let deadEndMismatches!: Array<MismatchInfo> = Array<MismatchInfo>()
    ) {}
    
    public prop isConsistent: Bool {
        get() { matchingPermutation.isSome() }
    }
    
    public func report(): String {
        if (let Some(e) <- exception) {
            "${modelCapitalizedName(consistencyModel)} validation error: ${e.message}"
        } else if (isConsistent) {
            "${modelCapitalizedName(consistencyModel)} - found matching ordering after ${operationChecks} operation checks (${prunedBranches} branches pruned, ${cacheHits} cache hits)"
        } else {
            "${modelNotName(consistencyModel)} - no valid ordering found after ${operationChecks} operation checks (${prunedBranches} branches pruned, ${cacheHits} cache hits)"
        }
    }

    public func getOperationHistory(): String {
        let sb = StringBuilder()
        sb.append("Executed operations:\n")
        for (actorId in history.groupByActor().keys()) {
            match (history.getActorInvocations(actorId)) {
                case Some(ops) =>
                    sb.append("  Actor ${actorId}:\n")
                    for (inv in ops) {
                        sb.append("    ${inv.operationName} -> ${inv.result}\n")
                    }
                case None => ()
            }
        }
        sb.toString()
    }
    
    public func getMatchingSequence(): ?String {
        let permutation = matchingPermutation ?? return None

        let sb = StringBuilder()
        sb.append("Matching ${modelDisplayName(consistencyModel)} sequence:\n")
        
        for (opId in permutation) {
            let actorId = opId / 1000
            let opIndex = opId % 1000
            match (history.getInvocation(actorId, opIndex)) {
                case Some(inv) =>
                    sb.append("  Actor ${inv.actorId} op[${inv.operationIndex}] '${inv.operationName}' -> result\n")
                case None => ()
            }
        }
        sb.toString()
    }

    public func getMismatchDetails(): ?String {
        if (deadEndMismatches.isEmpty()) {
            return None
        }

        let sb = StringBuilder()
        sb.append("Sequentialization failure at depth ${deadEndDepth}:\n")

        if (deadEndPrefix.isEmpty()) {
            sb.append("  After []:\n")
        } else {
            let parts = ArrayList<String>()
            for (opId in deadEndPrefix) {
                let actorId = opId / 1000
                let opIndex = opId % 1000
                match (history.getInvocation(actorId, opIndex)) {
                    case Some(inv) =>
                        parts.add("Actor ${inv.actorId}: ${inv.operationName} -> ${inv.result}")
                    case None => ()
                }
            }
            sb.append("  After [${String.join(parts.toArray(), delimiter: ", ")}]:\n")
        }

        for (m in deadEndMismatches) {
            sb.append("    Actor ${m.operation.actorId}: ${m.operation.operationName} (expected: ${m.expectedResult}, actual: ${m.operation.result})\n")
        }
        sb.toString()
    }
}