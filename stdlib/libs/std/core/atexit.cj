/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * atexitCallback will be executed, when the program exits.
 *
 * Registration protocol: lock-free Treiber-style push onto `atexitCallbackHead`
 * (compare-and-swap loop).
 *
 * Why lock-free: the previous implementation executed ALL user callbacks while
 * holding the native registration spinlock (a pure atomic_flag test_and_set
 * loop with no yield). A thread that registered concurrently with teardown
 * either busy-spun on that lock for the whole teardown (one core per spinner)
 * or was silently dropped by the callbacksRunningFlag guard, and a teardown
 * callback that blocked on such a spinner (the usual flush-and-join cleanup)
 * made exit() never return. Additionally, the runtime scheduler parks its
 * worker threads during exit; a thread parked inside the tiny lock window left
 * the lock held forever, wedging every later registrant. A CAS loop has no
 * lock to leak, never blocks, and at worst retries.
 */

@FastNative
foreign func CJ_CORE_AtExitCallbackListLock(): Unit

@FastNative
foreign func CJ_CORE_AtExitCallbackListUnlook(): Unit

// The node of atexitCallback function.
open class AtexitCallbackNode {
    var atexitCallback: () -> Unit
    var next: ?AtexitCallbackNode = None

    init(callback: () -> Unit) {
        atexitCallback = callback
    }
}

// atexitCallback list head. Accessed via CAS (Treiber stack); an empty list
// is a head pointing at the placeholder sentinel.
var atexitCallbackHeadBox = AtomicReference<AtexitCallbackNode>(AtexitCallbackNodePlaceholder())

// Sentinel object so AtomicReference can model an empty list: an empty list
// is a head pointing at the placeholder.
open class AtexitCallbackNodePlaceholder <: AtexitCallbackNode {
    init() {
        super(atexitCallbackStub)
    }
}

func atexitCallbackStub(): Unit {
    ()
}

var callbacksRunningFlag: Bool = false // the flag whether callbacks is being executed

// Add an atexitCallback Function to list.
protected func CJ_CORE_AddAtexitCallback(callback: () -> Unit): Unit {
    // NOTE: no callbacksRunningFlag guard here. The lock-free CAS push is safe
    // at any time (including during teardown), so a registration made inside a
    // callback or by another thread during teardown is appended to the fresh
    // list head and picked up by the drain loop in CJ_CORE_ExecAtexitCallbacks
    // (C atexit-style chained semantics) instead of being silently dropped.

    var newNode = AtexitCallbackNode(callback)
    while (true) {
        let oldHead = atexitCallbackHeadBox.load()
        newNode.next = Some(oldHead)
        if (atexitCallbackHeadBox.compareAndSwap(oldHead, newNode)) {
            return
        }
        // CAS failed: another thread changed the head; retry with fresh head.
    }
}

// Execute all atexitCallback functions. Reverse order of addition.
protected func CJ_CORE_ExecAtexitCallbacks(): Unit {
    if (callbacksRunningFlag) { // Prevent deadlock caused by calling CJ_CORE_ExecAtexitCallbacks inside a callback function
        return
    }
    // Claim the teardown. The native lock is only taken for this flag flip and
    // the single head swap below; it is NEVER held across user callbacks, so a
    // blocking callback cannot stall registrants and a parked thread cannot
    // leak the lock.
    unsafe { CJ_CORE_AtExitCallbackListLock() }
    if (callbacksRunningFlag) { // Prevent multiple threads from calling CJ_CORE_ExecAtexitCallbacks, which causes the callback function to be executed multiple times.
        unsafe { CJ_CORE_AtExitCallbackListUnlook() }
        return
    }
    callbacksRunningFlag = true
    // Detach the whole list with a CAS loop. A plain load()+store() here
    // would race the lock-free registration path (load X; registrant
    // CAS(X -> node); store placeholder erases the node's only inbound edge)
    // and silently drop that callback. The CAS loop guarantees the swap only
    // succeeds when the head is still the value we read, so a concurrent
    // push either lands before the swap (and is included in the detached
    // chain) or after it (and lands on the fresh sentinel head, which the
    // drain loop below picks up).
    var detached: ?AtexitCallbackNode = None
    while (true) {
        let old = atexitCallbackHeadBox.load()
        if (atexitCallbackHeadBox.compareAndSwap(old, AtexitCallbackNodePlaceholder())) {
            detached = Some(old)
            break
        }
        // CAS failed: a concurrent registration changed the head; retry with
        // the fresh head so the new node is not lost.
    }
    var currPos: ?AtexitCallbackNode = detached
    unsafe { CJ_CORE_AtExitCallbackListUnlook() }

    // Run the callbacks with NO lock held. Registrations that arrive DURING
    // teardown CAS onto the fresh sentinel head; drain them too (C-atexit
    // chained semantics) until the list is exhausted. A registration made by
    // a callback in the SAME thread is also honored this way, so the old
    // silent-drop for chained registration is gone as well.
    while (true) {
        match (currPos) {
            case Some(node) =>
                if (!(node is AtexitCallbackNodePlaceholder)) {
                    try {
                        node.atexitCallback()
                    } catch (t: Exception) {
                        t.printStackTrace()
                    } catch (t: Error) {
                        t.printStackTrace()
                    }
                }
                currPos = node.next
            case None =>
                // Drained everything visible so far. Atomically swap out any
                // callbacks registered during teardown (fresh head); finish
                // when the head is back to the sentinel.
                let newHead = atexitCallbackHeadBox.load()
                if (newHead is AtexitCallbackNodePlaceholder) {
                    break
                }
                if (atexitCallbackHeadBox.compareAndSwap(newHead, AtexitCallbackNodePlaceholder())) {
                    currPos = Some(newHead)
                }
                // CAS failed: someone concurrently pushed; retry the swap.
        }
    }
}
