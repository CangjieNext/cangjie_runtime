import std.collection.*
import std.convert.*
import std.env.*
import std.fs.*
import std.io.*
import std.process.*
import std.sync.*
import std.time.*
import stdx.net.http.*
import stdx.encoding.json.*

// ============================================================
//   Case27: HTTP Complex-JSON Concurrent Benchmark
//
//   场景：内嵌 HTTP Server + 多并发 Client，Client 从外部 JSON 文件
//   读取 payload 并 POST 给 Server；Server 端反序列化为 JsonValue 树，
//   对若干字段做 read+modify+write（计数器+1、score 翻倍、events[].seq+=1、
//   user.attrs.credits *= 2、counters.in/ok 累加，并向根对象 append
//   "processed_at" 字段），再序列化回响应给 Client。Client 反序列化响应
//   并校验 server 添加的标记字段，确保两端 serde 路径都被压测。
//
//   单次运行只测试一组 (concurrency × payload)。
//
//   环境变量：
//     CASE26_PAYLOAD       : 1K | 10K | 100K | 1M  (default 10K)
//     CASE26_CONCURRENCY   : Int>=1  (default 8)
//     CASE26_DURATION_MS   : Int; >0 means finite run, <=0 means no time limit
//                            (default 60000)
//     CASE26_PAYLOAD_DIR   : 目录，包含 <SIZE>.json (default ./payloads)
//     CASE26_SERVER_PORT   : Int  (default 18125)
//     CASE26_CLIENT_TIMEOUT_MS : HTTP client read/write timeout (default 5000)
//     CASE26_ROLE          : both | server (default both)
//     CASE26_CLIENT_MODE   : sync | async (default sync)
//     CASE26_SEND_INTERVAL_MS : async sender tick interval (default 1)
//     CASE26_MAX_INFLIGHT  : async global inflight cap (default 32)
//     CASE26_PERSISTENT_BYTES : server persistent heap bytes (default 5 GiB)
//     CASE26_PERSISTENT_WORKERS : persistent allocation coroutines (default 8)
// ============================================================

let SERVER_ADDR: String = "127.0.0.1"
let DEFAULT_PORT: Int64 = 18125

// ---------- 工具函数 ----------

func elapsedMs(start: DateTime): Int64 {
    let d = DateTime.now() - start
    d.toMilliseconds()
}

func getEnvStr(key: String, def: String): String {
    match (getVariable(key)) {
        case Some(v) => v
        case None => def
    }
}

func getEnvInt(key: String, def: Int64): Int64 {
    match (getVariable(key)) {
        case Some(raw) => match (Int64.tryParse(raw)) {
            case Some(v) => v
            case None => def
        }
        case None => def
    }
}

let CLIENT_TIMEOUT_MS: Int64 = getEnvInt("CASE26_CLIENT_TIMEOUT_MS", 5_000)

// ---------- Server persistent heap baseline ----------

let PERSISTENT_SMALL_OBJECT_SIZES: Array<Int64> =
    [128, 256, 512, 1024, 2048, 4096, 8192, 16384]
let PERSISTENT_LARGE_OBJECT_SIZES: Array<Int64> = [65536, 262144, 1048576]

class PersistentRootShard {
    let objects: ArrayList<Array<Byte>> = ArrayList<Array<Byte>>()
}

// Top-level storage is a static GC root. Each worker owns one shard, so filling
// the shards concurrently does not mutate the same ArrayList.
let PERSISTENT_STATIC_ROOT_SHARDS = ArrayList<PersistentRootShard>()
let PERSISTENT_TOUCH_SINK = AtomicInt64(0)

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
    return targetBytes / 100 / alignment * alignment
}

func persistentRootCapacity(targetBytes: Int64): Int64 {
    let largeBytes = persistentLargeBytes(targetBytes)
    let smallBytes = targetBytes - largeBytes
    return persistentObjectCount(smallBytes, PERSISTENT_SMALL_OBJECT_SIZES) +
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
    return allocated
}

func allocatePersistentRoots(roots: ArrayList<Array<Byte>>, targetBytes: Int64,
                             seed: Byte): Int64 {
    let largeBytes = persistentLargeBytes(targetBytes)
    let smallBytes = targetBytes - largeBytes
    var allocated = appendPersistentTier(
        roots, smallBytes, PERSISTENT_SMALL_OBJECT_SIZES, seed)
    allocated = allocated + appendPersistentTier(
        roots, largeBytes, PERSISTENT_LARGE_OBJECT_SIZES, seed)
    return allocated
}

func touchPersistentRoots(roots: ArrayList<Array<Byte>>): Int64 {
    var checksum: Int64 = 0
    for (object in roots) {
        if (object.size > 0) {
            checksum = checksum + Int64(object[0]) +
                Int64(object[object.size - 1]) + object.size
        }
    }
    return checksum
}

