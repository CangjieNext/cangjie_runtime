package std.unittest

import std.unittest.concurrent.*
import std.unittest.prop_test.RandomSource
import std.collection.*

class OperationDesc<S> {
    OperationDesc(
        let group!: Option<String>,
        let name!: String,
        let operationIteratorProducer!: (Configuration) -> Iterator<Operation<S>>,
    ) {}
}

class ScenarioParams<S> {
    ScenarioParams(
        let minActors!: Int64,
        let maxActors!: Int64,
        let minOperationPerActor!: Int64,
        let maxOperationPerActor!: Int64,
        let scenarioBase!: () -> ScenarioBuilder<S>,
    ) {}    
}

// FIXME: rename?
public class OperationsManager<S> {
    let operations = ArrayList<OperationDesc<S>>()

    public OperationsManager(let scenarioBase: () -> ScenarioBuilder<S>) {}

    private func formatOperationName(name: String, inputs: Array<InputParameter>): String {
        let sb = StringBuilder()
        sb.append(name)
        if (!inputs.isEmpty()) {
            sb.append("(")
            for (i in 0..inputs.size) {
                if (i != 0) {
                    sb.append(", ")
                }
                sb.append(inputs[i].repr)
            }
            sb.append(")")
        }
        sb.toString()
    }

    public func addOperation<T, R>(
        group!: Option<String>,
        name!: String,
        body!: (T, S) -> R,
        dataStrategy!: DataStrategyProcessor<T>
    ): Unit where R <: Equatable<R> {
        let operationDesc = OperationDesc(group: group, name: name) { configuration =>
            dataStrategy.provide(configuration) |> map { args: T => 
                let opName = formatOperationName(name, dataStrategy.lastItemInfo())
                operation(opName) { state: S => body(args, state) }
            }
        }
        operations.add(operationDesc)
    }

    public func addOperation<R>(
        group!: Option<String>,
        name!: String,
        body!: (S) -> R,
    ): Unit where R <: Equatable<R> {
        let operationDesc = OperationDesc(group: group, name: name) { configuration =>
            [operation(name, body)].iterator()
        }
        operations.add(operationDesc)
    }

    public func scenarios(
        minActors!: Int64 = 2,
        maxActors!: Int64 = 3,
        minOperationPerActor!: Int64 = 2,
        maxOperationPerActor!: Int64 = 10,
    ): DataStrategy<ScenarioBuilder<S>> {
        ScenarioDataStrategy(
            operations.toArray(),
            ScenarioParams(
                minActors: minActors,
                maxActors: maxActors,
                minOperationPerActor: minOperationPerActor,
                maxOperationPerActor: maxOperationPerActor,
                scenarioBase: scenarioBase,
            ),
        )
    }
}

class ScenarioIterator<S> <: Iterator<ScenarioBuilder<S>> {
    let operationIterators: Array<Iterator<Operation<S>>>
    let random: RandomSource

    ScenarioIterator(
        let configuration: Configuration,
        let operations: Array<OperationDesc<S>>,
        let scenarioParams: ScenarioParams<S>,
    ) {
        random = configuration.get<RandomSource>(KeyRandom.random) ??
            throw IllegalArgumentException("RandomDataProvider initialized without configured random generator instance")

        operationIterators = Array(operations.size) { idx =>
            operations[idx].operationIteratorProducer(configuration)
        }
    }

    private func nextPosInt64(upper: Int64): Int64 {
        Int64(random.nextUInt64(UInt64(upper)))
    }

    private func nextPosInt64(min: Int64, upper: Int64): Int64 {
        Int64(random.nextUInt64(UInt64(upper - min))) + min
    }

    private func nextOperation(idx: Int64): Operation<S> {
        var op = operationIterators[idx].next()
        if (op.isNone()) {
            operationIterators[idx] = operations[idx].operationIteratorProducer(configuration)
            op = operationIterators[idx].next()
        }

        if (let Some(op) <- op) {
            op
        } else {
            throw IllegalArgumentException("Iterator must not be empty")
        }
    }

