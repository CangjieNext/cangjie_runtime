import std.collection.*
import std.time.*

// ============================================================
//   Cangjie GC Benchmark Suite
//   目标：功能正确性验证 + 性能基线采集
//   输出：每个 Phase 输出 PASS/FAIL + 耗时(ms)，便于 grep 提取
//   用法：
//     cjc gc_benchmark.cj -o gc_benchmark
//     MRT_REPORT=./gc_bench.log cjHeapSize=256mb ./gc_benchmark
// ============================================================

// ---------- 数据结构定义 ----------

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

// ---------- 工具函数 ----------

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

// ---------- 功能测试 ----------

// F1: 二叉树完整性 — 建树→校验→丢弃→再建→校验
func benchF1_TreeIntegrity(): Bool {
    let t0 = DateTime.now()
    var pass = true
    // depth=16 完全二叉树: 2^17 - 1 = 131071 节点
    let expectedNodes: Int64 = 131071
    var expectedCs: Int64 = 0
    for (round in 0..20) {
        let tree = buildTree(16)
        let cs = treeChecksum(Some(tree))
        let nodes = countNodes(Some(tree))
        if (nodes != expectedNodes) { pass = false }
        if (round == 0) {
            expectedCs = cs
        } else if (cs != expectedCs) {
            pass = false
        }
    }
    let ms = elapsedMs(t0)
    let tag = if (pass) { "PASS" } else { "FAIL" }
    println("[BENCH] F1_TreeIntegrity         ${tag}  ${ms} ms")
    pass
}

// F2: 分代存活验证 — 长期树 + 短命树翻搅
func benchF2_Generational(): Bool {
    let t0 = DateTime.now()
    let longLived = buildTree(18)  // 262143 节点
    let expectedCs = treeChecksum(Some(longLived))
    var pass = true
    for (_ in 0..200) {
        let temp = buildTree(14)  // 16383 节点，短命
        let _ = treeChecksum(Some(temp))
    }
    let verify = treeChecksum(Some(longLived))
    if (verify != expectedCs) { pass = false }
    let ms = elapsedMs(t0)
    let tag = if (pass) { "PASS" } else { "FAIL" }
    println("[BENCH] F2_Generational          ${tag}  ${ms} ms")
    pass
}

// F3: 大对象正确性 — 64KB~1MB 分配+校验
func benchF3_LargeObject(): Bool {
    let t0 = DateTime.now()
    let sizes: Array<Int64> = [8192, 32768, 65536, 131072]  // 64KB~1MB
    var pass = true
    for (round in 0..50) {
        for (s in 0..4) {
            let obj = LargeObject(sizes[s], round + 1)
            // 验证 checksum — 元素为 i * id
            let n = sizes[s]
            let id = round + 1
            let expected = id * n * (n - 1) / 2
            if (obj.checksum() != expected) { pass = false }
        }
    }
    let ms = elapsedMs(t0)
    let tag = if (pass) { "PASS" } else { "FAIL" }
    println("[BENCH] F3_LargeObject           ${tag}  ${ms} ms")
    pass
}

// F4: 链表载荷完整性
func benchF4_LinkedList(): Bool {
    let t0 = DateTime.now()
    var pass = true
    for (_ in 0..30) {
        let chain = buildChain(500, 1024)  // 500 节点 × 8KB 载荷
        let sum = sumChain(Some(chain))
        let expected: Int64 = 500 * 499 / 2  // 0+1+2+...+499
        if (sum != expected) { pass = false }
    }
    let ms = elapsedMs(t0)
    let tag = if (pass) { "PASS" } else { "FAIL" }
    println("[BENCH] F4_LinkedList            ${tag}  ${ms} ms")
    pass
}

// F5: HashMap 正确性
func benchF5_HashMap(): Bool {
    let t0 = DateTime.now()
    var pass = true
    for (round in 0..20) {
        let map = HashMap<Int64, ArrayList<Int64>>()
        let mapSize: Int64 = 10000
        for (i in 0..mapSize) {
            let vals = ArrayList<Int64>()
            vals.add(i)
            vals.add(i * 2)
            map[i] = vals
        }
        // 验证
        for (i in 0..mapSize) {
            match (map.get(i)) {
                case Some(v) =>
                    if (v[0] != i || v[1] != i * 2) { pass = false }
                case None => pass = false
            }
        }
    }
    let ms = elapsedMs(t0)
    let tag = if (pass) { "PASS" } else { "FAIL" }
    println("[BENCH] F5_HashMap               ${tag}  ${ms} ms")
    pass
}

// ---------- 性能测试 ----------

// P1: 小对象分配吞吐 — 高频建树(depth=16)×50 轮
func benchP1_SmallAlloc(): Int64 {
    let t0 = DateTime.now()
    let rounds: Int64 = 50
    var totalNodes: Int64 = 0
    for (_ in 0..rounds) {
        let tree = buildTree(16)  // 131071 节点
        totalNodes += countNodes(Some(tree))
    }
    let ms = elapsedMs(t0)
    // 吞吐: 节点数/ms ≈ 对象分配速度
    let throughput = if (ms > 0) { totalNodes / ms } else { 0 }
    println("[BENCH] P1_SmallAlloc            ${ms} ms  ${totalNodes} nodes  ${throughput} nodes/ms")
    ms
}

