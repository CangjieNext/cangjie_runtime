import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Case27: HTTP Complex-JSON Concurrent Benchmark (Java mirror)
 *
 * Single-process HTTP Server + concurrent HTTP Client.
 * Server: deserialize JSON tree → read-modify-write multiple fields → re-serialize.
 * Client: deserialize response → verify server_ok marker field.
 *
 * Environment variables:
 *   CASE26_PAYLOAD         1K|10K|100K|1M  (default 10K)
 *   CASE26_CONCURRENCY     Int>=1           (default 8)
 *   CASE26_DURATION_MS     Int; >0=finite, <=0=no limit (default 60000)
 *   CASE26_PAYLOAD_DIR     payload directory (default ./payloads)
 *   CASE26_SERVER_PORT     Int              (default 18125)
 *   CASE26_CLIENT_TIMEOUT_MS HTTP timeout   (default 5000)
 *   CASE26_ROLE            both|server      (default both)
 *   CASE26_CLIENT_MODE     sync|async       (default sync)
 *   CASE26_SEND_INTERVAL_MS async tick      (default 1)
 *   CASE26_MAX_INFLIGHT    async cap        (default 32)
 *   CASE26_PERSISTENT_BYTES persistent heap bytes (default 5 GiB)
 *   CASE26_PERSISTENT_WORKERS persistent workers (default 8)
 */
public class Case27Benchmark {

    // ---------- constants ----------
    private static final String DEFAULT_SERVER_ADDR = "127.0.0.1";
    private static final int DEFAULT_PORT = 18125;
    private static final int DEFAULT_CONCURRENCY = 8;
    private static final int DEFAULT_DURATION_MS = 60_000;
    private static final int DEFAULT_TIMEOUT_MS = 5_000;
    private static final int PROGRESS_INTERVAL_MS = 2_000;
    private static final int WARMUP_REQUESTS = 20;
    private static final int BODY_CHUNK_SIZE = 8192;
    private static final long DEFAULT_PERSISTENT_BYTES = 5L << 30;
    private static final int DEFAULT_PERSISTENT_WORKERS = 8;
    private static final int[] PERSISTENT_SMALL_OBJECT_SIZES =
            {128, 256, 512, 1024, 2048, 4096, 8192, 16384};
    private static final int[] PERSISTENT_LARGE_OBJECT_SIZES =
            {64 << 10, 256 << 10, 1 << 20};
    private static volatile List<byte[]>[] persistentStaticRootShards;
    private static final AtomicLong PERSISTENT_TOUCH_SINK = new AtomicLong(0L);

    private static final ObjectMapper MAPPER = new ObjectMapper();

    // ---------- config ----------
    record BenchConfig(
            String payloadName,
            int concurrency,
            long durationMs,
            boolean runForever,
            String payloadDir,
            String serverAddr,
            int serverPort,
            long clientTimeoutMs,
            String role,
            String clientMode,
            long sendIntervalMs,
            int maxInflight,
            long persistentBytes,
            int persistentWorkers
    ) {}

    // ---------- stats ----------
    static class AtomicStats {
        final AtomicLong total = new AtomicLong(0);
        final AtomicLong ok = new AtomicLong(0);
        final AtomicLong fail = new AtomicLong(0);
        final AtomicLong bytesSent = new AtomicLong(0);
        final AtomicLong bytesRecv = new AtomicLong(0);
        final AtomicLong inflight = new AtomicLong(0);

        RunStats snapshot() {
            return new RunStats(
                    total.get(), ok.get(), fail.get(),
                    bytesSent.get(), bytesRecv.get(), inflight.get()
            );
        }
    }

    record RunStats(long total, long ok, long fail, long bytesSent, long bytesRecv, long inflight) {}

    // ---------- env helpers ----------

    private static String getEnvStr(String key, String def) {
        String val = System.getenv(key);
        return (val != null && !val.isEmpty()) ? val : def;
    }

    private static long getEnvLong(String key, long def) {
        String raw = System.getenv(key);
        if (raw == null || raw.isEmpty()) return def;
        try {
            return Long.parseLong(raw);
        } catch (NumberFormatException e) {
            return def;
        }
    }

    private static int getEnvInt(String key, int def) {
        return (int) getEnvLong(key, def);
    }

    // ---------- config loading ----------

