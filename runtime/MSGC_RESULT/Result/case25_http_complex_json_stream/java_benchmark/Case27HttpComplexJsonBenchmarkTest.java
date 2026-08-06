import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

public final class Case27HttpComplexJsonBenchmarkTest {
    public static void main(String[] args) throws Exception {
        String input = "{"
                + "\"counters\":{\"in\":1,\"ok\":2},"
                + "\"events\":[{"
                + "\"seq\":3,\"score\":1.5,"
                + "\"user\":{\"attrs\":{\"credits\":4.0}},"
                + "\"custom\":{\"product_ids\":[1]}"
                + "}]"
                + "}";

        byte[] response = Case27HttpComplexJsonBenchmark.processRequestJson(
                input.getBytes(StandardCharsets.UTF_8), 7L);
        String json = new String(response, StandardCharsets.UTF_8);

        assertContains(json, "\"in\":2");
        assertContains(json, "\"ok\":3");
        assertContains(json, "\"processed_seq\":7");
        assertContains(json, "\"server_ok\":true");
        assertContains(json, "\"seq\":4");
        assertContains(json, "\"score\":3.0");
        assertContains(json, "\"credits\":8.0");
        assertContains(json, "\"product_ids\":[1,9999]");
        assertContains(json, "\"server_marker\":\"m-0\"");

        assertProcessFails("{\"counters\":{\"in\":1.0,\"ok\":2}}", 8L);
        assertProcessFails("{\"counters\":{\"in\":1}}", 9L);
        assertProcessFails("{\"counters\":[]}", 10L);
        assertProcessFails("{\"events\":{}}", 11L);
        assertProcessFails("{\"events\":[1]}", 12L);
        assertProcessFails("{\"events\":[{\"seq\":1.0}]}", 13L);
        assertProcessFails("{\"events\":[{\"user\":1}]}", 14L);
        assertProcessFails("{\"events\":[{\"user\":{\"attrs\":1}}]}", 15L);
        assertProcessFails(
                "{\"events\":[{\"user\":{\"attrs\":{\"credits\":true}}}]}", 16L);
        assertProcessFails("{\"events\":[{\"custom\":1}]}", 17L);
        assertProcessFails(
                "{\"events\":[{\"custom\":{\"product_ids\":{}}}]}", 18L);

        byte[] integerFloatResponse = Case27HttpComplexJsonBenchmark.processRequestJson(
                "{\"events\":[{\"score\":1,\"user\":{\"attrs\":{\"credits\":4}}}]}"
                        .getBytes(StandardCharsets.UTF_8), 19L);
        String integerFloatJson = new String(integerFloatResponse, StandardCharsets.UTF_8);
        assertContains(integerFloatJson, "\"score\":2.0");
        assertContains(integerFloatJson, "\"credits\":8.0");

        byte[] duplicateFieldResponse = Case27HttpComplexJsonBenchmark.processRequestJson(
                ("{\"processed_seq\":-1,\"server_ok\":false,\"events\":[{"
                        + "\"server_marker\":\"old\"}]}" )
                        .getBytes(StandardCharsets.UTF_8), 20L);
        String duplicateFieldJson = new String(duplicateFieldResponse, StandardCharsets.UTF_8);
        assertContains(duplicateFieldJson, "\"processed_seq\":-1");
        assertContains(duplicateFieldJson, "\"processed_seq\":20");
        assertContains(duplicateFieldJson, "\"server_ok\":false");
        assertContains(duplicateFieldJson, "\"server_ok\":true");
        assertContains(duplicateFieldJson, "\"server_marker\":\"old\"");
        assertContains(duplicateFieldJson, "\"server_marker\":\"m-0\"");
        assertOccurrences(duplicateFieldJson, "\"processed_seq\"", 2);
        assertOccurrences(duplicateFieldJson, "\"server_ok\"", 2);
        assertOccurrences(duplicateFieldJson, "\"server_marker\"", 2);

        Case27HttpComplexJsonBenchmark.PersistentLayout layout =
                Case27HttpComplexJsonBenchmark.PersistentLayout.create(5L << 30, 8);
        assertEquals(5L << 30, layout.totalBytes, "total bytes");
        assertEquals((5L << 30) / 2L, layout.staticBytes, "static bytes");
        assertEquals((5L << 30) / 2L, layout.stackBytes, "stack bytes");
        if (layout.largeBytes * 1000L < layout.totalBytes * 9L
                || layout.largeBytes * 100L > layout.totalBytes) {
            throw new AssertionError("large objects are not approximately 1%: "
                    + layout.largeBytes);
        }

        Case27HttpComplexJsonBenchmark.PersistentBaseline baseline =
                Case27HttpComplexJsonBenchmark.PersistentBaseline.start(64L << 20, 2);
        try {
            Case27HttpComplexJsonBenchmark.PersistentStats stats = baseline.stats();
            assertEquals(64L << 20, stats.totalBytes, "retained bytes");
            assertEquals(stats.totalBytes, stats.staticBytes + stats.stackBytes,
                    "root accounting");
            if (Case27HttpComplexJsonBenchmark.persistentStaticRootShards.length != 2) {
                throw new AssertionError("expected one static root shard per worker");
            }
            boolean sawLargeObject = false;
            for (java.util.List<byte[]> shard
                    : Case27HttpComplexJsonBenchmark.persistentStaticRootShards) {
                for (byte[] object : shard) {
                    if (!isDeclaredPersistentSize(object.length)) {
                        throw new AssertionError(
                                "unexpected persistent object size " + object.length);
                    }
                    if (object.length > (16 << 10)) {
                        sawLargeObject = true;
                    }
                }
            }
        if (!sawLargeObject) {
            throw new AssertionError(
                    "expected the smoke baseline to allocate declared large objects");
        }

        assertSourceUsesJacksonStreaming();
        } finally {
            baseline.close();
        }
    }

