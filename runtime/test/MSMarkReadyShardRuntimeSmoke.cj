import std.runtime.*

class MarkReadyNode {
    public let value: Int64
    public var next: Option<MarkReadyNode>

    public init(value: Int64) {
        this.value = value
        this.next = None
    }
}

func buildMarkReadyChain(length: Int64): MarkReadyNode {
    var head = MarkReadyNode(0)
    for (i in 1..length) {
        let node = MarkReadyNode(i)
        node.next = Some(head)
        head = node
    }
    head
}

main() {
    let length: Int64 = 10_000
    let live = buildMarkReadyChain(length)
    gc()
    let checksum = match (live.next) {
        case Some(next) => live.value + next.value
        case None => -1
    }
    println("MSMarkReadyShardRuntimeSmoke PASS checksum=${checksum}")
}
