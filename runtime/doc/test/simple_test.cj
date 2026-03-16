class SimpleNode {
    public var val: Int64
    public init(v: Int64) {
        this.val = v
    }
}

main() {
    let n = SimpleNode(42)
    println(n.val.toString())
}
