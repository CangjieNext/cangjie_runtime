import std.time.*
import std.runtime.*
import std.core.sleep
import common.*

var testField: String = "Initial value"

func test(producer: () -> Unit, msg: String): Unit {
    for (i in 0..3) {
        println(msg)

        producer()
        println("Before GC: ${testField}")

		gc(heavy: false)
		sleep(Duration.second * 1)

        println("After GC: ${testField}")
	}
}

main() {
    println(testField)

    test(c2i.invoke0, "[App] call produceSlow1")
    test(c2i.invoke1, "[App] call produceSlow2")

    test(c2i.invoke2, "[App] call produceFast1")
    test(c2i.invoke3, "[App] call produceFast2")

    println("GC count: ${getGCCount()}")
}
