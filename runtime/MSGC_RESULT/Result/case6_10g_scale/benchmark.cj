import std.collection.*
import std.runtime.*
import std.time.*

// ============================================================
//   Case 6: 10GB 大规模压力测试
//   ~5GB 持久存活 + 每轮 ~500MB 临时翻搅 × 10 轮
//   50% 访问 + 50% 创建
//   用法：
//     cjc benchmark.cj -o benchmark
//     MRT_REPORT=./runtime.log cjHeapSize=12gb ./benchmark
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

main() {
    println("========================================")
    println("  Case 6: 10GB Scale Benchmark")
    println("  ~5GB alive + ~5GB temp churn")
    println("========================================")
    println("")

    let totalStart = DateTime.now()

    // ============================================================
    //  Phase 1: 构建存活集合 (~5GB)
    // ============================================================
    println("[PHASE1] Building persistent set (~5GB)...")
    let t0 = DateTime.now()

    // 存活大对象: 300 × 2097152 Int64 = 300 × 16MB = ~4.8GB
    let persistentLargeCount: Int64 = 300
    let largeElementCount: Int64 = 2097152  // 16MB per object
    let persistentLarge = ArrayList<LargeObject>()
    for (i in 0..persistentLargeCount) {
        persistentLarge.add(LargeObject(largeElementCount, i + 1))
        if ((i + 1) % 50 == 0) {
            let allocMB = (i + 1) * largeElementCount * 8 / 1048576
            println("[PHASE1] Allocated ${i + 1}/${persistentLargeCount} large objects (~${allocMB} MB)")
        }
    }

    // 存活树: 20 棵 depth=16 (131071 节点/棵, ~80MB 总计)
    let persistentTrees = ArrayList<TreeNode>()
    for (_ in 0..20) {
        persistentTrees.add(buildTree(16))
    }

    // 存活链表: 20 条 (每条 500 节点 × 4KB payload, ~40MB 总计)
    let persistentChains = ArrayList<PayloadNode>()
    for (_ in 0..20) {
        persistentChains.add(buildChain(500, 512))  // 512 × 8 = 4KB
    }

    // 存活 HashMap: 100000 KV (~50MB)
    let persistentMap = HashMap<Int64, ArrayList<Int64>>()
    for (i in 0..100000) {
        let vals = ArrayList<Int64>()
        vals.add(i)
        vals.add(i * 2)
        persistentMap[i] = vals
    }

    let phase1Ms = elapsedMs(t0)
    let persistentMB = persistentLargeCount * largeElementCount * 8 / 1048576
    println("[PHASE1] Done: ${persistentLargeCount} large (~${persistentMB} MB) + 20 trees + 20 chains + 100K map")
    println("[PHASE1] Build time: ${phase1Ms} ms")
    println("")

    // ============================================================
    //  Phase 2: 混合工作 (50% 访问 + 50% 创建, ~500MB temp/轮)
    // ============================================================
    let churnRounds: Int64 = 10
    var totalAccessOps: Int64 = 0
    var totalCreateBytes: Int64 = 0

    println("[PHASE2] Starting mixed work: ${churnRounds} rounds, ~500MB temp per round")

    for (round in 0..churnRounds) {
        let roundStart = DateTime.now()

        // ---- 访问阶段 (50%) ----

        // 访问大对象: 抽样 checksum (每轮访问 30 个)
        let sampleStep = persistentLargeCount / 30
        var accessSum: Int64 = 0
        for (i in 0..30) {
            let idx = i * sampleStep
            if (idx < persistentLargeCount) {
                accessSum += persistentLarge[idx].checksum()
            }
        }
        totalAccessOps += 30

        // 访问树: 遍历所有 20 棵
        for (tree in persistentTrees) {
            let _ = treeChecksum(Some(tree))
        }
        totalAccessOps += 20

        // 访问链表: 遍历所有 20 条
        for (chain in persistentChains) {
            let _ = sumChain(Some(chain))
        }
        totalAccessOps += 20

        // 访问 HashMap: 全量读
        for (i in 0..100000) {
            match (persistentMap.get(i)) {
                case Some(v) => accessSum += v[0]
                case None => ()
            }
        }
        totalAccessOps += 1

        // ---- 创建阶段 (50%): ~500MB 临时对象 ----
        var roundBytes: Int64 = 0

        // 临时大对象: 25 × 2097152 Int64 = 25 × 16MB = ~400MB
        for (i in 0..25) {
            let obj = LargeObject(2097152, round * 25 + i + 1)
            let _ = obj.checksum()
            roundBytes += 2097152 * 8
        }

        // 临时树: 50 × depth=12 (4095 节点/棵)
        for (_ in 0..50) {
            let tree = buildTree(12)
            let _ = countNodes(Some(tree))
            roundBytes += 4095 * 32
        }

        // 临时链表: 10 × 500 节点 × 2KB payload
        for (_ in 0..10) {
            let chain = buildChain(500, 256)
            let _ = sumChain(Some(chain))
            roundBytes += 500 * 2048
        }

        // 临时 HashMap: 50000 KV
        let tempMap = HashMap<Int64, Int64>()
        for (k in 0..50000) {
            tempMap[k] = k * round
        }
        roundBytes += 50000 * 16

        totalCreateBytes += roundBytes

        let roundMs = elapsedMs(roundStart)
        let roundMB = roundBytes / 1048576
        println("[PHASE2] Round ${round + 1}/${churnRounds}: ~${roundMB} MB temp, ${roundMs} ms")
    }
    println("")

    // ============================================================
    //  Phase 3: 验证存活集合完整性
    // ============================================================
    println("[PHASE3] Verifying persistent set...")
    let t3 = DateTime.now()

    // 验证大对象
    var largeOk: Int64 = 0
    var largeSampleOk: Int64 = 0
    for (obj in persistentLarge) {
        if (obj.data.size == largeElementCount) { largeOk += 1 }
    }
    // 抽样校验 checksum (全部校验太慢)
    let verifySampleStep = persistentLargeCount / 20
    for (i in 0..20) {
        let idx = i * verifySampleStep
        if (idx < persistentLargeCount) {
            let obj = persistentLarge[idx]
            let expected = obj.id * largeElementCount * (largeElementCount - 1) / 2
            if (obj.checksum() == expected) { largeSampleOk += 1 }
        }
    }

    // 验证树
    var treeOk: Int64 = 0
    for (tree in persistentTrees) {
        if (countNodes(Some(tree)) == 131071) { treeOk += 1 }
    }

    // 验证链表
    var chainOk: Int64 = 0
    for (chain in persistentChains) {
        let sum = sumChain(Some(chain))
        let expected: Int64 = 500 * 499 / 2
        if (sum == expected) { chainOk += 1 }
    }

    let phase3Ms = elapsedMs(t3)
    println("[PHASE3] Large size:      ${largeOk}/${persistentLargeCount} intact")
    println("[PHASE3] Large checksum:  ${largeSampleOk}/20 sampled ok")
    println("[PHASE3] Trees:           ${treeOk}/20 intact")
    println("[PHASE3] Chains:          ${chainOk}/20 intact")
    println("[PHASE3] Verify time:     ${phase3Ms} ms")
    println("")

    // ---- 汇总 ----
    let totalMs = elapsedMs(totalStart)
    let totalCreateMB = totalCreateBytes / 1048576
    println("========================================")
    println("[BENCH] SUMMARY (10GB Scale)")
    println("[BENCH] Persistent:     ~${persistentMB} MB (${persistentLargeCount} large + 20 trees + 20 chains + 100K map)")
    println("[BENCH] Total temp:     ~${totalCreateMB} MB across ${churnRounds} rounds")
    println("[BENCH] Access ops:     ${totalAccessOps}")
    println("[BENCH] Large intact:   ${largeOk}/${persistentLargeCount}")
    println("[BENCH] Large cs ok:    ${largeSampleOk}/20")
    println("[BENCH] Trees intact:   ${treeOk}/20")
    println("[BENCH] Chains intact:  ${chainOk}/20")
    println("[BENCH] Total wall time: ${totalMs} ms")
    println("========================================")
}
