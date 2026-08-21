/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

/**
 * Consistency model used to validate the recorded concurrent history of a scenario.
 *
 * The chosen model determines how the partial order over operation invocations is
 * derived: it constrains which permutations the consistency validator must search
 * when looking for a sequential re-execution that reproduces every observed
 * result. Stronger models admit fewer linearizations (faster validation, can
 * detect more bugs) but reject more concurrent executions (may flag valid
 * non-sequentially-consistent designs as violations).
 *
 * The three supported models, from strongest to weakest:
 * - `Linearizability` — preserves per-actor program order *and* cross-thread
 *   real-time happens-before edges (captured via vector clocks at each
 *   operation's invocation and response).
 * - `SequentialConsistency` — preserves per-actor program order only.
 * - `Serializability` — preserves nothing; the validator searches *every*
 *   permutation of the operations. Useful for data structures whose methods are
 *   intentionally not sequentially consistent (e.g. a stale `size` on a
 *   concurrent queue).
 *
 * Hierarchy: a history that is linearizable is also sequentially consistent and
 * serializable; a history that is sequentially consistent is also serializable.
 * Therefore `lin.passed ⇒ sc.passed ⇒ ser.passed`.
 */
public enum ConsistencyModel {
    /// Preserve per-actor program order only. Cross-thread real-time ordering is
    /// not enforced, so two operations that overlap in real time may be reordered
    /// relative to each other in the linearization.
    | SequentialConsistency
    /// Strongest model: preserve per-actor program order *and* cross-thread
    /// real-time happens-before (captured via per-operation vector clocks).
    /// Required to detect store-buffer effects and other real-time-ordering bugs.
    | Linearizability
    /// Weakest model: derive an empty partial order so the validator searches
    /// every permutation of the operations. Use this when the SUT's methods are
    /// intentionally not sequentially consistent (e.g. a `size` method that may
    /// return a stale or future value under concurrent modification).
    | Serializability
}

protected func modelDisplayName(model: ConsistencyModel): String {
    match (model) {
        case SequentialConsistency => "sequentially consistent"
        case Linearizability => "linearizable"
        case Serializability => "serializable"
    }
}

protected func modelCapitalizedName(model: ConsistencyModel): String {
    match (model) {
        case SequentialConsistency => "Sequentially consistent"
        case Linearizability => "Linearizable"
        case Serializability => "Serializable"
    }
}

protected func modelNotName(model: ConsistencyModel): String {
    match (model) {
        case SequentialConsistency => "NOT sequentially consistent"
        case Linearizability => "NOT linearizable"
        case Serializability => "NOT serializable"
    }
}