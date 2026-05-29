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

public class TopologicalPermutationGenerator {
    
    public static func generateAll(partialOrder: PartialOrder, commonPrefixLengths: ArrayList<Int64>): ArrayList<ArrayList<Int64>> {
        let perms = ArrayList<ArrayList<Int64>>()
        let ops = partialOrder.operations
        if (ops.size == 0) {
            return perms
        }
        
        collectRecursive(
            partialOrder,
            HashSet<Int64>(),
            ArrayList<Int64>(),
            perms,
            commonPrefixLengths,
            0
        )
        
        perms
    }
    
    private static func collectRecursive(
        partialOrder: PartialOrder,
        placed: HashSet<Int64>,
        currentPermutation: ArrayList<Int64>,
        result: ArrayList<ArrayList<Int64>>,
        commonPrefixLengths: ArrayList<Int64>,
        branchDepth: Int64
    ): Unit {
        let available = partialOrder.getAvailableOperations(placed)
        
        if (available.size == 0) {
            if (currentPermutation.size > 0) {
                let copy = ArrayList<Int64>()
                for (opId in currentPermutation) {
                    copy.add(opId)
                }
                result.add(copy)
                commonPrefixLengths.add(branchDepth)
            }
        } else {
            var currentBranchDepth = branchDepth
            for (opId in available) {
                placed.add(opId)
                currentPermutation.add(opId)
                
                collectRecursive(
                    partialOrder,
                    placed,
                    currentPermutation,
                    result,
                    commonPrefixLengths,
                    currentBranchDepth
                )
                
                currentPermutation.remove(at: currentPermutation.size - 1)
                placed.remove(opId)
                
                currentBranchDepth = currentPermutation.size
            }
        }
    }
    
    public static func generateWithCallback(
        partialOrder: PartialOrder,
        callback: (ArrayList<Int64>) -> Bool
    ): Unit {
        let ops = partialOrder.operations
        if (ops.size == 0) {
            return
        }
        
        generateRecursive(
            partialOrder,
            HashSet<Int64>(),
            ArrayList<Int64>(),
            callback
        )
    }
    
    private static func generateRecursive(
        partialOrder: PartialOrder,
        placed: HashSet<Int64>,
        currentPermutation: ArrayList<Int64>,
        callback: (ArrayList<Int64>) -> Bool
    ): Bool {
        let available = partialOrder.getAvailableOperations(placed)
        
        if (available.size == 0) {
            if (currentPermutation.size > 0) {
                callback(currentPermutation)
            } else {
                true
            }
        } else {
            var shouldContinue = true
            for (opId in available) {
                placed.add(opId)
                currentPermutation.add(opId)
                
                shouldContinue = generateRecursive(
                    partialOrder,
                    placed,
                    currentPermutation,
                    callback
                )
                
                currentPermutation.remove(at: currentPermutation.size - 1)
                placed.remove(opId)
                
                if (!shouldContinue) {
                    break
                }
            }
            shouldContinue
        }
    }
    
    public static func countPermutations(partialOrder: PartialOrder): Int64 {
        let ops = partialOrder.operations
        if (ops.size == 0) {
            return 0
        }
        
        countRecursive(partialOrder, HashSet<Int64>())
    }
    
    private static func countRecursive(
        partialOrder: PartialOrder,
        placed: HashSet<Int64>
    ): Int64 {
        let available = partialOrder.getAvailableOperations(placed)
        
        if (available.size == 0) {
            if (placed.size > 0) {
                1
            } else {
                0
            }
        } else {
            var total: Int64 = 0
            for (opId in available) {
                placed.add(opId)
                total += countRecursive(partialOrder, placed)
                placed.remove(opId)
            }
            total
        }
    }
}