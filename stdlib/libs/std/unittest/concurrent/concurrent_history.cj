/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.collection.ArrayList
import std.collection.HashMap

public class ConcurrentHistory {
    private let actorInvocations: HashMap<Int64, ArrayList<OperationInvocation>> = HashMap<Int64, ArrayList<OperationInvocation>>()
    
    public func recordActorInvocations(
        actorId: Int64,
        invocations: ArrayList<OperationInvocation>
    ): Unit {
        if (!actorInvocations.contains(actorId)) {
            actorInvocations[actorId] = ArrayList<OperationInvocation>()
        }
        for (inv in invocations) {
            actorInvocations[actorId].add(inv)
        }
    }
    
    public func groupByActor(): HashMap<Int64, ArrayList<OperationInvocation>> {
        actorInvocations
    }
    
    public func getActorInvocations(actorId: Int64): ?ArrayList<OperationInvocation> {
        actorInvocations[actorId]
    }
    
    public func getInvocation(actorId: Int64, index: Int64): ?OperationInvocation {
        match (actorInvocations.get(actorId)) {
            case Some(ops) =>
                ops.get(index)
            case None => None
        }
    }
    
    public func clear(): Unit {
        actorInvocations.clear()
    }
    
    public prop size: Int64 {
        get() {
            var total: Int64 = 0
            for (actorId in actorInvocations.keys()) {
                total += actorInvocations[actorId].size
            }
            total
        }
    }
    
    public prop actorCount: Int64 {
        get() { actorInvocations.size }
    }
}