func persistentAllocationWorker(workerId: Int64, staticTarget: Int64,
                                stackTarget: Int64, staticShard: PersistentRootShard,
                                persistentCompleted: AtomicInt64, persistentFailed: AtomicBool,
                                staticAllocated: AtomicInt64, stackAllocated: AtomicInt64): Unit {
    let seed: Byte = 1
    let staticRoots = staticShard.objects
    // This list is deliberately never published. The long-lived coroutine
    // stack is its only root for the full server lifetime.
    let stackRoots = ArrayList<Array<Byte>>()
    var successful = false
    try {
        staticRoots.reserve(persistentRootCapacity(staticTarget))
        stackRoots.reserve(persistentRootCapacity(stackTarget))
        let staticBytes = allocatePersistentRoots(staticRoots, staticTarget, seed)
        let stackBytes = allocatePersistentRoots(stackRoots, stackTarget, seed)
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
        let checksum = touchPersistentRoots(staticRoots) + touchPersistentRoots(stackRoots)
        PERSISTENT_TOUCH_SINK.fetchAdd(checksum)
        sleep(Duration.second)
    }
}

func isEnvEnabled(key: String): Bool {
    match (getVariable(key)) {
        case Some(raw) => raw != "" && raw != "0"
        case None => false
    }
}

// ---------- JSON 分阶段诊断（默认关闭） ----------

let JSON_PROFILE_ENABLED: Bool = isEnvEnabled("CASE26_JSON_PROFILE")

class JsonProfileStats {
    let serverParseUs = AtomicInt64(0)
    let serverMutateUs = AtomicInt64(0)
    let serverSerializeUs = AtomicInt64(0)
    let clientParseUs = AtomicInt64(0)
    let serverCalls = AtomicInt64(0)
    let clientCalls = AtomicInt64(0)
}

let JSON_PROFILE_STATS = JsonProfileStats()

func elapsedUs(start: DateTime): Int64 {
    (DateTime.now() - start).toMicroseconds()
}

func parseServerJson(bodyStr: String): JsonValue {
    if (!JSON_PROFILE_ENABLED) {
        return JsonValue.fromStr(bodyStr)
    }
    let start = DateTime.now()
    let value = JsonValue.fromStr(bodyStr)
    JSON_PROFILE_STATS.serverParseUs.fetchAdd(elapsedUs(start))
    JSON_PROFILE_STATS.serverCalls.fetchAdd(1)
    return value
}

func parseClientJson(bodyStr: String): JsonValue {
    if (!JSON_PROFILE_ENABLED) {
        return JsonValue.fromStr(bodyStr)
    }
    let start = DateTime.now()
    let value = JsonValue.fromStr(bodyStr)
    JSON_PROFILE_STATS.clientParseUs.fetchAdd(elapsedUs(start))
    JSON_PROFILE_STATS.clientCalls.fetchAdd(1)
    return value
}

func responseJsonCapacity(requestBytes: Int64): Int64 {
    // case26 只为原 JSON 增加少量 marker/product id。预留 12.5% 余量，
    // 避免 JsonValue.toString() 为估算容量而先完整遍历一次对象树。
    return requestBytes + requestBytes / 8 + 1024
}

func serializeServerJson(root: JsonObject, requestBytes: Int64): String {
    if (!JSON_PROFILE_ENABLED) {
        return root.toStringWithCapacity(responseJsonCapacity(requestBytes))
    }
    let start = DateTime.now()
    let value = root.toStringWithCapacity(responseJsonCapacity(requestBytes))
    JSON_PROFILE_STATS.serverSerializeUs.fetchAdd(elapsedUs(start))
    return value
}

// ---------- HTTP Body 读取 ----------

// case26 的请求/响应主体通常为 100K 以上。按协议提供的 bodySize 预留容量，
// 并减少分块次数，避免 ArrayList 在每次请求中反复扩容。
let BODY_READ_CHUNK_SIZE: Int64 = 64 * 1024

func reserveBodyCapacity(result: ArrayList<UInt8>, bodySize: Option<Int64>): Unit {
    match (bodySize) {
        case Some(size) =>
            if (size > 0) {
                result.reserve(size)
            }
        case None => ()
    }
}

func readHttpBodyString(body: InputStream, bodySize: Option<Int64>): String {
    let chunk = Array<UInt8>(BODY_READ_CHUNK_SIZE, repeat: 0)
    let result = ArrayList<UInt8>()
    reserveBodyCapacity(result, bodySize)
    var n = body.read(chunk)
    while (n > 0) {
        result.add(all: chunk[..n])
        n = body.read(chunk)
    }
    String.fromUtf8(result.toArray())
}

func verifyHttpBodyRead(actual: String, expected: String, caseName: String): Unit {
    if (actual != expected) {
        throw Exception("HTTP body read self-test failed: ${caseName}")
    }
}

