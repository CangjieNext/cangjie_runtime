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

public class ScenarioBuilder {
    private var _name: String = "unnamed_scenario"
    private var _iterations: Int64 = 1
    private var _resetBlock: ?() -> Unit = None
    private let _actors: ArrayList<Actor> = ArrayList<Actor>()
    private var _afterBlock: ?() -> Unit = None
    private var _verifyBlock: ?() -> Bool = None
    private var _configuration: Configuration = Configuration()
    private var _consistencyModel: ConsistencyModel = SequentialConsistency
    private var _explicitValidator: ?ConsistencyValidator = None
    
    public init() {}
    
    public init(name: String) {
        _name = name
    }
    
    public func name(n: String): ScenarioBuilder {
        _name = n
        this
    }
    
    public func iterations(n: Int64): ScenarioBuilder {
        if (n < 1) {
            throw IllegalArgumentException("Iteration count must be at least 1")
        }
        _iterations = n
        this
    }
    
    public func reset(body: () -> Unit): ScenarioBuilder {
        _resetBlock = Some(body)
        this
    }
    
    public func consistencyModel(model: ConsistencyModel): ScenarioBuilder {
        _consistencyModel = model
        this
    }
    
    public func actor<T>(repeat: Int64, body: () -> T): ScenarioBuilder where T <: Equatable<T> {
        if (repeat < 1) {
            throw IllegalArgumentException("Repeat count must be at least 1")
        }
        for (_ in 0..repeat) {
            this.actor(body)
        }
        this
    }
    
    public func actor<T>(body: () -> T): ScenarioBuilder where T <: Equatable<T> {
        _actors.add(Actor(
            id: _actors.size,
            operations: [{ => execResultOf(body) }],
            operationNames: ["op_0"],
            name: "actor_${_actors.size}"
        ))
        this
    }
    
    public func actor<T>(name: String, body: () -> T): ScenarioBuilder where T <: Equatable<T> {
        _actors.add(Actor(
            id: _actors.size,
            operations: [{ => execResultOf(body) }],
            operationNames: ["op_0"],
            name: name
        ))
        this
    }
    
    public func actors<T>(bodies: Array<() -> T>): ScenarioBuilder where T <: Equatable<T> {
        for (body in bodies) {
            _actors.add(Actor(
                id: _actors.size,
                operations: [{ => execResultOf(body) }],
                operationNames: ["op_0"],
                name: "actor_${_actors.size}"
            ))
        }
        this
    }
    
    public func actorWithOp(op: Operation): ScenarioBuilder {
        _actors.add(Actor(
            id: _actors.size,
            operations: [{ => op.executeWrapped() }],
            operationNames: [op.name],
            name: "actor_${_actors.size}"
        ))
        this
    }
    
    public func actorWithOps(ops: Array<Operation>): ScenarioBuilder {
        let wrappedOps = ops.map { op => 
            { => op.executeWrapped() }
        }
        let names = ops.map { op => op.name }
        _actors.add(Actor(
            id: _actors.size,
            operations: wrappedOps,
            operationNames: names,
            name: "actor_${_actors.size}"
        ))
        this
    }
    
    public func sequentialSpec<T>(
        createFresh: () -> T,
        execute: (T, OperationInvocation) -> EquatableObj
    ): ScenarioBuilder where T <: Any {
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
    
    public func after(body: () -> Unit): ScenarioBuilder {
        _afterBlock = Some(body)
        this
    }
    
    public func verify(body: () -> Bool): ScenarioBuilder {
        _verifyBlock = Some(body)
        this
    }
    
    public func configure(config: Configuration): ScenarioBuilder {
        _configuration = config
        this
    }
    
    public func build(): ConcurrentScenario {
        if (_actors.size == 0) {
            throw IllegalArgumentException("At least one actor is required")
        }
        
        let threadCount = _actors.size
        
        let finalValidator: ?ConsistencyValidator = if (_explicitValidator.isSome()) {
            _explicitValidator
        } else if (_resetBlock.isSome()) {
            let actorsArray = _actors.toArray()
            let createFreshAny: () -> Any = { => 
                match (_resetBlock) {
                    case Some(reset) => reset()
                    case None => ()
                }
            }
            let executeAny: (Any, OperationInvocation) -> EquatableObj = { _, inv =>
                let actor = actorsArray[inv.actorId]
                actor.operations[inv.operationIndex]()
            }
            Some(ConsistencyValidator(
                consistencyModel: _consistencyModel,
                createFreshInstance: createFreshAny,
                reExecuteOperation: executeAny
            ))
        } else {
            None
        }
        
        ConcurrentScenario(
            name: _name,
            threadCount: threadCount,
            iterations: _iterations,
            resetBlock: _resetBlock,
            actors: _actors.toArray(),
            afterBlock: _afterBlock,
            verifyBlock: _verifyBlock,
            configuration: _configuration,
            consistencyValidator: finalValidator,
            consistencyModel: _consistencyModel
        )
    }
}