/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

public class Operation<S> where S <: Any {
    let name: String
    let body: (?Any, S) -> EquatableObj
    let callArgs: ?Any

    init(name!: String, body!: (S) -> EquatableObj) {
        this.name = name
        this.body = { _, state => body(state) }
        this.callArgs = None
    }

    init(name!: String, callArgs!: Any, body!: (Any, S) -> EquatableObj) {
        this.name = name
        this.body = { args, state => body(args.getOrThrow(), state) }
        this.callArgs = callArgs
    }

    func executeWrapped(state: S): EquatableObj {
        body(callArgs, state)
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

public func operation<T, E, S>(name: String, callArgs: E, body: (E, S) -> T): Operation<S> where T <: Equatable<T>, S <: Any {
    Operation<S>(name: name, callArgs: callArgs) {
        args: Any, state: S => execResultOf({ => body((args as E).getOrThrow(), state) })
    }
}


public class OperationInvocation {
    protected OperationInvocation(
        public let actorId!: Int64,
        public let operationIndex!: Int64,
        public let operationName!: String,
        public let result!: EquatableObj,
        public let callArgs!: ?Any = None,
        let invocationVC!: ?Array<Int64> = None,
        let responseVC!: ?Array<Int64> = None
    ) {}

    prop operationId: Int64 {
        get() { actorId * 1000 + operationIndex }
    }
}