    private static BenchConfig loadConfigFromEnv() {
        String payloadName = getEnvStr("CASE26_PAYLOAD", "10K");
        int concurrency = getEnvInt("CASE26_CONCURRENCY", DEFAULT_CONCURRENCY);
        long durationMs = getEnvLong("CASE26_DURATION_MS", DEFAULT_DURATION_MS);
        boolean runForever = durationMs <= 0;
        String payloadDir = getEnvStr("CASE26_PAYLOAD_DIR", "./payloads");
        int serverPort = getEnvInt("CASE26_SERVER_PORT", DEFAULT_PORT);
        long clientTimeoutMs = getEnvLong("CASE26_CLIENT_TIMEOUT_MS", DEFAULT_TIMEOUT_MS);
        String role = getEnvStr("CASE26_ROLE", "both");
        String clientMode = getEnvStr("CASE26_CLIENT_MODE", "sync");
        long sendIntervalMs = getEnvLong("CASE26_SEND_INTERVAL_MS", 1);
        int maxInflight = getEnvInt("CASE26_MAX_INFLIGHT", 32);
        long persistentBytes = getEnvLong("CASE26_PERSISTENT_BYTES", DEFAULT_PERSISTENT_BYTES);
        int persistentWorkers = getEnvInt("CASE26_PERSISTENT_WORKERS", DEFAULT_PERSISTENT_WORKERS);

        if (concurrency <= 0) throw new IllegalArgumentException("CASE26_CONCURRENCY must be > 0");
        if (clientTimeoutMs <= 0) throw new IllegalArgumentException("CASE26_CLIENT_TIMEOUT_MS must be > 0");
        if (!role.equals("both") && !role.equals("server"))
            throw new IllegalArgumentException("unsupported CASE26_ROLE=" + role + "; expected both or server");
        if (!clientMode.equals("sync") && !clientMode.equals("async"))
            throw new IllegalArgumentException("unsupported CASE26_CLIENT_MODE=" + clientMode + "; expected sync or async");
        if (sendIntervalMs < 0)
            throw new IllegalArgumentException("CASE26_SEND_INTERVAL_MS must be >= 0");
        if (maxInflight <= 0)
            throw new IllegalArgumentException("CASE26_MAX_INFLIGHT must be > 0");
        if (serverPort <= 0 || serverPort > 65535)
            throw new IllegalArgumentException("CASE26_SERVER_PORT must be in 1..65535");
        if (persistentBytes <= 0)
            throw new IllegalArgumentException("CASE26_PERSISTENT_BYTES must be > 0");
        if (persistentWorkers < 2)
            throw new IllegalArgumentException("CASE26_PERSISTENT_WORKERS must be >= 2");

        return new BenchConfig(payloadName, concurrency, durationMs, runForever,
                payloadDir, DEFAULT_SERVER_ADDR, serverPort, clientTimeoutMs,
                role, clientMode, sendIntervalMs, maxInflight,
                persistentBytes, persistentWorkers);
    }

    // ---------- Persistent heap baseline ----------

    static final class PersistentLayout {
        final long totalBytes;
        final long staticBytes;
        final long stackBytes;
        final long largeBytes;
        final int workers;

        private PersistentLayout(long totalBytes, long staticBytes, long stackBytes,
                                 long largeBytes, int workers) {
            this.totalBytes = totalBytes;
            this.staticBytes = staticBytes;
            this.stackBytes = stackBytes;
            this.largeBytes = largeBytes;
            this.workers = workers;
        }

        static PersistentLayout create(long totalBytes, int workers) {
            if (totalBytes <= 0) {
                throw new IllegalArgumentException("persistent bytes must be > 0");
            }
            if (workers < 2) {
                throw new IllegalArgumentException("persistent workers must be >= 2");
            }
            long alignment = 2L * workers * PERSISTENT_SMALL_OBJECT_SIZES[0];
            if (totalBytes % alignment != 0) {
                throw new IllegalArgumentException(
                        "persistent bytes must be a multiple of " + alignment
                                + " for " + workers + " workers");
            }
            long staticBytes = totalBytes / 2;
            long stackBytes = totalBytes - staticBytes;
            long largeBytes = 0;
            for (int workerId = 0; workerId < workers; workerId++) {
                largeBytes += persistentLargeBytes(
                        splitPersistentBytes(staticBytes, workerId, workers));
                largeBytes += persistentLargeBytes(
                        splitPersistentBytes(stackBytes, workerId, workers));
            }
            return new PersistentLayout(totalBytes, staticBytes, stackBytes, largeBytes, workers);
        }
    }

    static final class PersistentStats {
        final long totalBytes;
        final long staticBytes;
        final long stackBytes;

        private PersistentStats(long totalBytes, long staticBytes, long stackBytes) {
            this.totalBytes = totalBytes;
            this.staticBytes = staticBytes;
            this.stackBytes = stackBytes;
        }
    }

