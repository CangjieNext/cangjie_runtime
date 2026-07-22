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
import std.collection.HashMap
import std.unittest.common.PrettyTable
import std.unittest.common.PrettyText

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
        getMismatchTable()
    }

    public func getMismatchTable(): ?String {
        if (deadEndMismatches.isEmpty()) {
            return None
        }

        let prefixSet = HashSet<Int64>()
        for (opId in deadEndPrefix) {
            prefixSet.add(opId)
        }
        let mismatchMap = HashMap<Int64, MismatchInfo>()
        for (m in deadEndMismatches) {
            mismatchMap[m.operation.operationId] = m
        }

        let actorIds = ArrayList<Int64>()
        for (actorId in history.groupByActor().keys()) {
            actorIds.add(actorId)
        }
        actorIds.sort()

        let topoOrder = topologicalSort()

        let cliques = buildCliques(topoOrder)

        let pt = PrettyTable(sep: " ", separateColumnsNames: false, leftAligneAll: true, minColumnWidth: 1)
        for (actorId in actorIds) {
            pt.addColumn("Actor ${actorId}")
        }

        let nextUnshownOpIndex = HashMap<Int64, Int64>()
        for (actorId in actorIds) {
            nextUnshownOpIndex[actorId] = 0
        }

        for (clique in cliques) {
            // Group operations in this clique by actor, preserving per-actor
            // order. A single clique (set of mutually-concurrent operations) may
            // contain more than one operation per actor — this happens e.g.
            // under Serializability, where the partial order is empty and every
            // operation is concurrent with every other. Render one sub-row per
            // "level" so that every operation is shown.
            let opsByActor = HashMap<Int64, ArrayList<OperationInvocation>>()
            for (opId in clique) {
                let actorId = opId / 1000
                match (history.getInvocation(actorId, opId % 1000)) {
                    case Some(inv) =>
                        if (!opsByActor.contains(actorId)) {
                            opsByActor[actorId] = ArrayList<OperationInvocation>()
                        }
                        opsByActor[actorId].add(inv)
                    case None => ()
                }
            }

            var maxOps: Int64 = 0
            for (actorId in actorIds) {
                match (opsByActor.get(actorId)) {
                    case Some(list) => if (list.size > maxOps) { maxOps = list.size }
                    case None => ()
                }
            }
            if (maxOps == 0) {
                maxOps = 1
            }

            for (k in 0..maxOps) {
                pt.nextRow()
                for (actorId in actorIds) {
                    let opsForActor = opsByActor.get(actorId) ?? ArrayList<OperationInvocation>()
                    if (k < opsForActor.size) {
                        let inv = opsForActor[k]
                        pt.addCell(formatCell(inv, prefixSet, mismatchMap))
                        nextUnshownOpIndex[actorId] = inv.operationIndex + 1
                    } else {
                        let nextIdx = nextUnshownOpIndex.get(actorId) ?? 0
                        if (nextIdx == 0) {
                            pt.addCell("-------")
                        } else {
                            match (history.getInvocation(actorId, nextIdx)) {
                                case Some(nextInv) =>
                                    var allPred = true
                                    for (opId in clique) {
                                        if (!partialOrder.isPredecessorOf(opId, nextInv.operationId)) {
                                            allPred = false
                                            break
                                        }
                                    }
                                    if (allPred) {
                                        pt.addCell("-------")
                                    } else {
                                        pt.addCell("...")
                                    }
                                case None =>
                                    pt.addCell("-------")
                            }
                        }
                    }
                }
            }
        }

        let sb = StringBuilder()
        sb.append(partialOrder.formatConstraints(consistencyModel))
        sb.append("\n")
        sb.append(PrettyText.of(pt).toString())
        sb.toString()
    }

    private func formatCell(
        inv: OperationInvocation,
        prefixSet: HashSet<Int64>,
        mismatchMap: HashMap<Int64, MismatchInfo>
    ): String {
        let opId = inv.operationId
        if (prefixSet.contains(opId)) {
            "${inv.operationName}: ${inv.result} \u{2713}"
        } else if (let Some(m) <- mismatchMap.get(opId)) {
            "${inv.operationName}: ${inv.result} \u{2260} ${m.expectedResult} \u{2717}"
        } else {
            "${inv.operationName}: ${inv.result}"
        }
    }

    private func topologicalSort(): ArrayList<Int64> {
        let result = ArrayList<Int64>()
        let placed = HashSet<Int64>()
        while (result.size < partialOrder.size) {
            let available = partialOrder.getAvailableOperations(placed)
            if (available.size == 0) {
                break
            }
            for (opId in available) {
                result.add(opId)
                placed.add(opId)
            }
        }
        result
    }

    private func buildCliques(topoOrder: ArrayList<Int64>): ArrayList<ArrayList<Int64>> {
        let heights = computeHeights(topoOrder)
        var maxHeight: Int64 = 0
        for (opId in topoOrder) {
            match (heights.get(opId)) {
                case Some(h) => if (h > maxHeight) { maxHeight = h }
                case None => ()
            }
        }
        if (maxHeight == 0) {
            maxHeight = 1
        }

        let depths = computeDepths(topoOrder)

        let numSlots = maxHeight
        let slots = ArrayList<ArrayList<Int64>>(Array<ArrayList<Int64>>(numSlots, { _ => ArrayList<Int64>() }))

        let actorOps = HashMap<Int64, ArrayList<Int64>>()
        for (opId in topoOrder) {
            let actorId = opId / 1000
            if (!actorOps.contains(actorId)) {
                actorOps[actorId] = ArrayList<Int64>()
            }
            actorOps[actorId].add(opId)
        }

        for (opId in topoOrder) {
            let h = heights.get(opId) ?? 1
            let d = depths.get(opId) ?? 1
            let earliest = h
            var latest = numSlots - d + 1
            if (latest < earliest) {
                latest = earliest
            }

            let actorId = opId / 1000
            let opsForActor = actorOps.get(actorId) ?? ArrayList<Int64>()
            let isLastOp = opsForActor.size > 0 && opId == opsForActor[opsForActor.size - 1]
            let isOnlyOp = opsForActor.size == 1

            let targetSlot = if (isLastOp && !isOnlyOp) {
                latest
            } else {
                earliest
            }

            var slotIdx = targetSlot
            while (slotIdx >= earliest && slotIdx >= 1) {
                let slot = slots[slotIdx - 1]
                var allConcurrent = true
                for (memberId in slot) {
                    if (!partialOrder.areConcurrent(memberId, opId)) {
                        allConcurrent = false
                        break
                    }
                }
                if (allConcurrent) {
                    slot.add(opId)
                    break
                }
                slotIdx--
            }
            if (slotIdx < earliest || slotIdx < 1) {
                slotIdx = targetSlot
                while (slotIdx <= latest && slotIdx <= numSlots) {
                    let slot = slots[slotIdx - 1]
                    var allConcurrent = true
                    for (memberId in slot) {
                        if (!partialOrder.areConcurrent(memberId, opId)) {
                            allConcurrent = false
                            break
                        }
                    }
                    if (allConcurrent) {
                        slot.add(opId)
                        break
                    }
                    slotIdx++
                }
                if (slotIdx > latest || slotIdx > numSlots) {
                    slots[earliest - 1].add(opId)
                }
            }
        }

        slots
    }

    private func computeHeights(topoOrder: ArrayList<Int64>): HashMap<Int64, Int64> {
        let heights = HashMap<Int64, Int64>()
        for (opId in topoOrder) {
            var maxH: Int64 = 0
            match (partialOrder.getPredecessors(opId)) {
                case Some(preds) =>
                    for (predId in preds) {
                        match (heights.get(predId)) {
                            case Some(h) => if (h > maxH) { maxH = h }
                            case None => ()
                        }
                    }
                case None => ()
            }
            heights[opId] = maxH + 1
        }
        heights
    }

    private func computeDepths(topoOrder: ArrayList<Int64>): HashMap<Int64, Int64> {
        let depths = HashMap<Int64, Int64>()
        let allOps = partialOrder.operations
        for (i in 0..topoOrder.size) {
            let opId = topoOrder[topoOrder.size - 1 - i]
            var maxD: Int64 = 0
            for (otherId in allOps) {
                if (partialOrder.isPredecessorOf(opId, otherId)) {
                    match (depths.get(otherId)) {
                        case Some(d) => if (d > maxD) { maxD = d }
                        case None => ()
                    }
                }
            }
            depths[opId] = maxD + 1
        }
        depths
    }
}
