import stdx.encoding.json.*

func requireEqual(actual: String, expected: String, label: String): Unit {
    if (actual != expected) {
        throw IllegalStateException("${label}: expected=${expected}, actual=${actual}")
    }
}

// Return the DOM after the source String has left its defining scope. This
// verifies that zero-copy unescaped strings keep their backing data alive.
func parseScoped(): JsonValue {
    let source = "{\"plain\":\"case26-no-escape\",\"empty\":\"\",\"utf8\":\"你好\"}"
    return JsonValue.fromStr(source)
}

main(): Int64 {
    requireEqual(
        parseScoped().toStringWithCapacity(128),
        "{\"plain\":\"case26-no-escape\",\"empty\":\"\",\"utf8\":\"你好\"}",
        "scoped plain strings and capacity writer"
    )
    requireEqual(
        JsonValue.fromStr("{\"escaped\":\"line\\nquote\\\"slash\\\\\"}").toStringWithCapacity(128),
        "{\"escaped\":\"line\\nquote\\\"slash\\\\\"}",
        "basic escape"
    )
    requireEqual(
        JsonValue.fromStr("{\"unicode\":\"\\u0061\\u4f60\\u597d\"}").toStringWithCapacity(128),
        "{\"unicode\":\"a你好\"}",
        "unicode escape"
    )
    requireEqual(
        JsonValue.fromStr("{\"pair\":\"\\ud83d\\ude00\"}").toStringWithCapacity(128),
        "{\"pair\":\"😀\"}",
        "surrogate pair"
    )
    requireEqual(
        JsonValue.fromStr("[null,true,1,1.5,\"x\",[],{}]").toStringWithCapacity(128),
        "[null,true,1,1.5,\"x\",[],{}]",
        "compact virtual dispatch for every JsonValue subtype"
    )
    requireEqual(
        JsonString("&").toString(),
        "\"\\u0026\"",
        "short string html-safe escape"
    )
    requireEqual(
        JsonString("&").toStringWithoutEscaping(),
        "\"&\"",
        "short string html-safe disabled"
    )
    requireEqual(
        JsonValue.fromStr("\"\\b\\f\\n\\r\\t\\\"\\\\\"").toStringWithCapacity(64),
        "\"\\b\\f\\n\\r\\t\\\"\\\\\"",
        "short string named escapes"
    )
    requireEqual(
        JsonValue.fromStr("\"\\u0001\\u007f\"").toStringWithCapacity(64),
        "\"\\u0001\\u007f\"",
        "short string hexadecimal escapes"
    )
    requireEqual(
        JsonString("abcdefghijklmnopqrstuvwxyz0123456789&").toString(),
        "\"abcdefghijklmnopqrstuvwxyz0123456789\\u0026\"",
        "long string native escape fallback"
    )
    println("[PASS] DOM JSON writer regression")
    return 0
}
