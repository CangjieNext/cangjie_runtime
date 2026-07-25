import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpHandler;
import com.sun.net.httpserver.HttpServer;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Case27 HTTP complex-JSON benchmark.
 *
 * The implementation intentionally uses only Java 8 standard-library APIs so
 * the benchmark can run in the same minimal environment as the existing sides.
 */
public final class Case27HttpComplexJsonBenchmark {
    private static final String SERVER_ADDR = "127.0.0.1";
    private static final int DEFAULT_PORT = 18125;
    private static final int DEFAULT_TIMEOUT_MS = 5000;
    private static final int DEFAULT_WARMUP_REQUESTS = 20;
    private static final long PROGRESS_INTERVAL_MS = 2000L;

    private Case27HttpComplexJsonBenchmark() {
    }

    public static void main(String[] args) {
        try {
            Config config = Config.fromEnvironment();
            System.out.println("[INFO] Case27 Java HTTP Complex-JSON Benchmark");
            System.out.println("[CONFIG] payload=" + config.payloadName
                    + " concurrency=" + config.concurrency
                    + " duration_ms=" + config.durationMs);
            System.out.println("[CONFIG] payload_dir=" + config.payloadDir
                    + " server_port=" + config.serverPort);
            System.out.println("[CONFIG] client_timeout_ms=" + config.clientTimeoutMs);
            System.out.println("[CONFIG] run_forever=" + config.runForever);
            System.out.println("[CONFIG] role=" + config.role);
            System.out.println("[CONFIG] client_mode=" + config.clientMode);
            System.out.println("[CONFIG] send_interval_ms=" + config.sendIntervalMs
                    + " max_inflight=" + config.maxInflight);

            Benchmark benchmark = new Benchmark(config);
            if ("server".equals(config.role)) {
                benchmark.runServerOnly();
            } else {
                benchmark.runBoth();
            }
        } catch (Exception exception) {
            System.out.println("[ERROR] " + exception.getMessage());
            exception.printStackTrace(System.out);
            System.exit(1);
        }
    }

    static byte[] processRequestJson(byte[] body, long sequence) throws IOException {
        String bodyText = new String(body, StandardCharsets.UTF_8);
        JsonValue value = JsonParser.parse(bodyText);
        if (!(value instanceof JsonObject)) {
            throw new IOException("root json must be object");
        }

        JsonObject root = (JsonObject) value;
        JsonValue countersValue = root.get("counters");
        if (countersValue != null) {
            JsonObject counters = requireObject(countersValue, "counters");
            JsonValue inValue = counters.get("in");
            counters.put("in", JsonNumber.integer(requireInteger(inValue, "counters.in").longValue() + 1L));
            JsonValue okValue = counters.get("ok");
            counters.put("ok", JsonNumber.integer(requireInteger(okValue, "counters.ok").longValue() + 1L));
        }

        root.put("processed_seq", JsonNumber.integer(sequence));
        root.put("server_ok", JsonBoolean.TRUE);

        JsonValue eventsValue = root.get("events");
        if (eventsValue != null) {
            JsonArray events = requireArray(eventsValue, "events");
            for (int index = 0; index < events.values.size(); index++) {
                JsonValue eventValue = events.values.get(index);
                String eventPath = "events[" + index + "]";
                JsonObject event = requireObject(eventValue, eventPath);

                JsonValue sequenceValue = event.get("seq");
                if (sequenceValue != null) {
                    event.put("seq", JsonNumber.integer(
                            requireInteger(sequenceValue, eventPath + ".seq").longValue() + 1L));
                }
                JsonValue scoreValue = event.get("score");
                if (scoreValue != null) {
                    event.put("score", JsonNumber.decimal(
                            requireFloat(scoreValue, eventPath + ".score").doubleValue() * 2.0));
                }

                JsonValue userValue = event.get("user");
                if (userValue != null) {
                    JsonObject user = requireObject(userValue, eventPath + ".user");
                    JsonValue attrsValue = user.get("attrs");
                    if (attrsValue != null) {
                        JsonObject attrs = requireObject(attrsValue, eventPath + ".user.attrs");
                        JsonValue creditsValue = attrs.get("credits");
                        if (creditsValue != null) {
                            attrs.put(
                                    "credits",
                                    JsonNumber.decimal(requireFloat(
                                            creditsValue, eventPath + ".user.attrs.credits").doubleValue() * 2.0));
                        }
                    }
                }

                JsonValue customValue = event.get("custom");
                if (customValue != null) {
                    JsonObject custom = requireObject(customValue, eventPath + ".custom");
                    JsonValue productIdsValue = custom.get("product_ids");
                    if (productIdsValue != null) {
                        requireArray(productIdsValue, eventPath + ".custom.product_ids")
                                .values.add(JsonNumber.integer(index + 9999L));
                    }
                }
                event.put("server_marker", new JsonString("m-" + index));
            }
        }

        return JsonWriter.write(root).getBytes(StandardCharsets.UTF_8);
    }

