import std.collection.*
import std.convert.*
import std.env.*
import std.io.*
import std.sync.*
import std.time.*
import stdx.net.http.*

let SERVER_ADDR = "127.0.0.1"
let DEFAULT_PORT: Int64 = 18229

class Stats {
    let total = AtomicInt64(0)
    let ok = AtomicInt64(0)
    let fail = AtomicInt64(0)
    let inflight = AtomicInt64(0)
}

func getEnvString(key: String, defaultValue: String): String {
    match (getVariable(key)) {
        case Some(value) => value
        case None => defaultValue
    }
}

func getEnvInt(key: String, defaultValue: Int64): Int64 {
    match (getVariable(key)) {
        case Some(value) => Int64.tryParse(value) ?? defaultValue
        case None => defaultValue
    }
}

func getQueryValue(rawQuery: String, key: String, defaultValue: String): String {
    let prefix = "${key}="
    for (part in rawQuery.split("&")) {
        if (part.startsWith(prefix)) {
            return part[prefix.size..]
        }
    }
    defaultValue
}

func getQueryInt(rawQuery: String, key: String, defaultValue: Int64): Int64 {
    Int64.tryParse(getQueryValue(rawQuery, key, "${defaultValue}")) ?? defaultValue
}

func buildClient(): Client {
    ClientBuilder()
        .poolSize(1)
        .readTimeout(Duration.second * 5)
        .writeTimeout(Duration.second * 5)
        .noProxy()
        .build()
}

func readResponseBody(response: HttpResponse): String {
    let expected = response.bodySize.getOrThrow()
    let bytes = Array<Byte>(expected, repeat: 0)
    var offset: Int64 = 0
    while (offset < expected) {
        let count = response.body.read(bytes.slice(offset, expected - offset))
        if (count <= 0) {
            throw Exception("case29 HTTP response body ended early")
        }
        offset += count
    }
    String.fromUtf8(bytes)
}

func responseMatches(client: Client, url: String, expected: String): Bool {
    let response = client.get(url)
    readResponseBody(response) == expected
}

func workerLoop(url: String, expected: String, stop: AtomicBool, stats: Stats): Unit {
    let client = buildClient()
    while (!stop.load()) {
        stats.inflight.fetchAdd(1)
        try {
            if (responseMatches(client, url, expected)) {
                stats.ok.fetchAdd(1)
            } else {
                stats.fail.fetchAdd(1)
            }
        } catch (e: Exception) {
            stats.fail.fetchAdd(1)
        } finally {
            stats.total.fetchAdd(1)
            stats.inflight.fetchAdd(-1)
        }
    }
    client.close()
}

main(): Unit {
    let seed = getEnvInt("CASE29_SEED", 0)
    let accountCount = getEnvInt("CASE29_ACCOUNT_COUNT", 1_000)
    let subjectId = getEnvString("CASE29_SUBJECT_ID", "subject-A")
    let limit = getEnvInt("CASE29_LIMIT", 50)
    let offset = getEnvInt("CASE29_OFFSET", 0)
    let concurrency = getEnvInt("CASE29_CONCURRENCY", 8)
    let durationMs = getEnvInt("CASE29_DURATION_MS", 60_000)
    let port = getEnvInt("CASE29_SERVER_PORT", DEFAULT_PORT)
    if (accountCount <= 0 || limit < 0 || offset < 0 || concurrency <= 0 || durationMs <= 0) {
        throw IllegalArgumentException("CASE29_ACCOUNT_COUNT, CASE29_CONCURRENCY and CASE29_DURATION_MS must be > 0; limit/offset must be >= 0")
    }

    let expected = renderMembers(seed, accountCount, subjectId, limit, offset)
    let server = ServerBuilder().addr(SERVER_ADDR).port(UInt16(port)).build()
    server.distributor.register("/api/v1/member-accounts", {
        context =>
        let query = context.request.url.rawQuery ?? ""
        let requestSubject = getQueryValue(query, "subjectId", "")
        let requestLimit = getQueryInt(query, "limit", 50)
        let requestOffset = getQueryInt(query, "offset", 0)
        context.responseBuilder
            .header("content-type", "application/json; charset=utf-8")
            .body(renderMembers(seed, accountCount, requestSubject, requestLimit, requestOffset))
    })
    let _ = spawn { server.serve() }
    sleep(Duration.second)

    let url = "http://${SERVER_ADDR}:${port}/api/v1/member-accounts?subjectId=${subjectId}&limit=${limit}&offset=${offset}"
    let warmupClient = buildClient()
    for (_ in 0..19) {
        if (!responseMatches(warmupClient, url, expected)) {
            throw Exception("case29 HTTP warmup response mismatch")
        }
    }
    warmupClient.close()

    println("[INFO] Case29: HTTP member-account in-memory benchmark")
    println("[CONFIG] seed=${seed} accounts=${accountCount} subject=${subjectId} limit=${limit} offset=${offset}")
    println("[CONFIG] concurrency=${concurrency} duration_ms=${durationMs} server=${url}")
    let stats = Stats()
    let stop = AtomicBool(false)
    let workers = ArrayList<Future<Unit>>()
    var workerIndex: Int64 = 0
    while (workerIndex < concurrency) {
        workers.add(spawn { workerLoop(url, expected, stop, stats) })
        workerIndex += 1
    }
    let start = DateTime.now()
    sleep(Duration.millisecond * durationMs)
    stop.store(true)
    let settleStart = DateTime.now()
    while (stats.inflight.load() > 0 && (DateTime.now() - settleStart).toMilliseconds() < 7_000) {
        sleep(Duration.millisecond * 20)
    }
    let elapsed = (DateTime.now() - start).toMilliseconds()
    let total = stats.total.load()
    let ok = stats.ok.load()
    let fail = stats.fail.load()
    let throughput = if (elapsed > 0) { ok * 1_000 / elapsed } else { 0 }
    println("[RESULT] payload=member-accounts concurrency=${concurrency} duration=${elapsed}ms")
    println("[RESULT] total=${total} ok=${ok} fail=${fail} inflight=${stats.inflight.load()} throughput=${throughput}req/s")
    if (fail > 0) {
        exit(1)
    }
    exit(0)
}