    static final class PersistentBaseline implements AutoCloseable {
        private final AtomicBoolean stop = new AtomicBoolean(false);
        private final AtomicLong totalAllocated = new AtomicLong(0);
        private final AtomicLong staticAllocated = new AtomicLong(0);
        private final AtomicLong stackAllocated = new AtomicLong(0);
        private final Thread[] threads;

        private PersistentBaseline(int workers) {
            this.threads = new Thread[workers];
        }

        @SuppressWarnings("unchecked")
        static PersistentBaseline start(long totalBytes, int workers) throws InterruptedException {
            PersistentLayout layout = PersistentLayout.create(totalBytes, workers);
            PersistentBaseline baseline = new PersistentBaseline(workers);
            persistentStaticRootShards = (List<byte[]>[]) new List<?>[workers];
            CountDownLatch ready = new CountDownLatch(workers);
            AtomicReference<Throwable> failure = new AtomicReference<>();

            for (int workerId = 0; workerId < workers; workerId++) {
                final int id = workerId;
                final long staticTarget = splitPersistentBytes(layout.staticBytes, workerId, workers);
                final long stackTarget = splitPersistentBytes(layout.stackBytes, workerId, workers);
                Thread thread = new Thread(() -> baseline.allocationWorker(
                        id, staticTarget, stackTarget, ready, failure),
                        "case26-persistent-" + workerId);
                thread.setDaemon(true);
                baseline.threads[workerId] = thread;
                thread.start();
            }

            ready.await();
            Throwable allocationFailure = failure.get();
            if (allocationFailure != null) {
                baseline.close();
                throw new IllegalStateException("persistent baseline allocation failed",
                        allocationFailure);
            }
            return baseline;
        }