    public override func next(): Option<ScenarioBuilder<S>> {
        let scenario = scenarioParams.scenarioBase()

        let groups = HashSet<String>()
        var hasGroupFreeOperation = false
        for (op in operations) {
            if (let Some(group) <- op.group) {
                groups.add(group)
            } else {
                hasGroupFreeOperation = true
            }
        }

        var maxActors = scenarioParams.maxActors
        if (!hasGroupFreeOperation) {
            maxActors = min(maxActors, groups.size) 
        }

        if (maxActors < scenarioParams.minActors) {
            if (maxActors != scenarioParams.maxActors) {
                throw IllegalArgumentException("Maximal count of actors must be greater or equal to minimum count ${scenarioParams.minActors}. Maximal count is limited by number of groups: ${maxActors}. Due to absence of operations without groups")
            } else {
                throw IllegalArgumentException("Maximal count of actors ${scenarioParams.maxActors} must be greater or equal to minimum count ${scenarioParams.minActors}")
            }
        }

        let actorsCount = nextPosInt64(scenarioParams.minActors, maxActors + 1);

        let actorsWithoutGroup = HashSet<Int64>(actorsCount) { i => i }
        let groupsToActors = HashMap<String, Int64>()
        var groupRemain = groups.size
        for (group in groups) {
            let actorIdx = if (!hasGroupFreeOperation && groupRemain == actorsWithoutGroup.size) {
                let availableActors = actorsWithoutGroup.toArray()
                let availableActorIdx = nextPosInt64(availableActors.size)
                availableActors[availableActorIdx]
            } else {
                nextPosInt64(actorsCount)
            }
            groupsToActors.add(group, actorIdx)
            actorsWithoutGroup.remove(actorIdx)
            groupRemain -= 1
        }

        for (actorIdx in 0..actorsCount) {
            let possibleOperations = operations |> enumerate |> filterMap { arg: (Int64, OperationDesc<S>) =>
                let (idx, op) = arg
                match (op.group) {
                    case None => Some(idx)
                    case Some(group) => if (groupsToActors[group] == actorIdx) { Some(idx) } else { Option<Int64>.None }
                }
            } |> collectArray

            if (possibleOperations.isEmpty()) {
                // Must be unreachable
                throw Exception("Distribution of groups by actors is contradictory")
            }

            let operationsCount =
                nextPosInt64(scenarioParams.minOperationPerActor, scenarioParams.maxOperationPerActor + 1)
            let actorOperations = Array(operationsCount) { _: Int64 =>
                let operationIdx = nextPosInt64(possibleOperations.size) 
                nextOperation(possibleOperations[operationIdx])
            }

            scenario.actorWithOps(actorOperations)
        }

        Some(scenario)
    }
}

class ScenarioGenerator<S> <: Iterable<ScenarioBuilder<S>> {
    ScenarioGenerator(
        let configuration: Configuration,
        let operations: Array<OperationDesc<S>>,
        let scenarioParams: ScenarioParams<S>,
    ) {}

    public override func iterator(): Iterator<ScenarioBuilder<S>> {
        ScenarioIterator(configuration, operations, scenarioParams)
    }
}

class ScenarioDataProvider<S> <: DataProvider<ScenarioBuilder<S>> {
    ScenarioDataProvider(
        let configuration: Configuration,
        let operations: Array<OperationDesc<S>>,
        let scenarioParams: ScenarioParams<S>,
    ) {}

    public override func provide(): Iterable<ScenarioBuilder<S>> {
        ScenarioGenerator(configuration, operations, scenarioParams)
    }
}

class ScenarioDataStrategy<S> <: DataStrategy<ScenarioBuilder<S>> {
    ScenarioDataStrategy(
        let operations: Array<OperationDesc<S>>,
        let scenarioParams: ScenarioParams<S>,
    ) {}

    public override func provider(configuration: Configuration): DataProvider<ScenarioBuilder<S>> {
        ScenarioDataProvider(configuration, operations, scenarioParams)
    }

    public override prop isInfinite: Bool {
        get() { true }
    }
}

