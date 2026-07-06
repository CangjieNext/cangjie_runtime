/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.collection.ArrayList
import std.collection.HashSet
import std.collection.HashMap

protected class PredecessorPair {
    protected PredecessorPair(
        let opId!: Int64,
        let predecessorId!: Int64
    ) {}
}

protected class PartialOrder {
    private let predecessors: ArrayList<PredecessorPair> = ArrayList<PredecessorPair>()
    private let predecessorMap: HashMap<Int64, ArrayList<Int64>> = HashMap<Int64, ArrayList<Int64>>()
    private let allOps: ArrayList<Int64> = ArrayList<Int64>()
    
    protected PartialOrder() {}
    
    func addPredecessor(opId: Int64, predecessorId: Int64): Unit {
        predecessors.add(PredecessorPair(opId: opId, predecessorId: predecessorId))
        if (!predecessorMap.contains(opId)) {
            predecessorMap[opId] = ArrayList<Int64>()
        }
        predecessorMap[opId].add(predecessorId)
        if (!allOps.contains(opId)) {
            allOps.add(opId)
        }
        if (!allOps.contains(predecessorId)) {
            allOps.add(predecessorId)
        }
    }
    
    func addOperation(opId: Int64): Unit {
        if (!allOps.contains(opId)) {
            allOps.add(opId)
        }
    }
    
    func isAvailable(opId: Int64, placed: HashSet<Int64>): Bool {
        match (predecessorMap.get(opId)) {
            case Some(predList) =>
                for (predId in predList) {
                    if (!placed.contains(predId)) {
                        return false
                    }
                }
            case None => ()
        }
        true
    }
    
    func getAvailableOperations(placed: HashSet<Int64>): ArrayList<Int64> {
        let available = ArrayList<Int64>()
        for (opId in allOps) {
            if (!placed.contains(opId) && isAvailable(opId, placed)) {
                available.add(opId)
            }
        }
        available
    }
    
    protected func predecessorCount(opId: Int64): Int64 {
        match (predecessorMap.get(opId)) {
            case Some(predList) => predList.size
            case None => 0
        }
    }
    
    prop predecessorPairCount: Int64 {
        get() { predecessors.size }
    }
    
    protected prop size: Int64 {
        get() { allOps.size }
    }
    
    protected prop operations: ArrayList<Int64> {
        get() { allOps }
    }
}

protected func computePartialOrder(history: ConcurrentHistory, model: ConsistencyModel): PartialOrder {
    match (model) {
        case SequentialConsistency => perActorOrder(history)
        case Linearizability => perActorAndVCOrder(history)
        case Serializability => emptyOrder(history)
    }
}

private func perActorOrder(history: ConcurrentHistory): PartialOrder {
    let po = PartialOrder()
    for (actorId in 0..history.actorCount) {
        let invocations = history.getActorInvocations(actorId) ?? continue   
        for (opIndex in 0..invocations.size) {
            let opId = actorId * 1000 + opIndex
            po.addOperation(opId)
            for (prevOpIndex in 0..opIndex) {
                po.addPredecessor(opId, actorId * 1000 + prevOpIndex)
            }
        }
    }
    po
}

private func perActorAndVCOrder(history: ConcurrentHistory): PartialOrder {
    let po = perActorOrder(history)
    
    let allOps = ArrayList<OperationInvocation>()
    for (actorId in 0..history.actorCount) {
        if (let Some(invocations) <- history.getActorInvocations(actorId)) {
            for (inv in invocations) {
                allOps.add(inv)
            }
        }
    }
    
    for (i in 0..allOps.size) {
        for (j in 0..allOps.size) {
            let opA = allOps[i]
            let opB = allOps[j]
            if (opA.actorId != opB.actorId) {
                if (vcHappensBefore(opA, opB)) {
                    po.addPredecessor(opB.operationId, opA.operationId)
                }
            }
        }
    }
    
    po
}

private func vcHappensBefore(opA: OperationInvocation, opB: OperationInvocation): Bool {
    let respVC = opA.responseVC ?? return false
    let invVC = opB.invocationVC ?? return false
    
    if (respVC.size != invVC.size) {
        return false
    }
    
    var allLessOrEqual = true
    var atLeastOneStrict = false
    
    for (k in 0..respVC.size) {
        if (respVC[k] > invVC[k]) {
            allLessOrEqual = false
            break
        }
        if (respVC[k] < invVC[k]) {
            atLeastOneStrict = true
        }
    }
    
    allLessOrEqual && atLeastOneStrict
}

private func emptyOrder(history: ConcurrentHistory): PartialOrder {
    let po = PartialOrder()
    for (actorId in 0..history.actorCount) {
        if (let Some(invocations) <- history.getActorInvocations(actorId)) {
            for (opIndex in 0..invocations.size) {
                po.addOperation(actorId * 1000 + opIndex)
            }
        }
    }
    po
}