        private void allocationWorker(int workerId, long staticTarget, long stackTarget,
                                      CountDownLatch ready, AtomicReference<Throwable> failure) {
            List<byte[]> staticRoots = null;
            List<byte[]> stackRoots = null;
            try {
                byte seed = (byte) (workerId % 251 + 1);
                staticRoots = allocatePersistentRoots(staticTarget, seed);
                stackRoots = allocatePersistentRoots(stackTarget, seed);
                persistentStaticRootShards[workerId] = staticRoots;
                staticAllocated.addAndGet(staticTarget);
                stackAllocated.addAndGet(stackTarget);
                totalAllocated.addAndGet(staticTarget + stackTarget);
            } catch (Throwable throwable) {
                failure.compareAndSet(null, throwable);
            } finally {
                ready.countDown();
            }

            if (staticRoots == null || stackRoots == null) {
                return;
            }
            while (!stop.get()) {
                PERSISTENT_TOUCH_SINK.addAndGet(
                        touchPersistentRoots(staticRoots) + touchPersistentRoots(stackRoots));
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
            PERSISTENT_TOUCH_SINK.addAndGet(
                    touchPersistentRoots(staticRoots) + touchPersistentRoots(stackRoots));
        }

        PersistentStats stats() {
            return new PersistentStats(totalAllocated.get(), staticAllocated.get(), stackAllocated.get());
        }

        @Override
        public void close() {
            if (!stop.compareAndSet(false, true)) {
                return;
            }
            for (Thread thread : threads) {
                if (thread != null) {
                    thread.interrupt();
                }
            }
            for (Thread thread : threads) {
                if (thread == null) {
                    continue;
                }
                try {
                    thread.join();
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
        }
    }

    private static long splitPersistentBytes(long total, int index, int count) {
        long base = total / count;
        return index < total % count ? base + 1 : base;
    }

    private static int persistentObjectCount(long targetBytes, int[] sizes) {
        long count = 0;
        long allocated = 0;
        long perClass = targetBytes / sizes.length;
        for (int size : sizes) {
            long objects = perClass / size;
            count += objects;
            allocated += objects * size;
        }
        count += (targetBytes - allocated) / sizes[0];
        return (int) Math.min(count, Integer.MAX_VALUE - 8L);
    }

    private static long persistentLargeBytes(long targetBytes) {
        long alignment = PERSISTENT_LARGE_OBJECT_SIZES[0];
        return targetBytes / 100 / alignment * alignment;
    }

    private static List<byte[]> allocatePersistentRoots(long targetBytes, byte seed) {
        long largeBytes = persistentLargeBytes(targetBytes);
        long smallBytes = targetBytes - largeBytes;
        int capacity = persistentObjectCount(smallBytes, PERSISTENT_SMALL_OBJECT_SIZES)
                + persistentObjectCount(largeBytes, PERSISTENT_LARGE_OBJECT_SIZES);
        List<byte[]> roots = new ArrayList<>(capacity);
        appendPersistentTier(roots, smallBytes, PERSISTENT_SMALL_OBJECT_SIZES, seed);
        appendPersistentTier(roots, largeBytes, PERSISTENT_LARGE_OBJECT_SIZES, seed);
        return roots;
    }

    private static void appendPersistentTier(List<byte[]> roots, long targetBytes,
                                             int[] sizes, byte seed) {
        long allocated = 0;
        long perClass = targetBytes / sizes.length;
        for (int size : sizes) {
            long fullObjects = perClass / size;
            for (long objectIndex = 0; objectIndex < fullObjects; objectIndex++) {
                roots.add(makeTouchedPersistentObject(size, seed));
                allocated += size;
            }
        }
        int smallestSize = sizes[0];
        long remainingObjects = (targetBytes - allocated) / smallestSize;
        for (long objectIndex = 0; objectIndex < remainingObjects; objectIndex++) {
            roots.add(makeTouchedPersistentObject(smallestSize, seed));
        }
    }

    private static byte[] makeTouchedPersistentObject(int size, byte seed) {
        byte[] object = new byte[size];
        for (int offset = 0; offset < object.length; offset += 4096) {
            object[offset] = seed;
        }
        if (object.length > 0) {
            object[object.length - 1] = seed;
        }
        return object;
    }

    private static long touchPersistentRoots(List<byte[]> roots) {
        long checksum = 0;
        for (byte[] object : roots) {
            if (object.length == 0) {
                continue;
            }
            checksum += (object[0] & 0xffL)
                    + (object[object.length - 1] & 0xffL) + object.length;
        }
        return checksum;
    }

    // ---------- helpers ----------

    private static String serverURL(BenchConfig cfg) {
        return "http://" + cfg.serverAddr() + ":" + cfg.serverPort() + "/process";
    }

    private static byte[] loadPayload(BenchConfig cfg) throws IOException {
        Path payloadPath = Path.of(cfg.payloadDir(), cfg.payloadName() + ".json");
        if (!Files.exists(payloadPath)) {
            throw new IOException("payload file not found: " + payloadPath);
        }
        byte[] data = Files.readAllBytes(payloadPath);
        System.out.println("[INFO] loaded payload bytes=" + data.length
                + " (" + (data.length / 1024) + "KB) from " + payloadPath);
        return data;
    }

    private static byte[] readAllBytes(InputStream is) throws IOException {
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        byte[] chunk = new byte[BODY_CHUNK_SIZE];
        int n;
        while ((n = is.read(chunk)) > 0) {
            buf.write(chunk, 0, n);
        }
        return buf.toByteArray();
    }

    // ---------- Server-side JSON processing ----------

    /**
     * Parse JSON, modify multiple nested fields, re-serialize.
     * Modifications:
     *   1. counters.in / counters.ok += 1
     *   2. Root: add processed_seq, server_ok
     *   3. events[i].seq += 1
     *   4. events[i].score *= 2.0
     *   5. events[i].user.attrs.credits *= 2.0 (4-level nesting)
     *   6. events[i].custom.product_ids append one element
     *   7. events[i] add server_marker
     */
    static byte[] processRequestJSON(byte[] body, long seq) throws IOException {
        JsonNode rootNode = MAPPER.readTree(body);
        if (!rootNode.isObject()) {
            throw new IOException("root json must be object");
        }
        ObjectNode root = (ObjectNode) rootNode;

        // (1) counters.in / counters.ok increment
        JsonNode countersNode = root.get("counters");
        if (countersNode != null && countersNode.isObject()) {
            ObjectNode counters = (ObjectNode) countersNode;
            if (counters.has("in")) {
                counters.put("in", counters.get("in").asLong() + 1);
            }
            if (counters.has("ok")) {
                counters.put("ok", counters.get("ok").asLong() + 1);
            }
        }

        // (2) Root markers
        root.put("processed_seq", seq);
        root.put("server_ok", true);

        // (3-7) events traversal
        JsonNode eventsNode = root.get("events");
        if (eventsNode != null && eventsNode.isArray()) {
            ArrayNode events = (ArrayNode) eventsNode;
            for (int i = 0; i < events.size(); i++) {
                JsonNode itemNode = events.get(i);
                if (!itemNode.isObject()) continue;
                ObjectNode item = (ObjectNode) itemNode;

                // seq += 1
                if (item.has("seq")) {
                    item.put("seq", item.get("seq").asLong() + 1);
                }

                // score *= 2.0
                if (item.has("score")) {
                    item.put("score", item.get("score").asDouble() * 2.0);
                }

                // user.attrs.credits *= 2.0
                JsonNode userNode = item.get("user");
                if (userNode != null && userNode.isObject()) {
                    JsonNode attrsNode = ((ObjectNode) userNode).get("attrs");
                    if (attrsNode != null && attrsNode.isObject()) {
                        ObjectNode attrs = (ObjectNode) attrsNode;
                        if (attrs.has("credits")) {
                            attrs.put("credits", attrs.get("credits").asDouble() * 2.0);
                        }
                    }
                }

                // custom.product_ids append
                JsonNode customNode = item.get("custom");
                if (customNode != null && customNode.isObject()) {
                    JsonNode idsNode = ((ObjectNode) customNode).get("product_ids");
                    if (idsNode != null && idsNode.isArray()) {
                        ((ArrayNode) idsNode).add(i + 9999);
                    }
                }

                // server_marker
                item.put("server_marker", "m-" + i);
            }
        }

        return MAPPER.writeValueAsBytes(root);
    }

    // ---------- Client-side response validation ----------

    private static void parseServerOK(byte[] body) throws IOException {
        JsonNode root = MAPPER.readTree(body);
        if (!root.isObject()) {
            throw new IOException("response root must be object");
        }
        JsonNode serverOk = root.get("server_ok");
        if (serverOk == null) {
            throw new IOException("response missing server_ok");
        }
        if (!serverOk.asBoolean()) {
            throw new IOException("response server_ok=false");
        }
    }

    // ---------- HTTP client helpers (raw Socket with TCP_NODELAY) ----------

    private record ReqResult(long sentBytes, long recvBytes, boolean success) {}

    /**
     * Send an HTTP/1.1 POST request using raw Socket with TCP_NODELAY.
     * Avoids the JDK HttpClient's Nagle/delayed-ACK interaction (~40ms/req).
     */
    private static ReqResult doRequest(String host, int port, byte[] payload,
                                        long workerId, long slotId, long reqId, long timeoutMs) {
        try (Socket socket = new Socket()) {
            socket.setTcpNoDelay(true);
            socket.connect(new InetSocketAddress(host, port), (int) timeoutMs);
            socket.setSoTimeout((int) timeoutMs);

            OutputStream out = socket.getOutputStream();
            InputStream in = socket.getInputStream();

            // Build HTTP request: headers + body in one contiguous write
            String headerStr = "POST /process HTTP/1.1\r\n"
                    + "Host: " + host + ":" + port + "\r\n"
                    + "Content-Type: application/json\r\n"
                    + "Content-Length: " + payload.length + "\r\n"
                    + "x-case27-client: java-benchmark\r\n"
                    + "x-case27-reqid: " + reqId + "\r\n"
                    + "x-case27-slotid: " + slotId + "\r\n"
                    + "x-case27-workerid: " + workerId + "\r\n"
                    + "Connection: close\r\n\r\n";
            byte[] headerBytes = headerStr.getBytes(StandardCharsets.UTF_8);
            byte[] request = new byte[headerBytes.length + payload.length];
            System.arraycopy(headerBytes, 0, request, 0, headerBytes.length);
            System.arraycopy(payload, 0, request, headerBytes.length, payload.length);
            out.write(request);
            out.flush();

            // Parse HTTP response
            String statusLine = readLine(in);
            if (statusLine == null || statusLine.isEmpty()) {
                System.out.printf("[CASE27-JAVA][client-fail] worker=%d slotId=%d reqId=%d err=empty status%n",
                        workerId, slotId, reqId);
                return new ReqResult(payload.length, 0, false);
            }
            String[] parts = statusLine.split(" ");
            int statusCode = Integer.parseInt(parts[1]);

            // Read headers to find Content-Length
            int contentLength = -1;
            String line;
            while (!(line = readLine(in)).isEmpty()) {
                if (line.regionMatches(true, 0, "content-length:", 0, 15)) {
                    contentLength = Integer.parseInt(line.substring(15).trim());
                }
            }

            // Read response body
            byte[] body;
            if (contentLength > 0) {
                body = new byte[contentLength];
                int offset = 0;
                while (offset < contentLength) {
                    int n = in.read(body, offset, contentLength - offset);
                    if (n < 0) break;
                    offset += n;
                }
            } else {
                body = readToEnd(in);
            }

            if (statusCode != 200) {
                System.out.printf("[CASE27-JAVA][client-fail] worker=%d slotId=%d reqId=%d err=status %d%n",
                        workerId, slotId, reqId, statusCode);
                return new ReqResult(payload.length, body.length, false);
            }

            parseServerOK(body);
            return new ReqResult(payload.length, body.length, true);
        } catch (Exception e) {
            System.out.printf("[CASE27-JAVA][client-fail] worker=%d slotId=%d reqId=%d err=%s%n",
                    workerId, slotId, reqId, e.getMessage());
            return new ReqResult(payload.length, 0, false);
        }
    }

    /** Read a CRLF-terminated line from input stream */
    private static String readLine(InputStream in) throws IOException {
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        int prev = -1;
        int ch;
        while ((ch = in.read()) != -1) {
            if (prev == '\r' && ch == '\n') {
                // Remove the trailing \r from buf
                byte[] bytes = buf.toByteArray();
                return new String(bytes, 0, bytes.length - 1, StandardCharsets.UTF_8);
            }
            buf.write(ch);
            prev = ch;
        }
        if (buf.size() > 0) {
            return buf.toString(StandardCharsets.UTF_8);
        }
        return null;
    }

    /** Read remaining bytes until EOF */
    private static byte[] readToEnd(InputStream in) throws IOException {
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        byte[] chunk = new byte[BODY_CHUNK_SIZE];
        int n;
        while ((n = in.read(chunk)) > 0) {
            buf.write(chunk, 0, n);
        }
        return buf.toByteArray();
    }

    // ---------- HTTP Server ----------

    private static HttpServer startHTTPServer(BenchConfig cfg, AtomicLong serverSeq) throws IOException {
        HttpServer server = HttpServer.create(new InetSocketAddress(cfg.serverAddr(), cfg.serverPort()), 0);
        server.createContext("/process", exchange -> {
            try {
                byte[] bodyBytes = readAllBytes(exchange.getRequestBody());
                byte[] respBytes = processRequestJSON(bodyBytes, serverSeq.getAndIncrement());
                exchange.getResponseHeaders().set("content-type", "application/json");
                exchange.sendResponseHeaders(200, respBytes.length);
                try (OutputStream os = exchange.getResponseBody()) {
                    os.write(respBytes);
                }
            } catch (Exception e) {
                System.out.printf("[CASE27-JAVA][server-fail] reqId=%s slotId=%s err=%s%n",
                        exchange.getRequestHeaders().getFirst("x-case27-reqid"),
                        exchange.getRequestHeaders().getFirst("x-case27-slotid"),
                        e.getMessage());
                try {
                    byte[] errMsg = "process request failed".getBytes();
                    exchange.sendResponseHeaders(400, errMsg.length);
                    try (OutputStream os = exchange.getResponseBody()) {
                        os.write(errMsg);
                    }
                } catch (IOException ignored) {}
            }
        });
        server.setExecutor(Executors.newVirtualThreadPerTaskExecutor());
        server.start();
        return server;
    }

    // ---------- Warmup ----------

    private static void warmup(String host, int port, byte[] payload, long timeoutMs) {
        System.out.println("[INFO] Warmup: " + WARMUP_REQUESTS + " sequential requests...");
        long start = System.currentTimeMillis();
        for (int i = 0; i < WARMUP_REQUESTS; i++) {
            ReqResult r = doRequest(host, port, payload, -1, -1, i, timeoutMs);
            if (!r.success()) {
                System.out.println("[WARN] warmup request failed");
            }
        }
        System.out.println("[INFO] Warmup done in " + (System.currentTimeMillis() - start) + "ms");
    }

    // ---------- Sync load runner ----------

    private static RunStats runSyncLoad(BenchConfig cfg, byte[] payload,
                                         AtomicBoolean stop, AtomicStats stats) throws InterruptedException {
        AtomicLong nextReqId = new AtomicLong(1);
        long start = System.currentTimeMillis();
        AtomicBoolean done = new AtomicBoolean(false);

        // Progress reporter with proper delta tracking
        ScheduledExecutorService progressScheduler = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "progress");
            t.setDaemon(true);
            return t;
        });
        AtomicLong lastTotal = new AtomicLong(0);
        progressScheduler.scheduleAtFixedRate(() -> {
            if (done.get()) return;
            long now = stats.total.get();
            long delta = now - lastTotal.getAndSet(now);
            long elapsed = System.currentTimeMillis() - start;
            System.out.printf("[PROGRESS] t=%dms total=%d ok=%d fail=%d inflight=%d +last2s=%d%n",
                    elapsed, now, stats.ok.get(), stats.fail.get(), stats.inflight.get(), delta);
        }, PROGRESS_INTERVAL_MS, PROGRESS_INTERVAL_MS, TimeUnit.MILLISECONDS);

