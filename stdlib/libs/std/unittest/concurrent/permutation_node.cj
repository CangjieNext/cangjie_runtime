/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *  * This source file is part of the Cangjie project, licensed under Apache-2.0
 *  * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.collection.HashMap

class PermutationNode {
    public PermutationNode(
        public let operation!: ?Int64,
        public let depth!: Int64
    ) {}

    private let childrenByOp: HashMap<Int64, PermutationNode> = HashMap<Int64, PermutationNode>()

    var cachedResult: ?EquatableObj = None
    var runId: Int64 = 0

    public func ensureChild(opId: Int64): PermutationNode {
        childrenByOp.get(opId).getOrDefault {
            let newChild = PermutationNode(operation: Some(opId), depth: depth + 1)
            childrenByOp[opId] = newChild
            newChild
        }
    }
}