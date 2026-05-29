/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *  * This source file is part of the Cangjie project, licensed under Apache-2.0
 *  * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.collection.ArrayList

public class ConsistencyValidationResult {
    public ConsistencyValidationResult(
        public let consistencyModel!: ConsistencyModel,
        public let matchingPermutation!: ?ArrayList<Int64>,
        public let operationChecks!: Int64,
        public let prunedBranches!: Int64,
        public let cachedPermutations!: Int64,
        public let history!: ConcurrentHistory,
        public let partialOrder!: PartialOrder,
        public let exception!: ?Exception = None
    ) {}
    
    public prop isConsistent: Bool {
        get() { matchingPermutation.isSome() }
    }
    
    public func report(): String {
        if (let Some(e) <- exception) {
            "${modelCapitalizedName(consistencyModel)} validation error: ${e.message}"
        } else if (isConsistent) {
            "${modelCapitalizedName(consistencyModel)} - found matching ordering after ${operationChecks} operation checks (${prunedBranches} branches pruned, ${cachedPermutations} from cache)"
        } else {
            "${modelNotName(consistencyModel)} - no valid ordering found after ${operationChecks} operation checks (${prunedBranches} branches pruned, ${cachedPermutations} from cache)"
        }
    }
    
    public func getMatchingSequence(): ?String {
        if (let Some(permutation) <- matchingPermutation) {
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
        } else {
            None
        }
    }
}