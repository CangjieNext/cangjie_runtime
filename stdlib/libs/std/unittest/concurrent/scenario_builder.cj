/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.unittest.concurrent

import std.collection.ArrayList
import std.unittest.common.Configuration

public class ScenarioBuilder<S> where S <: Any {
    private var _name: String = "unnamed_scenario"
    private var _iterations: Int64 = 1
    private var _stateFactory: ?() -> S = None
    // True iff the user explicitly called .stateFactory(...) (or used the typed
    // scenario<S>(name, factory) entry). Used by build() to decide whether to
    // construct the auto ConsistencyValidator: simple scenarios that only use
    // .reset { => var = ... } should preserve the original behavior of building
    // the validator iff _resetBlock is Some, while typed scenarios always build
    // it (each permutation-DFS restart gets a fresh isolated state instance).
    private var _stateFactoryExplicit: Bool = false
    // Tri-state: None = auto (parallel iff withState was used), Some(true)/Some(false)
    // = explicit user override. See parallelIterations(...) below.
    private var _parallelIterations: ?Bool = None
    // _resetBlock, _actors, _afterBlock, _verifyBlock are intentionally left without
    // `private` so that the simple `extend ScenarioBuilder<Unit>` block below can
    // manipulate them when S = Unit (Cangjie extensions cannot access private members).
    var _resetBlock: ?() -> Unit = None
    let _actors: ArrayList<Actor<S>> = ArrayList<Actor<S>>()
    var _afterBlock: ?(S) -> Unit = None
    var _verifyBlock: ?(S) -> Bool = None
    private var _configuration: Configuration = Configuration()
    private var _consistencyModel: ConsistencyModel = SequentialConsistency
    private var _explicitValidator: ?ConsistencyValidator = None

    public init() {}

    public init(name: String) {
        _name = name
    }

    public func name(n: String): ScenarioBuilder<S> {
        _name = n
        this
    }

    public func iterations(n: Int64): ScenarioBuilder<S> {
        if (n < 1) {
            throw IllegalArgumentException("Iteration count must be at least 1")
        }
        _iterations = n
        this
    }

    // Transitions this builder to a typed ScenarioBuilder<S2> with the given
    // per-iteration state factory. The state type S2 is inferred from the
    // factory argument. After this call, all state-aware methods (actor,
    // actorWithOp, actorWithOps, verify, after) accept lambdas that receive
    // the per-iteration state of type S2.
    //
    // Constraints:
    //   - May only be called once (throws if _stateFactoryExplicit is already
    //     true, e.g. from a previous withState call).
    //   - Must be called before any actors are added (throws if _actors is
    //     non-empty, since existing Unit-typed actors cannot be retyped to S2).
    //
    // Typical usage:
    //   scenario("queue_test")
    //       .withState({ => ConcurrentLinkedQueue<Int64>() })
    //       .actor({ q => q.add(1) })
    //       .build()
    public func withState<S2>(factory: () -> S2): ScenarioBuilder<S2> where S2 <: Any {
        if (_stateFactoryExplicit) {
            throw IllegalArgumentException(
                "withState can only be called once on a scenario builder"
            )
        }
        if (_actors.size > 0) {
            throw IllegalArgumentException(
                "withState must be called before adding actors"
            )
        }
        let newBuilder = ScenarioBuilder<S2>(_name)
        newBuilder._iterations = _iterations
        newBuilder._configuration = _configuration
        newBuilder._consistencyModel = _consistencyModel
        newBuilder._stateFactory = Some(factory)
        newBuilder._stateFactoryExplicit = true
        newBuilder._parallelIterations = _parallelIterations
        newBuilder
    }

    // Internal accessor used by the simple scenario(name) / scenario() factories
    // to pre-register the trivial { => () } state factory without setting the
    // "explicit" flag. This preserves the original behavior: simple scenarios
    // that don't call .reset { => ... } don't build an auto ConsistencyValidator
    // (which would otherwise re-execute operations against un-reset shared state
    // and produce spurious consistency failures).
    func defaultStateFactory(factory: () -> S): ScenarioBuilder<S> {
        _stateFactory = Some(factory)
        this
    }

