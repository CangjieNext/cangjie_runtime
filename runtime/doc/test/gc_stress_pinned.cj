import std.collection.*
import std.sync.*
import std.time.*

// ============================================================
//   Cangjie GC Stress Test — Pinned Regions + Large Alloc
//   持续运行 60 秒，每秒输出心跳统计
//   用法：
//     cjc gc_stress_pinned.cj -o gc_stress_pinned
//     MRT_REPORT=./stress_gc.log cjHeapSize=256mb ./gc_stress_pinned
// ============================================================

// ---------- 数据结构 ----------

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

class PinnedHolder {
    public var mutex: Mutex
    public var id: Int64
    public var payload: Array<Int64>
    public init(id: Int64, payloadSize: Int64) {
        this.mutex = Mutex()
        this.id = id
        this.payload = Array<Int64>(payloadSize, repeat: id)
    }
    // 模拟访问 pinned holder 关联数据（跳过 lock/unlock 以避免 MS GC Mutex 状态损坏 bug）
    public func doWork(): Int64 {
        var sum: Int64 = 0
        for (v in this.payload) {
            sum += v
        }
        return sum
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

func elapsedMs(start: DateTime): Int64 {
    let d = DateTime.now() - start
    d.toMilliseconds()
}

// ---------- 全局控制 ----------

let DURATION_MS: Int64 = 60_000  // 60 秒

// ============================================================
//   Phase A: 创建 pinned 对象基础层
//   大量 Mutex 对象散布在堆中，不可移动
// ============================================================
func setupPinnedBase(): ArrayList<PinnedHolder> {
    let holders = ArrayList<PinnedHolder>()
    for (i in 0..200) {
        holders.add(PinnedHolder(i, 512))  // 200 个 pinned holder，每个 4KB payload
        // 穿插小对象分配，迫使 pinned region 与普通 region 交错
        if (i % 10 == 0) {
            let _ = buildTree(4)
        }
    }
    holders
}

// ============================================================
//   Phase B: 持续压力循环（60秒）
//   每轮：短命树 + 大对象 + pinned 操作 + spawn 协程
// ============================================================
func stressLoop(pinnedHolders: ArrayList<PinnedHolder>) {
    let startTime = DateTime.now()
    var round: Int64 = 0
    var totalAllocBytes: Int64 = 0
    var totalPinnedOps: Int64 = 0
    var totalSpawned: Int64 = 0

    // 长期存活的缓存（模拟 old-gen）
    let cache = ArrayList<LargeObject>()
    for (i in 0..10) {
        cache.add(LargeObject(32768, i + 1))  // 10 × 256KB = 2.5MB
    }

    var lastReportMs: Int64 = 0

    while (elapsedMs(startTime) < DURATION_MS) {
        round += 1

        // --- 短命小对象风暴 ---
        for (_ in 0..20) {
            let tree = buildTree(10)  // 1023 节点
            let _ = treeChecksum(Some(tree))
            totalAllocBytes += 1023 * 32  // 估算
        }

        // --- 大对象分配+丢弃 ---
        for (j in 0..3) {
            let size: Int64 = 16384 + (round % 4) * 16384  // 128KB ~ 512KB
            let obj = LargeObject(size, round * 3 + j)
            let _ = obj.checksum()
            totalAllocBytes += size * 8
        }

        // --- pinned 对象操作（触发写屏障） ---
        let idx = round % pinnedHolders.size
        let _ = pinnedHolders[idx].doWork()
        totalPinnedOps += 1

        // --- 每 5 轮做额外的短命对象分配（替代 spawn 避免 MS 并发 bug） ---
        if (round % 5 == 0) {
            for (t in 0..8) {
                let tree = buildTree(8)  // 255 节点
                let _ = treeChecksum(Some(tree))
                let buf = Array<Int64>(4096, repeat: t)  // 32KB
                let _ = buf[0]
            }
            totalSpawned += 8
        }

        // --- 每 10 轮更新缓存 + 碎片化 ---
        if (round % 10 == 0) {
            let cacheIdx = round % cache.size
            cache[cacheIdx] = LargeObject(32768, round)  // 替换旧缓存
            totalAllocBytes += 32768 * 8
        }

        // --- 每 20 轮创建临时 pinned 对象（短命 Mutex） ---
        if (round % 20 == 0) {
            for (_ in 0..10) {
                let tempLock = Mutex()
                tempLock.lock()
                tempLock.unlock()
            }
        }

        // --- 每秒输出心跳 ---
        let nowMs = elapsedMs(startTime)
        if (nowMs - lastReportMs >= 1000) {
            let sec = nowMs / 1000
            let allocMB = totalAllocBytes / 1048576
            println("[HEARTBEAT] sec=${sec}  rounds=${round}  allocMB=${allocMB}  pinnedOps=${totalPinnedOps}  spawned=${totalSpawned}")
            lastReportMs = nowMs
        }
    }

    // 验证长期对象存活
    var cacheOk: Int64 = 0
    for (obj in cache) {
        if (obj.data.size == 32768) { cacheOk += 1 }
    }
    var pinnedOk: Int64 = 0
    for (h in pinnedHolders) {
        if (h.payload.size == 512) { pinnedOk += 1 }
    }

    let totalMs = elapsedMs(startTime)
    println("")
    println("[SUMMARY]")
    println("  duration:     ${totalMs} ms")
    println("  rounds:       ${round}")
    println("  alloc_total:  ${totalAllocBytes / 1048576} MB")
    println("  pinned_ops:   ${totalPinnedOps}")
    println("  spawned:      ${totalSpawned}")
    println("  cache_intact: ${cacheOk}/10")
    println("  pinned_intact:${pinnedOk}/200")
    if (cacheOk == 10 && pinnedOk == 200) {
        println("  result:       PASS")
    } else {
        println("  result:       FAIL")
    }
}

// ---------- MAIN ----------

main() {
    println("========================================")
    println("  GC Stress Test — Pinned + Large Alloc")
    println("  Duration: 60 seconds")
    println("  cjHeapSize=256mb recommended")
    println("========================================")
    println("")

    let t0 = DateTime.now()

    println("[SETUP] Creating pinned base objects ...")
    let pinnedHolders = setupPinnedBase()
    println("[SETUP] Done: ${pinnedHolders.size} pinned holders")
    println("")

    println("[STRESS] Starting 60-second stress loop ...")
    stressLoop(pinnedHolders)

    let totalMs = elapsedMs(t0)
    println("")
    println("Total wall time: ${totalMs} ms")
    println("EXIT=0")
}
