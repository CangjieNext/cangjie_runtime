import std.collection.*
import std.runtime.*
import std.time.*

// ============================================================
//   Case 5: 混合对象模型
//   大对象 + 锁保护对象 + 普通小对象
//   50% 存活 + 50% 临时, 50% 访问 + 50% 创建
//   用法：
//     cjc benchmark.cj -o benchmark
//     MRT_REPORT=./runtime.log cjHeapSize=1gb ./benchmark
// ============================================================

// ---------- 普通小对象: TreeNode (~32 bytes) ----------
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

// ---------- 大对象: LargeObject (64KB ~ 8MB) ----------
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

// ---------- 中等对象: PayloadNode (~1-16KB) ----------
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

// ---------- 锁保护对象: 模拟带同步计数器的共享状态 ----------
// 包含可变状态 + 引用变更，会频繁触发写屏障
class SyncCounter {
    public var count: Int64
    public var history: ArrayList<Int64>
    public var label: String
    public var relatedObjects: ArrayList<Option<SyncCounter>>
    public init(label: String) {
        this.count = 0
        this.history = ArrayList<Int64>()
        this.label = label
        this.relatedObjects = ArrayList<Option<SyncCounter>>()
    }
    public func increment(): Unit {
        this.count += 1
        this.history.add(this.count)
        // 保持 history 不超过 100 条
        if (this.history.size > 100) {
            let newHistory = ArrayList<Int64>()
            for (i in 50..this.history.size) {
                newHistory.add(this.history[i])
            }
            this.history = newHistory  // 触发写屏障
        }
    }
    public func linkTo(other: SyncCounter): Unit {
        this.relatedObjects.add(Some(other))  // 引用变更 → 写屏障
    }
    public func readAll(): Int64 {
        var sum = this.count
        for (h in this.history) { sum += h }
        sum
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

main() {
    println("========================================")
    println("  Case 5: Mixed Objects Benchmark")
    println("  Large + SyncCounter + Small objects")
    println("  50% alive, 50% access/create")
    println("========================================")
    println("")

    let totalStart = DateTime.now()

    // ============================================================
    //  Phase 1: 构建存活集合 (50% 持久)
    // ============================================================
    println("[PHASE1] Building persistent set (mixed types)...")
    let t0 = DateTime.now()

    // 存活: 10 棵 depth=14 树 (~16383 节点/棵, ~5MB 总计)
    let persistentTrees = ArrayList<TreeNode>()
    for (_ in 0..10) {
        persistentTrees.add(buildTree(14))
    }

    // 存活: 10 个大对象 (每个 ~2MB = 262144 Int64)
    let persistentLarge = ArrayList<LargeObject>()
    for (i in 0..10) {
        persistentLarge.add(LargeObject(262144, i + 1))
    }

    // 存活: 20 个锁保护对象，互相链接
    let persistentCounters = ArrayList<SyncCounter>()
    for (i in 0..20) {
        persistentCounters.add(SyncCounter("persistent_${i}"))
    }
    // 创建链接网络
    for (i in 0..20) {
        for (j in 0..3) {
            let target = (i + j + 1) % 20
            persistentCounters[i].linkTo(persistentCounters[target])
        }
    }

    // 存活: 10 条链表 (每条 200 节点 × 2KB payload)
    let persistentChains = ArrayList<PayloadNode>()
    for (_ in 0..10) {
        persistentChains.add(buildChain(200, 256))  // 256 × 8 = 2KB
    }

    // 存活: 1 个 HashMap (20000 KV)
    let persistentMap = HashMap<Int64, ArrayList<Int64>>()
    for (i in 0..20000) {
        let vals = ArrayList<Int64>()
        vals.add(i)
        vals.add(i * 3)
        persistentMap[i] = vals
    }

    let phase1Ms = elapsedMs(t0)
    println("[PHASE1] Done: 10 trees + 10 large + 20 counters + 10 chains + 20K map, ${phase1Ms} ms")
    println("")

    // ============================================================
    //  Phase 2: 混合工作 (50% 访问 + 50% 创建, 交替进行)
    // ============================================================
    let workRounds: Int64 = 30
    var totalAccessOps: Int64 = 0
    var totalCreateOps: Int64 = 0

    println("[PHASE2] Starting mixed work: ${workRounds} rounds")

    for (round in 0..workRounds) {
        let roundStart = DateTime.now()

        // ---- 访问阶段 (50% 时间) ----

        // 读树
        for (tree in persistentTrees) {
            let _ = treeChecksum(Some(tree))
        }
        totalAccessOps += 10

        // 读大对象
        for (obj in persistentLarge) {
            let _ = obj.checksum()
        }
        totalAccessOps += 10

        // 操作锁对象 (读 + 写计数器)
        for (counter in persistentCounters) {
            counter.increment()
            let _ = counter.readAll()
        }
        totalAccessOps += 20

        // 读链表
        for (chain in persistentChains) {
            let _ = sumChain(Some(chain))
        }
        totalAccessOps += 10

        // 查 HashMap
        for (i in 0..20000) {
            match (persistentMap.get(i)) {
                case Some(v) => let _ = v[0]
                case None => ()
            }
        }
        totalAccessOps += 1

        // ---- 创建阶段 (50% 时间) ----

        // 临时小对象: 20 棵 depth=10 树 (1023 节点/棵)
        for (_ in 0..20) {
            let tree = buildTree(10)
            let _ = countNodes(Some(tree))
        }
        totalCreateOps += 20

        // 临时大对象: 5 × 1MB
        for (i in 0..5) {
            let obj = LargeObject(131072, round * 5 + i + 1)  // 131072 × 8 = 1MB
            let _ = obj.checksum()
        }
        totalCreateOps += 5

        // 临时锁对象: 10 个，互相链接后丢弃
        let tempCounters = ArrayList<SyncCounter>()
        for (i in 0..10) {
            let c = SyncCounter("temp_${round}_${i}")
            c.increment()
            c.increment()
            c.increment()
            tempCounters.add(c)
        }
        for (i in 0..10) {
            for (j in 0..2) {
                let target = (i + j + 1) % 10
                tempCounters[i].linkTo(tempCounters[target])
            }
        }
        totalCreateOps += 10

        // 临时链表: 5 × 100 节点 × 4KB payload
        for (_ in 0..5) {
            let chain = buildChain(100, 512)  // 512 × 8 = 4KB
            let _ = sumChain(Some(chain))
        }
        totalCreateOps += 5

        // 临时 HashMap: 5000 KV
        let tempMap = HashMap<Int64, Int64>()
        for (k in 0..5000) {
            tempMap[k] = k * round
        }
        totalCreateOps += 1

        let roundMs = elapsedMs(roundStart)
        if (round % 5 == 0) {
            println("[PHASE2] Round ${round + 1}/${workRounds}: ${roundMs} ms")
        }
    }
    println("")

    // ============================================================
    //  Phase 3: 验证存活集合完整性
    // ============================================================
    println("[PHASE3] Verifying persistent set...")
    let t3 = DateTime.now()

    var treeOk: Int64 = 0
    for (tree in persistentTrees) {
        if (countNodes(Some(tree)) == 16383) { treeOk += 1 }
    }

    var largeOk: Int64 = 0
    for (obj in persistentLarge) {
        if (obj.data.size == 262144) { largeOk += 1 }
    }

    var counterOk: Int64 = 0
    for (counter in persistentCounters) {
        // 每轮 increment 一次, 共 workRounds 轮
        if (counter.count == workRounds) { counterOk += 1 }
    }

    var chainOk: Int64 = 0
    for (chain in persistentChains) {
        let sum = sumChain(Some(chain))
        let expected: Int64 = 200 * 199 / 2
        if (sum == expected) { chainOk += 1 }
    }

    let phase3Ms = elapsedMs(t3)
    println("[PHASE3] Trees:    ${treeOk}/10 intact")
    println("[PHASE3] Large:    ${largeOk}/10 intact")
    println("[PHASE3] Counters: ${counterOk}/20 correct count")
    println("[PHASE3] Chains:   ${chainOk}/10 intact")
    println("[PHASE3] Verify time: ${phase3Ms} ms")
    println("")

    // ---- 汇总 ----
    let totalMs = elapsedMs(totalStart)
    println("========================================")
    println("[BENCH] SUMMARY (Mixed Objects)")
    println("[BENCH] Access ops:     ${totalAccessOps}")
    println("[BENCH] Create ops:     ${totalCreateOps}")
    println("[BENCH] Trees intact:   ${treeOk}/10")
    println("[BENCH] Large intact:   ${largeOk}/10")
    println("[BENCH] Counters ok:    ${counterOk}/20")
    println("[BENCH] Chains intact:  ${chainOk}/10")
    println("[BENCH] Total wall time: ${totalMs} ms")
    println("========================================")
}
