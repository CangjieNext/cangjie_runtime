import std.time.*
import std.sync.*
import std.runtime.*
import std.core.sleep
import common.*

public var continueAllocation = true
let shouldStop = AtomicBool(false)

foreign func set_callback(cb: CFunc<(Int32) -> Unit>): Unit
foreign func call_callback(v: Int32): Unit

@C
func myCallback(s: Int32): Unit {
    println("[AOT @C] cangjie callback: ${s}")
	if (s == 1234) {
		println("[AOT @C] OK")
	}
	if (s < 0) {
    	throw Exception("Exception from callback")
	}
}

func setMyCallback() {
    unsafe {set_callback(myCallback)}
}

main() {
	println("[AOT] main started")
	setMyCallback()
	
	let futureAllocator = spawn {
		println("[AOT] Allocator started")
		while (continueAllocation && !shouldStop.load()) {
			let o = Object()
		}
    }

	let futureInterpreter = spawn {
		println("[AOT] Interpreter thread started, calling tests...")
		println("\n")
		c2i.invoke0()
		println("\n")
		c2i.invoke1()
		println("\n")
		c2i.invoke2()
		println("\n")
		try {
			c2i.invoke3()
			println("[AOT] #3 test: error")
			throw Exception("#3 test: error")
		} catch (e: Exception) {
			println("[AOT] Caught exception from callback: ${e}")
			println("[AOT] #3 test: OK")
		}

		c2i.invoke9() // stop
    }

	let timer = spawn { // timer for total test execution, if continueAllocation flag didn`t work
		sleep(Duration.second * 10)
		shouldStop.store(true)
	}

	while (continueAllocation && !shouldStop.load()) {
		gc(heavy: false)
		sleep(Duration.second)
		println("[AOT] GC count = ${getGCCount()}")
	}
	println("[AOT] finish main loop")
	continueAllocation = false
	shouldStop.store(true)

	futureInterpreter.get()
	println("[AOT] joined with interpreter")
	futureAllocator.get()
	println("[AOT] joined with allocator")
	println("[AOT] finish main")
}
