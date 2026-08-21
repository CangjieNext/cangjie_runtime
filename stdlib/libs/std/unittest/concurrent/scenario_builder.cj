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

/**
 * Fluent builder for a `ConcurrentScenario<S>`.
 *
 * A `ScenarioBuilder` is obtained from one of the `scenario(...)` entry points
 * (which pre-seed a `Unit` state factory) or constructed directly for typed
 * scenarios. Each fluent method mutates the builder in place and returns the
 * same builder (`this`) for chaining; the only method that does not return the
 * builder is `withState<S2>(...)`, which transitions to a typed
 * `ScenarioBuilder<S2>` (because Cangjie does not allow mutating a class's type
 * parameter in place), and `build()`, which finalises the scenario.
 *
 * A minimal valid scenario requires at least one actor (added via `actor`,
 * `actors`, `actorWithOp`, or `actorWithOps`) and a state factory (registered
 * automatically by `scenario(...)`, or explicitly via `.withState(factory)` /
 * `.reset { => ... }`). `build()` throws `IllegalArgumentException` if either
 * is missing.
 *
 * @param S per-iteration state type; `Unit` for capture-based scenarios built
 *          via the `scenario(...)` entry points
 */
public class ScenarioBuilder<S> {
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

    /** Construct an unnamed builder (`"unnamed_scenario"`); set the name via `name(n)`. */
    public init() {}

    /** Construct a builder pre-seeded with the given scenario `name`. */
    public init(name: String) {
        _name = name
    }

    /**
     * Override the scenario name.
     *
     * The name is surfaced in reports, mismatch tables, and (when wrapped in a
     * `ConcurrentTestCase`) in the `TestSuite` reporter output.
     *
     * @param n new scenario name
     * @return this builder, for fluent chaining
     */
    public func name(n: String): ScenarioBuilder<S> {
        _name = n
        this
    }

    /**
     * Set the number of iterations the scenario will be executed for.
     *
     * Iteration 0 always runs first sequentially (to calibrate the Gaussian
     * backoff sigma); subsequent iterations run either sequentially or in
     * parallel depending on the `parallelIterations` flag. As soon as an
     * iteration fails, no further iterations are dispatched (early-exit).
     *
     * @param n iteration count, must be `>= 1`
     * @return this builder, for fluent chaining
     * @throws IllegalArgumentException if `n < 1`
     */
    public func iterations(n: Int64): ScenarioBuilder<S> {
        if (n < 1) {
            throw IllegalArgumentException("Iteration count must be at least 1")
        }
        _iterations = n
        this
    }

