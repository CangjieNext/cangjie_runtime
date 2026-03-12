import std.time.*
import std.sync.*
import std.runtime.*
import std.core.sleep
import common.*

public var continueAllocation = true

let shouldStop = AtomicBool(false)

public func foo() {
	throw IllegalStateException()
}

public func i2cThrowAndCatchException() {
	println("[AOT] i2cThrowAndCatchException called")
	try {
		foo()
	} catch (e: IllegalStateException) {
		println("[AOT] Caught exception from compiled code: " + e.toString())
		println("[AOT] i2cThrowAndCatchException: OK")
	}
}

public func i2cThrowIllegalStateException() {
	throw IllegalStateException("IllegalStateException thrown from compiled code")
	println("[AOT] error: unreachable code in i2cThrowIllegalStateException\n");
}

public func i2cThrowOutFromCompiledCode() {
	throw Exception("Exception thrown from compiled code")
}

main() {
	// forbid functions elimination by DCE
	blackBox(i2cThrowAndCatchException)
	blackBox(i2cThrowIllegalStateException)
	blackBox(i2cThrowOutFromCompiledCode)

	println("[AOT] main started")
	
	println("[AOT] test0: throw exception from interpreter mock and catch in AOT")
	try {
		c2i.invoke0()
		println("[AOT] test0: error")
	} catch (e: Exception) {
		println("[AOT] Caught exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test0: OK")
	}

	println("\n\n[AOT] test1: throw exception from interpreter mock and catch in interpreter")
	try {
		c2i.invoke1()
		println("[AOT] test1: OK")
	} catch (e: Exception) {
		println("[AOT] Caught exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test1: error")
	}
	

	println("\n\n[AOT] test2: do call sequence C->C2I->I->I2C->C, throw exception catch in first compiled caller")
	try {
		c2i.invoke2()
		println("[AOT] test2: error")
	} catch (e: Exception) {
		println("[AOT] Caught exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test2: OK")
	}

	println("\n\n[AOT] test3: do call sequence C->C2I->I->I2I->I, throw exception and catch in interpreter")
	println("\n\n[AOT] test3: do call sequence C->C2I->I->I2I->I->I2C->C, throw exception and catch in first compiled caller")
	try {
		c2i.invoke3()
		println("[AOT] test3: error")
	} catch (e: Exception) {
		println("[AOT] Caught exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test3: OK")
	}

	println("\n\n[AOT] test4: throw ArithmeticException from interpreter mock and catch in AOT")
	try {
		c2i.invoke4()
		println("[AOT] test4: error")
	} catch (e: ArithmeticException) {
		println("[AOT] Caught ArithmeticException from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test4: OK")
	} catch (e: Exception) {
		println("[AOT] Caught Exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test4: error, expected ArithmeticException")
	}

	println("\n\n[AOT] test5: throw NoneValueException from interpreter mock and catch in AOT")
	try {
		c2i.invoke5()
		println("[AOT] test5: error")
	} catch (e: NoneValueException) {
		println("[AOT] Caught NoneValueException from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test5: OK")
	} catch (e: Exception) {
		println("[AOT] Caught Exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test5: error, expected NoneValueException")
	}

	println("\n\n[AOT] test6: throw OutOfMemoryError from interpreter mock and catch in AOT")
	try {
		c2i.invoke6()
		println("[AOT] test6: error")
	} catch (e: OutOfMemoryError) {
		println("[AOT] Caught OutOfMemoryError from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test6: OK")
	} catch (e: Exception) {
		println("[AOT] Caught Exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test6: error, expected OutOfMemoryError")
	}
	
	println("\n\n[AOT] test7: prepare for stack overflow test by setting reserve size in mock")
	try {
		c2i.invoke7()
		println("[AOT] test7: OK")
	} catch (e: Exception) {
		println("[AOT] Caught Exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test7: error")
	}
	
	println("\n\n[AOT] test8: stack overflow")
	try {
		c2i.invoke8()
		println("[AOT] test8: error - should have thrown an exception")
	} catch (e: StackOverflowError) {
		println("[AOT] Caught StackOverflowError from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test8: OK")
	}
	
	println("[AOT] finish main")
}