    public func consistencyModel(model: ConsistencyModel): ScenarioBuilder<S> {
        _consistencyModel = model
        this
    }

    public func actor<T>(body: (S) -> T): ScenarioBuilder<S> where T <: Equatable<T> {
        _actors.add(Actor<S>(
            id: _actors.size,
            operations: [{ state: S => execResultOf({ => body(state) }) }],
            operationNames: ["op_0"],
            name: "actor_${_actors.size}"
        ))
        this
    }

    public func actor<T>(name: String, body: (S) -> T): ScenarioBuilder<S> where T <: Equatable<T> {
        _actors.add(Actor<S>(
            id: _actors.size,
            operations: [{ state: S => execResultOf({ => body(state) }) }],
            operationNames: ["op_0"],
            name: name
        ))
        this
    }

    public func actor<T>(repeat: Int64, body: (S) -> T): ScenarioBuilder<S> where T <: Equatable<T> {
        if (repeat < 1) {
            throw IllegalArgumentException("Repeat count must be at least 1")
        }
        for (_ in 0..repeat) {
            this.actor(body)
        }
        this
    }

    public func actors<T>(bodies: Array<(S) -> T>): ScenarioBuilder<S> where T <: Equatable<T> {
        for (body in bodies) {
            _actors.add(Actor<S>(
                id: _actors.size,
                operations: [{ state: S => execResultOf({ => body(state) }) }],
                operationNames: ["op_0"],
                name: "actor_${_actors.size}"
            ))
        }
        this
    }

    public func actorWithOp(op: Operation<S>): ScenarioBuilder<S> {
        _actors.add(Actor<S>(
            id: _actors.size,
            operations: [{ state: S => op.executeWrapped(state) }],
            operationNames: [op.name],
            name: "actor_${_actors.size}"
        ))
        this
    }

    public func actorWithOps(ops: Array<Operation<S>>): ScenarioBuilder<S> {
        let wrappedOps = ops.map { op =>
            { state: S => op.executeWrapped(state) }
        }
        let names = ops.map { op => op.name }
        let callArgs = ops.map { op => op.callArgs }
        _actors.add(Actor<S>(
            id: _actors.size,
            operations: wrappedOps,
            operationNames: names,
            operationCallArgs: callArgs,
            name: "actor_${_actors.size}"
        ))
        this
    }

    public func sequentialSpec<T>(
        createFresh: () -> T,
        execute: (T, OperationInvocation) -> EquatableObj
    ): ScenarioBuilder<S> where T <: Any {
        let create: () -> Any = { => createFresh() }
        let executeAny: (Any, OperationInvocation) -> EquatableObj = { state, inv =>
            if (let Some(typedState) <- (state as T)) {
                execute(typedState, inv)
            } else {
                EqProxy<Unit>(())
            }
        }
        _explicitValidator = Some(ConsistencyValidator(
            consistencyModel: _consistencyModel,
            createFreshInstance: create,
            reExecuteOperation: executeAny
        ))
        this
    }

    public func after(body: (S) -> Unit): ScenarioBuilder<S> {
        _afterBlock = Some(body)
        this
    }

    public func verify(body: (S) -> Bool): ScenarioBuilder<S> {
        _verifyBlock = Some(body)
        this
    }

    public func configure(config: Configuration): ScenarioBuilder<S> {
        _configuration = config
        this
    }

    // Explicitly enables or disables parallel iteration execution for the
    // scenario being built.
    //
    // When parallel execution is enabled, iteration 0 runs first sequentially
    // (to calibrate backoff timing), then iterations 1..N-1 are dispatched to a
    // bounded pool of workers that complete concurrently; their results are
    // delivered to the callback in completion order (out-of-order relative to
    // iteration index).
    //
    // Default behaviour: parallel execution is enabled automatically when
    // .withState(factory) was used (because each iteration then creates its own
    // isolated SUT instance), and disabled otherwise (simple capture-based
    // scenarios share mutable state across iterations). Call this method to
    // override the default.
    //
    // Contract when enabled: the user guarantees that reset/after/verify and
    // actor operation lambdas are safe to run concurrently across iterations —
    // e.g. by using .withState(factory) so each iteration receives a fresh
    // isolated SUT instance, or by making the captured SUT internally
    // thread-safe.
    public func parallelIterations(enabled: Bool): ScenarioBuilder<S> {
        _parallelIterations = Some(enabled)
        this
    }

