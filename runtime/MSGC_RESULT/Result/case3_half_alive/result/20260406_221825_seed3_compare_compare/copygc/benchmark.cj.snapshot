import std.collection.*
import std.runtime.*
import std.time.*

// ============================================================
//   Case 3: 半存活半临时对象模型 (~500MB each)
//   存活集合 ~500MB 持久引用 + 每轮 ~500MB 临时对象翻搅
//   用法：
//     cjc benchmark.cj -o benchmark
//     MRT_REPORT=./runtime.log cjHeapSize=2gb ./benchmark
// ============================================================

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

func elapsedMs(start: DateTime): Int64 {
    let d = DateTime.now() - start
    d.toMilliseconds()
}

main() {
    println("========================================")
    println("  Case 3: Half Alive / Half Temporary")
    println("  ~500MB alive + ~500MB temp per round")
    println("========================================")
    println("")

    let totalStart = DateTime.now()

    // ---- Phase 1: 构建存活集合 (~500MB) ----
    // 60 个 LargeObject, 每个 1048576 个 Int64 = 8MB, 共 ~480MB
    println("[PHASE1] Building persistent set (~500MB)...")
    let t0 = DateTime.now()
    let persistentCount: Int64 = 60
    let elementsPer: Int64 = 1048576  // 8MB per object
    let persistent = ArrayList<LargeObject>()
    for (i in 0..persistentCount) {
        persistent.add(LargeObject(elementsPer, i + 1))
    }
    let phase1Ms = elapsedMs(t0)
    let persistentMB = persistentCount * elementsPer * 8 / 1048576
    println("[PHASE1] Done: ${persistentCount} objects, ~${persistentMB} MB, ${phase1Ms} ms")
    println("")

    // ---- Phase 2: 临时翻搅 (10 轮, 每轮 ~500MB) ----
    let churnRounds: Int64 = 10
    println("[PHASE2] Starting churn: ${churnRounds} rounds, ~500MB temp per round")

    for (round in 0..churnRounds) {
        let roundStart = DateTime.now()
        var roundBytes: Int64 = 0

        // 临时大对象: 30 × 16MB = ~480MB
        // 每个 2097152 个 Int64 = 16MB
        let tempLargeCount: Int64 = 30
        let tempElementsPer: Int64 = 2097152
        for (i in 0..tempLargeCount) {
            let obj = LargeObject(tempElementsPer, round * tempLargeCount + i + 1)
            let _ = obj.checksum()
            roundBytes += tempElementsPer * 8
        }

        // 临时树: 50 × depth=10 ≈ 1023 节点 × 50 ≈ 51150 节点
        for (_ in 0..50) {
            let tree = buildTree(10)
            let _ = countNodes(Some(tree))
            roundBytes += 1023 * 32  // 约 32 bytes/node
        }

        // 临时链表: 5 × 500 节点 × 1KB payload
        for (_ in 0..5) {
            let _ = buildChain(500, 128)  // 128 × 8 = 1KB payload
            roundBytes += 500 * 1024
        }

        let roundMs = elapsedMs(roundStart)
        let roundMB = roundBytes / 1048576
        println("[PHASE2] Round ${round + 1}/${churnRounds}: ~${roundMB} MB temp, ${roundMs} ms")
    }
    println("")

    // ---- Phase 3: 验证存活集合完整性 ----
    println("[PHASE3] Verifying persistent set...")
    let t3 = DateTime.now()
    var intactCount: Int64 = 0
    var checksumOk: Int64 = 0
    for (obj in persistent) {
        if (obj.data.size == elementsPer) { intactCount += 1 }
        let expected = obj.id * elementsPer * (elementsPer - 1) / 2
        if (obj.checksum() == expected) { checksumOk += 1 }
    }
    let phase3Ms = elapsedMs(t3)
    let sizePass = if (intactCount == persistentCount) { "PASS" } else { "FAIL" }
    let csPass = if (checksumOk == persistentCount) { "PASS" } else { "FAIL" }
    println("[PHASE3] Size check: ${sizePass} (${intactCount}/${persistentCount})")
    println("[PHASE3] Checksum:   ${csPass} (${checksumOk}/${persistentCount})")
    println("[PHASE3] Verify time: ${phase3Ms} ms")
    println("")

    // ---- 汇总 ----
    let totalMs = elapsedMs(totalStart)
    println("========================================")
    println("[BENCH] SUMMARY")
    println("[BENCH] Persistent set: ~${persistentMB} MB (${persistentCount} objects)")
    println("[BENCH] Churn rounds:   ${churnRounds} × ~500MB")
    println("[BENCH] Size check:     ${sizePass}")
    println("[BENCH] Checksum:       ${csPass}")
    println("[BENCH] Total wall time: ${totalMs} ms")
    println("========================================")
}