    private static void assertProcessFails(String input, long sequence) throws Exception {
        try {
            Case27HttpComplexJsonBenchmark.processRequestJson(
                    input.getBytes(StandardCharsets.UTF_8), sequence);
        } catch (Exception expected) {
            return;
        }
        throw new AssertionError("expected processRequestJson to fail for " + input);
    }

    private static void assertContains(String text, String expected) {
        if (!text.contains(expected)) {
            throw new AssertionError("missing '" + expected + "' in " + text);
        }
    }

    private static void assertOccurrences(String text, String needle, int expected) {
        int count = 0;
        int offset = 0;
        while (true) {
            int found = text.indexOf(needle, offset);
            if (found < 0) {
                break;
            }
            count++;
            offset = found + needle.length();
        }
        if (count != expected) {
            throw new AssertionError("expected " + expected + " occurrences of '" + needle
                    + "', got " + count + " in " + text);
        }
    }

    private static void assertEquals(long expected, long actual, String label) {
        if (expected != actual) {
            throw new AssertionError(label + ": expected " + expected + ", got " + actual);
        }
    }

    private static boolean isDeclaredPersistentSize(int size) {
        int[] allowed = {
                128, 256, 512, 1024, 2048, 4096, 8192, 16384,
                65536, 262144, 1048576
        };
        for (int candidate : allowed) {
            if (candidate == size) {
                return true;
            }
        }
        return false;
    }

    private static void assertSourceUsesJacksonStreaming() throws Exception {
        String caseDir = System.getenv("CASE_DIR");
        Path source = caseDir == null || caseDir.isEmpty()
                ? Paths.get("Case27HttpComplexJsonBenchmark.java")
                : Paths.get(caseDir, "java_benchmark", "Case27HttpComplexJsonBenchmark.java");
        String text = new String(Files.readAllBytes(source), StandardCharsets.UTF_8);
        assertContains(text, "JacksonStreamTransformer.transform");
        assertContains(text, "JacksonStreamTransformer.hasServerOk");
        assertContains(text, "com.fasterxml.jackson.core.JsonGenerator");
    }
}
