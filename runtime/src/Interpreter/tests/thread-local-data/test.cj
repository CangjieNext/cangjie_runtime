import std.time.*
import std.sync.*
import std.runtime.*
import std.core.sleep
import common.*

public var continueAllocation = true
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


main() {
	blackBox(i2cAllocationLoop)
	println("[AOT] main started")
	
	let futureAllocator = spawn {
		println("[AOT] Allocator started")
		while (continueAllocation && !shouldStop.load()) {
			let o = Object()
		}
    }

	let futureInterpreter = spawn {
		println("[AOT] Calling interpreter mock")
		c2i.invoke0()
		println("[AOT] test finished\n\n")
		c2i.invoke1()
    }

	let timer = spawn { // timer for total test execution, because continueAllocation flag is not volatile
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

	futureInterpreter.get()
	println("[AOT] joined with interpreter")
	futureAllocator.get()
	println("[AOT] joined with allocator")
	println("[AOT] finish main")
}
