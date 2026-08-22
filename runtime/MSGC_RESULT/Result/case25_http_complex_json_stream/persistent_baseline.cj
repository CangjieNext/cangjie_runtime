import std.collection.*
import std.sync.*

let PERSISTENT_SMALL_OBJECT_SIZES: Array<Int64> =
    [128, 256, 512, 1024, 2048, 4096, 8192, 16384]
let PERSISTENT_LARGE_OBJECT_SIZES: Array<Int64> = [65536, 262144, 1048576]

class PersistentRootShard {
    let objects: ArrayList<Array<Byte>> = ArrayList<Array<Byte>>()
}

// Static shards are GC roots. Stack roots remain reachable through each
// persistent worker's long-lived coroutine stack.
let PERSISTENT_STATIC_ROOT_SHARDS = ArrayList<PersistentRootShard>()
let PERSISTENT_TOUCH_SINK = AtomicInt64(0)

public func persistentBaselineEnabled(persistentBytes: Int64): Bool {
    persistentBytes > 0
}

public func validatePersistentBaseline(persistentBytes: Int64,
                                       persistentWorkers: Int64): Unit {
    if (persistentBytes < 0) {
        throw IllegalArgumentException("CASE25_PERSISTENT_BYTES must be >= 0, got ${persistentBytes}")
    }
    if (!persistentBaselineEnabled(persistentBytes)) {
        return
    }
    if (persistentWorkers < 2) {
        throw IllegalArgumentException("CASE25_PERSISTENT_WORKERS must be >= 2, got ${persistentWorkers}")
    }
    let alignment = persistentWorkers * 2 * PERSISTENT_SMALL_OBJECT_SIZES[0]
    if (persistentBytes % alignment != 0) {
        throw IllegalArgumentException(
            "CASE25_PERSISTENT_BYTES must be a multiple of ${alignment} for ${persistentWorkers} workers"
        )
    }
}

func splitPersistentBytes(total: Int64, index: Int64, count: Int64): Int64 {
    let base = total / count
    if (index < total % count) {
        return base + 1
    }
    return base
}

func persistentObjectCount(targetBytes: Int64, sizes: Array<Int64>): Int64 {
    var count: Int64 = 0
    var allocated: Int64 = 0
    let perClass = targetBytes / sizes.size
    for (index in 0..sizes.size) {
        let size = sizes[index]
        let objects = perClass / size
        count = count + objects
        allocated = allocated + objects * size
    }
    count = count + (targetBytes - allocated) / sizes[0]
    return count
}

func persistentLargeBytes(targetBytes: Int64): Int64 {
    let alignment = PERSISTENT_LARGE_OBJECT_SIZES[0]
    targetBytes / 100 / alignment * alignment
}

func persistentRootCapacity(targetBytes: Int64): Int64 {
    let largeBytes = persistentLargeBytes(targetBytes)
    let smallBytes = targetBytes - largeBytes
    persistentObjectCount(smallBytes, PERSISTENT_SMALL_OBJECT_SIZES) +
        persistentObjectCount(largeBytes, PERSISTENT_LARGE_OBJECT_SIZES)
}

func appendPersistentTier(roots: ArrayList<Array<Byte>>, targetBytes: Int64,
                          sizes: Array<Int64>, seed: Byte): Int64 {
    var allocated: Int64 = 0
    let perClass = targetBytes / sizes.size
    for (index in 0..sizes.size) {
        let size = sizes[index]
        let fullObjects = perClass / size
        for (_ in 0..fullObjects) {
            roots.add(Array<Byte>(size, repeat: seed))
            allocated = allocated + size
        }
    }
    let smallestSize = sizes[0]
    let remainingObjects = (targetBytes - allocated) / smallestSize
    for (_ in 0..remainingObjects) {
        roots.add(Array<Byte>(smallestSize, repeat: seed))
        allocated = allocated + smallestSize
    }
    allocated
}