func runHttpBodyReadSelfTest(): Unit {
    let expected = "{\"id\":7,\"name\":\"body-copy\",\"items\":[1,2,3]}"
    verifyHttpBodyRead(
        readHttpBodyString(ByteBuffer(expected.toArray()), Some(Int64(expected.size))),
        expected,
        "known-content-length"
    )
    let unknownBodySize: Option<Int64> = None
    verifyHttpBodyRead(
        readHttpBodyString(ByteBuffer(expected.toArray()), unknownBodySize),
        expected,
        "unknown-content-length"
    )
    let response = HttpResponseBuilder().body(expected).build()
    verifyHttpBodyRead(readRespStr(response), expected, "string-response-body")
    println("[PASS] HTTP body copy self-test")
}

func readBodyStr(req: HttpRequest): String {
    readHttpBodyString(req.body, req.bodySize)
}

func readRespStr(rsp: HttpResponse): String {
    readHttpBodyString(rsp.body, rsp.bodySize)
}

func isBodyDiagEnabled(): Bool {
    isEnvEnabled("CASE26_BODY_DIAG")
}

func isGraphDiagEnabled(): Bool {
    isEnvEnabled("CASE26_GRAPH_DIAG")
}

func isBadBodyDumpEnabled(): Bool {
    isEnvEnabled("CASE26_DUMP_BAD_BODY")
}

func graphDiagRate(): Int64 {
    let raw = getEnvInt("CASE26_GRAPH_DIAG_RATE", 512)
    if (raw <= 0) {
        return 512
    }
    raw
}

func checksumBytesForDiag(data: Array<Byte>): Int64 {
    var sum: Int64 = 0
    for (b in data) {
        sum = (sum + Int64(b)) % 1_000_000_007
    }
    return sum
}

func tailStringForDiag(data: Array<Byte>, maxLen: Int64): String {
    let size = data.size
    let tailLen = if (size > maxLen) {
        maxLen
    } else {
        size
    }
    if (tailLen <= 0) {
        return ""
    }
    let start = size - tailLen
    let tail = Array<Byte>(tailLen, repeat: 0)
    data.copyTo(tail, start, 0, tailLen)
    String.fromUtf8(tail)
}

func printServerBodyDiag(reqId: String, slotId: String, bodyStr: String, e: Exception): Unit {
    if (!isBodyDiagEnabled()) {
        return
    }
    let bytes = bodyStr.toArray()
    let checksum = checksumBytesForDiag(bytes)
    let tail = tailStringForDiag(bytes, 128)
    println("[CASE27-DIAG][server-body-fail] slotId=${slotId} reqId=${reqId} bodySize=${bytes.size} checksum=${checksum} tail=${tail} ex=${e.message}")
}

func shouldCaptureBodySnapshotForDiag(): Bool {
    isBodyDiagEnabled() || isBadBodyDumpEnabled()
}

func captureBodySnapshotForDiag(bodyStr: String): Option<Array<Byte>> {
    if (!shouldCaptureBodySnapshotForDiag()) {
        return None
    }
    Some(bodyStr.toArray())
}

func dumpBadBytesForDiag(stage: String, reqId: String, slotId: String, bytes: Array<Byte>): Unit {
    if (!isBadBodyDumpEnabled()) {
        return
    }
    try {
        let dumpDir = getEnvStr("CASE26_DUMP_DIR", ".")
        if (!exists(dumpDir)) {
            Directory.create(dumpDir, recursive: true)
        }
        let base = "${dumpDir}/case27_${stage}_req${reqId}_slot${slotId}"
        File.writeTo("${base}.json", bytes)
        let meta = "stage=${stage}\nreqId=${reqId}\nslotId=${slotId}\nbodySize=${bytes.size}\nchecksum=${checksumBytesForDiag(bytes)}\ntail=${tailStringForDiag(bytes, 256)}\n"
        File.writeTo("${base}.meta", meta.toArray())
        println("[CASE27-DIAG][dump-bad-body] stage=${stage} reqId=${reqId} slotId=${slotId} path=${base}.json")
    } catch (dumpEx: Exception) {
        println("[CASE27-DIAG][dump-bad-body-fail] stage=${stage} reqId=${reqId} slotId=${slotId} ex=${dumpEx.message}")
    }
}

func dumpBadBodyForDiag(stage: String, reqId: String, slotId: String, bodyStr: String): Unit {
    dumpBadBytesForDiag(stage, reqId, slotId, bodyStr.toArray())
}