// P2: 大对象分配吞吐 — 256KB×200 轮
func benchP2_LargeAlloc(): Int64 {
    let t0 = DateTime.now()
    let rounds: Int64 = 200
    var totalBytes: Int64 = 0
    for (i in 0..rounds) {
        let obj = LargeObject(32768, i + 1)  // 256KB each
        totalBytes += obj.data.size * 8
        let _ = obj.checksum()
    }
    let ms = elapsedMs(t0)
    let mbps = if (ms > 0) { totalBytes / 1024 / ms } else { 0 }  // KB/ms ≈ MB/s
    println("[BENCH] P2_LargeAlloc            ${ms} ms  ${totalBytes / 1048576} MB  ${mbps} MB/s")
    ms
}

// P3: 混合负载 — 大对象+小对象+HashMap 交替
func benchP3_MixedWorkload(): Int64 {
    let t0 = DateTime.now()
    let rounds: Int64 = 30
    for (round in 0..rounds) {
        // 小对象: 10 棵 depth=12 树
        for (_ in 0..10) {
            let t = buildTree(12)  // 4095 节点
            let _ = treeChecksum(Some(t))
        }
        // 大对象: 3 × 256KB
        for (j in 0..3) {
            let obj = LargeObject(32768, round * 3 + j + 1)
            let _ = obj.checksum()
        }
        // HashMap: 5000 KV
        let map = HashMap<Int64, Int64>()
        for (k in 0..5000) {
            map[k] = k * k
        }
    }
    let ms = elapsedMs(t0)
    println("[BENCH] P3_MixedWorkload         ${ms} ms  ${rounds} rounds")
    ms
}

// P4: 长期存活+短命翻搅 — 模拟真实服务器
func benchP4_LongLivedChurn(): Int64 {
    let t0 = DateTime.now()
    // 长期存活: 20 × 512KB 大对象
    let persistent = ArrayList<LargeObject>()
    for (i in 0..20) {
        persistent.add(LargeObject(65536, i + 1))
    }
    // 短命翻搅: 100 轮
    for (round in 0..100) {
        let trees = ArrayList<TreeNode>()
        for (_ in 0..100) {
            trees.add(buildTree(8))  // 255 节点 × 100
        }
        let chain = buildChain(200, 512)
        let _ = sumChain(Some(chain))
        let arr = ArrayList<Int64>()
        for (i in 0..50000) { arr.add(i) }
    }
    // 验证长期对象存活
    var intactCount: Int64 = 0
    for (obj in persistent) {
        if (obj.data.size == 65536) { intactCount += 1 }
    }
    let ms = elapsedMs(t0)
    println("[BENCH] P4_LongLivedChurn        ${ms} ms  persistent_intact=${intactCount}/20")
    ms
}

// P5: 碎片化压力 — 交替分配释放制造空洞
func benchP5_Fragmentation(): Int64 {
    let t0 = DateTime.now()
    let rounds: Int64 = 15
    for (round in 0..rounds) {
        // 分配 30 个 128KB 大对象
        let holders = ArrayList<Option<LargeObject>>()
        for (i in 0..30) {
            holders.add(Some(LargeObject(16384, round * 30 + i + 1)))
        }
        // 释放奇数 → 空洞
        for (i in 0..30) {
            if (i % 2 == 1) { holders[i] = None }
        }
        // 在空洞中分配更大对象 256KB
        let bigFills = ArrayList<LargeObject>()
        for (j in 0..8) {
            bigFills.add(LargeObject(32768, round * 100 + j + 1))
        }
        // 穿插小对象
        for (_ in 0..500) {
            let _ = buildTree(6)
        }
    }
    let ms = elapsedMs(t0)
    println("[BENCH] P5_Fragmentation         ${ms} ms  ${rounds} rounds")
    ms
}

// ---------- MAIN ----------

main() {
    println("========================================")
    println("  Cangjie GC Benchmark Suite")
    println("  Tip: MRT_REPORT=./gc_bench.log cjHeapSize=256mb ./gc_benchmark")
    println("========================================")
    println("")

    let totalStart = DateTime.now()

    // ---- 功能测试 ----
    println("--- Functional Tests ---")
    var funcPass: Int64 = 0
    var funcTotal: Int64 = 0
    funcTotal += 1; if (benchF1_TreeIntegrity())  { funcPass += 1 }
    funcTotal += 1; if (benchF2_Generational())   { funcPass += 1 }
    funcTotal += 1; if (benchF3_LargeObject())    { funcPass += 1 }
    funcTotal += 1; if (benchF4_LinkedList())      { funcPass += 1 }
    funcTotal += 1; if (benchF5_HashMap())         { funcPass += 1 }
    println("")

    // ---- 性能测试 ----
    println("--- Performance Tests ---")
    let p1 = benchP1_SmallAlloc()
    let p2 = benchP2_LargeAlloc()
    let p3 = benchP3_MixedWorkload()
    let p4 = benchP4_LongLivedChurn()
    let p5 = benchP5_Fragmentation()
    println("")

    // ---- 汇总 ----
    let totalMs = elapsedMs(totalStart)
    println("========================================")
    println("[BENCH] SUMMARY")
    println("[BENCH] Functional: ${funcPass}/${funcTotal} passed")
    println("[BENCH] P1_SmallAlloc:      ${p1} ms")
    println("[BENCH] P2_LargeAlloc:      ${p2} ms")
    println("[BENCH] P3_MixedWorkload:   ${p3} ms")
    println("[BENCH] P4_LongLivedChurn:  ${p4} ms")
    println("[BENCH] P5_Fragmentation:   ${p5} ms")
    println("[BENCH] Total wall time:    ${totalMs} ms")
    println("========================================")
}