        Thread[] workers = new Thread[cfg.concurrency()];
        for (int w = 0; w < cfg.concurrency(); w++) {
            final long workerId = w;
            workers[w] = Thread.ofVirtual().start(() -> {
                while (!stop.get()) {
                    stats.inflight.incrementAndGet();
                    long reqId = nextReqId.getAndIncrement();
                    ReqResult r = doRequest(cfg.serverAddr(), cfg.serverPort(), payload, workerId, -1, reqId, cfg.clientTimeoutMs());
                    stats.total.incrementAndGet();
                    stats.bytesSent.addAndGet(r.sentBytes());
                    stats.bytesRecv.addAndGet(r.recvBytes());
                    stats.inflight.decrementAndGet();
                    if (r.success()) {
                        stats.ok.incrementAndGet();
                    } else {
                        stats.fail.incrementAndGet();
                    }
                }
            });
        }

        for (Thread w : workers) {
            w.join();
        }
        done.set(true);
        progressScheduler.shutdown();
        return stats.snapshot();
    }

    // ---------- Async load runner ----------

    static class AsyncSlot {
        final AtomicBoolean busy = new AtomicBoolean(false);
        final long id;

        AsyncSlot(long id) {
            this.id = id;
        }
    }

    private static AsyncSlot acquireAsyncSlot(AsyncSlot[] slots) {
        for (AsyncSlot slot : slots) {
            if (slot.busy.compareAndSet(false, true)) {
                return slot;
            }
        }
        return null;
    }

