package test

// import common.*
import std.core.*
import std.runtime.*
import std.reflect.*
import common.*

public var to_be_set_in_mock: Option<Object> = None
public var pinned_obj: Option<Object> = None
public var placeholder_array: Array<TestData> = Array<TestData>()

public class TestData {
    var x: Int64
    init(x: Int64) { this.x = x }
}

public func do_test() {
    c2i.invoke0()
    println("cjnative main")
    gc(heavy: false)

    match (to_be_set_in_mock) {
        case None => throw Exception("error: global variable is unset, should be set by mock")
        case Some(x) =>
            let info = TypeInfo.of(x);
            println("ok: global variable is set");
            println("Type is: " + info.toString())
            if (info.toString() != "Object") {
                throw Exception("error: global variable has unexpected type: " + info.toString())
            }
    }
    gc(heavy: false)

    match (pinned_obj) {
        case None => throw Exception("error: pinned object is unset, should be set by mock")
        case Some(x) =>
            let info = TypeInfo.of(x);
            println("ok: pinned object is set");
            println("Pinned type is: " + info.toString())
            if (info.toString() != "Object") {
                throw Exception("error: pinned object has unexpected type: " + info.toString())
            }
    }
    gc(heavy: false)

    if (placeholder_array.size == 0) {
        throw Exception("error: placeholder_array is empty")
    } else {
        println("ok: placeholder_array is not empty, size: " + placeholder_array.size.toString())
        for (elem in placeholder_array) {
            let info = TypeInfo.of(elem);
            if (info.toString() != "test.TestData") {
                throw Exception("error: element is not of type test.TestData");
            }
        }
    }

    return 0
}

main() {
	do_test()
}
