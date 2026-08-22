import std.io.*
import stdx.encoding.json.stream.*

let NAME_IN = "in".toArray()
let NAME_OK = "ok".toArray()
let NAME_CREDITS = "credits".toArray()
let NAME_ATTRS = "attrs".toArray()
let NAME_PRODUCT_IDS = "product_ids".toArray()
let NAME_SEQ = "seq".toArray()
let NAME_SCORE = "score".toArray()
let NAME_USER = "user".toArray()
let NAME_CUSTOM = "custom".toArray()
let NAME_COUNTERS = "counters".toArray()
let NAME_EVENTS = "events".toArray()
let NAME_SERVER_MARKER = "server_marker".toArray()
let NAME_PROCESSED_SEQ = "processed_seq".toArray()
let NAME_SERVER_OK = "server_ok".toArray()
const RESPONSE_CAPACITY_MARGIN: Int64 = 8 * 1024
const FALLBACK_RESPONSE_CAPACITY: Int64 = 8 * 1024

public func initialResponseCapacity(bodySize: ?Int64): Int64 {
    match (bodySize) {
        case Some(size) => if (size > 0) { size + RESPONSE_CAPACITY_MARGIN } else { FALLBACK_RESPONSE_CAPACITY }
        case _ => FALLBACK_RESPONSE_CAPACITY
    }
}

// MSGC SDK: copy one complete JSON value without materializing it as a JsonValue tree.
func copyRawValue(reader: JsonReader, writer: JsonWriter): Unit {
    reader.copyValueTo(writer)
}

func transformCounters(reader: JsonReader, writer: JsonWriter): Unit {
    reader.startObject()
    writer.startObject()
    while (reader.peek() != Some(EndObject)) {
        if (reader.nextNameEquals(NAME_IN) || reader.nextNameEquals(NAME_OK)) {
            reader.copyNameTo(writer)
            writer.writeValue<Int64>(reader.readValue<Int64>() + 1)
        } else {
            reader.copyNameTo(writer)
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
        if (reader.nextNameEquals(NAME_CREDITS)) {
            reader.copyNameTo(writer)
            writer.writeValue<Float64>(reader.readValue<Float64>() * 2.0)
        } else {
            reader.copyNameTo(writer)
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
        if (reader.nextNameEquals(NAME_ATTRS)) {
            reader.copyNameTo(writer)
            transformAttrs(reader, writer)
        } else {
            reader.copyNameTo(writer)
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
        if (reader.nextNameEquals(NAME_PRODUCT_IDS)) {
            reader.copyNameTo(writer)
            transformProductIds(reader, writer, eventIndex)
        } else {
            reader.copyNameTo(writer)
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
        if (reader.nextNameEquals(NAME_SEQ)) {
            reader.copyNameTo(writer)
            writer.writeValue<Int64>(reader.readValue<Int64>() + 1)
        } else if (reader.nextNameEquals(NAME_SCORE)) {
            reader.copyNameTo(writer)
            writer.writeValue<Float64>(reader.readValue<Float64>() * 2.0)
        } else if (reader.nextNameEquals(NAME_USER)) {
            reader.copyNameTo(writer)
            transformUser(reader, writer)
        } else if (reader.nextNameEquals(NAME_CUSTOM)) {
            reader.copyNameTo(writer)
            transformCustom(reader, writer, eventIndex)
        } else {
            reader.copyNameTo(writer)
            copyRawValue(reader, writer)
        }
    }
    reader.endObject()
    writer.writeRawName(NAME_SERVER_MARKER, 0, NAME_SERVER_MARKER.size).writeValue<String>("m-${eventIndex}")
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
    writer.writeRawName(NAME_SERVER_OK, 0, NAME_SERVER_OK.size).writeValue<Bool>(true)
    writer.writeRawName(NAME_PROCESSED_SEQ, 0, NAME_PROCESSED_SEQ.size).writeValue<Int64>(seq)
    while (reader.peek() != Some(EndObject)) {
        if (reader.nextNameEquals(NAME_COUNTERS)) {
            reader.copyNameTo(writer)
            transformCounters(reader, writer)
        } else if (reader.nextNameEquals(NAME_EVENTS)) {
            reader.copyNameTo(writer)
            transformEvents(reader, writer)
        } else {
            reader.copyNameTo(writer)
            copyRawValue(reader, writer)
        }
    }
    reader.endObject()
    writer.endObject()
    writer.flush()
}

public func responseHasServerOk(input: InputStream): Bool {
    let reader = JsonReader(input)
    reader.startObject()
    let serverOk = if (reader.nextNameEquals(NAME_SERVER_OK)) {
        reader.skipName()
        reader.readValue<Bool>()
    } else {
        false
    }
    reader.discardRemaining()
    return serverOk
}
