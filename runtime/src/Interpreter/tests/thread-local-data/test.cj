package test

import std.time.*
import std.sync.*
import std.runtime.*
import std.core.sleep
import common.*

public var static_field_for_fastpath_barriers: Object = Object()
public var globalObjectWithRecField: ObjectWithRecField = ObjectWithRecField()

let shouldStop = AtomicBool(false)

public class ObjectContainer {
    public var obj: Object 
    
    init() {
        obj = Object()
    }
}

public struct RecField {
	public var obj0: Object = Object()
	public var obj1: Object = Object()
	public var obj2: Object = Object()
	public var obj3: Object = Object()
}

public class ObjectWithRecField {
	public var rec: RecField = RecField()
}

var allocatedObject: Object = Object()
public func i2cAllocationLoop() {
	for (i in 0..100000) {
		allocatedObject = Object()
	}
}


public func do_test() {
	blackBox(i2cAllocationLoop)
	println("[AOT] main started")
	
	let futureAllocator = spawn {
		println("[AOT] Allocator started")
		while (!shouldStop.load()) {
			let o = Object()
		}
    }

	let futureInterpreter = spawn {
		c2i.invoke8() // init
		println("[AOT] Calling interpreter mock")
		c2i.invoke0()
		println("[AOT] test finished\n\n")

		shouldStop.store(true)
    }

	while (!shouldStop.load()) {
		gc(heavy: false)
		sleep(Duration.second)
		println("[AOT] GC count = ${getGCCount()}")
	}
	println("[AOT] finish main loop")

	futureInterpreter.get()
	println("[AOT] joined with interpreter")
	futureAllocator.get()
	println("[AOT] joined with allocator")
	println("[AOT] finish main")
}

main() {
	do_test()
}
