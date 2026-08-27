import std.collection.*
import std.convert.*
import std.env.*
import std.fs.*
import std.io.*
import std.sync.*
import std.time.*
import stdx.net.http.*

let SERVER_ADDR = "127.0.0.1"
let DEFAULT_PORT: Int64 = 18225

func getEnvStr(key: String, def: String): String {
    match (getVariable(key)) {
        case Some(value) => value
        case None => def
    }
}

func getEnvInt(key: String, def: Int64): Int64 {
    match (getVariable(key)) {
        case Some(raw) => Int64.tryParse(raw) ?? def
        case None => def
    }
}

func elapsedMs(start: DateTime): Int64 {
    (DateTime.now() - start).toMilliseconds()
}

func readFileBytes(path: String): Array<Byte> {
    File.readFrom(path)
}

class Stats {
    let total = AtomicInt64(0)
    let ok = AtomicInt64(0)
    let fail = AtomicInt64(0)
    let bytesSent = AtomicInt64(0)
    let bytesRecv = AtomicInt64(0)
    let inflight = AtomicInt64(0)
}

func buildClient(timeoutMs: Int64): Client {
    ClientBuilder()
        .poolSize(1)
        .readTimeout(Duration.millisecond * timeoutMs)
        .writeTimeout(Duration.millisecond * timeoutMs)
        .noProxy()
        .build()
}

func runRequest(client: Client, url: String, payload: Array<Byte>, stats: Stats): Unit {
    try {
        let request = HttpRequestBuilder()
            .post()
            .url(url)
            .body(ByteBuffer(payload))
            .build()
        let response = client.send(request)
        if (responseHasServerOk(response.body)) {
            stats.ok.fetchAdd(1)
        } else {
            stats.fail.fetchAdd(1)
        }
        stats.bytesSent.fetchAdd(payload.size)
    } catch (e: Exception) {
        stats.fail.fetchAdd(1)
    } finally {
        stats.total.fetchAdd(1)
    }
}

func workerLoop(url: String, payload: Array<Byte>, timeoutMs: Int64,
                stop: AtomicBool, stats: Stats): Unit {
    let client = buildClient(timeoutMs)
    while (!stop.load()) {
        stats.inflight.fetchAdd(1)
        try {
            runRequest(client, url, payload, stats)
        } finally {
            stats.inflight.fetchAdd(-1)
        }
    }
    client.close()
}

func startServer(port: Int64): Server {
    let builder = ServerBuilder()
        .addr(SERVER_ADDR)
        .port(UInt16(port))
        .afterBind({ => println("[READY] HTTP Server ready at http://${SERVER_ADDR}:${port}/process") })
    let server = builder.build()
    let serverSequence = AtomicInt64(0)
    server.distributor.register("/process", {
        ctx =>
        let output = ByteBuffer(initialResponseCapacity(ctx.request.bodySize))
        transformRequest(ctx.request.body, output, serverSequence.fetchAdd(1))
        ctx.responseBuilder
            .header("content-type", "application/json")
            .body(output)
    })
    server
}

main() {
    let payloadName = getEnvStr("CASE25_PAYLOAD", "10K")
    let concurrency = getEnvInt("CASE25_CONCURRENCY", 8)
    let durationMs = getEnvInt("CASE25_DURATION_MS", 60_000)
    let payloadDir = getEnvStr(
        "CASE25_PAYLOAD_DIR",
        "payloads"
    )
    let port = getEnvInt("CASE25_SERVER_PORT", DEFAULT_PORT)
    let timeoutMs = getEnvInt("CASE25_CLIENT_TIMEOUT_MS", 5_000)
    let role = getEnvStr("CASE25_ROLE", "both")
    let persistentBytes = getEnvInt("CASE25_PERSISTENT_BYTES", 0)
    let persistentWorkers = getEnvInt("CASE25_PERSISTENT_WORKERS", 8)
    if (concurrency <= 0 || durationMs <= 0 || timeoutMs <= 0) {
        println("[ERROR] CASE25_CONCURRENCY, CASE25_DURATION_MS and CASE25_CLIENT_TIMEOUT_MS must be > 0")
        return
    }
    if (role != "both" && role != "server" && role != "client") {
        println("[ERROR] unsupported CASE25_ROLE=${role}; expected both, server, or client")
        return
    }
    try {
        validatePersistentBaseline(persistentBytes, persistentWorkers)
    } catch (e: Error) {
        println("[ERROR] ${e.message}")
        return
    } catch (e: Exception) {
        println("[ERROR] ${e.message}")
        return
    }

    try {
        initializePersistentBaseline(persistentBytes, persistentWorkers)
    } catch (e: Error) {
        println("[ERROR] ${e.message}")
        return
    } catch (e: Exception) {
        println("[ERROR] ${e.message}")
        return
    }
    if (role == "server") {
        println("[INFO] Case25: HTTP complex JSON stream benchmark (server)")
        let server = startServer(port)
        server.serve()
        return
    }

    let payloadFile = "${payloadDir}/${payloadName}.json"
    if (!exists(payloadFile)) {
        println("[ERROR] payload file not found: ${payloadFile}")
        return
    }
    let payload = readFileBytes(payloadFile)
    if (role == "both") {
        let server = startServer(port)
        let _ = spawn { server.serve() }
        sleep(Duration.second)
    }

    let url = "http://${SERVER_ADDR}:${port}/process"
    println("[INFO] Case25: HTTP complex JSON stream benchmark")
    println("[CONFIG] payload=${payloadName} bytes=${payload.size} concurrency=${concurrency} duration_ms=${durationMs}")
    println("[CONFIG] server=${url} payload_dir=${payloadDir}")
    println("[CONFIG] persistent_bytes=${persistentBytes} persistent_workers=${persistentWorkers}")

    let warmupClient = buildClient(timeoutMs)
    for (_ in 0..20) {
        runRequest(warmupClient, url, payload, Stats())
    }
    warmupClient.close()

    let stats = Stats()
    let stop = AtomicBool(false)
    let workers = ArrayList<Future<Unit>>()
    for (_ in 0..concurrency) {
        workers.add(spawn { workerLoop(url, payload, timeoutMs, stop, stats) })
    }
    let start = DateTime.now()
    var lastTotal: Int64 = 0
    while (elapsedMs(start) < durationMs) {
        sleep(Duration.second * 2)
        let total = stats.total.load()
        println("[PROGRESS] t=${elapsedMs(start)}ms total=${total} ok=${stats.ok.load()} fail=${stats.fail.load()} inflight=${stats.inflight.load()} +last2s=${total - lastTotal}")
        lastTotal = total
    }
    stop.store(true)

    let settleStart = DateTime.now()
    while (stats.inflight.load() > 0 && elapsedMs(settleStart) < timeoutMs + 2_000) {
        sleep(Duration.millisecond * 100)
    }
    let _ = workers

    let elapsed = elapsedMs(start)
    let total = stats.total.load()
    let ok = stats.ok.load()
    let fail = stats.fail.load()
    let throughput = if (elapsed > 0) { ok * 1_000 / elapsed } else { 0 }
    println("[RESULT] payload=${payloadName} concurrency=${concurrency} duration=${elapsed}ms")
    println("[RESULT] total=${total} ok=${ok} fail=${fail} inflight=${stats.inflight.load()} throughput=${throughput}req/s")
    println("[DONE]")
    exit(0)
}
