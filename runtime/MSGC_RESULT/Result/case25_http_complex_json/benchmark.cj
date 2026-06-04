import std.collection.*
import std.convert.*
import std.env.*
import std.fs.*
import std.process.*
import std.sync.*
import std.time.*
import stdx.net.http.*
import stdx.encoding.json.*

// ============================================================
//   Case25: HTTP Complex-JSON Concurrent Benchmark
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
//     CASE25_PAYLOAD       : 1K | 10K | 100K | 1M | 10M  (default 10K)
//     CASE25_CONCURRENCY   : Int>=1  (default 8)
//     CASE25_DURATION_MS   : Int>=1000 (default 60000)
//     CASE25_PAYLOAD_DIR   : 目录，包含 <SIZE>.json (default ./payloads)
//     CASE25_SERVER_PORT   : Int  (default 18125)
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

// ---------- HTTP Body 读取 ----------

func readBodyStr(req: HttpRequest): String {
    let chunk = Array<UInt8>(8192, repeat: 0)
    let result = ArrayList<UInt8>()
    var n = req.body.read(chunk)
    while (n > 0) {
        for (i in 0..n) { result.add(chunk[i]) }
        n = req.body.read(chunk)
    }
    String.fromUtf8(result.toArray())
}

func readRespStr(rsp: HttpResponse): String {
    let chunk = Array<UInt8>(8192, repeat: 0)
    let result = ArrayList<UInt8>()
    var n = rsp.body.read(chunk)
    while (n > 0) {
        for (i in 0..n) { result.add(chunk[i]) }
        n = rsp.body.read(chunk)
    }
    String.fromUtf8(result.toArray())
}

// ---------- 文件读取 ----------

func readFileAsString(path: String): String {
    let bytes = File.readFrom(path)
    String.fromUtf8(bytes)
}

// ---------- Server-side: 解析 → 修改若干字段 → 序列化 ----------
// 修改操作覆盖：
//   1. 顶层 counters.in/ok 累加（read-modify-write 普通 int 字段）
//   2. 根对象插入新 key "processed_at" / "processed_seq"
//   3. events 数组每个对象 seq+=1，score *= 2.0 (Float)
//   4. events[i].user.attrs.credits *= 2.0
//   5. events[i].custom.product_ids 追加一个元素
//   6. 在 events[i] 上插入新 key "server_marker"

func processRequest(bodyStr: String, seq: Int64): String {
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

    root.toString()
}

// ---------- Client ----------

func buildClient(): Client {
    ClientBuilder()
        .poolSize(8)
        .readTimeout(Duration.second * 5)
        .writeTimeout(Duration.second * 5)
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
}

// ---------- 单 worker 循环 ----------

func workerLoop(workerId: Int64, url: String, payload: String, stop: AtomicBool, stats: Stats): Unit {
    let client = buildClient()
    while (!stop.load()) {
        try {
            let rsp = client.post(url, payload)
            let body = readRespStr(rsp)
            // 校验：反序列化 + 确认 server_ok 字段存在
            let jv = JsonValue.fromStr(body)
            let serverOk = jv.asObject().get("server_ok").getOrThrow().asBool().getValue()
            if (serverOk) {
                stats.ok.fetchAdd(1)
            } else {
                stats.fail.fetchAdd(1)
            }
            stats.bytesSent.fetchAdd(Int64(payload.size))
            stats.bytesRecv.fetchAdd(Int64(body.size))
        } catch (e: Exception) {
            stats.fail.fetchAdd(1)
        }
        stats.total.fetchAdd(1)
    }
    client.close()
    let _ = workerId
}

// ---------- Main ----------

main() {
    println("[INFO] Case25: HTTP Complex-JSON Concurrent Benchmark")

    // 1. 读取环境变量
    let payloadName = getEnvStr("CASE25_PAYLOAD", "10K")
    let concurrency = getEnvInt("CASE25_CONCURRENCY", 8)
    let durationMs = getEnvInt("CASE25_DURATION_MS", 60_000)
    let payloadDir = getEnvStr("CASE25_PAYLOAD_DIR", "./payloads")
    let serverPort = getEnvInt("CASE25_SERVER_PORT", DEFAULT_PORT)

    println("[CONFIG] payload=${payloadName} concurrency=${concurrency} duration_ms=${durationMs}")
    println("[CONFIG] payload_dir=${payloadDir} server_port=${serverPort}")

    // 2. 读取 payload 文件
    let payloadFile = "${payloadDir}/${payloadName}.json"
    if (!exists(payloadFile)) {
        println("[ERROR] payload file not found: ${payloadFile}")
        return
    }
    let payload = readFileAsString(payloadFile)
    println("[INFO] loaded payload bytes=${payload.size} (${payload.size / 1024}KB) from ${payloadFile}")

    // 3. 启动 HTTP Server
    let server = ServerBuilder()
        .addr(SERVER_ADDR)
        .port(UInt16(serverPort))
        .build()
    let serverSeq = AtomicInt64(0)
    server.distributor.register("/process", {
        ctx =>
        let bodyStr = readBodyStr(ctx.request)
        let s = serverSeq.fetchAdd(1)
        let respStr = processRequest(bodyStr, s)
        ctx.responseBuilder.header("content-type", "application/json").body(respStr)
    })
    let _ = spawn { server.serve() }
    sleep(Duration.second)
    let url = "http://${SERVER_ADDR}:${serverPort}/process"
    println("[INFO] HTTP Server ready at ${url}")

    // 4. Warmup（单线程少量请求，预热 server & JIT-cache）
    println("[INFO] Warmup: 20 sequential requests...")
    let warmClient = buildClient()
    let tWarm = DateTime.now()
    for (_ in 0..20) {
        try {
            let rsp = warmClient.post(url, payload)
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
    for (w in 0..concurrency) {
        futures.add(spawn { workerLoop(w, url, payload, stop, stats) })
    }
    println("[INFO] Started ${concurrency} workers; running for ${durationMs}ms...")

    // 6. 主线程轮询打印进度，到时间通知 stop
    var lastTotal: Int64 = 0
    while (elapsedMs(t0) < durationMs) {
        sleep(Duration.second * 2)
        let now = stats.total.load()
        let delta = now - lastTotal
        lastTotal = now
        let el = elapsedMs(t0)
        println("[PROGRESS] t=${el}ms total=${now} ok=${stats.ok.load()} fail=${stats.fail.load()} +last2s=${delta}")
    }
    stop.store(true)

    // 7. 给 worker 一些 in-flight 请求收尾时间，不做 join（避免 socket 阻塞导致死锁）
    sleep(Duration.second * 2)
    let _ = futures  // suppress unused

    let totalMs = elapsedMs(t0)
    let totalReq = stats.total.load()
    let okReq = stats.ok.load()
    let failReq = stats.fail.load()
    let rps = if (totalMs > 0) { totalReq * 1000 / totalMs } else { 0 }
    let sentMB = stats.bytesSent.load() / 1024 / 1024
    let recvMB = stats.bytesRecv.load() / 1024 / 1024

    println("[RESULT] payload=${payloadName} concurrency=${concurrency} duration=${totalMs}ms")
    println("[RESULT] total=${totalReq} ok=${okReq} fail=${failReq} throughput=${rps}req/s")
    println("[RESULT] bytes_sent=${sentMB}MB bytes_recv=${recvMB}MB")
    println("[DONE]")
    Process.current.exit(0)
}
