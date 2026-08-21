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
        protected let operation!: OperationInvocation,
        protected let expectedResult!: EquatableObj
    ) {}
}

/**
 * Per-iteration verdict of the consistency validator: was the recorded
 * concurrent history of this iteration consistent under the chosen
 * `ConsistencyModel`, and if not, why.
 *
 * A `ConsistencyValidationResult` is produced by `ConsistencyValidator.validate`
 * (invoked once per iteration from the scenario runner). It exposes:
 * - the model under which validation ran;
 * - whether a valid linearization was found (`isConsistent`);
 * - the matching permutation (on success) or the deepest dead-end prefix and
 *   per-branch mismatches (on failure);
 * - validation statistics (`operationChecks`, `prunedBranches`, `cacheHits`)
 *   for profiling;
 * - an optional `exception` if validation could not run (e.g. the
 *   `Serializability` `n > 10` guard);
 * - public renderers `report()` (one-line verdict) and `getMismatchDetails()`
 *   (per-actor mismatch table with `✓` / `≠` / `✗` markers and `-------`
 *   barriers).
 */
public class ConsistencyValidationResult {
    protected ConsistencyValidationResult(
        /** Consistency model under which validation was performed. */
        public let consistencyModel!: ConsistencyModel,
        /**
         * The linearization (as a list of operation ids) that reproduced every
         * observed result, if one was found; `None` otherwise.
         */
        public let matchingPermutation!: ?ArrayList<Int64>,
        protected let operationChecks!: Int64,
        protected let prunedBranches!: Int64,
        protected let cacheHits!: Int64,
        protected let history!: ConcurrentHistory,
        protected let partialOrder!: PartialOrder,
        /**
         * Exception that prevented validation from running, if any (e.g. the
         * `Serializability` `n > 10` guard returns an `IllegalArgumentException`
         * rather than throwing, so a misconfigured scenario does not abort the
         * test runner). `None` for normal validation outcomes.
         */
        public let exception!: ?Exception = None,
        protected let deadEndDepth!: Int64 = -1,
        protected let deadEndPrefix!: Array<Int64> = Array<Int64>(),
        protected let deadEndMismatches!: Array<MismatchInfo> = Array<MismatchInfo>()
    ) {}
    
    /**
     * `true` iff a sequential re-execution reproducing every observed result
     * was found under the chosen consistency model.
     *
     * Equivalently, `matchingPermutation.isSome()`. A `false` value means the
     * recorded concurrent history is *not* consistent under the model — a
     * genuine consistency violation that is never a false positive (the
     * validator only reports failure after exhausting every legal
     * linearization without finding a match).
     */
    public prop isConsistent: Bool {
        get() { matchingPermutation.isSome() }
    }
    
    /**
     * Render the one-line verdict for this iteration as a `PrettyText`.
     *
     * Three forms:
     * - if `exception` is set: `<Model> validation error: <message>` —
     *   validation could not run;
     * - if `isConsistent`: `<Model> - found matching ordering after N
     *   operation checks (P branches pruned, C cache hits)`;
     * - otherwise: `NOT <model> - no valid ordering found after N operation
     *   checks (P branches pruned, C cache hits)`.
     *
     * The statistics let users profile validation cost and identify the most
     * expensive iterations.
     *
     * @return a `PrettyText` rendering of the one-line verdict
     */
    public func report(): PrettyText {
        if (let Some(e) <- exception) {
            PrettyText("${modelCapitalizedName(consistencyModel)} validation error: ${e.message}")
        } else if (isConsistent) {
            PrettyText("${modelCapitalizedName(consistencyModel)} - found matching ordering after ${operationChecks} operation checks (${prunedBranches} branches pruned, ${cacheHits} cache hits)")
        } else {
            PrettyText("${modelNotName(consistencyModel)} - no valid ordering found after ${operationChecks} operation checks (${prunedBranches} branches pruned, ${cacheHits} cache hits)")
        }
    }

    protected func getOperationHistory(): String {
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

    /**
     * Render the per-actor mismatch table for the deepest dead-end reached
     * during the consistency-validation DFS, if any.
     *
     * Returns `None` when there is no mismatch to report — either because the
     * iteration was consistent (`isConsistent == true`), because validation
     * could not run (`exception` is set), or because no dead-end was recorded
     * (the DFS found a match without ever pruning a branch). Otherwise
     * returns a `PrettyText` containing:
     * - a header line documenting the partial-order constraints under the
     *   chosen model (e.g. `Ordering: no constraints (all reorderings are
     *   valid)` for `Serializability`, or `Ordering: per-actor + real-time
     *   (------- = barrier)` for `Linearizability`);
     * - a `PrettyTable` with one column per actor and one sub-row per "level"
     *   of the partial order, where each cell shows `<opName>: <observed-result>`
     *   followed by `✓` (the operation was successfully placed at this depth),
     *   `≠ <expected> ✗` (the operation's observed result has no equivalent in
     *   any sequential re-execution at this depth), or nothing (the operation
     *   is not yet reachable — `-------` barrier or `...` placeholder).
     *
     * The deepest dead-end pinpoints the operation whose mismatch makes the
     * entire history non-serializable; together with the one-line verdict from
     * `report()` it gives a precise per-actor diagnostic of the consistency
     * violation.
     *
     * @return a `PrettyText` rendering of the mismatch table, or `None` if
     *         there is no dead-end to report
     */
    public func getMismatchDetails(): ?PrettyText {
        getMismatchTable()
    }

    protected func getMismatchTable(): ?PrettyText {
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

        let result = PrettyText()
        result.append(partialOrder.formatConstraints(consistencyModel))
              .newLine()
              .append(PrettyText.of(pt))
        Some(result)
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
