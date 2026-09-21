/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.sync

import std.time.MonoTime

const MAX_TIMEOUT_NS: Int64 = 0x7FFF_FFFF_FFFF_FFFF
let MAX_TIMEOUT_DURATION: Duration = Duration.nanosecond * MAX_TIMEOUT_NS

class NotifyOnce {
    let m = Monitor()
    var notified: Bool = false

    func notify(): Unit {
        synchronized(m) {
            if (!notified) {
                notified = true
                m.notify()
            }
        }
    }

    func wait(timeout!: Duration = Duration.Max): Bool {
        if (timeout >= MAX_TIMEOUT_DURATION) {
            waitWithoutTimeout()
        } else {
            waitWithTimeout(timeout)
        }
        return notified
    }

    private func waitWithoutTimeout(): Unit {
        synchronized(m) {
            // In case of spurious-wakeup, we should put `wait` inside a while loop.
            while (!notified) {
                m.wait()
            }
        }
    }

    private func waitWithTimeout(timeout: Duration): Unit {
        var restOfTimeout = timeout
        synchronized(m) {
            // In case of spurious-wakeup, we should put `wait` inside a while loop.
            while (!notified && restOfTimeout > Duration.Zero) {
                // Use a monotonic clock to compute the remaining timeout:
                // wall-clock steps (NTP, manual date changes) must not distort
                // the duration of a timed wait.
                let startTime = MonoTime.now()
                m.wait(timeout: restOfTimeout)
                let endTime = MonoTime.now()
                restOfTimeout -= (endTime - startTime)
            }
        }
    }
}

// Linked-list node
class SyncNode {
    let n = NotifyOnce()
    var next: ?SyncNode = None
    var prev: ?SyncNode = None
    var dead: Bool = false
}

// SyncList is used to achieve "unpark-all" behaviour
class SyncList {
    private var head: ?SyncNode = None
    private let m = Monitor()

    private func addWaiter(node: SyncNode) {
        node.next = head
        if (let Some(h) <- head) {
            h.prev = node
        }
        head = node
    }

    // Remove a (possibly dead) node from the linked list.
    // Must be called while holding `m`.
    private func removeNode(node: SyncNode) {
        if (let Some(p) <- node.prev) {
            p.next = node.next
        } else {
            head = node.next
        }
        if (let Some(n) <- node.next) {
            n.prev = node.prev
        }
        node.next = None
        node.prev = None
    }

    // Mark a node as dead and remove it from the list.
    // Used when a timed wait times out so the stale node can no longer
    // consume a subsequent `notifyOne`.
    private func markDead(node: SyncNode) {
        synchronized(m) {
            if (!node.dead) {
                node.dead = true
                removeNode(node)
            }
        }
    }

    func waitIf(pred: () -> Bool, timeout!: Duration = Duration.Max): Unit {
        if (!pred()) {
            return
        }
        let node = SyncNode()
        synchronized(m) {
            if (!pred()) {
                return
            }
            addWaiter(node)
        }
        node.n.wait(timeout: timeout)
        if (!node.n.notified) {
            // timed out: the node was not signalled, remove it if still linked
            markDead(node)
        }
    }

    func waitInLock(unlock: () -> Unit, timeout!: Duration = Duration.Max): Bool {
        let node = SyncNode()
        synchronized(m) {
            addWaiter(node)
        }
        unlock()
        let ok = node.n.wait(timeout: timeout)
        if (!ok) {
            markDead(node)
        }
        return ok
    }

    // Release one sema and remove it from the head of queue
    func notifyOne(): Bool {
        synchronized(m) {
            // Skip any stale (dead) nodes lingering at the head: a timed-out
            // waiter's node must not consume this wake-up.
            while (true) {
                if (let Some(current) <- head) {
                    if (current.dead) {
                        head = current.next
                        if (let Some(n) <- current.next) {
                            n.prev = None
                        }
                        current.next = None
                        continue
                    }
                }
                break
            }
            if (let Some(current) <- head) {
                head = current.next
                if (let Some(n) <- current.next) {
                    n.prev = None
                }
                current.next = None
                current.n.notify()
                return true
            }
            return false
        }
    }

    // Notify all node in linked-list which head is the first node.
    // After this method, head node would be None
    func notifyAll(): Bool {
        synchronized(m) {
            if (let None <- head) {
                return false
            }
            while (let Some(current) <- head) {
                head = current.next
                if (let Some(n) <- current.next) {
                    n.prev = None
                }
                current.next = None
                if (!current.dead) {
                    current.n.notify()
                }
            }
            return true
        }
    }
}
