/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

public class Operation<S> where S <: Any {
    Operation(
        let name!: String,
        let body!: (S) -> EquatableObj,
    ) {}

    func executeWrapped(state: S): EquatableObj {
        body(state)
    }
}

// Simple stateless factory: produces Operation<Unit> for capture-based scenarios.
// The body lambda ignores the (Unit) state placeholder.
public func operation<T>(name: String, body: () -> T): Operation<Unit> where T <: Equatable<T> {
    Operation<Unit>(name: name) { _: Unit => execResultOf(body) }
}

// State-aware factory: produces Operation<S> for typed per-iteration-state scenarios.
// The body lambda receives the per-iteration state instance of type S.
public func operation<T, S>(name: String, body: (S) -> T): Operation<S> where T <: Equatable<T>, S <: Any {
    Operation<S>(name: name) { state: S => execResultOf({ => body(state) }) }
}


public class OperationInvocation {
    protected OperationInvocation(
        public let actorId!: Int64,
        public let operationIndex!: Int64,
        public let operationName!: String,
        public let result!: EquatableObj,
        let invocationVC!: ?Array<Int64> = None,
        let responseVC!: ?Array<Int64> = None
    ) {}

    prop operationId: Int64 {
        get() { actorId * 1000 + operationIndex }
    }
}
