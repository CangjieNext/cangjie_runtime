import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.net.URLEncoder;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

public final class Case29MemberAccountsBenchmark {
    private Case29MemberAccountsBenchmark() {}

    private static String pad4(long value) {
        if (value < 10) return "000" + value;
        if (value < 100) return "00" + value;
        if (value < 1000) return "0" + value;
        return Long.toString(value);
    }

    private static boolean isActiveAccount(long value) {
        return value % 5 != 0;
    }

    private static boolean belongsToSubject(long value, String subjectId) {
        return (value % 2 == 0 ? "subject-A" : "subject-B").equals(subjectId);
    }

    private static String activeCardNumbers(long value) {
        List<String> cards = new ArrayList<>();
        for (long cardIndex = 0; cardIndex <= value % 3; cardIndex++) {
            if ((value + cardIndex) % 3 != 0) {
                cards.add("card-" + pad4(value) + "-" + cardIndex);
            }
        }
        return cards.isEmpty() ? null : String.join(",", cards);
    }

    private static void appendStringField(StringBuilder out, String name, String value, boolean trailingComma) {
        out.append("    \"").append(name).append("\": \"").append(value).append("\"");
        if (trailingComma) out.append(',');
        out.append('\n');
    }

    private static void appendMember(StringBuilder out, long value) {
        String padded = pad4(value);
        out.append("  {\n");
        appendStringField(out, "id", "acct-" + padded, true);
        appendStringField(out, "memberId", "member-" + padded, true);
        appendStringField(out, "accountCode", "AC-" + padded, true);
        appendStringField(out, "name", "Member " + padded, true);
        String department = pad4(value % 4).substring(2);
        appendStringField(out, "accountDepartmentId", "dept-" + department, true);
        appendStringField(out, "accountDepartment", "Department " + department, true);
        String cards = activeCardNumbers(value);
        out.append("    \"cardNo\": ");
        if (cards == null) out.append("null\n");
        else out.append('\"').append(cards).append("\"\n");
        out.append("  }");
    }

    public static String renderMembers(long seed, int accountCount, String subjectId, int limit, int offset) {
        if (accountCount <= 0 || limit <= 0 || offset < 0) return "[]";
        StringBuilder out = new StringBuilder();
        long matched = 0;
        long emitted = 0;
        for (long department = 0; department < 4; department++) {
            for (long index = 0; index < accountCount; index++) {
                long value = seed + index;
                if (value % 4 != department || !isActiveAccount(value) || !belongsToSubject(value, subjectId)) continue;
                if (matched++ < offset) continue;
                if (emitted == limit) break;
                if (emitted++ == 0) out.append("[\n");
                else out.append(",\n");
                appendMember(out, value);
            }
            if (emitted == limit) break;
        }
        if (emitted == 0) return "[]";
        return out.append("\n]").toString();
    }

    private static long envLong(String key, long defaultValue) {
        try {
            String value = System.getenv(key);
            return value == null ? defaultValue : Long.parseLong(value);
        } catch (NumberFormatException ignored) {
            return defaultValue;
        }
    }

    private static String queryValue(String query, String name, String defaultValue) {
        if (query == null || query.isEmpty()) return defaultValue;
        for (String part : query.split("&")) {
            String[] entry = part.split("=", 2);
            if (entry.length == 2 && entry[0].equals(name)) {
                return java.net.URLDecoder.decode(entry[1], StandardCharsets.UTF_8);
            }
        }
        return defaultValue;
    }

    private static int queryInt(String query, String name, int defaultValue) {
        try {
            return Integer.parseInt(queryValue(query, name, Integer.toString(defaultValue)));
        } catch (NumberFormatException ignored) {
            return defaultValue;
        }
    }

    private static ServerHandle startServer(long seed, int accountCount, int port, int threads) throws IOException {
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", port), 0);
        ExecutorService executor = Executors.newFixedThreadPool(Math.max(1, threads));
        server.setExecutor(executor);
        server.createContext("/api/v1/member-accounts", exchange -> {
            String query = exchange.getRequestURI().getRawQuery();
            String subject = queryValue(query, "subjectId", "");
            int limit = queryInt(query, "limit", 50);
            int offset = queryInt(query, "offset", 0);
            byte[] body = renderMembers(seed, accountCount, subject, limit, offset)
                .getBytes(StandardCharsets.UTF_8);
            exchange.getResponseHeaders().set("Content-Type", "application/json; charset=utf-8");
            exchange.sendResponseHeaders(200, body.length);
            exchange.getResponseBody().write(body);
            exchange.close();
        });
        server.start();
        return new ServerHandle(server, executor, server.getAddress().getPort());
    }

