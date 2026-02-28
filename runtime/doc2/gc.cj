import std.collection.*
import std.sync.*

// ============================================================
//   Cangjie GC Stress Test Suite
//   测试目标：高频创建/销毁对象、大对象分配、复杂对象图、
//            pinned 对象（锁对象）对 GC 碎片化的影响
// ============================================================

// ---------- 数据结构定义 ----------

// Pinned 对象容器：Mutex 在运行时中以 pinned region 分配，GC 不会移动
// 用于模拟真实场景中散布在堆中的锁对象
class PinnedHolder {
    public var mutex: Mutex
    public var id: Int64
    public var payload: Array<Int64>  // 关联数据（普通对象，可被 GC 移动）

    public init(id: Int64, payloadSize: Int64) {
        this.mutex = Mutex()  // pinned 对象 - 分配在 pinned region
        this.id = id
        this.payload = Array<Int64>(payloadSize, repeat: id)
    }

    // 模拟一次 synchronized 操作
    public func doWork(): Int64 {
        mutex.lock()
        var sum: Int64 = 0
        for (v in this.payload) {
            sum += v
        }
        mutex.unlock()
        return sum
    }
}

// 二叉树节点（引用类型，GC 管理）
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

// 大对象：内含不同大小的数组
class LargeObject {
    public var data: Array<Int64>
    public var id: Int64

    public init(elementCount: Int64, id: Int64) {
        this.id = id
        this.data = Array<Int64>(elementCount, { i => i * id })
    }

    public func checksum(): Int64 {
        var sum: Int64 = 0
        for (v in this.data) {
            sum += v
        }
        sum
    }
}

// 链表节点：每个节点携带一块载荷数组（模拟碎片化）
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

// ============================================================
// Test 1: 二叉树翻搅
//   反复创建完全二叉树 → 遍历计算校验和 → 丢弃引用
//   测试：短命对象海量分配 + 树形对象图回收
// ============================================================
func testBinaryTreeChurn(depth: Int64, iterations: Int64) {
    println("[Test 1] Binary Tree Churn  depth=${depth}  iterations=${iterations}")
    var totalCheck: Int64 = 0
    for (i in 0..iterations) {
        let tree = buildTree(depth)
        let cs = treeChecksum(Some(tree))
        totalCheck += cs
        if (i % 10 == 0) {
            let nodes = countNodes(Some(tree))
            println("  round ${i}: nodes=${nodes}  checksum=${cs}")
        }
        // tree 在每轮结束后失去引用 → GC 可回收整棵树
    }
    println("  total checksum: ${totalCheck}")
    println("  DONE\n")
}

// ============================================================
// Test 2: 分代 GC 测试
//   一棵长期存活的大树 + 大量短命临时树
//   测试：GC 在存在老对象时回收年轻对象的效率
// ============================================================
func testGenerational(longDepth: Int64, shortDepth: Int64, iterations: Int64) {
    println("[Test 2] Generational  long-lived=${longDepth}  short-lived=${shortDepth}  iterations=${iterations}")

    // 创建长期存活的大树（不释放）
    let longLived = buildTree(longDepth)
    let longCheck = treeChecksum(Some(longLived))
    println("  long-lived tree checksum: ${longCheck}")

    // 反复创建短命树
    for (i in 0..iterations) {
        let temp = buildTree(shortDepth)
        let cs = treeChecksum(Some(temp))
        if (i % 20 == 0) {
            println("  short-lived round ${i}: checksum=${cs}")
        }
    }

    // 验证长期树完整性
    let verify = treeChecksum(Some(longLived))
    if (verify == longCheck) {
        println("  long-lived tree intact: PASSED")
    } else {
        println("  long-lived tree CORRUPTED! expected=${longCheck} got=${verify}")
    }
    println("  DONE\n")
}