func printBodyMutationDiag(stage: String, reqId: String, slotId: String,
                          snapshot: Option<Array<Byte>>, bodyStr: String): Unit {
    if (!isBodyDiagEnabled()) {
        return
    }
    match (snapshot) {
        case Some(oldBytes) =>
            let newBytes = bodyStr.toArray()
            let oldSize = oldBytes.size
            let newSize = newBytes.size
            let compareSize = min(oldSize, newSize)
            var firstDiff: Int64 = -1
            for (i in 0..compareSize) {
                if (oldBytes[i] != newBytes[i]) {
                    firstDiff = i
                    break
                }
            }
            if (firstDiff < 0 && oldSize == newSize) {
                return
            }
            println("[CASE27-DIAG][${stage}-mutation] slotId=${slotId} reqId=${reqId} oldSize=${oldSize} newSize=${newSize} firstDiff=${firstDiff} oldChecksum=${checksumBytesForDiag(oldBytes)} newChecksum=${checksumBytesForDiag(newBytes)} oldTail=${tailStringForDiag(oldBytes, 128)} newTail=${tailStringForDiag(newBytes, 128)}")
        case None => ()
    }
}

func printClientResponseDiag(workerId: Int64, slotId: Int64, reqId: Int64, bodyStr: String, e: Exception): Unit {
    if (!isBodyDiagEnabled()) {
        return
    }
    let bytes = bodyStr.toArray()
    let checksum = checksumBytesForDiag(bytes)
    let tail = tailStringForDiag(bytes, 128)
    println("[CASE27-DIAG][client-response-fail] worker=${workerId} slotId=${slotId} reqId=${reqId} bodySize=${bytes.size} checksum=${checksum} tail=${tail} ex=${e.message}")
}

func jsonKindForDiag(json: JsonValue): String {
    match (json) {
        case _: JsonObject => "object"
        case _: JsonArray => "array"
        case _: JsonString => "string"
        case _: JsonBool => "bool"
        case _: JsonInt => "int"
        case _: JsonFloat => "float"
        case _: JsonNull => "null"
        case _ => "unknown"
    }
}

func diagJsonWalk(reqId: String, seq: Int64, json: JsonValue, path: String, depth: Int64,
                  count: AtomicInt64, printRate: Int64): Int64 {
    let node = count.fetchAdd(1) + 1
    if (node % printRate == 0) {
        println("[CASE27-DIAG][json-walk] reqId=${reqId} seq=${seq} node=${node} depth=${depth} path=${path} kind=${jsonKindForDiag(json)}")
    }

    match (json) {
        case jo: JsonObject =>
            var sum: Int64 = 2
            for ((k, v) in jo.getFields()) {
                sum = sum + k.size + 4
                sum = sum + diagJsonWalk(reqId, seq, v, "${path}.${k}", depth + 1, count, printRate)
            }
            return sum
        case ja: JsonArray =>
            var sum: Int64 = 2
            var i: Int64 = 0
            for (v in ja.getItems()) {
                sum = sum + diagJsonWalk(reqId, seq, v, "${path}[${i}]", depth + 1, count, printRate) + 1
                i = i + 1
            }
            return sum
        case js: JsonString => return js.getValue().size + 2
        case _: JsonBool => return 5
        case _: JsonInt => return 22
        case _: JsonFloat => return 200
        case _ => return 4
    }
}

func printJsonGraphDiagBeforeString(reqId: String, seq: Int64, root: JsonValue): Unit {
    if (!isGraphDiagEnabled()) {
        return
    }
    let count = AtomicInt64(0)
    let rate = graphDiagRate()
    println("[CASE27-DIAG][json-walk-start] reqId=${reqId} seq=${seq} rate=${rate}")
    let cap = diagJsonWalk(reqId, seq, root, "$", 0, count, rate)
    println("[CASE27-DIAG][json-walk-done] reqId=${reqId} seq=${seq} nodes=${count.load()} cap=${cap}")
    println("[CASE27-DIAG][json-before-to-string] reqId=${reqId} seq=${seq}")
}

// ---------- 文件读取 ----------

func readFileBytes(path: String): Array<Byte> {
    File.readFrom(path)
}

// ---------- Server-side: 解析 → 修改若干字段 → 序列化 ----------
// 修改操作覆盖：
//   1. 顶层 counters.in/ok 累加（read-modify-write 普通 int 字段）
//   2. 根对象插入新 key "processed_at" / "processed_seq"
//   3. events 数组每个对象 seq+=1，score *= 2.0 (Float)
//   4. events[i].user.attrs.credits *= 2.0
//   5. events[i].custom.product_ids 追加一个元素
//   6. 在 events[i] 上插入新 key "server_marker"

