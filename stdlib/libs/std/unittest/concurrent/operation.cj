/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

public class Operation {
    Operation(
        public let name!: String,
        public let id!: Int64 = 0,
        public let body!: () -> EquatableObj,
    ) {}
    
    func executeWrapped(): EquatableObj {
        body()
    }
}

public func operation<T>(name: String, body: () -> T): Operation where T <: Equatable<T> {
    Operation(name: name) { => execResultOf(body) }
}


public class OperationInvocation {
    public OperationInvocation(
        public let actorId!: Int64,
        public let operationIndex!: Int64,
        public let operationName!: String,
        public let result!: EquatableObj,
        public let invocationVC!: ?Array<Int64> = None,
        public let responseVC!: ?Array<Int64> = None
    ) {}
    
    public prop operationId: Int64 {
        get() { actorId * 1000 + operationIndex }
    }
}