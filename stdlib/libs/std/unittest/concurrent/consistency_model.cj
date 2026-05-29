/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

public enum ConsistencyModel {
    | SequentialConsistency
    | Linearizability
    | Serializability
}

public func modelDisplayName(model: ConsistencyModel): String {
    match (model) {
        case SequentialConsistency => "sequentially consistent"
        case Linearizability => "linearizable"
        case Serializability => "serializable"
    }
}

public func modelCapitalizedName(model: ConsistencyModel): String {
    match (model) {
        case SequentialConsistency => "Sequentially consistent"
        case Linearizability => "Linearizable"
        case Serializability => "Serializable"
    }
}

public func modelNotName(model: ConsistencyModel): String {
    match (model) {
        case SequentialConsistency => "NOT sequentially consistent"
        case Linearizability => "NOT linearizable"
        case Serializability => "NOT serializable"
    }
}