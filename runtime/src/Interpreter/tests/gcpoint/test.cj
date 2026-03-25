import std.time.*
import std.runtime.*
import std.core.sleep
import common.*

main() {
	println("[AOT] main started")
	println("[AOT] creating interpreter thread:")
	let futureInterpreter = spawn {
		println("[AOT] Transfer execution to interpreter")
		c2i.invoke0()
    }

	sleep(Duration.millisecond * 300)
	
	println("[AOT] creating allocator thread:")
	let futureAllocator = spawn {
		println("[AOT] Allocator started")
		for (i in 0..100000000) {
			let o = Object()
		}
    }

	for (i in 0..5) {
		gc(heavy: false)
		sleep(Duration.second)
		println("[AOT] GC count = ${getGCCount()}")
	}

	futureInterpreter.get()
	futureAllocator.get()
	println("[AOT] finish main")
}