// ============================================================
// Test 3: 大对象分配
//   创建 64KB / 256KB / 512KB / 1MB 的数组对象
//   测试：大对象堆分配 + 回收（LOH 场景）
// ============================================================
func testLargeObjects(iterations: Int64) {
    println("[Test 3] Large Object Allocation  iterations=${iterations}")

    // sizes: Int64 元素个数（每个 8 字节）
    let sizes: Array<Int64> = [8192, 32768, 65536, 131072]     // 64KB, 256KB, 512KB, 1MB
    let labels: Array<String> = ["64KB", "256KB", "512KB", "1MB"]

    for (round in 0..iterations) {
        for (s in 0..4) {
            let obj = LargeObject(sizes[s], round + 1)
            let cs = obj.checksum()
            if (round == 0) {
                println("  ${labels[s]}: elements=${sizes[s]}  checksum=${cs}")
            }
            // obj 在每个内层循环结束后可被回收
        }
        if (round % 5 == 0) {
            println("  round ${round} done")
        }
    }
    println("  DONE\n")
}

// ============================================================
// Test 4: 链表 + 大载荷
//   每个节点挂一个数组，组成长链 → 模拟碎片化内存布局
//   测试：指针密集的线性对象链 + 载荷块回收
// ============================================================
func testLinkedListPayload(chainLen: Int64, payloadSize: Int64, iterations: Int64) {
    println("[Test 4] Linked List + Payload  length=${chainLen}  payload=${payloadSize}  iterations=${iterations}")

    for (i in 0..iterations) {
        let chain = buildChain(chainLen, payloadSize)
        let sum = sumChain(Some(chain))
        if (i % 5 == 0) {
            println("  round ${i}: sum=${sum}")
        }
    }
    println("  DONE\n")
}

// ============================================================
// Test 5: ArrayList 动态扩容
//   反复把 ArrayList 从 0 增长到十万级，然后丢弃
//   测试：底层数组反复扩容拷贝、旧数组回收
// ============================================================
func testArrayListStress(maxSize: Int64, iterations: Int64) {
    println("[Test 5] ArrayList Stress  maxSize=${maxSize}  iterations=${iterations}")

    for (round in 0..iterations) {
        let list = ArrayList<Int64>()
        for (i in 0..maxSize) {
            list.add(i * i)
        }
        var sum: Int64 = 0
        for (v in list) {
            sum += v
        }
        if (round % 10 == 0) {
            println("  round ${round}: size=${list.size}  sum=${sum}")
        }
    }
    println("  DONE\n")
}

// ============================================================
// Test 6: HashMap 翻搅
//   万级 KV 对，value 是 ArrayList → 复杂嵌套对象图
//   测试：哈希桶 + 动态数组的复合结构频繁创建/回收
// ============================================================
func testHashMapChurn(mapSize: Int64, iterations: Int64) {
    println("[Test 6] HashMap Churn  size=${mapSize}  iterations=${iterations}")

    for (round in 0..iterations) {
        let map = HashMap<Int64, ArrayList<Int64>>()
        for (i in 0..mapSize) {
            let vals = ArrayList<Int64>()
            vals.add(i)
            vals.add(i * 2)
            vals.add(i * 3)
            map[i] = vals
        }
        var total: Int64 = 0
        for ((k, v) in map) {
            for (item in v) {
                total += item
            }
        }
        if (round % 5 == 0) {
            println("  round ${round}: entries=${map.size}  total=${total}")
        }
    }
    println("  DONE\n")
}

