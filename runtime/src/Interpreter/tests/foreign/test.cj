package test

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

public func do_test() {
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
		var test3Passed = false
		try {
			c2i.invoke3()
		} catch (e: Exception) {
			println("[AOT] Caught exception from callback: ${e}")
			println("[AOT] #3 test: OK")
			test3Passed = true
		}
		if (!test3Passed) {
			println("[AOT] #3 test: error")
			throw Exception("#3 test: error")
		}

		// println("\n")
		// println("[AOT] #4 test: stack overflow in I2N transition")
		// try {
		// 	c2i.invoke4()
		// 	println("[AOT] #4 test: error")
		// 	throw Exception("#4 test: error")
		// } catch (e: StackOverflowError) {
		// 	println("[AOT] Caught StackOverflowError in I2N transition: ${e}")
		// 	println("[AOT] #4 test: OK")
		// }

		continueAllocation = false
		shouldStop.store(true)
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

main() {
	do_test()
}