    private static JsonObject requireObject(JsonValue value, String path) throws IOException {
        if (!(value instanceof JsonObject)) {
            throw new IOException(path + " must be an object");
        }
        return (JsonObject) value;
    }

    private static JsonArray requireArray(JsonValue value, String path) throws IOException {
        if (!(value instanceof JsonArray)) {
            throw new IOException(path + " must be an array");
        }
        return (JsonArray) value;
    }

    private static JsonNumber requireInteger(JsonValue value, String path) throws IOException {
        if (!(value instanceof JsonNumber) || !((JsonNumber) value).isInteger()) {
            throw new IOException(path + " must be an integer");
        }
        return (JsonNumber) value;
    }

    private static JsonNumber requireFloat(JsonValue value, String path) throws IOException {
        if (!(value instanceof JsonNumber)) {
            throw new IOException(path + " must be a number");
        }
        return (JsonNumber) value;
    }

    private static boolean parseServerOk(byte[] body) throws IOException {
        JsonValue value = JsonParser.parse(new String(body, StandardCharsets.UTF_8));
        if (!(value instanceof JsonObject)) {
            throw new IOException("response root json must be object");
        }
        JsonValue serverOk = ((JsonObject) value).get("server_ok");
        if (!(serverOk instanceof JsonBoolean) || !((JsonBoolean) serverOk).value) {
            throw new IOException("response missing server_ok=true");
        }
        return true;
    }