// ============================================================
// Test 7: "瑞士奶酪"碎片化测试
//   分配 N 个大对象（128KB），保留偶数下标引用、释放奇数下标
//   → 在 MAIN 中形成等间距空洞
//   穿插 Mutex pinned 对象，用于测试 pinned region 加剧碎片化
//   然后尝试分配更大的对象（256KB），看是否成功
//   无 LOS: 大对象在 MAIN 中留下空洞 + pinned 不可移动
//   有 LOS: 大对象在 LOS 中隔离，MAIN 空间干净，但 pinned 仍占位
// ============================================================
func testSwissCheese(numObjects: Int64, iterations: Int64) {
    println("[Test 7] Swiss Cheese Fragmentation  objects=${numObjects}  iterations=${iterations}")

    // 创建大量长期存活的 pinned 对象（锁），与大对象交替创建
    // 穿插普通对象分配，迫使 pinned region 与普通 region 交错分布
    let swissLocks = ArrayList<Mutex>()
    for (i in 0..numObjects * 5) {
        swissLocks.add(Mutex())  // pinned, 不会被 GC 移动
        // 每创建几个锁，穿插普通小对象分配，迫使 region 交错
        if (i % 10 == 0) {
            let filler = Array<Int64>(4096, repeat: i)  // 32KB filler
            let _ = filler[0]
        }
    }
    println("  created ${swissLocks.size} pinned locks scattered in heap")

    for (round in 0..iterations) {
        // 第一步：分配 numObjects 个 128KB 大对象（16384 个 Int64）
        let holders = ArrayList<Option<LargeObject>>()
        for (i in 0..numObjects) {
            holders.add(Some(LargeObject(16384, i + 1)))  // 128KB each
        }

        // 第二步：释放奇数下标的对象 → 造出等间距空洞
        for (i in 0..numObjects) {
            if (i % 2 == 1) {
                holders[i] = None
            }
        }

        // 第三步：在空洞间隙中尝试分配更大的对象（256KB = 32768 个 Int64）
        var successCount: Int64 = 0
        let bigOnes = ArrayList<LargeObject>()
        for (i in 0..numObjects / 4) {
            let big = LargeObject(32768, round * 1000 + i)  // 256KB each
            bigOnes.add(big)
            successCount++
        }

        // 同时穿插小对象分配，检测 MAIN 是否因碎片而效率降低
        let smallOnes = ArrayList<TreeNode>()
        for (i in 0..1000) {
            smallOnes.add(buildTree(6))  // 127 节点的小树
        }

        if (round % 5 == 0) {
            // 验证存活大对象的完整性
            var liveCount: Int64 = 0
            for (opt in holders) {
                match (opt) {
                    case Some(obj) =>
                        let _ = obj.checksum()
                        liveCount++
                    case None => ()
                }
            }
            println("  round ${round}: live_large=${liveCount}  bigAllocs=${successCount}  smallTrees=${smallOnes.size}  pinnedLocks=${swissLocks.size}")
        }
        // 每轮全部丢弃，让 GC 回收碎片化内存
    }
    println("  DONE\n")
}

// ============================================================
// Test 8: 大小对象交替风暴
//   交替分配：一个大对象（256KB）+ 100 个小对象（树）
//   在 MAIN 中会出现 "大-小-大-小" 的交替布局
//   无 LOS: 大小对象混排，回收大对象后留下不连续的大空洞
//   有 LOS: 大对象走 LOS，MAIN 中只有连续的小对象，零碎片
// ============================================================
func testInterleavedStorm(rounds: Int64) {
    println("[Test 8] Interleaved Large/Small Storm  rounds=${rounds}")

    // 长期存活的 pinned 锁对象（模拟应用中的全局锁）
    // 大量 Mutex 确保占满多个 pinned region
    let stormLocks = ArrayList<Mutex>()
    for (i in 0..100) {
        stormLocks.add(Mutex())  // 100 个 pinned 对象散布在堆中
        // 穿插普通对象，迫使 pinned region 分散
        if (i % 5 == 0) {
            let filler = Array<Int64>(2048, repeat: i)  // 16KB filler
            let _ = filler[0]
        }
    }

    // 长期存活的小对象容器（不释放，模拟 old-gen 占位）
    let persistentSmall = ArrayList<TreeNode>()
    for (i in 0..500) {
        persistentSmall.add(buildTree(8))  // 255 节点小树 × 500 = ~60K 对象
    }
    println("  persistent small objects: ${persistentSmall.size} trees")

    for (round in 0..rounds) {
        // 大对象：256KB（32768 个 Int64），分配后部分保留部分丢弃
        let largeTemp = ArrayList<LargeObject>()
        for (i in 0..10) {
            largeTemp.add(LargeObject(32768, round * 10 + i))  // 10 × 256KB = 2.5MB
        }

        // 穿插小对象
        let smallTemp = ArrayList<TreeNode>()
        for (i in 0..500) {
            smallTemp.add(buildTree(6))  // 500 × 127 节点小树
        }

        // 穿插临时 pinned 对象（每轮创建几个临时 Mutex，很快释放）
        for (k in 0..5) {
            let tempLock = Mutex()  // 短命 pinned 对象
            tempLock.lock()
            tempLock.unlock()
            // tempLock 失去引用 → pinned region 空洞
        }

        // 再来一波大对象
        let largeBatch2 = ArrayList<LargeObject>()
        for (i in 0..5) {
            largeBatch2.add(LargeObject(65536, round * 100 + i))  // 5 × 512KB = 2.5MB
        }

        if (round % 10 == 0) {
            var bigSum: Int64 = 0
            for (obj in largeTemp) {
                bigSum += obj.checksum()
            }
            println("  round ${round}: large10×256KB + small500 + large5×512KB  bigChecksum=${bigSum}")
        }
        // 每轮末尾 largeTemp, smallTemp, largeBatch2 全部失去引用
        // 无 LOS 时碎片: [persistent_small]...[空洞256KB]...[persistent_small]...[空洞512KB]...
    }

    // 验证长期存活对象完整性
    var treeSum: Int64 = 0
    for (t in persistentSmall) {
        treeSum += treeChecksum(Some(t))
    }
    println("  persistent trees intact, checksum=${treeSum}")
    println("  DONE\n")
}

