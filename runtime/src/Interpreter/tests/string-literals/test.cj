package test

import std.time.*
import std.runtime.*
import std.core.sleep
import common.*

var testField: String = "Initial value"

func test(producer: () -> Unit, msg: String): Unit {
    for (i in 0..3) {
        println(msg)

        producer()
        println("[AOT] Before GC: ${testField}")

		gc(heavy: false)
		sleep(Duration.second * 1)

        println("[AOT] After GC: ${testField}")
	}
}

public func do_test() {
    println("[AOT] Initial value: ${testField}")

    c2i.invoke0() // init static string literal

    test(c2i.invoke1, "[AOT] call produceSlow1")
    test(c2i.invoke2, "[AOT] call produceSlow2")

    test(c2i.invoke3, "[AOT] call produceFast1")
    test(c2i.invoke4, "[AOT] call produceFast2")

    println("[AOT] GC count: ${getGCCount()}")
    println("[AOT] finish main")
}

main() {
	do_test()
}