func mutateRequestJson(root: JsonObject, seq: Int64): Unit {
    // (1) counters.in / counters.ok 累加
    match (root.get("counters")) {
        case Some(c) =>
            let co = c.asObject()
            let inV = co.get("in").getOrThrow().asInt().getValue() + 1
            let okV = co.get("ok").getOrThrow().asInt().getValue() + 1
            co.put("in", JsonInt(inV))
            co.put("ok", JsonInt(okV))
        case None => ()
    }

    // (2) 根对象添加 marker
    root.put("processed_seq", JsonInt(seq))
    root.put("server_ok", JsonBool(true))

    // (3-6) events 遍历修改
    match (root.get("events")) {
        case Some(ev) =>
            let arr = ev.asArray()
            let n = arr.size()
            for (i in 0..n) {
                let item = arr.get(i).getOrThrow().asObject()
                // seq+=1
                match (item.get("seq")) {
                    case Some(s) =>
                        let v = s.asInt().getValue() + 1
                        item.put("seq", JsonInt(v))
                    case None => ()
                }
                // score *= 2.0
                match (item.get("score")) {
                    case Some(s) =>
                        let v = s.asFloat().getValue() * 2.0
                        item.put("score", JsonFloat(v))
                    case None => ()
                }
                // user.attrs.credits *= 2.0
                match (item.get("user")) {
                    case Some(u) =>
                        let uo = u.asObject()
                        match (uo.get("attrs")) {
                            case Some(a) =>
                                let ao = a.asObject()
                                match (ao.get("credits")) {
                                    case Some(cv) =>
                                        let v = cv.asFloat().getValue() * 2.0
                                        ao.put("credits", JsonFloat(v))
                                    case None => ()
                                }
                            case None => ()
                        }
                    case None => ()
                }
                // custom.product_ids 追加 1 项
                match (item.get("custom")) {
                    case Some(cu) =>
                        let cuo = cu.asObject()
                        match (cuo.get("product_ids")) {
                            case Some(pa) =>
                                let _ = pa.asArray().add(JsonInt(Int64(i) + 9999))
                            case None => ()
                        }
                    case None => ()
                }
                // 给 event 加一个 marker
                item.put("server_marker", JsonString("m-${i}"))
            }
        case None => ()
    }

}

func processRequest(bodyStr: String, seq: Int64, reqId: String): String {
    let root = parseServerJson(bodyStr).asObject()
    if (JSON_PROFILE_ENABLED) {
        let start = DateTime.now()
        mutateRequestJson(root, seq)
        JSON_PROFILE_STATS.serverMutateUs.fetchAdd(elapsedUs(start))
    } else {
        mutateRequestJson(root, seq)
    }
    printJsonGraphDiagBeforeString(reqId, seq, root)
    serializeServerJson(root, bodyStr.size)
}

// ---------- Client ----------

func buildClient(): Client {
    ClientBuilder()
        .poolSize(1)
        .readTimeout(Duration.millisecond * CLIENT_TIMEOUT_MS)
        .writeTimeout(Duration.millisecond * CLIENT_TIMEOUT_MS)
        .noProxy()
        .build()
}

// ---------- Stats（共享原子计数器） ----------
class Stats {
    let total: AtomicInt64 = AtomicInt64(0)
    let ok: AtomicInt64 = AtomicInt64(0)
    let fail: AtomicInt64 = AtomicInt64(0)
    let bytesSent: AtomicInt64 = AtomicInt64(0)
    let bytesRecv: AtomicInt64 = AtomicInt64(0)
    let inflight: AtomicInt64 = AtomicInt64(0)
}

let CLIENT_REQ_ID = AtomicInt64(1)

func buildAsyncSlotClient(): Client {
    ClientBuilder()
        .poolSize(1)
        .readTimeout(Duration.millisecond * CLIENT_TIMEOUT_MS)
        .writeTimeout(Duration.millisecond * CLIENT_TIMEOUT_MS)
        .noProxy()
        .build()
}

func runRequest(client: Client, workerId: Int64, slotId: Int64, reqId: Int64, url: String, payload: Array<Byte>, stats: Stats): Unit {
    try {
        let req = HttpRequestBuilder()
            .post()
            .url(url)
            .header("x-case27-reqid", reqId.toString())
            .header("x-case27-slotid", slotId.toString())
            .body(ByteBuffer(payload))
            .build()
        let rsp = client.send(req)
        let body = readRespStr(rsp)
        let bodySnapshot = captureBodySnapshotForDiag(body)
        // 校验：反序列化 + 确认 server_ok 字段存在
        try {
            let jv = parseClientJson(body)
            let serverOk = jv.asObject().get("server_ok").getOrThrow().asBool().getValue()
            if (serverOk) {
                stats.ok.fetchAdd(1)
            } else {
                stats.fail.fetchAdd(1)
            }
        } catch (e: Exception) {
            printBodyMutationDiag("client-response", reqId.toString(), slotId.toString(), bodySnapshot, body)
            printClientResponseDiag(workerId, slotId, reqId, body, e)
            match (bodySnapshot) {
                case Some(snapshotBytes) =>
                    dumpBadBytesForDiag("client-response-snapshot", reqId.toString(), slotId.toString(), snapshotBytes)
                case None => ()
            }
            dumpBadBodyForDiag("client-response-fail", reqId.toString(), slotId.toString(), body)
            throw e
        }
        stats.bytesSent.fetchAdd(Int64(payload.size))
        stats.bytesRecv.fetchAdd(Int64(body.size))
    } catch (e: Exception) {
        stats.fail.fetchAdd(1)
    }
}