// ============================================================
// Test 9: 阶梯式大对象尺寸碎片化
//   按递增尺寸分配大对象：128KB, 192KB, 256KB, 384KB, 512KB, 768KB, 1MB
//   然后释放最大和最小的 → 剩余对象之间产生不同尺寸的空洞
//   再尝试分配特定尺寸对象，检测碎片化影响
//   无 LOS: 不同尺寸的空洞散布在 MAIN 中，新分配可能无法利用
//   有 LOS: LOS 中使用 FreeRegionManager 合并相邻空闲块
// ============================================================
func testGraduatedSizeLadder(iterations: Int64) {
    println("[Test 9] Graduated Size Ladder  iterations=${iterations}")

    // sizes: Int64 元素个数
    let sizes: Array<Int64> = [16384, 24576, 32768, 49152, 65536, 98304, 131072]
    //                         128KB  192KB  256KB  384KB  512KB  768KB  1MB
    let labels: Array<String> = ["128KB", "192KB", "256KB", "384KB", "512KB", "768KB", "1MB"]

    for (round in 0..iterations) {
        // 分配阶梯（全部保持引用）
        let ladder = ArrayList<Option<LargeObject>>()
        for (s in 0..7) {
            ladder.add(Some(LargeObject(sizes[s], round * 7 + s + 1)))
        }

        // 释放最小(128KB)和最大(1MB)及中间(384KB) → 留下大小各异的空洞
        ladder[0] = None  // 释放 128KB
        ladder[3] = None  // 释放 384KB
        ladder[6] = None  // 释放 1MB

        // 在空洞中尝试分配各种尺寸的新对象
        // 尝试分配 192KB → 应利用 384KB 空洞（有碎片利用问题）
        let fill1 = LargeObject(24576, round * 1000 + 1)   // 192KB
        // 尝试分配 512KB → 应利用 1MB 空洞
        let fill2 = LargeObject(65536, round * 1000 + 2)   // 512KB
        // 再来一个 128KB
        let fill3 = LargeObject(16384, round * 1000 + 3)   // 128KB

        if (round % 5 == 0) {
            var liveCount: Int64 = 0
            var totalElements: Int64 = 0
            for (opt in ladder) {
                match (opt) {
                    case Some(obj) =>
                        liveCount++
                        totalElements += obj.data.size
                    case None => ()
                }
            }
            let fillCheck = fill1.checksum() + fill2.checksum() + fill3.checksum()
            println("  round ${round}: ladder_live=${liveCount}  elements=${totalElements}  fillCheck=${fillCheck}")
        }
    }
    println("  DONE\n")
}

