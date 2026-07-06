/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *  * This source file is part of the Cangjie project, licensed under Apache-2.0
 *  * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.collection.ArrayList
import std.collection.HashSet
import std.time.DateTime

protected class ConsistencyValidator {
    protected ConsistencyValidator(
        let consistencyModel!: ConsistencyModel,
        private let createFreshInstance!: () -> Any,
        private let reExecuteOperation!: (Any, OperationInvocation) -> EquatableObj
    ) {}

    private var operationChecks: Int64 = 0
    private var cacheChecks: Int64 = 0
    private var prunedBranches: Int64 = 0
    private var foundPermutation: ?ArrayList<Int64> = None
    private var cachedTreeRoot: ?PermutationNode = None
    private var cachedTreeSize: Int64 = -1
    private var currentRunId: Int64 = 0
    private var deepestDeadEndDepth: Int64 = -1
    private var deepestDeadEndPrefix: ArrayList<Int64> = ArrayList<Int64>()
    private var deepestDeadEndMismatches: ArrayList<MismatchInfo> = ArrayList<MismatchInfo>()

    protected func validate(history: ConcurrentHistory, verbose!: Bool = false): ConsistencyValidationResult {
        operationChecks = 0
        cacheChecks = 0
        prunedBranches = 0
        foundPermutation = None
        deepestDeadEndDepth = -1
        deepestDeadEndPrefix = ArrayList<Int64>()
        deepestDeadEndMismatches = ArrayList<MismatchInfo>()
        currentRunId += 1

        let po = computePartialOrder(history, consistencyModel)

        if (let Serializability <- consistencyModel && po.size > 10) {
            return ConsistencyValidationResult(
                consistencyModel: consistencyModel,
                matchingPermutation: None,
                operationChecks: 0,
                prunedBranches: 0,
                cacheHits: 0,
                history: history,
                partialOrder: po,
                exception: Some(IllegalArgumentException(
                    "Serializability check requires exhaustive search of all n! permutations. " +
                    "Current scenario has ${po.size} operations (${factorial(po.size)} permutations), " +
                    "which exceeds the practical limit of 10. " +
                    "Consider using SequentialConsistency or Linearizability instead, " +
                    "or reduce the number of operations."
                ))
            )
        }

        var treeRoot: PermutationNode
        if (let Some(root) <- cachedTreeRoot && cachedTreeSize == po.size) {
            treeRoot = root
        } else {
            treeRoot = PermutationNode(operation: None, depth: 0)
            cachedTreeRoot = Some(treeRoot)
            cachedTreeSize = po.size
        }

        let start = DateTime.now()

        let placed = HashSet<Int64>()
        let prefixOps = ArrayList<Int64>()
        pruningTreeDFSRecursive(treeRoot, po, history, placed, prefixOps, None, 0)

        let totalChecks = operationChecks + cacheChecks
        if (verbose) {
            println("Operation checks: ${operationChecks}, cache hits: ${cacheChecks}, pruned: ${prunedBranches} (pruning DFS on permutation tree).")
            if (totalChecks > 0) {
                println("Time per check ${(DateTime.now() - start) / totalChecks}.")
            }
        }

        ConsistencyValidationResult(
            consistencyModel: consistencyModel,
            matchingPermutation: foundPermutation,
            operationChecks: operationChecks,
            prunedBranches: prunedBranches,
            cacheHits: cacheChecks,
            history: history,
            partialOrder: po,
            deadEndDepth: deepestDeadEndDepth,
            deadEndPrefix: deepestDeadEndPrefix.toArray(),
            deadEndMismatches: deepestDeadEndMismatches.toArray()
        )
    }

    private func pruningTreeDFSRecursive(
        node: PermutationNode,
        po: PartialOrder,
        hist: ConcurrentHistory,
        placed: HashSet<Int64>,
        prefixOps: ArrayList<Int64>,
        state: ?Any,
        depth: Int64
    ): Bool {
        let available = po.getAvailableOperations(placed)

        if (available.size == 0) {
            if (prefixOps.size > 0) {
                foundPermutation = copyPermutation(prefixOps)
                return true
            }
            return false
        }
        var currentState: ?Any = state
        let localMismatches = ArrayList<MismatchInfo>()
        for (opId in available) {
            let child = node.ensureChild(opId)

            if (child.runId != currentRunId) {
                child.runId = currentRunId
            }

            let actorId = opId / 1000
            let opIndex = opId % 1000
            let inv = hist.getInvocation(actorId, opIndex) ?? return false

            var recurseState: ?Any = None
            var isMatch = false
            var consumedState = false
            var seqResult: ?EquatableObj = None

            if (let Some(cachedRes) <- child.cachedResult) {
                seqResult = Some(cachedRes)
                isMatch = cachedRes.eq(inv.result)
                cacheChecks += 1
            } else {
                let builtState = currentState.getOrDefault {
                    let s = createFreshInstance()
                    for (prefixOpId in prefixOps) {
                        let prefixInv = hist.getInvocation(prefixOpId / 1000, prefixOpId % 1000) ?? return false
                        reExecuteOperation(s, prefixInv)
                    }
                    s
                }

                let actualResult = reExecuteOperation(builtState, inv)
                seqResult = Some(actualResult)
                child.cachedResult = actualResult
                operationChecks += 1
                isMatch = actualResult.eq(inv.result)
                recurseState = Some(builtState)
                consumedState = true
            }

            if (!isMatch) {
                if (let Some(expected) <- seqResult) {
                    localMismatches.add(MismatchInfo(operation: inv, expectedResult: expected))
                }
                prunedBranches += 1
                if (consumedState) { currentState = None }
                continue
            }

            placed.add(opId)
            prefixOps.add(opId)
            if (pruningTreeDFSRecursive(child, po, hist, placed, prefixOps, recurseState, depth + 1)) {
                return true
            }
            if (consumedState) { currentState = None }
            placed.remove(opId)
            prefixOps.remove(at: prefixOps.size - 1)
        }

        if (available.size > 0 && localMismatches.size == available.size) {
            if (depth > deepestDeadEndDepth) {
                deepestDeadEndDepth = depth
                deepestDeadEndPrefix = copyPermutation(prefixOps)
                deepestDeadEndMismatches = ArrayList<MismatchInfo>()
                for (m in localMismatches) {
                    deepestDeadEndMismatches.add(m)
                }
            }
        }

        return false
    }

    private func copyPermutation(permutation: ArrayList<Int64>): ArrayList<Int64> {
        let copy = ArrayList<Int64>()
        for (opId in permutation) {
            copy.add(opId)
        }
        copy
    }

    private func factorial(n: Int64): Int64 {
        if (n <= 1) { return 1 }
        var result: Int64 = 1
        for (i in 2..n + 1) {
            result *= i
        }
        result
    }
}