func acquireAsyncSlot(slotBusy: Array<AtomicInt64>, maxInflight: Int64): Int64 {
    for (i in 0..maxInflight) {
        if (slotBusy[i].compareAndSwap(0, 1)) {
            return i
        }
    }
    return -1
}

func syncWorkerLoop(workerId: Int64, url: String, payload: Array<Byte>, stop: AtomicBool, stats: Stats): Unit {
    let client = buildClient()
    while (!stop.load()) {
        let _ = stats.inflight.fetchAdd(1)
        let reqId = CLIENT_REQ_ID.fetchAdd(1)
        try {
            runRequest(client, workerId, -1, reqId, url, payload, stats)
        } finally {
            stats.total.fetchAdd(1)
            stats.inflight.fetchAdd(-1)
        }
    }
    client.close()
}

func asyncSenderLoop(url: String, payload: Array<Byte>, stop: AtomicBool, stats: Stats,
                     sendIntervalMs: Int64, maxInflight: Int64, clients: Array<Client>,
                     slotBusy: Array<AtomicInt64>): Unit {
    while (!stop.load()) {
        let slot = acquireAsyncSlot(slotBusy, maxInflight)
        if (slot >= 0) {
            let mySlot = slot
            let myClient = clients[mySlot]
            let mySlotBusy = slotBusy[mySlot]
            let _ = stats.inflight.fetchAdd(1)
            let reqId = CLIENT_REQ_ID.fetchAdd(1)
            let _ = spawn {
                try {
                    runRequest(myClient, 0, mySlot, reqId, url, payload, stats)
                } finally {
                    stats.total.fetchAdd(1)
                    stats.inflight.fetchAdd(-1)
                    mySlotBusy.store(0)
                }
            }
        }
        if (sendIntervalMs > 0) {
            sleep(Duration.millisecond * sendIntervalMs)
        }
    }
}

// ---------- Main ----------