// ============================================================
// Test 10: 大对象长期驻留 + 小对象高频翻搅
//   分配 30 个长期存活的大对象（512KB），永不释放
//   同时高频创建/销毁小对象（树 + 链表）
//   → 考验 GC 在大对象"钉"在 MAIN 中时的小对象回收效率
//   无 LOS: 大对象钉在 MAIN 中，小对象只能在空隙中分配
//   有 LOS: 大对象在 LOS 中，MAIN 完全给小对象用
// ============================================================
func testPinnedLargeWithSmallChurn(numPinned: Int64, smallIterations: Int64) {
    println("[Test 10] Pinned Large + Small Churn  pinned=${numPinned}  iterations=${smallIterations}")

    // 长期驻留的大对象（模拟缓存、大数据结构等）
    let pinnedLarge = ArrayList<LargeObject>()
    var pinnedMB: Int64 = 0
    for (i in 0..numPinned) {
        let obj = LargeObject(65536, i + 1)  // 每个 512KB
        pinnedLarge.add(obj)
        pinnedMB += 1  // 实际 0.5MB，约计
    }
    println("  pinned ${numPinned} large objects (~${numPinned / 2}MB)")

    // 高频翻搅小对象
    for (round in 0..smallIterations) {
        // 混合小对象分配：树、链表、数组
        let trees = ArrayList<TreeNode>()
        for (i in 0..200) {
            trees.add(buildTree(8))  // 255 节点
        }

        let chain = buildChain(500, 512)  // 500 节点 × 4KB 载荷 = ~2MB
        let chainSum = sumChain(Some(chain))

        let arr = ArrayList<Int64>()
        for (i in 0..100000) {
            arr.add(i * i)
        }

        if (round % 20 == 0) {
            // 验证 pinned 大对象完整性
            var pinnedCheck: Int64 = 0
            for (obj in pinnedLarge) {
                pinnedCheck += obj.data[0]
            }
            println("  round ${round}: small_trees=${trees.size}  chain_sum=${chainSum}  arr_size=${arr.size}  pinned_ok=${pinnedCheck}")
        }
        // trees, chain, arr 每轮结束后全部可回收
        // 但 pinnedLarge 始终存活 → 无 LOS 时这些大块钉在 MAIN 中间
    }

    // 最终验证
    var finalCheck: Int64 = 0
    for (obj in pinnedLarge) {
        finalCheck += obj.checksum()
    }
    println("  final pinned checksum: ${finalCheck}")
    println("  DONE\n")
}

