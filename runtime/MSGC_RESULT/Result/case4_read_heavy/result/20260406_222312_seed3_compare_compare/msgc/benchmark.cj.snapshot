import std.collection.*
import std.runtime.*
import std.time.*

// ============================================================
//   Case 4: 读密集型模型 (访问:创建 = 10:1)
//   构建工作集后，每 1 次写操作对应 10 次读操作
//   用法：
//     cjc benchmark.cj -o benchmark
//     MRT_REPORT=./runtime.log cjHeapSize=512mb ./benchmark
// ============================================================

class TreeNode {
    public var val: Int64
    public var left: Option<TreeNode>
    public var right: Option<TreeNode>
    public init(val: Int64) {
        this.val = val
        this.left = None
        this.right = None
    }
}

class LargeObject {
    public var data: Array<Int64>
    public var id: Int64
    public init(elementCount: Int64, id: Int64) {
        this.id = id
        this.data = Array<Int64>(elementCount, { i => i * id })
    }
    public func checksum(): Int64 {
        var sum: Int64 = 0
        for (v in this.data) { sum += v }
        sum
    }
}

class PayloadNode {
    public var val: Int64
    public var payload: Array<Int64>
    public var next: Option<PayloadNode>
    public init(val: Int64, payloadSize: Int64) {
        this.val = val
        this.payload = Array<Int64>(payloadSize, repeat: val)
        this.next = None
    }
}

func buildTree(depth: Int64): TreeNode {
    let node = TreeNode(depth)
    if (depth > 0) {
        node.left = Some(buildTree(depth - 1))
        node.right = Some(buildTree(depth - 1))
    }
    node
}

func treeChecksum(opt: Option<TreeNode>): Int64 {
    match (opt) {
        case Some(n) => n.val + treeChecksum(n.left) - treeChecksum(n.right)
        case None => 0
    }
}

func countNodes(opt: Option<TreeNode>): Int64 {
    match (opt) {
        case Some(n) => 1 + countNodes(n.left) + countNodes(n.right)
        case None => 0
    }
}

func buildChain(length: Int64, payloadSize: Int64): PayloadNode {
    let head = PayloadNode(0, payloadSize)
    var cur = head
    for (i in 1..length) {
        let node = PayloadNode(i, payloadSize)
        cur.next = Some(node)
        cur = node
    }
    head
}

func sumChain(opt: Option<PayloadNode>): Int64 {
    match (opt) {
        case Some(n) => n.val + sumChain(n.next)
        case None => 0
    }
}

func elapsedMs(start: DateTime): Int64 {
    let d = DateTime.now() - start
    d.toMilliseconds()
}

// 读操作：遍历树并校验
func readTree(tree: TreeNode): Int64 {
    let cs = treeChecksum(Some(tree))
    let n = countNodes(Some(tree))
    cs + n
}

// 读操作：遍历链表并求和
func readChain(head: PayloadNode): Int64 {
    sumChain(Some(head))
}

// 读操作：查询 HashMap
func readMap(map: HashMap<Int64, ArrayList<Int64>>, mapSize: Int64): Int64 {
    var sum: Int64 = 0
    for (i in 0..mapSize) {
        match (map.get(i)) {
            case Some(v) => sum += v[0]
            case None => ()
        }
    }
    sum
}

// 读操作：遍历大对象数组
func readLargeObjects(objects: ArrayList<LargeObject>): Int64 {
    var sum: Int64 = 0
    for (obj in objects) {
        sum += obj.checksum()
    }
    sum
}