    public func scoped(): ScopedScenarioBuilder {
        if (_stateFactory.isNone()) {
            throw IllegalStateException("scoped() method must be used only with explicit preceding .stateFactory(factory) call")
        }
        ScopedScenarioBuilder(this)
    }

    public func buildScoped(scopedBuilder: ScopedScenarioBuilder, scopedBuild: (S, ScopedScenarioBuilder) -> Unit): ConcurrentScenarioProducer<S> {
        ConcurrentScenarioProducer<S> { providedScope =>
            let scope = providedScope ?? _stateFactory.getOrThrow()()
            scopedBuild(scope, scopedBuilder)
            _iterations = scopedBuilder._iterations
            if (let Some(afterBlock) <- scopedBuilder._afterBlock) {
                _afterBlock = Some({ _ => afterBlock() })
            }
            if (let Some(verifyBlock) <- scopedBuilder._verifyBlock) {
                _verifyBlock = Some({ _ => verifyBlock() })
            }
            if (let Some(name) <- scopedBuilder._name) {
                _name = name
            }
            _consistencyModel = scopedBuilder._consistencyModel 
            (scope, build())
        }
    }

    public func build(): ConcurrentScenario<S> {
        if (_actors.size == 0) {
            throw IllegalArgumentException("At least one actor is required")
        }
        if (_stateFactory.isNone()) {
            // The simple scenario(name) / scenario() factories pre-register a
            // default { => () } state factory, so reaching here means the user
            // constructed ScenarioBuilder<S>(name) directly without calling
            // stateFactory(...). For typed S, that's a programming error.
            throw IllegalArgumentException(
                "ScenarioBuilder requires stateFactory(...) to be set " +
                "(use scenario(name, factory) or call .stateFactory(factory))"
            )
        }

        let threadCount = _actors.size

        // Build the auto ConsistencyValidator only when there is a mechanism to
        // produce a fresh isolated state per permutation-DFS restart:
        //   - Explicitly registered state factory (typed scenarios), OR
        //   - Simple .reset { => var = ... } block (capture-based scenarios).
        // Simple scenarios that use neither (e.g. .actor { => counter.fetchAdd(1) }
        // without .reset) deliberately skip validation, matching the original
        // behavior — the validator would otherwise re-execute operations against
        // un-reset shared state and produce spurious consistency failures.
        let shouldBuildAutoValidator = _stateFactoryExplicit || _resetBlock.isSome()

        let finalValidator: ?ConsistencyValidator = if (_explicitValidator.isSome()) {
            _explicitValidator
        } else if (shouldBuildAutoValidator) {
            let actorsArray = _actors.toArray()
            let resetBlock = _resetBlock
            let stateFactoryOpt = _stateFactory
            let createFreshAny: () -> Any = { =>
                if (let Some(reset) <- resetBlock) {
                    reset()
                }
                // stateFactory is guaranteed Some by the check above.
                match (stateFactoryOpt) {
                    case Some(factory) => factory()
                    case None => ()
                }
            }
            let executeAny: (Any, OperationInvocation) -> EquatableObj = { state, inv =>
                if (let Some(s) <- (state as S)) {
                    let actor = actorsArray[inv.actorId]
                    actor.operations[inv.operationIndex](s)
                } else {
                    EqProxy<Unit>(())
                }
            }
            Some(ConsistencyValidator(
                consistencyModel: _consistencyModel,
                createFreshInstance: createFreshAny,
                reExecuteOperation: executeAny
            ))
        } else {
            None
        }

        // Default: parallel execution iff the user opted into the typed
        // per-iteration-state API (.withState). Simple capture-based scenarios
        // default to sequential to preserve the original reset->run->verify
        // per-iteration semantics on shared mutable state.
        let parallelMode = _parallelIterations ?? _stateFactoryExplicit

        ConcurrentScenario<S>(
            name: _name,
            threadCount: threadCount,
            iterations: _iterations,
            resetBlock: _resetBlock,
            stateFactory: _stateFactory,
            actors: _actors.toArray(),
            afterBlock: _afterBlock,
            verifyBlock: _verifyBlock,
            configuration: _configuration,
            consistencyValidator: finalValidator,
            consistencyModel: _consistencyModel,
            parallelIterations: parallelMode
        )
    }
}