// ============================================================
// Test 11: Pinned Region 碎片化测试
//   创建大量 Mutex 对象（pinned region），散布在堆中
//   pinned region 不会被 GC 移动，类似真实应用中的锁对象
//   测试 pinned region 对 ShrinkInactiveZone 和碎片整理的影响
//
//   场景 A: 长期存活的全局锁 — 永不释放，钉在堆中
//   场景 B: 短命临时锁 — 用完释放，留下 pinned 空洞
//   场景 C: 锁 + 大对象 + 小对象交替 — 模拟真实混合负载
//
//   无 LOS: pinned region 钉在 MAIN 中间，大对象也在 MAIN
//           → 双重碎片（pinned + large 都不可移动）
//   有 LOS: 大对象走 LOS，但 pinned region 仍钉在 MAIN
//           → pinned 对 MAIN 的碎片化效果可量化
// ============================================================
func testPinnedRegionFragmentation(numGlobalLocks: Int64, numTempLocks: Int64, iterations: Int64) {
    println("[Test 11] Pinned Region Fragmentation  globalLocks=${numGlobalLocks}  tempLocks=${numTempLocks}  iterations=${iterations}")

    // ---- 场景 A: 创建长期存活的全局锁，散布在堆中 ----
    // 每个 PinnedHolder 包含: 1 个 Mutex (pinned) + 1 个 payload 数组 (movable)
    // payload 大小递增，使 pinned region 散布在不同位置
    // 穿插大量普通对象分配，迫使 pinned region 与普通 region 交错
    let globalLocks = ArrayList<PinnedHolder>()
    let fillerArrays = ArrayList<Array<Int64>>()  // 保持引用，防止被回收
    for (i in 0..numGlobalLocks) {
        // payload 大小在 128B ~ 1KB 之间变化（减小以留更多空间给 Mutex）
        let payloadSize = 16 + (i % 8) * 16  // 128B ~ 1KB (Int64 count)
        let holder = PinnedHolder(i + 1, payloadSize)
        globalLocks.add(holder)

        // 每创建 50 个锁，穿插 filler 数组分配
        // → filler 使 pinned region 与 normal region 在地址空间中交错
        if (i % 50 == 0) {
            let filler = Array<Int64>(2048, repeat: i)  // 16KB filler (保持存活)
            fillerArrays.add(filler)
        }
        // 穿插树对象（短命，增加 GC 压力），让 GC 更频繁触发
        if (i % 100 == 0) {
            let tempTree = buildTree(10)  // 1023 节点小树（会被回收）
            let _ = treeChecksum(Some(tempTree))
        }
    }
    println("  created ${numGlobalLocks} global locks + ${fillerArrays.size} fillers")

    // ---- 主循环：混合负载 ----
    for (round in 0..iterations) {

        // 场景 B: 创建短命临时锁 → 用完释放
        // 模拟 synchronized(tempMutex) { ... } 场景
        let tempLocks = ArrayList<PinnedHolder>()
        for (j in 0..numTempLocks) {
            let temp = PinnedHolder(round * 1000 + j, 64)  // 小 payload 512B
            let _ = temp.doWork()  // 上锁→计算→解锁
            tempLocks.add(temp)
        }
        // tempLocks 在每轮结束后失去引用
        // 但 pinned region 中的空间需要 pinned GC 回收
        // → 产生 pinned region 空洞

        // 场景 C: 穿插大对象分配
        let largeObjs = ArrayList<LargeObject>()
        for (k in 0..3) {
            largeObjs.add(LargeObject(32768, round * 10 + k))  // 3 × 256KB
        }

        // 穿插小对象翻搅
        let smallTrees = ArrayList<TreeNode>()
        for (k in 0..100) {
            smallTrees.add(buildTree(7))  // 127 节点小树
        }

        // 使用全局锁做一些工作
        var lockSum: Int64 = 0
        for (holder in globalLocks) {
            lockSum += holder.doWork()
        }

        if (round % 10 == 0) {
            println("  round ${round}: tempLocks=${tempLocks.size}  large=${largeObjs.size}  small=${smallTrees.size}  globalLockSum=${lockSum}")
        }

        // 每轮结束: tempLocks, largeObjs, smallTrees 全部可回收
        // 但 globalLocks 始终存活 → pinned region 钉在 MAIN 中
    }

    // 最终验证全局锁完整性
    var finalSum: Int64 = 0
    for (holder in globalLocks) {
        finalSum += holder.doWork()
    }
    println("  final global locks intact, sum=${finalSum}")
    println("  DONE\n")
}