    private static byte[] readAll(InputStream input) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = input.read(buffer)) >= 0) {
            if (count > 0) {
                output.write(buffer, 0, count);
            }
        }
        return output.toByteArray();
    }

    private static void sleepMillis(long millis) throws InterruptedException {
        if (millis > 0) {
            Thread.sleep(millis);
        }
    }

    private static final class Config {
        private final String payloadName;
        private final int concurrency;
        private final long durationMs;
        private final boolean runForever;
        private final String payloadDir;
        private final int serverPort;
        private final int clientTimeoutMs;
        private final String role;
        private final String clientMode;
        private final long sendIntervalMs;
        private final int maxInflight;

        private Config(String payloadName, int concurrency, long durationMs, String payloadDir,
                        int serverPort, int clientTimeoutMs, String role, String clientMode,
                        long sendIntervalMs, int maxInflight) {
            this.payloadName = payloadName;
            this.concurrency = concurrency;
            this.durationMs = durationMs;
            this.runForever = durationMs <= 0;
            this.payloadDir = payloadDir;
            this.serverPort = serverPort;
            this.clientTimeoutMs = clientTimeoutMs;
            this.role = role;
            this.clientMode = clientMode;
            this.sendIntervalMs = sendIntervalMs;
            this.maxInflight = maxInflight;
        }

        private static Config fromEnvironment() {
            String payloadName = envString("CASE27_PAYLOAD", "10K");
            int concurrency = envInt("CASE27_CONCURRENCY", 8);
            long durationMs = envLong("CASE27_DURATION_MS", 60000L);
            String payloadDir = envString("CASE27_PAYLOAD_DIR", "./payloads");
            int serverPort = envInt("CASE27_SERVER_PORT", DEFAULT_PORT);
            int timeoutMs = envInt("CASE27_CLIENT_TIMEOUT_MS", DEFAULT_TIMEOUT_MS);
            String role = envString("CASE27_ROLE", "both");
            String clientMode = envString("CASE27_CLIENT_MODE", "sync");
            long sendIntervalMs = envLong("CASE27_SEND_INTERVAL_MS", 1L);
            int maxInflight = envInt("CASE27_MAX_INFLIGHT", 32);

            if (concurrency <= 0) {
                throw new IllegalArgumentException("CASE27_CONCURRENCY must be > 0");
            }
            if (timeoutMs <= 0) {
                throw new IllegalArgumentException("CASE27_CLIENT_TIMEOUT_MS must be > 0");
            }
            if (!"both".equals(role) && !"server".equals(role)) {
                throw new IllegalArgumentException(
                        "unsupported CASE27_ROLE=" + role + "; expected both or server");
            }
            if (!"sync".equals(clientMode) && !"async".equals(clientMode)) {
                throw new IllegalArgumentException(
                        "unsupported CASE27_CLIENT_MODE=" + clientMode + "; expected sync or async");
            }
            if (sendIntervalMs < 0) {
                throw new IllegalArgumentException("CASE27_SEND_INTERVAL_MS must be >= 0");
            }
            if (maxInflight <= 0) {
                throw new IllegalArgumentException("CASE27_MAX_INFLIGHT must be > 0");
            }
            if (serverPort <= 0 || serverPort > 65535) {
                throw new IllegalArgumentException("CASE27_SERVER_PORT must be in 1..65535");
            }
            return new Config(payloadName, concurrency, durationMs, payloadDir, serverPort,
                    timeoutMs, role, clientMode, sendIntervalMs, maxInflight);
        }
    }

    private static final class Benchmark {
        private final Config config;
        private final AtomicLong serverSequence = new AtomicLong(0L);
        private HttpServer server;
        private ExecutorService serverExecutor;

        private Benchmark(Config config) {
            this.config = config;
        }

        private void startServer() throws IOException {
            server = HttpServer.create(new InetSocketAddress(SERVER_ADDR, config.serverPort), 0);
            server.createContext("/process", new ProcessHandler());
            int serverThreads = Math.max(4,
                    "async".equals(config.clientMode) ? config.maxInflight : config.concurrency);
            serverExecutor = Executors.newFixedThreadPool(serverThreads);
            server.setExecutor(serverExecutor);
            server.start();
        }

        private void stopServer() {
            if (server != null) {
                server.stop(0);
            }
            if (serverExecutor != null) {
                serverExecutor.shutdownNow();
            }
        }

        private void runServerOnly() throws IOException, InterruptedException {
            startServer();
            System.out.println("[INFO] HTTP Server ready at http://" + SERVER_ADDR + ":"
                    + config.serverPort + "/process");
            System.out.println("[INFO] Running in server-only mode; waiting for external clients...");
            try {
                new CountDownLatch(1).await();
            } finally {
                stopServer();
            }
        }

        private void runBoth() throws Exception {
            byte[] payload;
            String payloadPath = Paths.get(config.payloadDir, config.payloadName + ".json").toString();
            if (!Files.exists(Paths.get(payloadPath))) {
                throw new IOException("payload file not found: " + payloadPath);
            }
            payload = Files.readAllBytes(Paths.get(payloadPath));
            System.out.println("[INFO] loaded payload bytes=" + payload.length + " ("
                    + payload.length / 1024 + "KB) from " + payloadPath);

            startServer();
            sleepMillis(1000L);
            String url = "http://" + SERVER_ADDR + ":" + config.serverPort + "/process";
            System.out.println("[INFO] HTTP Server ready at " + url);
            warmup(url, payload);

            Stats stats = new Stats();
            AtomicBoolean stop = new AtomicBoolean(false);
            AtomicLong nextRequestId = new AtomicLong(1L);
            long startNanos = System.nanoTime();
            ExecutorService workerExecutor = null;
            ExecutorService requestExecutor = null;
            Thread asyncSender = null;
            try {
                if ("sync".equals(config.clientMode)) {
                    workerExecutor = Executors.newFixedThreadPool(config.concurrency);
                    for (int workerId = 0; workerId < config.concurrency; workerId++) {
                        final int id = workerId;
                        workerExecutor.submit(new Runnable() {
                            @Override
                            public void run() {
                                syncWorkerLoop(id, url, payload, stop, stats, nextRequestId);
                            }
                        });
                    }
                    System.out.println(startMessage(config, false));
                } else {
                    requestExecutor = Executors.newFixedThreadPool(config.maxInflight);
                    final ExecutorService requests = requestExecutor;
                    asyncSender = new Thread(new Runnable() {
                        @Override
                        public void run() {
                            asyncSenderLoop(url, payload, stop, stats, nextRequestId, requests);
                        }
                    }, "case27-java-async-sender");
                    asyncSender.start();
                    System.out.println(startMessage(config, true));
                }

                printProgressUntilDone(startNanos, stop, stats);
            } finally {
                stop.set(true);
                if (asyncSender != null) {
                    asyncSender.interrupt();
                    asyncSender.join(config.clientTimeoutMs + 2000L);
                }
                shutdownExecutor(workerExecutor, config.clientTimeoutMs + 2000L);
                shutdownExecutor(requestExecutor, config.clientTimeoutMs + 2000L);
            }

            long totalMs = Math.max(1L, TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - startNanos));
            long total = stats.total.get();
            long ok = stats.ok.get();
            long fail = stats.fail.get();
            long throughput = total * 1000L / totalMs;
            long sentMb = stats.bytesSent.get() / 1024L / 1024L;
            long recvMb = stats.bytesReceived.get() / 1024L / 1024L;
            System.out.println("[RESULT] payload=" + config.payloadName
                    + " concurrency=" + config.concurrency
                    + " duration=" + totalMs + "ms client_mode=" + config.clientMode);
            System.out.println("[RESULT] total=" + total + " ok=" + ok + " fail=" + fail
                    + " inflight=" + stats.inflight.get() + " throughput=" + throughput + "req/s");
            System.out.println("[RESULT] bytes_sent=" + sentMb + "MB bytes_recv=" + recvMb + "MB");
            System.out.println("[DONE]");
            stopServer();
        }

        private void warmup(String url, byte[] payload) {
            System.out.println("[INFO] Warmup: " + DEFAULT_WARMUP_REQUESTS + " sequential requests...");
            long start = System.nanoTime();
            for (int index = 0; index < DEFAULT_WARMUP_REQUESTS; index++) {
                try {
                    doRequest(url, payload, -1, -1, index);
                } catch (Exception exception) {
                    System.out.println("[WARN] warmup request failed: " + exception.getMessage());
                }
            }
            long elapsed = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - start);
            System.out.println("[INFO] Warmup done in " + elapsed + "ms");
        }

        private void syncWorkerLoop(int workerId, String url, byte[] payload, AtomicBoolean stop,
                                    Stats stats, AtomicLong nextRequestId) {
            while (!stop.get() && !Thread.currentThread().isInterrupted()) {
                long requestId = nextRequestId.getAndIncrement();
                executeRequest(url, payload, workerId, -1, requestId, stop, stats);
            }
        }

        private void asyncSenderLoop(String url, byte[] payload, AtomicBoolean stop, Stats stats,
                                     AtomicLong nextRequestId, ExecutorService requests) {
            Semaphore slots = new Semaphore(config.maxInflight);
            long slotSequence = 0L;
            while (!stop.get() && !Thread.currentThread().isInterrupted()) {
                if (slots.tryAcquire()) {
                    final long requestId = nextRequestId.getAndIncrement();
                    final int slotId = requestSlotId(slotSequence++, config.maxInflight);
                    try {
                        requests.submit(new Runnable() {
                            @Override
                            public void run() {
                                try {
                                    executeRequest(url, payload, 0, slotId, requestId, stop, stats);
                                } finally {
                                    slots.release();
                                }
                            }
                        });
                    } catch (RuntimeException exception) {
                        slots.release();
                        if (!stop.get()) {
                            System.out.println("[CASE27-JAVA][client-fail] worker=0 slotId="
                                    + slotId + " reqId=" + requestId + " err=" + exception.getMessage());
                        }
                        break;
                    }
                }
                try {
                    sleepMillis(config.sendIntervalMs);
                } catch (InterruptedException exception) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
        }

        private void executeRequest(String url, byte[] payload, int workerId, int slotId,
                                    long requestId, AtomicBoolean stop, Stats stats) {
            stats.inflight.incrementAndGet();
            try {
                RequestResult result = doRequest(url, payload, workerId, slotId, requestId);
                stats.bytesSent.addAndGet(result.sentBytes);
                stats.bytesReceived.addAndGet(result.receivedBytes);
                stats.ok.incrementAndGet();
            } catch (Exception exception) {
                stats.fail.incrementAndGet();
                System.out.println("[CASE27-JAVA][client-fail] worker=" + workerId
                        + " slotId=" + slotId + " reqId=" + requestId
                        + " err=" + exception.getMessage());
            } finally {
                stats.total.incrementAndGet();
                stats.inflight.decrementAndGet();
            }
        }

        private RequestResult doRequest(String url, byte[] payload, int workerId,
                                        int slotId, long requestId) throws IOException {
            HttpURLConnection connection = null;
            try {
                connection = (HttpURLConnection) new URL(url).openConnection();
                connection.setConnectTimeout(config.clientTimeoutMs);
                connection.setReadTimeout(config.clientTimeoutMs);
                connection.setRequestMethod("POST");
                connection.setDoOutput(true);
                connection.setUseCaches(false);
                connection.setFixedLengthStreamingMode(payload.length);
                connection.setRequestProperty("content-type", "application/json");
                connection.setRequestProperty("x-case27-client", "java-benchmark");
                connection.setRequestProperty("x-case27-reqid", Long.toString(requestId));
                connection.setRequestProperty("x-case27-slotid", Integer.toString(slotId));
                connection.setRequestProperty("x-case27-workerid", Integer.toString(workerId));
                OutputStream output = connection.getOutputStream();
                try {
                    output.write(payload);
                } finally {
                    output.close();
                }

                int status = connection.getResponseCode();
                InputStream input = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
                byte[] response = input == null ? new byte[0] : readAll(input);
                if (input != null) {
                    input.close();
                }
                if (status != HttpURLConnection.HTTP_OK) {
                    throw new IOException("unexpected status: " + status);
                }
                parseServerOk(response);
                return new RequestResult(payload.length, response.length);
            } finally {
                if (connection != null) {
                    connection.disconnect();
                }
            }
        }

        private void printProgressUntilDone(long startNanos, AtomicBoolean stop, Stats stats)
                throws InterruptedException {
            long lastTotal = 0L;
            while (config.runForever || elapsedMillis(startNanos) < config.durationMs) {
                long sleepMs = PROGRESS_INTERVAL_MS;
                if (!config.runForever) {
                    sleepMs = Math.min(sleepMs, Math.max(1L, config.durationMs - elapsedMillis(startNanos)));
                }
                sleepMillis(sleepMs);
                long total = stats.total.get();
                System.out.println("[PROGRESS] t=" + elapsedMillis(startNanos)
                        + "ms total=" + total
                        + " ok=" + stats.ok.get()
                        + " fail=" + stats.fail.get()
                        + " inflight=" + stats.inflight.get()
                        + " +last2s=" + (total - lastTotal));
                lastTotal = total;
                if (!config.runForever && elapsedMillis(startNanos) >= config.durationMs) {
                    break;
                }
            }
            stop.set(true);
        }

        private static long elapsedMillis(long startNanos) {
            return TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - startNanos);
        }

        private static String startMessage(Config config, boolean async) {
            String mode = async ? "async sender" : config.concurrency + " workers";
            String duration = config.runForever ? "without time limit" : "for " + config.durationMs + "ms";
            return "[INFO] Started " + mode + " in " + config.clientMode + " mode; running " + duration + "...";
        }

        private static int requestSlotId(long requestIndex, int maxInflight) {
            return (int) (requestIndex % maxInflight);
        }

        private static void shutdownExecutor(ExecutorService executor, long waitMs) {
            if (executor == null) {
                return;
            }
            executor.shutdown();
            try {
                if (!executor.awaitTermination(waitMs, TimeUnit.MILLISECONDS)) {
                    executor.shutdownNow();
                    executor.awaitTermination(waitMs, TimeUnit.MILLISECONDS);
                }
            } catch (InterruptedException exception) {
                executor.shutdownNow();
                Thread.currentThread().interrupt();
            }
        }

        private final class ProcessHandler implements HttpHandler {
            @Override
            public void handle(HttpExchange exchange) throws IOException {
                try {
                    byte[] body = readAll(exchange.getRequestBody());
                    long sequence = serverSequence.getAndIncrement();
                    byte[] response;
                    try {
                        response = processRequestJson(body, sequence);
                    } catch (Exception exception) {
                        String requestId = exchange.getRequestHeaders().getFirst("x-case27-reqid");
                        String slotId = exchange.getRequestHeaders().getFirst("x-case27-slotid");
                        System.out.println("[CASE27-JAVA][server-fail] reqId=" + requestId
                                + " slotId=" + slotId + " err=" + exception.getMessage());
                        sendResponse(exchange, 400, "process request failed".getBytes(StandardCharsets.UTF_8),
                                "text/plain");
                        return;
                    }
                    sendResponse(exchange, 200, response, "application/json");
                } finally {
                    exchange.close();
                }
            }

            private void sendResponse(HttpExchange exchange, int status, byte[] body, String contentType)
                    throws IOException {
                exchange.getResponseHeaders().set("content-type", contentType);
                exchange.sendResponseHeaders(status, body.length);
                OutputStream output = exchange.getResponseBody();
                try {
                    output.write(body);
                } finally {
                    output.close();
                }
            }
        }
    }

    private static final class Stats {
        private final AtomicLong total = new AtomicLong(0L);
        private final AtomicLong ok = new AtomicLong(0L);
        private final AtomicLong fail = new AtomicLong(0L);
        private final AtomicLong bytesSent = new AtomicLong(0L);
        private final AtomicLong bytesReceived = new AtomicLong(0L);
        private final AtomicLong inflight = new AtomicLong(0L);
    }

    private static final class RequestResult {
        private final long sentBytes;
        private final long receivedBytes;

        private RequestResult(long sentBytes, long receivedBytes) {
            this.sentBytes = sentBytes;
            this.receivedBytes = receivedBytes;
        }
    }

    private interface JsonValue {
    }

    private static final class JsonObject implements JsonValue {
        private final LinkedHashMap<String, JsonValue> fields = new LinkedHashMap<String, JsonValue>();

        private JsonValue get(String key) {
            return fields.get(key);
        }

        private void put(String key, JsonValue value) {
            fields.put(key, value);
        }
    }

    private static final class JsonArray implements JsonValue {
        private final List<JsonValue> values = new ArrayList<JsonValue>();
    }

    private static final class JsonString implements JsonValue {
        private final String value;

        private JsonString(String value) {
            this.value = value;
        }
    }

    private static final class JsonNumber implements JsonValue {
        private final String raw;
        private final boolean integer;

        private JsonNumber(String raw, boolean integer) {
            this.raw = raw;
            this.integer = integer;
        }

        private static JsonNumber integer(long value) {
            return new JsonNumber(Long.toString(value), true);
        }

        private static JsonNumber decimal(double value) {
            if (Double.isNaN(value) || Double.isInfinite(value)) {
                throw new IllegalArgumentException("non-finite JSON number");
            }
            return new JsonNumber(Double.toString(value), false);
        }

        private boolean isInteger() {
            return integer;
        }

        private long longValue() throws IOException {
            try {
                return Long.parseLong(raw);
            } catch (NumberFormatException exception) {
                throw new IOException("integer out of range: " + raw);
            }
        }

        private double doubleValue() {
            return Double.parseDouble(raw);
        }
    }

    private static final class JsonBoolean implements JsonValue {
        private static final JsonBoolean TRUE = new JsonBoolean(true);
        private static final JsonBoolean FALSE = new JsonBoolean(false);
        private final boolean value;

        private JsonBoolean(boolean value) {
            this.value = value;
        }
    }

    private static final class JsonNull implements JsonValue {
        private static final JsonNull INSTANCE = new JsonNull();
    }

    private static final class JsonParser {
        private final String text;
        private int index;

        private JsonParser(String text) {
            this.text = text;
        }

        private static JsonValue parse(String text) throws IOException {
            JsonParser parser = new JsonParser(text);
            JsonValue value = parser.parseValue();
            parser.skipWhitespace();
            if (parser.index != parser.text.length()) {
                throw parser.error("trailing characters");
            }
            return value;
        }

        private JsonValue parseValue() throws IOException {
            skipWhitespace();
            if (index >= text.length()) {
                throw error("unexpected end of input");
            }
            char current = text.charAt(index);
            if (current == '{') {
                return parseObject();
            }
            if (current == '[') {
                return parseArray();
            }
            if (current == '"') {
                return new JsonString(parseString());
            }
            if (text.startsWith("true", index)) {
                index += 4;
                return JsonBoolean.TRUE;
            }
            if (text.startsWith("false", index)) {
                index += 5;
                return JsonBoolean.FALSE;
            }
            if (text.startsWith("null", index)) {
                index += 4;
                return JsonNull.INSTANCE;
            }
            return parseNumber();
        }

        private JsonObject parseObject() throws IOException {
            expect('{');
            JsonObject object = new JsonObject();
            skipWhitespace();
            if (consume('}')) {
                return object;
            }
            while (true) {
                skipWhitespace();
                if (index >= text.length() || text.charAt(index) != '"') {
                    throw error("object key must be string");
                }
                String key = parseString();
                skipWhitespace();
                expect(':');
                object.put(key, parseValue());
                skipWhitespace();
                if (consume('}')) {
                    return object;
                }
                expect(',');
            }
        }

        private JsonArray parseArray() throws IOException {
            expect('[');
            JsonArray array = new JsonArray();
            skipWhitespace();
            if (consume(']')) {
                return array;
            }
            while (true) {
                array.values.add(parseValue());
                skipWhitespace();
                if (consume(']')) {
                    return array;
                }
                expect(',');
            }
        }

        private String parseString() throws IOException {
            expect('"');
            StringBuilder value = new StringBuilder();
            while (index < text.length()) {
                char current = text.charAt(index++);
                if (current == '"') {
                    return value.toString();
                }
                if (current == '\\') {
                    if (index >= text.length()) {
                        throw error("unterminated escape");
                    }
                    char escaped = text.charAt(index++);
                    switch (escaped) {
                        case '"':
                        case '\\':
                        case '/':
                            value.append(escaped);
                            break;
                        case 'b':
                            value.append('\b');
                            break;
                        case 'f':
                            value.append('\f');
                            break;
                        case 'n':
                            value.append('\n');
                            break;
                        case 'r':
                            value.append('\r');
                            break;
                        case 't':
                            value.append('\t');
                            break;
                        case 'u':
                            value.append(parseUnicodeEscape());
                            break;
                        default:
                            throw error("invalid escape");
                    }
                } else {
                    if (current < 0x20) {
                        throw error("unescaped control character");
                    }
                    value.append(current);
                }
            }
            throw error("unterminated string");
        }

        private char parseUnicodeEscape() throws IOException {
            if (index + 4 > text.length()) {
                throw error("short unicode escape");
            }
            int code = 0;
            for (int offset = 0; offset < 4; offset++) {
                int digit = Character.digit(text.charAt(index++), 16);
                if (digit < 0) {
                    throw error("invalid unicode escape");
                }
                code = code * 16 + digit;
            }
            return (char) code;
        }

        private JsonNumber parseNumber() throws IOException {
            int start = index;
            boolean integer = true;
            if (consume('-')) {
                // sign consumed
            }
            if (consume('0')) {
                // zero has no integer tail
            } else {
                if (index >= text.length() || !isDigitOneToNine(text.charAt(index))) {
                    throw error("invalid number");
                }
                while (index < text.length() && isDigit(text.charAt(index))) {
                    index++;
                }
            }
            if (consume('.')) {
                integer = false;
                if (index >= text.length() || !isDigit(text.charAt(index))) {
                    throw error("invalid number fraction");
                }
                while (index < text.length() && isDigit(text.charAt(index))) {
                    index++;
                }
            }
            if (index < text.length() && (text.charAt(index) == 'e' || text.charAt(index) == 'E')) {
                integer = false;
                index++;
                if (index < text.length() && (text.charAt(index) == '+' || text.charAt(index) == '-')) {
                    index++;
                }
                if (index >= text.length() || !isDigit(text.charAt(index))) {
                    throw error("invalid number exponent");
                }
                while (index < text.length() && isDigit(text.charAt(index))) {
                    index++;
                }
            }
            String raw = text.substring(start, index);
            if (integer) {
                try {
                    Long.parseLong(raw);
                } catch (NumberFormatException exception) {
                    throw error("integer out of range");
                }
            } else {
                double value = Double.parseDouble(raw);
                if (Double.isNaN(value) || Double.isInfinite(value)) {
                    throw error("non-finite JSON number");
                }
            }
            return new JsonNumber(raw, integer);
        }

        private static boolean isDigit(char value) {
            return value >= '0' && value <= '9';
        }

        private static boolean isDigitOneToNine(char value) {
            return value >= '1' && value <= '9';
        }

        private void skipWhitespace() {
            while (index < text.length()) {
                char current = text.charAt(index);
                if (current != ' ' && current != '\t' && current != '\n' && current != '\r') {
                    return;
                }
                index++;
            }
        }

        private boolean consume(char expected) {
            if (index < text.length() && text.charAt(index) == expected) {
                index++;
                return true;
            }
            return false;
        }

        private void expect(char expected) throws IOException {
            if (!consume(expected)) {
                throw error("expected '" + expected + "'");
            }
        }

        private IOException error(String message) {
            return new IOException(message + " at offset " + index);
        }
    }

    private static final class JsonWriter {
        private JsonWriter() {
        }

        private static String write(JsonValue value) throws IOException {
            StringBuilder output = new StringBuilder();
            append(output, value);
            return output.toString();
        }

        private static void append(StringBuilder output, JsonValue value) throws IOException {
            if (value instanceof JsonObject) {
                output.append('{');
                boolean first = true;
                for (Map.Entry<String, JsonValue> entry : ((JsonObject) value).fields.entrySet()) {
                    if (!first) {
                        output.append(',');
                    }
                    first = false;
                    appendString(output, entry.getKey());
                    output.append(':');
                    append(output, entry.getValue());
                }
                output.append('}');
            } else if (value instanceof JsonArray) {
                output.append('[');
                boolean first = true;
                for (JsonValue item : ((JsonArray) value).values) {
                    if (!first) {
                        output.append(',');
                    }
                    first = false;
                    append(output, item);
                }
                output.append(']');
            } else if (value instanceof JsonString) {
                appendString(output, ((JsonString) value).value);
            } else if (value instanceof JsonNumber) {
                output.append(((JsonNumber) value).raw);
            } else if (value instanceof JsonBoolean) {
                output.append(((JsonBoolean) value).value ? "true" : "false");
            } else if (value instanceof JsonNull) {
                output.append("null");
            } else {
                throw new IOException("unsupported JSON node");
            }
        }

        private static void appendString(StringBuilder output, String value) {
            output.append('"');
            for (int index = 0; index < value.length(); index++) {
                char current = value.charAt(index);
                switch (current) {
                    case '"':
                        output.append("\\\"");
                        break;
                    case '\\':
                        output.append("\\\\");
                        break;
                    case '\b':
                        output.append("\\b");
                        break;
                    case '\f':
                        output.append("\\f");
                        break;
                    case '\n':
                        output.append("\\n");
                        break;
                    case '\r':
                        output.append("\\r");
                        break;
                    case '\t':
                        output.append("\\t");
                        break;
                    default:
                        if (current < 0x20) {
                            output.append(String.format("\\u%04x", (int) current));
                        } else {
                            output.append(current);
                        }
                }
            }
            output.append('"');
        }
    }

    private static String envString(String key, String defaultValue) {
        String value = System.getenv(key);
        return value == null ? defaultValue : value;
    }

    private static int envInt(String key, int defaultValue) {
        String value = System.getenv(key);
        if (value == null || value.length() == 0) {
            return defaultValue;
        }
        try {
            return Integer.parseInt(value);
        } catch (NumberFormatException exception) {
            return defaultValue;
        }
    }

    private static long envLong(String key, long defaultValue) {
        String value = System.getenv(key);
        if (value == null || value.length() == 0) {
            return defaultValue;
        }
        try {
            return Long.parseLong(value);
        } catch (NumberFormatException exception) {
            return defaultValue;
        }
    }
}
