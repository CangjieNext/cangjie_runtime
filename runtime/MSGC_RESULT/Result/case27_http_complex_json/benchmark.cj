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
//     CASE27_PAYLOAD       : 1K | 10K | 100K | 1M  (default 10K)
//     CASE27_CONCURRENCY   : Int>=1  (default 8)
//     CASE27_DURATION_MS   : Int; >0 means finite run, <=0 means no time limit
//                            (default 60000)
//     CASE27_PAYLOAD_DIR   : 目录，包含 <SIZE>.json (default ./payloads)
//     CASE27_SERVER_PORT   : Int  (default 18125)
//     CASE27_CLIENT_TIMEOUT_MS : HTTP client read/write timeout (default 5000)
//     CASE27_ROLE          : both | server (default both)
//     CASE27_CLIENT_MODE   : sync | async (default sync)
//     CASE27_SEND_INTERVAL_MS : async sender tick interval (default 1)
//     CASE27_MAX_INFLIGHT  : async global inflight cap (default 32)
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

let CLIENT_TIMEOUT_MS: Int64 = getEnvInt("CASE27_CLIENT_TIMEOUT_MS", 5_000)

func isEnvEnabled(key: String): Bool {
    match (getVariable(key)) {
        case Some(raw) => raw != "" && raw != "0"
        case None => false
    }
}

// ---------- HTTP Body 读取 ----------

func readBodyStr(req: HttpRequest): String {
    let chunk = Array<UInt8>(8192, repeat: 0)
    let result = ArrayList<UInt8>()
    var n = req.body.read(chunk)
    while (n > 0) {
        result.add(all: chunk[..n])
        n = req.body.read(chunk)
    }
    String.fromUtf8(result.toArray())
}

func readRespStr(rsp: HttpResponse): String {
    let chunk = Array<UInt8>(8192, repeat: 0)
    let result = ArrayList<UInt8>()
    var n = rsp.body.read(chunk)
    while (n > 0) {
        result.add(all: chunk[..n])
        n = rsp.body.read(chunk)
    }
    String.fromUtf8(result.toArray())
}

func isBodyDiagEnabled(): Bool {
    isEnvEnabled("CASE27_BODY_DIAG")
}

func isGraphDiagEnabled(): Bool {
    isEnvEnabled("CASE27_GRAPH_DIAG")
}

func isBadBodyDumpEnabled(): Bool {
    isEnvEnabled("CASE27_DUMP_BAD_BODY")
}

func graphDiagRate(): Int64 {
    let raw = getEnvInt("CASE27_GRAPH_DIAG_RATE", 512)
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
        let dumpDir = getEnvStr("CASE27_DUMP_DIR", ".")
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

func processRequest(bodyStr: String, seq: Int64, reqId: String): String {
    let jv = JsonValue.fromStr(bodyStr)
    let root = jv.asObject()

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

    printJsonGraphDiagBeforeString(reqId, seq, root)
    root.toString()
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
            let jv = JsonValue.fromStr(body)
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

    // 1. 读取环境变量
    let payloadName = getEnvStr("CASE27_PAYLOAD", "10K")
    let concurrency = getEnvInt("CASE27_CONCURRENCY", 8)
    let durationMs = getEnvInt("CASE27_DURATION_MS", 60_000)
    let runForever = durationMs <= 0
    let payloadDir = getEnvStr("CASE27_PAYLOAD_DIR", "./payloads")
    let serverPort = getEnvInt("CASE27_SERVER_PORT", DEFAULT_PORT)
    let role = getEnvStr("CASE27_ROLE", "both")
    let clientMode = getEnvStr("CASE27_CLIENT_MODE", "sync")
    let sendIntervalMs = getEnvInt("CASE27_SEND_INTERVAL_MS", 1)
    let maxInflight = getEnvInt("CASE27_MAX_INFLIGHT", 32)

    if (role != "both" && role != "server") {
        println("[ERROR] unsupported CASE27_ROLE=${role}; expected both or server")
        return
    }
    if (clientMode != "sync" && clientMode != "async") {
        println("[ERROR] unsupported CASE27_CLIENT_MODE=${clientMode}; expected sync or async")
        return
    }
    if (sendIntervalMs < 0) {
        println("[ERROR] CASE27_SEND_INTERVAL_MS must be >= 0, got ${sendIntervalMs}")
        return
    }
    if (maxInflight <= 0) {
        println("[ERROR] CASE27_MAX_INFLIGHT must be > 0, got ${maxInflight}")
        return
    }

    println("[CONFIG] payload=${payloadName} concurrency=${concurrency} duration_ms=${durationMs}")
    println("[CONFIG] payload_dir=${payloadDir} server_port=${serverPort}")
    println("[CONFIG] client_timeout_ms=${CLIENT_TIMEOUT_MS}")
    println("[CONFIG] run_forever=${runForever}")
    println("[CONFIG] role=${role}")
    println("[CONFIG] client_mode=${clientMode}")
    println("[CONFIG] send_interval_ms=${sendIntervalMs} max_inflight=${maxInflight}")

    // 2. 启动 HTTP Server
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
            .body(ByteBuffer(respStr.toArray()))
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

    // 3. 读取 payload 文件
    let payloadFile = "${payloadDir}/${payloadName}.json"
    if (!exists(payloadFile)) {
        println("[ERROR] payload file not found: ${payloadFile}")
        return
    }
    let payload = readFileBytes(payloadFile)
    println("[INFO] loaded payload bytes=${payload.size} (${payload.size / 1024}KB) from ${payloadFile}")

    // 4. Warmup（单线程少量请求，预热 server & JIT-cache）
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

    // 5. 启动 N 并发 worker
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

    // 6. 主线程轮询打印进度，到时间通知 stop
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

    // 7. 给 worker 一些 in-flight 请求收尾时间，不做 join（避免 socket 阻塞导致死锁）
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

    let totalMs = elapsedMs(t0)
    let totalReq = stats.total.load()
    let okReq = stats.ok.load()
    let failReq = stats.fail.load()
    let rps = if (totalMs > 0) { totalReq * 1000 / totalMs } else { 0 }
    let sentMB = stats.bytesSent.load() / 1024 / 1024
    let recvMB = stats.bytesRecv.load() / 1024 / 1024

    println("[RESULT] payload=${payloadName} concurrency=${concurrency} duration=${totalMs}ms client_mode=${clientMode}")
    println("[RESULT] total=${totalReq} ok=${okReq} fail=${failReq} inflight=${stats.inflight.load()} throughput=${rps}req/s")
    println("[RESULT] bytes_sent=${sentMB}MB bytes_recv=${recvMB}MB")
    println("[DONE]")
    Process.current.exit(0)
}
