package test

import std.time.*
import std.sync.*
import std.runtime.*
import std.core.sleep
import common.*

foreign func restoreInterpreterStackReserveSize(): Unit

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

public func do_test() {
	// forbid functions elimination by DCE
	blackBox(i2cThrowAndCatchException)
	blackBox(i2cThrowIllegalStateException)
	blackBox(i2cThrowOutFromCompiledCode)

	println("[AOT] main started")
	
	println("[AOT] test0: throw exception from interpreter mock and catch in AOT")
	var test0Passed = false
	try {
		c2i.invoke0()
	} catch (e: Exception) {
		println("[AOT] Caught exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test0: OK")
		test0Passed = true
	}
	if (!test0Passed) {
		println("[AOT] test0: error")
		throw Exception("[AOT] test0: error")
	}

	println("\n\n[AOT] test1: throw exception from interpreter mock and catch in interpreter")
	try {
		c2i.invoke1()
		println("[AOT] test1: OK")
	} catch (e: Exception) {
		println("[AOT] Caught exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test1: error")
		throw Exception("[AOT] test1: error")
	}
	

	println("\n\n[AOT] test2: do call sequence C->C2I->I->I2C->C, throw exception catch in first compiled caller")
	var test2Passed = false
	try {
		c2i.invoke2()
	} catch (e: Exception) {
		println("[AOT] Caught exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test2: OK")
		test2Passed = true
	}
	if (!test2Passed) {
		println("[AOT] test2: error")
		throw Exception("[AOT] test2: error")
	}

	println("\n\n[AOT] test3: do call sequence C->C2I->I->I2I->I, throw exception and catch in interpreter")
	println("\n\n[AOT] test3: do call sequence C->C2I->I->I2I->I->I2C->C, throw exception and catch in first compiled caller")
	var test3Passed = false
	try {
		c2i.invoke3()
	} catch (e: Exception) {
		println("[AOT] Caught exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test3: OK")
		test3Passed = true
	}
	if (!test3Passed) {
		println("[AOT] test3: error")
		throw Exception("[AOT] test3: error")
	}

	println("\n\n[AOT] test4: throw ArithmeticException from interpreter mock and catch in AOT")
	var test4Passed = false
	try {
		c2i.invoke4()
	} catch (e: ArithmeticException) {
		println("[AOT] Caught ArithmeticException from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test4: OK")
		test4Passed = true
	} catch (e: Exception) {
		println("[AOT] Caught Exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test4: error, expected ArithmeticException")
		throw Exception("[AOT] test4: error, expected ArithmeticException")
	}
	if (!test4Passed) {
		println("[AOT] test4: error")
		throw Exception("[AOT] test4: error")
	}

	println("\n\n[AOT] test5: throw NoneValueException from interpreter mock and catch in AOT")
	var test5Passed = false
	try {
		c2i.invoke5()
	} catch (e: NoneValueException) {
		println("[AOT] Caught NoneValueException from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test5: OK")
		test5Passed = true
	} catch (e: Exception) {
		println("[AOT] Caught Exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test5: error, expected NoneValueException")
		throw Exception("[AOT] test5: error, expected NoneValueException")
	}
	if (!test5Passed) {
		println("[AOT] test5: error")
		throw Exception("[AOT] test5: error")
	}

	println("\n\n[AOT] test6: throw OutOfMemoryError from interpreter mock and catch in AOT")
	var test6Passed = false
	try {
		c2i.invoke6()
	} catch (e: OutOfMemoryError) {
		println("[AOT] Caught OutOfMemoryError from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test6: OK")
		test6Passed = true
	} catch (e: Exception) {
		println("[AOT] Caught Exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test6: error, expected OutOfMemoryError")
		throw Exception("[AOT] test6: error, expected OutOfMemoryError")
	}
	if (!test6Passed) {
		println("[AOT] test6: error")
		throw Exception("[AOT] test6: error")
	}

	println("\n\n[AOT] test7: prepare for stack overflow test by setting reserve size in mock")
	try {
		c2i.invoke7()
		println("[AOT] test7: OK")
	} catch (e: Exception) {
		println("[AOT] Caught Exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test7: error")
		throw Exception("[AOT] test7: error")
	}
	
	println("\n\n[AOT] test8: stack overflow")
	var test8Passed = false
	try {
		c2i.invoke8()
	} catch (e: StackOverflowError) {
		println("[AOT] Caught StackOverflowError from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test8: OK")
		test8Passed = true
	} finally {
		unsafe { restoreInterpreterStackReserveSize() }
	}
	if (!test8Passed) {
		println("[AOT] test8: error - should have thrown an exception")
		throw Exception("[AOT] test8: error - should have thrown an exception")
	}

	println("\n\n[AOT] test9: dump all stack traces while in C2I/I2I")
	try {
		c2i.invoke9()
		println("[AOT] test9: OK")
	} catch (e: Exception) {
		println("[AOT] Caught Exception from mock: " + e.toString())
		e.printStackTrace()
		println("[AOT] test9: error")
		throw Exception("[AOT] test9: error")
	}
	
	println("[AOT] finish main")
}

main() {
	do_test()
}