    /**
     * Transition this builder to a typed `ScenarioBuilder<S2>` with the given
     * per-iteration state factory.
     *
     * The state type `S2` is inferred from the factory argument. After this
     * call, all state-aware methods (`actor`, `actorWithOp`, `actorWithOps`,
     * `verify`, `after`) accept lambdas that receive the per-iteration state of
     * type `S2`. Each iteration (and each permutation-DFS restart inside the
     * consistency validator) calls `factory` to produce a fresh, isolated SUT
     * instance, so parallel iteration execution is enabled by default.
     *
     * Typical usage:
     * ```
     * scenario("queue_test")
     *     .withState({ => ConcurrentLinkedQueue<Int64>() })
     *     .actor({ q => q.add(1) })
     *     .build()
     * ```
     *
     * @param S2 per-iteration state type, inferred from `factory`
     * @param factory closure that produces a fresh `S2` instance per iteration
     * @return a new `ScenarioBuilder<S2>` carrying over the name, iteration
     *         count, configuration, and consistency model of this builder
     * @throws IllegalArgumentException if `withState` has already been called
     *         on this builder, or if any actors have already been added
     *         (existing `Unit`-typed actors cannot be retyped to `S2`)
     */
    public func withState<S2>(factory: () -> S2): ScenarioBuilder<S2> {
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

    /**
     * Select the consistency model under which the scenario's recorded history
     * will be validated. Defaults to `SequentialConsistency`.
     *
     * Stronger models (`Linearizability`) admit fewer linearizations and can
     * detect more bugs (e.g. store-buffer effects) at higher per-iteration
     * validation cost; weaker models (`Serializability`) search every
     * permutation and are appropriate for SUTs whose methods are intentionally
     * not sequentially consistent.
     *
     * @param model consistency model to use
     * @return this builder, for fluent chaining
     */
    public func consistencyModel(model: ConsistencyModel): ScenarioBuilder<S> {
        _consistencyModel = model
        this
    }

    /**
     * Add a single-op actor whose body closes over the per-iteration state `S`.
     *
     * The body's return type `T` must satisfy `T <: Equatable<T>` so the
     * framework can compare the observed result against the sequentially
     * re-executed result. The result (or any thrown exception) is captured via
     * `execResultOf`. The actor is given an auto-generated name
     * `"actor_<index>"`.
     *
     * @param T result type of the body
     * @param body closure `(S) -> T` performing the operation
     * @return this builder, for fluent chaining
     */
    public func actor<T>(body: (S) -> T): ScenarioBuilder<S> where T <: Equatable<T> {
        _actors.add(Actor<S>(
            id: _actors.size,
            operations: [{ state: S => execResultOf({ => body(state) }) }],
            operationNames: ["op_0"],
            name: "actor_${_actors.size}"
        ))
        this
    }

    /**
     * Add a single-op actor with an explicit name, whose body closes over the
     * per-iteration state `S`.
     *
     * Same as `actor(body)` but uses the user-supplied `name` (surfaced in
     * mismatch tables and reports) instead of the auto-generated
     * `"actor_<index>"`.
     *
     * @param T result type of the body
     * @param name human-readable actor name
     * @param body closure `(S) -> T` performing the operation
     * @return this builder, for fluent chaining
     */
    public func actor<T>(name: String, body: (S) -> T): ScenarioBuilder<S> where T <: Equatable<T> {
        _actors.add(Actor<S>(
            id: _actors.size,
            operations: [{ state: S => execResultOf({ => body(state) }) }],
            operationNames: ["op_0"],
            name: name
        ))
        this
    }

    /**
     * Add `repeat` copies of a single-op actor, each running the same `body`
     * against the per-iteration state `S`.
     *
     * Equivalent to calling `actor(body)` `repeat` times. Useful for spawning
     * multiple contending actors with the same operation without writing a
     * loop. Each copy is given an auto-generated name `"actor_<index>"`.
     *
     * @param T result type of the body
     * @param repeat number of copies to add, must be `>= 1`
     * @param body closure `(S) -> T` performing the operation
     * @return this builder, for fluent chaining
     * @throws IllegalArgumentException if `repeat < 1`
     */
    public func actor<T>(repeat: Int64, body: (S) -> T): ScenarioBuilder<S> where T <: Equatable<T> {
        if (repeat < 1) {
            throw IllegalArgumentException("Repeat count must be at least 1")
        }
        for (_ in 0..repeat) {
            this.actor(body)
        }
        this
    }

    /**
     * Add multiple single-op actors in one call, each closing over the
     * per-iteration state `S`.
     *
     * Equivalent to calling `actor(body)` for each `body` in `bodies`, in
     * order. Each actor is given an auto-generated name `"actor_<index>"`.
     *
     * @param T result type of each body
     * @param bodies array of closures `(S) -> T`, one per actor
     * @return this builder, for fluent chaining
     */
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

    /**
     * Add a single-op actor whose body is the supplied `Operation<S>`.
     *
     * The actor's name is taken from `op.name` (set when the `Operation` was
     * constructed via the `operation<T, S>(name, body)` factory). Useful when
     * the same operation appears in multiple actors or in mismatch tables
     * under a meaningful name.
     *
     * @param op the operation to wrap as a single-op actor
     * @return this builder, for fluent chaining
     */
    public func actorWithOp(op: Operation<S>): ScenarioBuilder<S> {
        _actors.add(Actor<S>(
            id: _actors.size,
            operations: [{ state: S => op.executeWrapped(state) }],
            operationNames: [op.name],
            name: "actor_${_actors.size}"
        ))
        this
    }

    /**
     * Add a multi-op actor that runs the given operations sequentially in one
     * thread, in listed order.
     *
     * The actor's name is auto-generated (`"actor_<index>"`); each operation's
     * name (set when the `Operation` was constructed) is surfaced individually
     * in mismatch tables. Use this when an actor should perform several
     * distinct operations against the shared state (e.g. `add → peek → add`
     * for a queue).
     *
     * @param ops array of `Operation<S>` to execute in order within one thread
     * @return this builder, for fluent chaining
     */
    public func actorWithOps(ops: Array<Operation<S>>): ScenarioBuilder<S> {
        let wrappedOps = ops.map { op =>
            { state: S => op.executeWrapped(state) }
        }
        let names = ops.map { op => op.name }
        _actors.add(Actor<S>(
            id: _actors.size,
            operations: wrappedOps,
            operationNames: names,
            name: "actor_${_actors.size}"
        ))
        this
    }

    /**
     * Provide an explicit sequential reference implementation for the scenario.
     *
     * By default the framework re-executes the *same* operation lambdas that
     * the actors run, against a fresh per-iteration state, to obtain the
     * sequential result that the observed result is compared against
     * (auto-spec). This is convenient but assumes that running the operations
     * sequentially is itself correct — if the user is unsure, they can supply
     * an explicit reference implementation via `sequentialSpec`.
     *
     * `createFresh` produces a fresh instance of the reference state `T` at the
     * start of each permutation-DFS restart; `execute` re-executes a single
     * operation (identified by its `OperationInvocation`) against the reference
     * state and returns the type-erased result. The framework compares the
     * returned `EquatableObj` against the observed result captured during the
     * parallel execution.
     *
     * @param T reference state type
     * @param createFresh closure producing a fresh `T` per DFS restart
     * @param execute closure `(T, OperationInvocation) -> EquatableObj`
     *        re-executing one operation against the reference state
     * @return this builder, for fluent chaining
     */
    public func sequentialSpec<T>(
        createFresh: () -> T,
        execute: (T, OperationInvocation) -> EquatableObj
    ): ScenarioBuilder<S> {
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

    /**
     * Register a sequential post-block that runs on the orchestrator thread
     * (T0) after all actor futures are joined, before `verify` and the
     * consistency validator.
     *
     * Use this for teardown / inspection that should happen once per iteration
     * on the main thread (e.g. snapshot the final state, log a metric). The
     * block receives the per-iteration state `S`.
     *
     * @param body closure `(S) -> Unit` to run after the parallel part
     * @return this builder, for fluent chaining
     */
    public func after(body: (S) -> Unit): ScenarioBuilder<S> {
        _afterBlock = Some(body)
        this
    }

    /**
     * Register a post-iteration predicate that runs on the orchestrator thread
     * after `after` and before the consistency validator.
     *
     * If the predicate returns `false`, the iteration is marked as failed. A
     * `false` return on iteration 0 triggers early-exit (no further iterations
     * are dispatched). `verify` is fully orthogonal to consistency validation:
     * a scenario may use `verify` alone, `consistencyModel(...)` alone, or both.
     *
     * @param body closure `(S) -> Bool` returning `true` iff the final state
     *        is acceptable
     * @return this builder, for fluent chaining
     */
    public func verify(body: (S) -> Bool): ScenarioBuilder<S> {
        _verifyBlock = Some(body)
        this
    }

    /**
     * Pass through a framework `Configuration` to the scenario (e.g. to
     * enable verbose mode via `Configuration().set(KeyVerbose.verbose, true)`).
     *
     * @param config configuration to attach to the built scenario
     * @return this builder, for fluent chaining
     */
    public func configure(config: Configuration): ScenarioBuilder<S> {
        _configuration = config
        this
    }

    /**
     * Explicitly enable or disable parallel iteration execution for the
     * scenario being built.
     *
     * When parallel execution is enabled, iteration 0 runs first sequentially
     * (to calibrate backoff timing), then iterations 1..N-1 are dispatched to a
     * bounded pool of workers that complete concurrently; their results are
     * delivered to the callback in completion order (out-of-order relative to
     * iteration index).
     *
     * Default behaviour: parallel execution is enabled automatically when
     * `.withState(factory)` was used (because each iteration then creates its own
     * isolated SUT instance), and disabled otherwise (simple capture-based
     * scenarios share mutable state across iterations). Call this method to
     * override the default.
     *
     * Contract when enabled: the user guarantees that reset/after/verify and
     * actor operation lambdas are safe to run concurrently across iterations —
     * e.g. by using `.withState(factory)` so each iteration receives a fresh
     * isolated SUT instance, or by making the captured SUT internally
     * thread-safe.
     *
     * @param enabled `true` to force-enable parallel iterations, `false` to
     *        force-disable
     * @return this builder, for fluent chaining
     */
    public func parallelIterations(enabled: Bool): ScenarioBuilder<S> {
        _parallelIterations = Some(enabled)
        this
    }

    /**
     * Finalise the scenario and return an immutable `ConcurrentScenario<S>`.
     *
     * Builds the auto `ConsistencyValidator` only when there is a mechanism to
     * produce a fresh isolated state per permutation-DFS restart (an explicit
     * `withState` factory or a `.reset { => ... }` block); simple capture-based
     * scenarios that use neither deliberately skip validation, matching the
     * original behavior.
     *
     * @return the built, immutable `ConcurrentScenario<S>`
     * @throws IllegalArgumentException if no actors have been added, or if no
     *         state factory is set (the latter cannot happen for builders
     *         returned by `scenario(...)` because they pre-register a default
     *         `{ => () }` factory)
     */
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

// Simple, stateless API. Available only when S = Unit (i.e. for builders
// returned by scenario(name) / scenario()). The operation lambdas capture
// shared mutable state externally and ignore the (Unit) state placeholder.
extend ScenarioBuilder<Unit> {
    /**
     * Register a reset block that re-initialises the captured shared mutable
     * state before each iteration.
     *
     * Available only when `S = Unit`. Use this for capture-based scenarios
     * where the SUT is shared mutable state closed over by the actor lambdas
     * (e.g. an `AtomicInt64` declared in the test scope) — `reset` is called
     * on the orchestrator thread at the start of every iteration to put the
     * SUT back into a known state. When `reset` is set, the auto
     * `ConsistencyValidator` is built (each permutation-DFS restart invokes
     * `reset` to obtain an isolated SUT).
     *
     * @param body closure `() -> Unit` re-initialising the captured state
     * @return this builder, for fluent chaining
     */
    public func reset(body: () -> Unit): ScenarioBuilder<Unit> {
        _resetBlock = Some(body)
        this
    }

    /**
     * Add a single-op actor whose body captures shared mutable state
     * externally and ignores the `Unit` state placeholder.
     *
     * `Unit`-typed overload of `actor<T>(body: (S) -> T)` for capture-based
     * scenarios. The body's result type `T` must satisfy
     * `T <: Equatable<T>`. The actor is given an auto-generated name
     * `"actor_<index>"`.
     *
     * @param T result type of the body
     * @param body closure `() -> T` performing the operation
     * @return this builder, for fluent chaining
     */
    public func actor<T>(body: () -> T): ScenarioBuilder<Unit> where T <: Equatable<T> {
        _actors.add(Actor<Unit>(
            id: _actors.size,
            operations: [{ _: Unit => execResultOf(body) }],
            operationNames: ["op_0"],
            name: "actor_${_actors.size}"
        ))
        this
    }

    /**
     * Add a single-op actor with an explicit name, whose body captures shared
     * mutable state externally.
     *
     * `Unit`-typed overload of `actor<T>(name, body: (S) -> T)` for
     * capture-based scenarios. Same as `actor(body)` but uses the
     * user-supplied `name`.
     *
     * @param T result type of the body
     * @param name human-readable actor name
     * @param body closure `() -> T` performing the operation
     * @return this builder, for fluent chaining
     */
    public func actor<T>(name: String, body: () -> T): ScenarioBuilder<Unit> where T <: Equatable<T> {
        _actors.add(Actor<Unit>(
            id: _actors.size,
            operations: [{ _: Unit => execResultOf(body) }],
            operationNames: ["op_0"],
            name: name
        ))
        this
    }

    /**
     * Add `repeat` copies of a single-op actor, each running the same
     * `body` that captures shared mutable state externally.
     *
     * `Unit`-typed overload of `actor<T>(repeat, body: (S) -> T)` for
     * capture-based scenarios. Equivalent to calling `actor(body)` `repeat`
     * times.
     *
     * @param T result type of the body
     * @param repeat number of copies to add, must be `>= 1`
     * @param body closure `() -> T` performing the operation
     * @return this builder, for fluent chaining
     * @throws IllegalArgumentException if `repeat < 1`
     */
    public func actor<T>(repeat: Int64, body: () -> T): ScenarioBuilder<Unit> where T <: Equatable<T> {
        if (repeat < 1) {
            throw IllegalArgumentException("Repeat count must be at least 1")
        }
        for (_ in 0..repeat) {
            this.actor(body)
        }
        this
    }

    /**
     * Add multiple single-op actors in one call, each capturing shared mutable
     * state externally.
     *
     * `Unit`-typed overload of `actors<T>(bodies: Array<(S) -> T>)` for
     * capture-based scenarios. Equivalent to calling `actor(body)` for each
     * `body` in `bodies`, in order.
     *
     * @param T result type of each body
     * @param bodies array of closures `() -> T`, one per actor
     * @return this builder, for fluent chaining
     */
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

    /**
     * Register a sequential post-block that runs on the orchestrator thread
     * after all actor futures are joined.
     *
     * `Unit`-typed overload of `after(body: (S) -> Unit)` for capture-based
     * scenarios — the body takes no arguments and operates on the captured
     * shared mutable state.
     *
     * @param body closure `() -> Unit` to run after the parallel part
     * @return this builder, for fluent chaining
     */
    public func after(body: () -> Unit): ScenarioBuilder<Unit> {
        _afterBlock = Some({ _: Unit => body() })
        this
    }

    /**
     * Register a post-iteration predicate; `false` fails the iteration.
     *
     * `Unit`-typed overload of `verify(body: (S) -> Bool)` for capture-based
     * scenarios — the body takes no arguments and operates on the captured
     * shared mutable state. A `false` return on iteration 0 triggers
     * early-exit.
     *
     * @param body closure `() -> Bool` returning `true` iff the final state
     *        is acceptable
     * @return this builder, for fluent chaining
     */
    public func verify(body: () -> Bool): ScenarioBuilder<Unit> {
        _verifyBlock = Some({ _: Unit => body() })
        this
    }
}