// ============================================================
// Test 12: 混合 OOM 测试 — 小对象占 60%
//   按内存比例持续累积三类对象直到 OOM：
//     小对象 ~60%: 48 × 8KB = 384KB/轮  (Array<Int64>(1024))
//     Pinned ~2.5%: 16 × Mutex+1KB = 16KB/轮 (Mutex pinned + payload movable)
//     大对象 ~37.5%: 1 × 256KB/轮  (Array<Int64>(32768))
//   总计每轮: 384 + 16 + 256 = 656KB
// ============================================================
func testMixedOOM() {
    println("[Test 12] Mixed OOM — two-phase fragmentation + accumulation")
    println("  WARNING: This test is designed to exhaust heap memory!")
    println("  Phase 1 (30 rounds): allocate + release ~50% → fragment the heap")
    println("  Phase 2: accumulate all objects until OOM")
    println("  Each round: 48 small(8KB) + 16 pinned(Mutex+1KB) + 1 large(256KB)")
    println("  Memory per round: 384KB small + 16KB pinned + 256KB large = 656KB\n")

    // --- Retained object lists (survive across rounds) ---
    let smallObjs = ArrayList<Array<Int64>>()
    let pinnedObjs = ArrayList<PinnedHolder>()
    let largeObjs = ArrayList<Array<Int64>>()

    var round: Int64 = 0
    var smallKB: Int64 = 0
    var pinnedCount: Int64 = 0
    var largeKB: Int64 = 0

    // =========================================================
    // Phase 1: Fragmentation — 30 rounds of alloc + partial release
    //   Each round: allocate a batch, then release ~50% of ALL
    //   accumulated small/large objects (keep odd-indexed, drop even).
    //   This creates holes in the heap that GC must deal with.
    // =========================================================
    let fragRounds: Int64 = 30
    println("  === Phase 1: Fragmentation (${fragRounds} rounds) ===")

    for (_ in 0..fragRounds) {
        let r = round

        // Allocate small objects
        for (s in 0..48) {
            let arr = Array<Int64>(1024, { i => i + r * 48 + s })
            smallObjs.add(arr)
            smallKB += 8
        }

        // Allocate pinned objects
        for (p in 0..16) {
            let holder = PinnedHolder(r * 16 + p, 128)
            let _ = holder.doWork()
            pinnedObjs.add(holder)
            pinnedCount += 1
        }

        // Allocate large object
        let largeR = r + 1
        let largeArr = Array<Int64>(32768, { i => i * largeR })
        largeObjs.add(largeArr)
        largeKB += 256

        round++

        // After every round in phase 1, release ~50% of small and large objects
        // Keep odd-indexed, release even-indexed → creates scattered holes
        if (round % 2 == 0) {
            // Release ~50% small objects (every other one from the back half)
            let smallHalf = smallObjs.size / 2
            var si = smallHalf - 1
            while (si >= 0) {
                if (si % 2 == 0) {
                    smallObjs.remove(si..(si + 1))
                    smallKB -= 8
                }
                si -= 1
            }

            // Release ~50% large objects (every other one)
            let largeHalf = largeObjs.size / 2
            var li = largeHalf - 1
            while (li >= 0) {
                if (li % 2 == 0) {
                    largeObjs.remove(li..(li + 1))
                    largeKB -= 256
                }
                li -= 1
            }
        }

        // Also churn some temporary trees to trigger GC
        for (_ in 0..5) {
            let tempTree = buildTree(12)
            let _ = treeChecksum(Some(tempTree))
        }

        if (round % 10 == 0) {
            let totalKB = smallKB + pinnedCount + largeKB
            let smallPct = if (totalKB > 0) { smallKB * 100 / totalKB } else { 0 }
            println("  [P1] round ${round}: small=${smallKB}KB(${smallObjs.size},${smallPct}%)  pinned=${pinnedCount}  large=${largeKB}KB(${largeObjs.size})  total~${totalKB/1024}MB")
        }
    }

    let fragSmallKB = smallKB
    let fragLargeKB = largeKB
    println("  === Phase 1 done: retained small=${fragSmallKB}KB(${smallObjs.size})  pinned=${pinnedCount}  large=${fragLargeKB}KB(${largeObjs.size}) ===\n")

    // =========================================================
    // Phase 2: Accumulation — keep everything, grow until OOM
    //   The heap is already fragmented from phase 1.
    //   Now we just keep piling on objects until OOM.
    // =========================================================
    println("  === Phase 2: Accumulation until OOM ===")

    while (true) {
        let r = round

        // Allocate small objects
        for (s in 0..48) {
            let arr = Array<Int64>(1024, { i => i + r * 48 + s })
            smallObjs.add(arr)
            smallKB += 8
        }

        // Allocate pinned objects
        for (p in 0..16) {
            let holder = PinnedHolder(r * 16 + p, 128)
            let _ = holder.doWork()
            pinnedObjs.add(holder)
            pinnedCount += 1
        }

        // Allocate large object
        let largeR = r + 1
        let largeArr = Array<Int64>(32768, { i => i * largeR })
        largeObjs.add(largeArr)
        largeKB += 256

        round++

        if (round % 20 == 0) {
            // Light churn to occasionally trigger GC
            for (_ in 0..10) {
                let tempTree = buildTree(10)
                let _ = treeChecksum(Some(tempTree))
            }
            let totalKB = smallKB + pinnedCount + largeKB
            let smallPct = if (totalKB > 0) { smallKB * 100 / totalKB } else { 0 }
            println("  [P2] round ${round}: small=${smallKB}KB(${smallObjs.size},${smallPct}%)  pinned=${pinnedCount}  large=${largeKB}KB(${largeObjs.size})  total~${totalKB/1024}MB")
        }
    }
}