    private static RunStats runAsyncLoad(BenchConfig cfg, byte[] payload,
                                          AtomicBoolean stop, AtomicStats stats) throws InterruptedException {
        AtomicLong nextReqId = new AtomicLong(1);
        long start = System.currentTimeMillis();
        AtomicBoolean done = new AtomicBoolean(false);

        // Progress reporter
        ScheduledExecutorService progressScheduler = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "progress");
            t.setDaemon(true);
            return t;
        });
        AtomicLong lastTotal = new AtomicLong(0);
        progressScheduler.scheduleAtFixedRate(() -> {
            if (done.get()) return;
            long now = stats.total.get();
            long delta = now - lastTotal.getAndSet(now);
            long elapsed = System.currentTimeMillis() - start;
            System.out.printf("[PROGRESS] t=%dms total=%d ok=%d fail=%d inflight=%d +last2s=%d%n",
                    elapsed, now, stats.ok.get(), stats.fail.get(), stats.inflight.get(), delta);
        }, PROGRESS_INTERVAL_MS, PROGRESS_INTERVAL_MS, TimeUnit.MILLISECONDS);

        AsyncSlot[] slots = new AsyncSlot[cfg.maxInflight()];
        for (int i = 0; i < cfg.maxInflight(); i++) {
            slots[i] = new AsyncSlot(i);
        }

        while (!stop.get()) {
            AsyncSlot slot = acquireAsyncSlot(slots);
            if (slot != null) {
                long reqId = nextReqId.getAndIncrement();
                stats.inflight.incrementAndGet();
                Thread.ofVirtual().start(() -> {
                    try {
                        ReqResult r = doRequest(cfg.serverAddr(), cfg.serverPort(), payload, 0, slot.id, reqId, cfg.clientTimeoutMs());
                        stats.total.incrementAndGet();
                        stats.bytesSent.addAndGet(r.sentBytes());
                        stats.bytesRecv.addAndGet(r.recvBytes());
                        if (r.success()) {
                            stats.ok.incrementAndGet();
                        } else {
                            stats.fail.incrementAndGet();
                        }
                    } finally {
                        stats.inflight.decrementAndGet();
                        slot.busy.set(false);
                    }
                });
            }
            if (cfg.sendIntervalMs() > 0) {
                try {
                    Thread.sleep(cfg.sendIntervalMs());
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
        }

        // Wait for in-flight requests to drain
        long settleStart = System.currentTimeMillis();
        long settleTimeout = cfg.clientTimeoutMs() + 2_000;
        while (stats.inflight.get() > 0
                && (System.currentTimeMillis() - settleStart) < settleTimeout) {
            Thread.sleep(100);
        }
        done.set(true);
        progressScheduler.shutdown();
        return stats.snapshot();
    }

    // ---------- Summary ----------

    private static void printSummary(BenchConfig cfg, RunStats stats, long totalMs) {
        long throughput = totalMs > 0 ? stats.ok() * 1000 / totalMs : 0;
        System.out.printf("[RESULT] payload=%s concurrency=%d duration=%dms client_mode=%s%n",
                cfg.payloadName(), cfg.concurrency(), totalMs, cfg.clientMode());
        System.out.printf("[RESULT] total=%d ok=%d fail=%d inflight=%d throughput=%dreq/s%n",
                stats.total(), stats.ok(), stats.fail(), stats.inflight(), throughput);
        System.out.printf("[RESULT] bytes_sent=%dMB bytes_recv=%dMB%n",
                stats.bytesSent() / 1024 / 1024, stats.bytesRecv() / 1024 / 1024);
        System.out.println("[DONE]");
    }

    // ---------- Main ----------

    public static void main(String[] args) throws Exception {
        System.out.println("[INFO] Case27 Java HTTP Complex-JSON Benchmark");

        BenchConfig cfg = loadConfigFromEnv();

        System.out.println("[CONFIG] payload=" + cfg.payloadName()
                + " concurrency=" + cfg.concurrency()
                + " duration_ms=" + cfg.durationMs());
        System.out.println("[CONFIG] payload_dir=" + cfg.payloadDir()
                + " server_port=" + cfg.serverPort());
        System.out.println("[CONFIG] client_timeout_ms=" + cfg.clientTimeoutMs());
        System.out.println("[CONFIG] run_forever=" + cfg.runForever());
        System.out.println("[CONFIG] role=" + cfg.role());
        System.out.println("[CONFIG] client_mode=" + cfg.clientMode());
        System.out.println("[CONFIG] send_interval_ms=" + cfg.sendIntervalMs()
                + " max_inflight=" + cfg.maxInflight());
        System.out.println("[CONFIG] persistent_bytes=" + cfg.persistentBytes()
                + " persistent_workers=" + cfg.persistentWorkers());

        PersistentBaseline baseline = PersistentBaseline.start(
                cfg.persistentBytes(), cfg.persistentWorkers());
        PersistentStats persistentStats = baseline.stats();
        System.out.println("[INFO] Persistent heap baseline ready: total="
                + persistentStats.totalBytes + " static=" + persistentStats.staticBytes
                + " stack=" + persistentStats.stackBytes);
        try {

            // Start HTTP Server
            AtomicLong serverSeq = new AtomicLong(0);
            HttpServer server = startHTTPServer(cfg, serverSeq);
            Thread.sleep(1000); // let server start
            String url = serverURL(cfg);
            System.out.println("[INFO] HTTP Server ready at " + url);

            if (cfg.role().equals("server")) {
                System.out.println("[INFO] Running in server-only mode; waiting for external clients...");
                while (true) {
                    Thread.sleep(60_000);
                }
            }

            // Load payload
            byte[] payload = loadPayload(cfg);

            // Warmup
            warmup(cfg.serverAddr(), cfg.serverPort(), payload, cfg.clientTimeoutMs());

            // Start workers
            AtomicStats stats = new AtomicStats();
            AtomicBoolean stop = new AtomicBoolean(false);
            long t0 = System.currentTimeMillis();

            if (cfg.clientMode().equals("async")) {
                if (cfg.runForever()) {
                    System.out.println("[INFO] Started async sender in async mode; running without time limit...");
                } else {
                    System.out.println("[INFO] Started async sender in async mode; running for " + cfg.durationMs() + "ms...");
                }
            } else {
                if (cfg.runForever()) {
                    System.out.println("[INFO] Started " + cfg.concurrency() + " workers in sync mode; running without time limit...");
                } else {
                    System.out.println("[INFO] Started " + cfg.concurrency() + " workers in sync mode; running for " + cfg.durationMs() + "ms...");
                }
            }

            // Run benchmark in background thread
            final RunStats[] resultHolder = new RunStats[1];
            Thread benchThread = Thread.ofVirtual().start(() -> {
                try {
                    if (cfg.clientMode().equals("async")) {
                        resultHolder[0] = runAsyncLoad(cfg, payload, stop, stats);
                    } else {
                        resultHolder[0] = runSyncLoad(cfg, payload, stop, stats);
                    }
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            });

            // Main thread: wait for duration, then signal stop
            if (cfg.runForever()) {
                // Run until externally killed (e.g. SIGINT, SIGTERM)
                while (true) {
                    Thread.sleep(60_000);
                }
            } else {
                long elapsed = System.currentTimeMillis() - t0;
                long remaining = cfg.durationMs() - elapsed;
                while (remaining > 0) {
                    Thread.sleep(Math.min(PROGRESS_INTERVAL_MS, remaining));
                    remaining = cfg.durationMs() - (System.currentTimeMillis() - t0);
                }
            }
            stop.set(true);

            // Wait for bench thread with settle time for in-flight requests
            benchThread.join(cfg.clientTimeoutMs() + 2_000);

            long totalMs = System.currentTimeMillis() - t0;
            RunStats finalStats = resultHolder[0] != null ? resultHolder[0] : stats.snapshot();
            printSummary(cfg, finalStats, totalMs);

            server.stop(1);
        } finally {
            baseline.close();
        }
    }
}
