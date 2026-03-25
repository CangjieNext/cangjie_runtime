// import common.*
import std.core.*
import std.runtime.*
import std.reflect.*
import common.*

public var to_be_set_in_mock: Option<Object> = None
public var placeholder_array: Array<TestData> = Array<TestData>()

public class TestData {
    var x: Int64
    init(x: Int64) { this.x = x }
}

main() {
    c2i.invoke0()
    println("cjnative main")
    gc(heavy: false)

    match (to_be_set_in_mock) {
        case None => println("error: global variable is unset, should be set by mock")
        case Some(x) =>
            let info = TypeInfo.of(x);
            println("ok: global variable is set");
            println("Type is: " + info.toString())
            if (info.toString() != "Object") {
                return 1
            }
    }
    gc(heavy: false)

    if (placeholder_array.size == 0) {
        println("error: placeholder_array is empty")
        return 1
    } else {
        println("ok: placeholder_array is not empty, size: " + placeholder_array.size.toString())
        for (elem in placeholder_array) {
            let info = TypeInfo.of(elem);
            if (info.toString() != "default.TestData") {
                println("error: element is not of type default:TestData");
                return 1;
            }
        }
    }

    return 0
}