// ============================================================
//   MAIN
// ============================================================
main() {
    println("========================================")
    println("   Cangjie GC Stress Test Suite")
    println("========================================")
    println("  Tip: export MRT_REPORT=./gc.log   to enable GC logging")
    println("  Tip: export cjHeapSize=256mb       to limit heap size")
    println("========================================\n")

    // Test 1: 二叉树翻搅 — depth 18 → 每棵 262143 节点，建 200 棵
    testBinaryTreeChurn(18, 200)

    // Test 2: 分代测试 — 长期树 depth 18（262K 节点），短命树 depth 14 × 500
    testGenerational(18, 14, 500)

    // Test 3: 大对象 — 64KB~1MB 各 100 轮
    testLargeObjects(100)

    // Test 4: 链表载荷 — 1000 节点链 × 2048 元素载荷 × 100 轮
    testLinkedListPayload(1000, 2048, 100)

    // Test 5: ArrayList — 50 万级 × 200 轮
    testArrayListStress(500000, 200)

    // Test 6: HashMap — 5 万 KV × 100 轮
    testHashMapChurn(50000, 100)

    println("========================================")
    println("   Phase 2: Fragmentation-focused tests")
    println("   (LOS vs no-LOS comparison)")
    println("========================================\n")

    // Test 7: 瑞士奶酪碎片化 — 40 个大对象(128KB)，释放奇数，回填更大对象，20 轮
    testSwissCheese(40, 20)

    // Test 8: 大小交替风暴 — 大(256KB/512KB) + 小(树) 反复穿插, 50 轮
    testInterleavedStorm(50)

    // Test 9: 阶梯尺寸碎片化 — 128KB~1MB 递增分配再部分释放, 20 轮
    testGraduatedSizeLadder(20)

    // Test 10: 大对象钉死 + 小对象翻搅 — 30 个 512KB 常驻 + 小对象高频 GC, 100 轮
    testPinnedLargeWithSmallChurn(30, 100)

    println("========================================")
    println("   Phase 3: Pinned region tests")
    println("   (Mutex/lock objects = pinned, not moved by GC)")
    println("========================================\n")

    // Test 11: Pinned Region 碎片化 — 8000 个全局锁 + 5000 个临时锁 × 20 轮
    //   每个 Mutex ~96 字节, 一个 pinned region (64KB) 装 ~680 个
    //   8000 个全局锁 → ~12 个 pinned region 常驻
    //   + 每轮 5000 个临时 Mutex → 峰值 ~20 个 pinned region
    //   穿插大对象和小对象，测试 pinned region 对碎片整理的影响
    testPinnedRegionFragmentation(8000, 5000, 20)

    println("========================================")
    println("   Phase 4: Mixed OOM (frag + accumulate)")
    println("   30 rounds frag → then accumulate to OOM")
    println("========================================\n")

    // Test 12: 混合 OOM — Phase 1: 30 轮碎片化(分配+释放50%) → Phase 2: 累积到 OOM
    testMixedOOM()
}