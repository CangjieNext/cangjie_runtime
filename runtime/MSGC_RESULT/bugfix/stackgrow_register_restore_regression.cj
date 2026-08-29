class Node {
    var parent: ?Node
    var value: Int64

    public init(parent: ?Node, value: Int64) {
        this.parent = parent
        this.value = value
    }
}

func Recurse(depth: Int64, parent: ?Node): Int64 {
    let node = Node(parent, depth)
    if (depth == 0) {
        return node.value
    }
    let result = Recurse(depth - 1, node)
    return result + node.value % 2
}

main(): Int64 {
    let result = Recurse(5000, None)
    println(result)
    return 0
}
