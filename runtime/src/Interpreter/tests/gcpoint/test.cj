package test

import std.time.*
import std.runtime.*
import std.sync.*
import std.core.sleep
import common.*

let shouldStop = AtomicBool(false)

public func do_test() {
	println("[AOT] main started")
	println("[AOT] creating interpreter thread:")
	let futureInterpreter = spawn {
		println("[AOT] Transfer execution to interpreter")
		c2i.invoke0()
		shouldStop.store(true)
    }
	sleep(Duration.millisecond * 100)

	
	println("[AOT] creating allocator thread:")
	let futureAllocator = spawn {
		println("[AOT] Allocator started")
		while (!shouldStop.load()) {
			let o = Object()
		}
    }

	while (!shouldStop.load()) {
		gc(heavy: false)
		sleep(Duration.second)
		println("[AOT] GC count = ${getGCCount()}")
	}

	futureInterpreter.get()
	futureAllocator.get()
	println("[AOT] finish main")
}

main() {
	do_test()
}