main() {
    println("[INFO] Case27: HTTP Complex-JSON Concurrent Benchmark")

    if (isEnvEnabled("CASE26_BODY_READ_SELF_TEST")) {
        runHttpBodyReadSelfTest()
        return
    }

    // 1. 读取环境变量
    let payloadName = getEnvStr("CASE26_PAYLOAD", "10K")
    let concurrency = getEnvInt("CASE26_CONCURRENCY", 8)
    let durationMs = getEnvInt("CASE26_DURATION_MS", 60_000)
    let runForever = durationMs <= 0
    let payloadDir = getEnvStr("CASE26_PAYLOAD_DIR", "./payloads")
    let serverPort = getEnvInt("CASE26_SERVER_PORT", DEFAULT_PORT)
    let role = getEnvStr("CASE26_ROLE", "both")
    let clientMode = getEnvStr("CASE26_CLIENT_MODE", "sync")
    let sendIntervalMs = getEnvInt("CASE26_SEND_INTERVAL_MS", 1)
    let maxInflight = getEnvInt("CASE26_MAX_INFLIGHT", 32)
    let persistentBytes = getEnvInt("CASE26_PERSISTENT_BYTES", 5_368_709_120)
    let persistentWorkers = getEnvInt("CASE26_PERSISTENT_WORKERS", 8)

    if (role != "both" && role != "server") {
        println("[ERROR] unsupported CASE26_ROLE=${role}; expected both or server")
        return
    }
    if (clientMode != "sync" && clientMode != "async") {
        println("[ERROR] unsupported CASE26_CLIENT_MODE=${clientMode}; expected sync or async")
        return
    }
    if (sendIntervalMs < 0) {
        println("[ERROR] CASE26_SEND_INTERVAL_MS must be >= 0, got ${sendIntervalMs}")
        return
    }
    if (maxInflight <= 0) {
        println("[ERROR] CASE26_MAX_INFLIGHT must be > 0, got ${maxInflight}")
        return
    }
    if (persistentBytes <= 0) {
        println("[ERROR] CASE26_PERSISTENT_BYTES must be > 0, got ${persistentBytes}")
        return
    }
    if (persistentWorkers < 2) {
        println("[ERROR] CASE26_PERSISTENT_WORKERS must be >= 2, got ${persistentWorkers}")
        return
    }
    let persistentAlignment = persistentWorkers * 2 * PERSISTENT_SMALL_OBJECT_SIZES[0]
    if (persistentBytes % persistentAlignment != 0) {
        println("[ERROR] CASE26_PERSISTENT_BYTES must be a multiple of ${persistentAlignment} for ${persistentWorkers} workers")
        return
    }

    println("[CONFIG] payload=${payloadName} concurrency=${concurrency} duration_ms=${durationMs}")
    println("[CONFIG] payload_dir=${payloadDir} server_port=${serverPort}")
    println("[CONFIG] client_timeout_ms=${CLIENT_TIMEOUT_MS}")
    println("[CONFIG] run_forever=${runForever}")
    println("[CONFIG] role=${role}")
    println("[CONFIG] client_mode=${clientMode}")
    println("[CONFIG] send_interval_ms=${sendIntervalMs} max_inflight=${maxInflight}")
    println("[CONFIG] persistent_bytes=${persistentBytes} persistent_workers=${persistentWorkers}")

    // 2. 多协程创建并持有持久堆对象；全部完成后才启动 HTTP Server。
    println("[INFO] Creating persistent heap baseline: bytes=${persistentBytes} workers=${persistentWorkers}...")
    let staticTarget = persistentBytes / 2
    let stackTarget = persistentBytes - staticTarget
    let persistentCompleted = AtomicInt64(0)
    let persistentFailed = AtomicBool(false)
    let persistentStaticAllocated = AtomicInt64(0)
    let persistentStackAllocated = AtomicInt64(0)
    let persistentFutures = ArrayList<Future<Unit>>()
    for (workerId in 0..persistentWorkers) {
        let staticShard = PersistentRootShard()
        PERSISTENT_STATIC_ROOT_SHARDS.add(staticShard)
        let workerStaticTarget = splitPersistentBytes(
            staticTarget, workerId, persistentWorkers)
        let workerStackTarget = splitPersistentBytes(
            stackTarget, workerId, persistentWorkers)
        persistentFutures.add(spawn { persistentAllocationWorker(
            workerId, workerStaticTarget, workerStackTarget, staticShard,
            persistentCompleted, persistentFailed,
            persistentStaticAllocated, persistentStackAllocated
        ) })
    }
    while (persistentCompleted.load() < persistentWorkers) {
        sleep(Duration.millisecond * 100)
    }
    if (persistentFailed.load()) {
        println("[ERROR] Persistent heap baseline creation failed; HTTP server will not start")
        return
    }
    let persistentStaticBytes = persistentStaticAllocated.load()
    let persistentStackBytes = persistentStackAllocated.load()
    println("[INFO] Persistent heap baseline ready: total=${persistentStaticBytes + persistentStackBytes} static=${persistentStaticBytes} stack=${persistentStackBytes}")

    // 3. 启动 HTTP Server
    let server = ServerBuilder()
        .addr(SERVER_ADDR)
        .port(UInt16(serverPort))
        .build()
    let serverSeq = AtomicInt64(0)
    server.distributor.register("/process", {
        ctx =>
        let reqId = ctx.request.headers.getFirst("x-case27-reqid") ?? "unknown"
        let slotId = ctx.request.headers.getFirst("x-case27-slotid") ?? "unknown"
        let bodyStr = readBodyStr(ctx.request)
        let bodySnapshot = captureBodySnapshotForDiag(bodyStr)
        let s = serverSeq.fetchAdd(1)
        let respStr = try {
            processRequest(bodyStr, s, reqId)
        } catch (e: Exception) {
            printBodyMutationDiag("server-body", reqId, slotId, bodySnapshot, bodyStr)
            printServerBodyDiag(reqId, slotId, bodyStr, e)
            match (bodySnapshot) {
                case Some(snapshotBytes) =>
                    dumpBadBytesForDiag("server-body-snapshot", reqId, slotId, snapshotBytes)
                case None => ()
            }
            dumpBadBodyForDiag("server-body-fail", reqId, slotId, bodyStr)
            throw e
        }
        ctx.responseBuilder
            .header("content-type", "application/json")
            .body(respStr)
    })
    let _ = spawn { server.serve() }
    sleep(Duration.second)
    let url = "http://${SERVER_ADDR}:${serverPort}/process"
    println("[INFO] HTTP Server ready at ${url}")

    if (role == "server") {
        println("[INFO] Running in server-only mode; waiting for external clients...")
        while (true) {
            sleep(Duration.second * 60)
        }
    }

    // 4. 读取 payload 文件
    let payloadFile = "${payloadDir}/${payloadName}.json"
    if (!exists(payloadFile)) {
        println("[ERROR] payload file not found: ${payloadFile}")
        return
    }
    let payload = readFileBytes(payloadFile)
    println("[INFO] loaded payload bytes=${payload.size} (${payload.size / 1024}KB) from ${payloadFile}")

    // 5. Warmup（单线程少量请求，预热 server & JIT-cache）
    println("[INFO] Warmup: 20 sequential requests...")
    let warmClient = buildClient()
    let tWarm = DateTime.now()
    for (_ in 0..20) {
        try {
            let rsp = warmClient.send(
                HttpRequestBuilder()
                    .post()
                    .url(url)
                    .body(ByteBuffer(payload))
                    .build()
            )
            let _ = readRespStr(rsp)
        } catch (e: Exception) {
            println("[WARN] warmup request failed: ${e.message}")
        }
    }
    warmClient.close()
    println("[INFO] Warmup done in ${elapsedMs(tWarm)}ms")

    // 6. 启动 N 并发 worker
    let stats = Stats()
    let stop = AtomicBool(false)
    let t0 = DateTime.now()
    let futures = ArrayList<Future<Unit>>()
    let asyncClientArr = ArrayList<Client>()
    let asyncSlotBusyArr = ArrayList<AtomicInt64>()
    if (clientMode == "async") {
        for (_ in 0..maxInflight) {
            asyncClientArr.add(buildAsyncSlotClient())
            asyncSlotBusyArr.add(AtomicInt64(0))
        }
    }
    let asyncClients = asyncClientArr.toArray()
    let asyncSlotBusy = asyncSlotBusyArr.toArray()
    if (clientMode == "async") {
        futures.add(spawn { asyncSenderLoop(url, payload, stop, stats, sendIntervalMs, maxInflight, asyncClients, asyncSlotBusy) })
    } else {
        for (w in 0..concurrency) {
            futures.add(spawn { syncWorkerLoop(w, url, payload, stop, stats) })
        }
    }
    if (runForever) {
        if (clientMode == "async") {
            println("[INFO] Started async sender in async mode; running without time limit...")
        } else {
            println("[INFO] Started ${concurrency} workers in sync mode; running without time limit...")
        }
    } else {
        if (clientMode == "async") {
            println("[INFO] Started async sender in async mode; running for ${durationMs}ms...")
        } else {
            println("[INFO] Started ${concurrency} workers in sync mode; running for ${durationMs}ms...")
        }
    }

    // 7. 主线程轮询打印进度，到时间通知 stop
    var lastTotal: Int64 = 0
    while (runForever || elapsedMs(t0) < durationMs) {
        sleep(Duration.second * 2)
        let now = stats.total.load()
        let delta = now - lastTotal
        lastTotal = now
        let el = elapsedMs(t0)
        println("[PROGRESS] t=${el}ms total=${now} ok=${stats.ok.load()} fail=${stats.fail.load()} inflight=${stats.inflight.load()} +last2s=${delta}")
    }
    stop.store(true)

    // 8. 给 worker 一些 in-flight 请求收尾时间，不做 join（避免 socket 阻塞导致死锁）
    let settleStart = DateTime.now()
    while (stats.inflight.load() > 0 && elapsedMs(settleStart) < CLIENT_TIMEOUT_MS + 2_000) {
        sleep(Duration.millisecond * 100)
    }
    if (clientMode == "async") {
        for (client in asyncClients) {
            client.close()
        }
    }
    let _ = futures  // suppress unused
    let _ = persistentFutures  // keep allocation coroutine handles live

    let totalMs = elapsedMs(t0)
    let totalReq = stats.total.load()
    let okReq = stats.ok.load()
    let failReq = stats.fail.load()
    let rps = if (totalMs > 0) { okReq * 1000 / totalMs } else { 0 }
    let sentMB = stats.bytesSent.load() / 1024 / 1024
    let recvMB = stats.bytesRecv.load() / 1024 / 1024

    println("[RESULT] payload=${payloadName} concurrency=${concurrency} duration=${totalMs}ms client_mode=${clientMode}")
    println("[RESULT] total=${totalReq} ok=${okReq} fail=${failReq} inflight=${stats.inflight.load()} throughput=${rps}req/s")
    println("[RESULT] bytes_sent=${sentMB}MB bytes_recv=${recvMB}MB")
    if (JSON_PROFILE_ENABLED) {
        let serverCalls = JSON_PROFILE_STATS.serverCalls.load()
        let clientCalls = JSON_PROFILE_STATS.clientCalls.load()
        let serverDivisor = if (serverCalls > 0) { serverCalls } else { 1 }
        let clientDivisor = if (clientCalls > 0) { clientCalls } else { 1 }
        let parseUs = JSON_PROFILE_STATS.serverParseUs.load()
        let mutateUs = JSON_PROFILE_STATS.serverMutateUs.load()
        let serializeUs = JSON_PROFILE_STATS.serverSerializeUs.load()
        let clientParseUs = JSON_PROFILE_STATS.clientParseUs.load()
        println("[JSON-PROFILE] server_calls=${serverCalls} parse_total_us=${parseUs} parse_avg_us=${parseUs / serverDivisor}")
        println("[JSON-PROFILE] mutate_total_us=${mutateUs} mutate_avg_us=${mutateUs / serverDivisor}")
        println("[JSON-PROFILE] serialize_total_us=${serializeUs} serialize_avg_us=${serializeUs / serverDivisor}")
        println("[JSON-PROFILE] client_calls=${clientCalls} parse_total_us=${clientParseUs} parse_avg_us=${clientParseUs / clientDivisor}")
    }
    println("[DONE]")
    Process.current.exit(0)
}
