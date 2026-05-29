/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.collection.ArrayList

public class SequentialValidationResult {
    public SequentialValidationResult(
        public let matchingPermutation!: ?ArrayList<Int64>,
        public let testedPermutations!: Int64,
        public let history!: ConcurrentHistory,
        public let exception!: ?Exception = None
    ) {}
    
    public prop isConsistent: Bool {
        get() { matchingPermutation.isSome() }
    }
    
    public func report(): String {
        if (let Some(e) <- exception) {
            "Sequential validation error: ${e.message}"
        } else if (isConsistent) {
            "Sequentially consistent - found matching ordering after testing ${testedPermutations} permutations"
        } else {
            "NOT sequentially consistent - no valid ordering found after ${testedPermutations} permutations"
        }
    }
    
    public func getMatchingSequence(): ?String {
        if (let Some(permutation) <- matchingPermutation) {
            let sb = StringBuilder()
            sb.append("Matching sequential sequence:\n")
            
            let actorIndices = ArrayList<Int64>(history.actorCount, { _ => 0 })
            for (actorId in permutation) {
                let currentIndex = actorIndices[actorId]
                match (history.getInvocation(actorId, currentIndex)) {
                    case Some(inv) =>
                        sb.append("  Actor ${inv.actorId} op[${inv.operationIndex}] '${inv.operationName}' -> result\n")
                        actorIndices[actorId] = currentIndex + 1
                    case None => ()
                }
            }
            sb.toString()
        } else {
            None
        }
    }
}