main() {
    println("========================================")
    println("  Case 4: Read-Heavy Benchmark (10:1)")
    println("  10 reads per 1 write operation")
    println("========================================")
    println("")

    let totalStart = DateTime.now()

    // ---- Phase 1: 构建工作集 ----
    println("[PHASE1] Building working set...")
    let t0 = DateTime.now()

    // 5 棵 depth=16 树 (每棵 131071 节点)
    let trees = ArrayList<TreeNode>()
    for (_ in 0..5) {
        trees.add(buildTree(16))
    }

    // 5 条链表 (每条 1000 节点 × 2KB payload)
    let chains = ArrayList<PayloadNode>()
    for (_ in 0..5) {
        chains.add(buildChain(1000, 256))  // 256 × 8 = 2KB
    }

    // HashMap: 50000 KV
    let map = HashMap<Int64, ArrayList<Int64>>()
    let mapSize: Int64 = 50000
    for (i in 0..mapSize) {
        let vals = ArrayList<Int64>()
        vals.add(i)
        vals.add(i * 2)
        map[i] = vals
    }

    // 20 个大对象 (每个 ~4MB)
    let largeObjects = ArrayList<LargeObject>()
    for (i in 0..20) {
        largeObjects.add(LargeObject(524288, i + 1))  // 524288 × 8 = 4MB
    }

    let phase1Ms = elapsedMs(t0)
    println("[PHASE1] Done: 5 trees + 5 chains + 50K map + 20 large objects, ${phase1Ms} ms")
    println("")

    // ---- Phase 2: 读密集型工作 (访问:创建 = 10:1) ----
    let workRounds: Int64 = 50
    var totalReads: Int64 = 0
    var totalWrites: Int64 = 0

    println("[PHASE2] Starting read-heavy work: ${workRounds} rounds (10 reads + 1 write each)")

    for (round in 0..workRounds) {
        let roundStart = DateTime.now()

        // ---- 10 次读操作 ----
        for (_ in 0..10) {
            // 读所有树
            for (tree in trees) {
                let _ = readTree(tree)
            }
            // 读所有链表
            for (chain in chains) {
                let _ = readChain(chain)
            }
            // 读 HashMap
            let _ = readMap(map, mapSize)
            // 读大对象
            let _ = readLargeObjects(largeObjects)
            totalReads += 1
        }

        // ---- 1 次写操作 ----
        // 替换一棵树 (产生垃圾)
        let treeIdx = round % 5
        trees[treeIdx] = buildTree(16)

        // 刷新 HashMap 的一个 batch (1000 kv)
        let batchStart = (round * 1000) % mapSize
        for (i in batchStart..(batchStart + 1000)) {
            let key = i % mapSize
            let vals = ArrayList<Int64>()
            vals.add(key + round)
            vals.add(key * 3)
            map[key] = vals
        }
        totalWrites += 1

        let roundMs = elapsedMs(roundStart)
        if (round % 10 == 0) {
            println("[PHASE2] Round ${round + 1}/${workRounds}: ${roundMs} ms")
        }
    }
    println("")

    // ---- Phase 3: 最终验证 ----
    println("[PHASE3] Final verification...")
    let t3 = DateTime.now()
    var treeOk: Int64 = 0
    for (tree in trees) {
        if (countNodes(Some(tree)) == 131071) { treeOk += 1 }
    }
    var largeOk: Int64 = 0
    for (obj in largeObjects) {
        if (obj.data.size == 524288) { largeOk += 1 }
    }
    let phase3Ms = elapsedMs(t3)
    println("[PHASE3] Trees: ${treeOk}/5 intact, LargeObjects: ${largeOk}/20 intact, ${phase3Ms} ms")
    println("")

    // ---- 汇总 ----
    let totalMs = elapsedMs(totalStart)
    let ratio = if (totalWrites > 0) { totalReads / totalWrites } else { 0 }
    println("========================================")
    println("[BENCH] SUMMARY (Read-Heavy 10:1)")
    println("[BENCH] Total reads:    ${totalReads}")
    println("[BENCH] Total writes:   ${totalWrites}")
    println("[BENCH] Read:Write:     ${ratio}:1")
    println("[BENCH] Trees intact:   ${treeOk}/5")
    println("[BENCH] Large intact:   ${largeOk}/20")
    println("[BENCH] Total wall time: ${totalMs} ms")
    println("========================================")
}