// FIXME: Rename?
public class ScopedScenarioBuilder {
    protected var _iterations: Int64 = 1
    protected var _afterBlock: ?() -> Unit = None
    protected var _verifyBlock: ?() -> Bool = None
    protected var _name: ?String = None
    protected var _consistencyModel: ConsistencyModel = SequentialConsistency

    public ScopedScenarioBuilder(protected let builder: Any) {}

    // The number of iterations must not depend on state
    public func iterations(n: Int64): ScopedScenarioBuilder {
        if (n < 1) {
            throw IllegalArgumentException("Iteration count must be at least 1")
        }
        _iterations = n
        this
    }

    // The consistencyModel must not depend on state
    public func consistencyModel(model: ConsistencyModel): ScopedScenarioBuilder {
        _consistencyModel = model
        this
    }

    public func after(body: () -> Unit): ScopedScenarioBuilder {
        _afterBlock = Some(body)
        this
    }

    public func verify(body: () -> Bool): ScopedScenarioBuilder {
        _verifyBlock = Some(body)
        this
    }

    public func name(name: String): ScopedScenarioBuilder {
        _name = Some(name)
        this
    }

    public func build<S>(scopedBuild: (S, ScopedScenarioBuilder) -> Unit): ConcurrentScenarioProducer<S> {
        (this.builder as ScenarioBuilder<S>).getOrThrow().buildScoped(this, scopedBuild)
    }
}

// Simple, stateless API. Available only when S = Unit (i.e. for builders
// returned by scenario(name) / scenario()). The operation lambdas capture
// shared mutable state externally and ignore the (Unit) state placeholder.
extend ScenarioBuilder<Unit> {
    public func reset(body: () -> Unit): ScenarioBuilder<Unit> {
        _resetBlock = Some(body)
        this
    }

    public func actor<T>(body: () -> T): ScenarioBuilder<Unit> where T <: Equatable<T> {
        _actors.add(Actor<Unit>(
            id: _actors.size,
            operations: [{ _: Unit => execResultOf(body) }],
            operationNames: ["op_0"],
            name: "actor_${_actors.size}"
        ))
        this
    }

    public func actor<T>(name: String, body: () -> T): ScenarioBuilder<Unit> where T <: Equatable<T> {
        _actors.add(Actor<Unit>(
            id: _actors.size,
            operations: [{ _: Unit => execResultOf(body) }],
            operationNames: ["op_0"],
            name: name
        ))
        this
    }

    public func actor<T>(repeat: Int64, body: () -> T): ScenarioBuilder<Unit> where T <: Equatable<T> {
        if (repeat < 1) {
            throw IllegalArgumentException("Repeat count must be at least 1")
        }
        for (_ in 0..repeat) {
            this.actor(body)
        }
        this
    }

    public func actors<T>(bodies: Array<() -> T>): ScenarioBuilder<Unit> where T <: Equatable<T> {
        for (body in bodies) {
            _actors.add(Actor<Unit>(
                id: _actors.size,
                operations: [{ _: Unit => execResultOf(body) }],
                operationNames: ["op_0"],
                name: "actor_${_actors.size}"
            ))
        }
        this
    }

    // Note: actorWithOp / actorWithOps are intentionally NOT redefined here.
    // When S = Unit, the generic class's actorWithOp(op: Operation<S>) and
    // actorWithOps(ops: Array<Operation<S>>) already accept Operation<Unit>
    // (since S = Unit), and the simple operation<T>(name, body: () -> T) factory
    // returns Operation<Unit>, so the generic class's methods are invoked
    // directly with the correct S = Unit binding. Re-declaring them here would
    // produce an illegal shadow of the generic class's methods.

    public func after(body: () -> Unit): ScenarioBuilder<Unit> {
        _afterBlock = Some({ _: Unit => body() })
        this
    }

    public func verify(body: () -> Bool): ScenarioBuilder<Unit> {
        _verifyBlock = Some({ _: Unit => body() })
        this
    }
}