func allocatePersistentRoots(roots: ArrayList<Array<Byte>>, targetBytes: Int64,
                             seed: Byte): Int64 {
    let largeBytes = persistentLargeBytes(targetBytes)
    let smallBytes = targetBytes - largeBytes
    var allocated = appendPersistentTier(roots, smallBytes, PERSISTENT_SMALL_OBJECT_SIZES, seed)
    allocated = allocated + appendPersistentTier(roots, largeBytes, PERSISTENT_LARGE_OBJECT_SIZES, seed)
    allocated
}

func touchPersistentRoots(roots: ArrayList<Array<Byte>>): Int64 {
    var checksum: Int64 = 0
    for (object in roots) {
        if (object.size > 0) {
            checksum = checksum + Int64(object[0]) + Int64(object[object.size - 1]) + object.size
        }
    }
    checksum
}

func persistentAllocationWorker(workerId: Int64, staticTarget: Int64,
                                stackTarget: Int64, staticShard: PersistentRootShard,
                                persistentCompleted: AtomicInt64, persistentFailed: AtomicBool,
                                staticAllocated: AtomicInt64, stackAllocated: AtomicInt64): Unit {
    let staticRoots = staticShard.objects
    // This list is intentionally only rooted by this long-lived coroutine.
    let stackRoots = ArrayList<Array<Byte>>()
    var successful = false
    try {
        staticRoots.reserve(persistentRootCapacity(staticTarget))
        stackRoots.reserve(persistentRootCapacity(stackTarget))
        let staticBytes = allocatePersistentRoots(staticRoots, staticTarget, 1)
        let stackBytes = allocatePersistentRoots(stackRoots, stackTarget, 1)
        staticAllocated.fetchAdd(staticBytes)
        stackAllocated.fetchAdd(stackBytes)
        successful = true
        println("[INFO] Persistent worker ${workerId} ready: static=${staticBytes} stack=${stackBytes}")
    } catch (e: Error) {
        persistentFailed.store(true)
        println("[ERROR] Persistent worker ${workerId} failed: ${e.message}")
    } catch (e: Exception) {
        persistentFailed.store(true)
        println("[ERROR] Persistent worker ${workerId} failed: ${e.message}")
    } finally {
        persistentCompleted.fetchAdd(1)
    }
    if (!successful) {
        return
    }

    while (true) {
        PERSISTENT_TOUCH_SINK.fetchAdd(
            touchPersistentRoots(staticRoots) + touchPersistentRoots(stackRoots)
        )
        sleep(Duration.second)
    }
}

public func initializePersistentBaseline(persistentBytes: Int64,
                                         persistentWorkers: Int64): Unit {
    if (!persistentBaselineEnabled(persistentBytes)) {
        println("[INFO] Persistent heap baseline disabled")
        return
    }
    validatePersistentBaseline(persistentBytes, persistentWorkers)
    println("[INFO] Creating persistent heap baseline: bytes=${persistentBytes} workers=${persistentWorkers}...")

    let staticTarget = persistentBytes / 2
    let stackTarget = persistentBytes - staticTarget
    let persistentCompleted = AtomicInt64(0)
    let persistentFailed = AtomicBool(false)
    let staticAllocated = AtomicInt64(0)
    let stackAllocated = AtomicInt64(0)
    let persistentFutures = ArrayList<Future<Unit>>()
    for (workerId in 0..persistentWorkers) {
        let staticShard = PersistentRootShard()
        PERSISTENT_STATIC_ROOT_SHARDS.add(staticShard)
        persistentFutures.add(spawn {
            persistentAllocationWorker(
                workerId,
                splitPersistentBytes(staticTarget, workerId, persistentWorkers),
                splitPersistentBytes(stackTarget, workerId, persistentWorkers),
                staticShard,
                persistentCompleted,
                persistentFailed,
                staticAllocated,
                stackAllocated
            )
        })
    }
    while (persistentCompleted.load() < persistentWorkers) {
        sleep(Duration.millisecond * 100)
    }
    if (persistentFailed.load()) {
        throw IllegalStateException("Persistent heap baseline creation failed")
    }
    println("[INFO] Persistent heap baseline ready: total=${staticAllocated.load() + stackAllocated.load()} static=${staticAllocated.load()} stack=${stackAllocated.load()}")
    let _ = persistentFutures
}
