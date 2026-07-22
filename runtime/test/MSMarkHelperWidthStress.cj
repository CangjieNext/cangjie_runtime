import std.runtime.*

class FanoutNode {
    public let value: Int64
    public var next: Option<FanoutNode>

    public init(value: Int64) {
        this.value = value
        this.next = None
    }
}

func buildWideGraph(width: Int64): Array<FanoutNode> {
    let nodes = Array<FanoutNode>(width, {i: Int64 => FanoutNode(i)})
    for (i in 1..width) {
        nodes[i - 1].next = Some(nodes[i])
    }
    nodes
}

func allocateDeadGraph(width: Int64): Int64 {
    let graph = buildWideGraph(width)
    graph[0].value + graph[width - 1].value
}

main() {
    let liveWidth: Int64 = 200_000
    let deadWidth: Int64 = 1_000_000
    let live = buildWideGraph(liveWidth)
    var checksum = live[0].value + live[liveWidth - 1].value

    // The root Array publishes hundreds of full WorkBufs when scanned. Between
    // explicit collections, dead graphs create Sweep pressure so a one-helper
    // Sweep epoch is followed by a Mark epoch that must restore all helpers.
    gc()
    for (_ in 0..3) {
        checksum += allocateDeadGraph(deadWidth)
        gc()
        sleep(Duration.millisecond * 200)
    }

    checksum += live[liveWidth / 2].value
    println("MSMarkHelperWidthStress PASS checksum=${checksum}")
}
