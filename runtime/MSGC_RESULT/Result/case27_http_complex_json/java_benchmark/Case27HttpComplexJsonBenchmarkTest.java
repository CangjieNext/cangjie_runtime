import java.nio.charset.StandardCharsets;

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
}
