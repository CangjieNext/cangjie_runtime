import std.time.*
import std.runtime.*
import std.core.sleep
import std.sync.*
import common.*

public func test() {
	for (i in 0..3) {
		gc(heavy: false)
		sleep(Duration.second * 1)
		println("GC count = ${getGCCount()}")
	}
}

public var n:    AtomicInt64 = AtomicInt64(100000)
public var elem: AtomicInt64 = AtomicInt64(1)

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
	println("[AOT] Save pointer to stack inside mock")
	c2i.invoke1()

	println("[AOT] Exec recursive function to trigger stack expansion")
	let res = nonTailRecursiveSummator(n.load())
	println("[AOT] summator result: ${res}")

	println("[AOT] Check that stack pointers were adjusted")
	c2i.invoke2()
}

main() {
	// forbid functions elimination by DCE
	blackBox(test)
	
	c2i.invoke8() // init

	println("[AOT] --- Subtest 1: GC roots ---")
	subTest1()

	println("[AOT] --- Subtest 2: Stack expansion ---")
	subTest2()

	println("Finished")
}
