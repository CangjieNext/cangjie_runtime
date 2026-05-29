/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *  * This source file is part of the Cangjie project, licensed under Apache-2.0
 *  * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.collection.ArrayList
import std.collection.HashMap
import std.collection.HashSet
import std.time.DateTime

public class ConsistencyValidator {
    public ConsistencyValidator(
        public let consistencyModel!: ConsistencyModel,
        private let createFreshInstance!: () -> Any,
        private let reExecuteOperation!: (Any, OperationInvocation) -> EquatableObj
    ) {}
    
    private var operationChecks: Int64 = 0
    private var prunedBranches: Int64 = 0
    private var cachedCount: Int64 = 0
    private var foundPermutation: ?ArrayList<Int64> = None
    private var history: ?ConcurrentHistory = None
    private var partialOrder: ?PartialOrder = None
    private var permutationResultCache: HashMap<String, ArrayList<EquatableObj>> = HashMap<String, ArrayList<EquatableObj>>()
    private var cachedPermutationList: ArrayList<ArrayList<Int64>> = ArrayList<ArrayList<Int64>>()
    private var cachedCommonPrefixLengths: ArrayList<Int64> = ArrayList<Int64>()
    private var cachedPartialOrder: ?PartialOrder = None
    
    public func validate(history: ConcurrentHistory): ConsistencyValidationResult {
        this.history = history
        operationChecks = 0
        prunedBranches = 0
        cachedCount = 0
        foundPermutation = None
        
        let po = computePartialOrder(history, consistencyModel)
        this.partialOrder = po
        
        if (let Serializability <- consistencyModel && po.size > 10) {
            return ConsistencyValidationResult(
                consistencyModel: consistencyModel,
                matchingPermutation: None,
                operationChecks: 0,
                prunedBranches: 0,
                cachedPermutations: 0,
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
        
        let isCacheableModel = match (consistencyModel) {
            case SequentialConsistency => true
            case Serializability => true
            case _ => false
        }
        let start = DateTime.now()
        
        if (isCacheableModel) {
            var useCache = false
            if (cachedPermutationList.size > 0) {
                match (cachedPartialOrder) {
                    case Some(cachedPO) =>
                        if (cachedPO.size == po.size && cachedPO.predecessorPairCount == po.predecessorPairCount) {
                            useCache = true
                        }
                    case None => ()
                }
            }
            
            if (useCache) {
                validateWithCachedList(history, po)
                println("Operation checks: ${operationChecks}, pruned: ${prunedBranches}, cache hits: ${cachedCount} (permutation list cached, ${cachedPermutationList.size} total).")
            } else {
                cachedCommonPrefixLengths = ArrayList<Int64>()
                cachedPermutationList = TopologicalPermutationGenerator.generateAll(po, cachedCommonPrefixLengths)
                cachedPartialOrder = Some(po)
                validateWithCachedList(history, po)
                println("Operation checks: ${operationChecks}, pruned: ${prunedBranches}, cache hits: ${cachedCount} (permutation list fresh, ${cachedPermutationList.size} total).")
            }
        } else {
            pruningDFSValidate(history, po)
            println("Operation checks: ${operationChecks}, pruned: ${prunedBranches}, cache hits: ${cachedCount} (pruning DFS for linearizability).")
        }
        
        if (operationChecks > 0) {
            println("Time per operation check ${(DateTime.now() - start) / operationChecks}.")
        }
        
        ConsistencyValidationResult(
            consistencyModel: consistencyModel,
            matchingPermutation: foundPermutation,
            operationChecks: operationChecks,
            prunedBranches: prunedBranches,
            cachedPermutations: cachedCount,
            history: history,
            partialOrder: po
        )
    }
    
    private func pruningDFSValidate(hist: ConcurrentHistory, po: PartialOrder): Unit {
        let state = createFreshInstance()
        let placed = HashSet<Int64>()
        let prefixOps = ArrayList<Int64>()
        pruningDFSRecursive(po, hist, placed, prefixOps, state, 0)
    }
    
    private func pruningDFSRecursive(
        po: PartialOrder,
        hist: ConcurrentHistory,
        placed: HashSet<Int64>,
        prefixOps: ArrayList<Int64>,
        state: Any,
        depth: Int64
    ): Bool {
        let available = po.getAvailableOperations(placed)
        
        if (available.size == 0) {
            if (prefixOps.size > 0) {
                foundPermutation = copyPermutation(prefixOps)
                cacheSuccessfulPermutation(prefixOps, hist)
                return true
            }
            return false
        }
        
        var currentState = state
        for (i in 0..available.size) {
            let opId = available[i]
            
            if (i > 0) {
                currentState = createFreshInstance()
                for (j in 0..prefixOps.size) {
                    let prefixOpId = prefixOps[j]
                    match (hist.getInvocation(prefixOpId / 1000, prefixOpId % 1000)) {
                        case Some(inv) => reExecuteOperation(currentState, inv)
                        case None => return false
                    }
                }
            }
            
            let actorId = opId / 1000
            let opIndex = opId % 1000
            match (hist.getInvocation(actorId, opIndex)) {
                case Some(inv) =>
                    let actualResult = reExecuteOperation(currentState, inv)
                    operationChecks += 1
                    
                    if (!actualResult.eq(inv.result)) {
                        prunedBranches += 1
                        continue
                    }
                case None => return false
            }
            
            placed.add(opId)
            prefixOps.add(opId)
            
            if (pruningDFSRecursive(po, hist, placed, prefixOps, currentState, depth + 1)) {
                return true
            }
            
            placed.remove(opId)
            prefixOps.remove(at: prefixOps.size - 1)
        }
        
        return false
    }
    
    private func validateWithCachedList(hist: ConcurrentHistory, po: PartialOrder): Unit {
        for (i in 0..cachedPermutationList.size) {
            let perm = cachedPermutationList[i]
            let commonPrefixLen = cachedCommonPrefixLengths[i]
            
            let key = permutationKey(perm)
            var actualResults: ArrayList<EquatableObj> = ArrayList<EquatableObj>()
            var fromCache = false
            
            match (permutationResultCache.get(key)) {
                case Some(cached) =>
                    cachedCount += 1
                    actualResults = cached
                    fromCache = true
                case None => ()
            }
            
            if (!fromCache) {
                let state = createFreshInstance()
                for (opId in perm) {
                    match (hist.getInvocation(opId / 1000, opId % 1000)) {
                        case Some(inv) =>
                            let result = reExecuteOperation(state, inv)
                            actualResults.add(result)
                        case None => return
                    }
                }
                permutationResultCache[key] = actualResults
            }
            
            var allMatch = true
            for (j in commonPrefixLen..perm.size) {
                operationChecks += 1
                let opId = perm[j]
                match (hist.getInvocation(opId / 1000, opId % 1000)) {
                    case Some(inv) =>
                        if (!actualResults[j].eq(inv.result)) {
                            allMatch = false
                            break
                        }
                    case None => return
                }
            }
            
            if (allMatch) {
                foundPermutation = Some(perm)
                return
            }
        }
    }
    
    private func copyPermutation(permutation: ArrayList<Int64>): ArrayList<Int64> {
        let copy = ArrayList<Int64>()
        for (opId in permutation) {
            copy.add(opId)
        }
        copy
    }
    
    private func cacheSuccessfulPermutation(permutation: ArrayList<Int64>, hist: ConcurrentHistory): Unit {
        let key = permutationKey(permutation)
        if (permutationResultCache.contains(key)) {
            return
        }
        let state = createFreshInstance()
        let results = ArrayList<EquatableObj>()
        for (opId in permutation) {
            match (hist.getInvocation(opId / 1000, opId % 1000)) {
                case Some(inv) =>
                    let result = reExecuteOperation(state, inv)
                    results.add(result)
                case None => return
            }
        }
        permutationResultCache[key] = results
    }
    
    private func permutationKey(permutation: ArrayList<Int64>): String {
        let sb = StringBuilder()
        for (i in 0..permutation.size) {
            if (i > 0) {
                sb.append("_")
            }
            sb.append("${permutation[i]}")
        }
        sb.toString()
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