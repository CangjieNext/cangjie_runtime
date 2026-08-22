import std.io.*
import stdx.encoding.json.stream.*

const RESPONSE_CAPACITY_MARGIN: Int64 = 8 * 1024
const FALLBACK_RESPONSE_CAPACITY: Int64 = 8 * 1024

// benchmark.cj is compiled with one transform source per GC implementation.
// Keep this common server-side capacity helper in each source so CopyGC and
// MSGC can both compile that shared benchmark independently.
public func initialResponseCapacity(bodySize: ?Int64): Int64 {
    match (bodySize) {
        case Some(size) => if (size > 0) { size + RESPONSE_CAPACITY_MARGIN } else { FALLBACK_RESPONSE_CAPACITY }
        case _ => FALLBACK_RESPONSE_CAPACITY
    }
}

// CopyGC SDK does not provide JsonReader.copyValueTo. Keep the same stream
// transformation semantics with the raw-value API available in that SDK.
func copyRawValue(reader: JsonReader, writer: JsonWriter): Unit {
    writer.jsonValue(String.fromUtf8(reader.readValueBytes()))
}

func transformCounters(reader: JsonReader, writer: JsonWriter): Unit {
    reader.startObject()
    writer.startObject()
    while (reader.peek() != Some(EndObject)) {
        let name = reader.readName()
        writer.writeName(name)
        if (name == "in" || name == "ok") {
            writer.writeValue<Int64>(reader.readValue<Int64>() + 1)
        } else {
            copyRawValue(reader, writer)
        }
    }
    reader.endObject()
    writer.endObject()
}

func transformAttrs(reader: JsonReader, writer: JsonWriter): Unit {
    reader.startObject()
    writer.startObject()
    while (reader.peek() != Some(EndObject)) {
        let name = reader.readName()
        writer.writeName(name)
        if (name == "credits") {
            writer.writeValue<Float64>(reader.readValue<Float64>() * 2.0)
        } else {
            copyRawValue(reader, writer)
        }
    }
    reader.endObject()
    writer.endObject()
}

func transformUser(reader: JsonReader, writer: JsonWriter): Unit {
    reader.startObject()
    writer.startObject()
    while (reader.peek() != Some(EndObject)) {
        let name = reader.readName()
        writer.writeName(name)
        if (name == "attrs") {
            transformAttrs(reader, writer)
        } else {
            copyRawValue(reader, writer)
        }
    }
    reader.endObject()
    writer.endObject()
}

func transformProductIds(reader: JsonReader, writer: JsonWriter, eventIndex: Int64): Unit {
    reader.startArray()
    writer.startArray()
    while (reader.peek() != Some(EndArray)) {
        copyRawValue(reader, writer)
    }
    reader.endArray()
    writer.writeValue<Int64>(eventIndex + 9_999)
    writer.endArray()
}

func transformCustom(reader: JsonReader, writer: JsonWriter, eventIndex: Int64): Unit {
    reader.startObject()
    writer.startObject()
    while (reader.peek() != Some(EndObject)) {
        let name = reader.readName()
        writer.writeName(name)
        if (name == "product_ids") {
            transformProductIds(reader, writer, eventIndex)
        } else {
            copyRawValue(reader, writer)
        }
    }
    reader.endObject()
    writer.endObject()
}

func transformEvent(reader: JsonReader, writer: JsonWriter, eventIndex: Int64): Unit {
    reader.startObject()
    writer.startObject()
    while (reader.peek() != Some(EndObject)) {
        let name = reader.readName()
        writer.writeName(name)
        match (name) {
            case "seq" => writer.writeValue<Int64>(reader.readValue<Int64>() + 1)
            case "score" => writer.writeValue<Float64>(reader.readValue<Float64>() * 2.0)
            case "user" => transformUser(reader, writer)
            case "custom" => transformCustom(reader, writer, eventIndex)
            case _ => copyRawValue(reader, writer)
        }
    }
    reader.endObject()
    writer.writeName("server_marker").writeValue<String>("m-${eventIndex}")
    writer.endObject()
}

func transformEvents(reader: JsonReader, writer: JsonWriter): Unit {
    reader.startArray()
    writer.startArray()
    var eventIndex: Int64 = 0
    while (reader.peek() != Some(EndArray)) {
        transformEvent(reader, writer, eventIndex)
        eventIndex++
    }
    reader.endArray()
    writer.endArray()
}

public func transformRequest(input: InputStream, output: OutputStream, seq: Int64): Unit {
    let reader = JsonReader(input)
    let writer = JsonWriter(output)
    reader.startObject()
    writer.startObject()
    while (reader.peek() != Some(EndObject)) {
        let name = reader.readName()
        writer.writeName(name)
        match (name) {
            case "counters" => transformCounters(reader, writer)
            case "events" => transformEvents(reader, writer)
            case _ => copyRawValue(reader, writer)
        }
    }
    reader.endObject()
    writer.writeName("processed_seq").writeValue<Int64>(seq)
    writer.writeName("server_ok").writeValue<Bool>(true)
    writer.endObject()
    writer.flush()
}

public func responseHasServerOk(input: InputStream): Bool {
    let reader = JsonReader(input)
    reader.startObject()
    var serverOk = false
    while (reader.peek() != Some(EndObject)) {
        let name = reader.readName()
        if (name == "server_ok") {
            serverOk = reader.readValue<Bool>()
        } else {
            reader.skip()
        }
    }
    reader.endObject()
    return serverOk
}
