package test

import std.time.*
import std.runtime.*
import std.core.sleep
import std.sync.*
import common.*

public var n:    AtomicInt64 = AtomicInt64(5000)
public var elem: AtomicInt64 = AtomicInt64(1)

// Called from mock
public func test1() {
	for (i in 0..7) {
		gc(heavy: false)
		sleep(Duration.second * 1)
		println("GC count = ${getGCCount()}")
	}
}

// Called from mock
public func test2() {
	println("[AOT] Exec recursive function to trigger stack expansion")
	let res = nonTailRecursiveSummator(n.load())
	println("[AOT] summator result: ${res}")	
}

public func nonTailRecursiveSummator(i: Int64): Int64 {
	match (i) {
		case 0 => elem.load()
		case _ => elem.load() + nonTailRecursiveSummator(i - 1) + (elem.load() + 1)
	}
}

func subTest1() {
	c2i.invoke0()
}

func subTest2() {
	println("[AOT] Call function that creates stack ptr")
	c2i.invoke1()
}

public func do_test() {
	// forbid functions elimination by DCE
	blackBox(test1)
	blackBox(test2)
	
	c2i.invoke8() // init

	println("[AOT] --- Subtest 1: GC roots ---")
	subTest1()

	println("[AOT] --- Subtest 2: Stack expansion ---")
	subTest2()

	println("Finished")
}

main() {
	do_test()
}