    public static boolean runHttpSelfTest() {
        try (ServerHandle handle = startServer(0, 10, 0, 1)) {
            HttpClient client = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(3)).build();
            HttpRequest request = HttpRequest.newBuilder()
                .uri(URI.create("http://127.0.0.1:" + handle.port + "/api/v1/member-accounts?subjectId=subject-A&limit=50&offset=0"))
                .GET().build();
            HttpResponse<String> response = client.send(request, HttpResponse.BodyHandlers.ofString());
            return response.statusCode() == 200
                && response.body().equals(renderMembers(0, 10, "subject-A", 50, 0));
        } catch (Exception ignored) {
            return false;
        }
    }

    private static final class ServerHandle implements AutoCloseable {
        private final HttpServer server;
        private final ExecutorService executor;
        private final int port;

        private ServerHandle(HttpServer server, ExecutorService executor, int port) {
            this.server = server;
            this.executor = executor;
            this.port = port;
        }

        @Override
        public void close() {
            server.stop(0);
            executor.shutdownNow();
        }
    }

    public static void main(String[] args) throws Exception {
        long seed = envLong("CASE29_SEED", 0);
        int accountCount = (int) envLong("CASE29_ACCOUNT_COUNT", 1000);
        String subjectId = System.getenv().getOrDefault("CASE29_SUBJECT_ID", "subject-A");
        int limit = (int) envLong("CASE29_LIMIT", 50);
        int offset = (int) envLong("CASE29_OFFSET", 0);
        int concurrency = Math.max(1, (int) envLong("CASE29_CONCURRENCY", 8));
        long durationMs = Math.max(1, envLong("CASE29_DURATION_MS", 60_000));
        int port = (int) envLong("CASE29_SERVER_PORT", 18229);
        String expected = renderMembers(seed, accountCount, subjectId, limit, offset);

        try (ServerHandle handle = startServer(seed, accountCount, port, concurrency)) {
            String encodedSubject = URLEncoder.encode(subjectId, StandardCharsets.UTF_8);
            URI endpoint = URI.create("http://127.0.0.1:" + handle.port
                + "/api/v1/member-accounts?subjectId=" + encodedSubject
                + "&limit=" + limit + "&offset=" + offset);
            HttpClient client = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(5)).build();
            for (int i = 0; i < 20; i++) {
                client.send(HttpRequest.newBuilder(endpoint).GET().build(), HttpResponse.BodyHandlers.ofString());
            }

            AtomicBoolean stop = new AtomicBoolean(false);
            AtomicLong total = new AtomicLong();
            AtomicLong ok = new AtomicLong();
            AtomicLong fail = new AtomicLong();
            List<Thread> workers = new ArrayList<>();
            for (int i = 0; i < concurrency; i++) {
                Thread worker = Thread.ofPlatform().start(() -> {
                    while (!stop.get()) {
                        try {
                            HttpResponse<String> response = client.send(
                                HttpRequest.newBuilder(endpoint).GET().build(),
                                HttpResponse.BodyHandlers.ofString());
                            if (response.statusCode() == 200 && response.body().equals(expected)) ok.incrementAndGet();
                            else fail.incrementAndGet();
                        } catch (Exception ignored) {
                            fail.incrementAndGet();
                        } finally {
                            total.incrementAndGet();
                        }
                    }
                });
                workers.add(worker);
            }
            long startNanos = System.nanoTime();
            Thread.sleep(durationMs);
            stop.set(true);
            for (Thread worker : workers) worker.join(7_000);
            long elapsedMs = Math.max(1, (System.nanoTime() - startNanos) / 1_000_000);
            long throughput = ok.get() * 1_000 / elapsedMs;
            System.out.println("[RESULT] payload=member-accounts concurrency=" + concurrency + " duration=" + elapsedMs + "ms");
            System.out.println("[RESULT] total=" + total.get() + " ok=" + ok.get() + " fail=" + fail.get()
                + " throughput=" + throughput + "req/s");
            if (fail.get() != 0) System.exit(1);
        }
    }
}
