package blocking_queue_bench

import std.sync.*
import std.collection.ArrayList
/**
 * ENTRY<E> Type defines the result of getting an element at the head of the block:
 * - BlockDone: all of the element in the block have been consumed
 * - NoEntry: there is no element in the block
 * - ELEM(E): gets the element at the head of the block successfully
 */
enum ENTRY<E> {
    | BlockDone
    | NoEntry
    | ELEM(E)
}

/**
 * PushRes Type defines the result of pushing an element into buffer[index]:
 * - BlockDone: buffer is full
 * - CONFLICT: another thread sends the element into buffer[index]
 * - SUCCEED: sends element at the buffer[index] successfully
 */
enum PushRes {
    | BlockDone
    | CONFLICT
    | SUCCEED
}

const BUF_SIZE_BITS = 7 // shift for the size of Block.
const BUF_SIZE: Int64 = 1 << BUF_SIZE_BITS // the initial size of the buffer in the block.
class Block<E> {
    let version: UInt64 // identifier of the Block

    let isExtended = AtomicBool(false) // mark whether the Block should be extended
    let next = AtomicOptionReference<Block<E>>(None)
    private let allocated = AtomicInt64(0) // index of the entry for sending next element.
    let committed = AtomicInt64(0) // justifies whether there is unfinished senders.
    let reserved = AtomicInt64(0) // index of the entry for getting next element.
    let buffer = unsafe { Array<E>(BUF_SIZE, repeat: zeroValue<E>()) } // buffer for saving the elements

    init(version: UInt64) {
        this.version = version
    }

    /**
     * Sends the @p element into the buffer.
     *
     * @param element: the element to put into the buffer
     *
     * @return:
     * - true: sends @p element into the buffer successfully
     * - false: fails to send into the buffer
     */
    func push(element: E): Bool {
        // buffer full
        if (allocated.load() >= BUF_SIZE) {
            return false
        }

        let index = allocated.fetchAdd(1)

        // buffer full
        if (index >= BUF_SIZE) {
            return false
        }

        buffer[index] = element
        committed.fetchAdd(1)

        return true
    }

    /**
     * Sends the @p element at the buffer[@p index].
     *
     * @param element: the element to put into the buffer
     * @param index: the index of the buffer to store @p element
     *
     * @return:
     * - SUCCEED: sends @p element at the buffer[@p index] successfully
     * - BlockDone: buffer full
     * - CONFLICT: another thread sends the element into buffer[@p index]
     */
    func pushAt(element: E, index: Int64): PushRes {
        // buffer full
        if (index >= BUF_SIZE) {
            return BlockDone
        }

        // Exists another thread sends the element at buffer[@p index]
        if (!allocated.compareAndSwap(index, index + 1)) {
            return CONFLICT
        }

        buffer[index] = element
        committed.fetchAdd(1)

        return SUCCEED
    }

    /**
     * Gets and removes the element at the head of the buffer.
     *
     * @return:
     * - NoEntry: if there is no element in the buffer
     * - BlockDone: if all of the entries in the buffer have been consumed
     * - ELEM(e): if gets the element e at the head of the buffer successfully
     */
    func pop(): ENTRY<E> {
        while (true) {
            let index = reserved.load()

            // all of the entries in the buffer have been consumed.
            if (index >= BUF_SIZE) {
                return BlockDone
            }

            let currentCommitted = committed.load()
            if (currentCommitted != BUF_SIZE) {
                // Retry: existing unfinished senders in this block.
                if (currentCommitted != allocated.load()) {
                    continue
                }
            }

            // there is no element in the buffer
            if (index == currentCommitted) {
                return NoEntry
            }

            // get and remove the element of buffer[index]
            if (reserved.compareAndSwap(index, index + 1)) {
                let elem = ELEM(buffer[index])
                buffer[index] = unsafe { zeroValue<E>() }
                return elem
            }
        }

        return BlockDone
    }

    /**
     * Gets the element at the head of the buffer.
     *
     * @return
     * - NoEntry: if there is no element in the buffer
     * - BlockDone: if all of the entries in the buffer have been consumed
     * - ELEM(e): if gets the element e at the head of the buffer successfully
     */
    func front(): ENTRY<E> {
        while (true) {
            let index = reserved.load()

            // all of the entries in the buffer have been consumed.
            if (index >= BUF_SIZE) {
                return BlockDone
            }

            let currentCommitted = committed.load()
            if (currentCommitted != BUF_SIZE) {
                // Retry: existing unfinished senders in this block.
                if (currentCommitted != allocated.load()) {
                    continue
                }
            }

            // there is no element in the buffer.
            if (index == currentCommitted) {
                return NoEntry
            }

            // get the element of buffer[index].
            return ELEM(buffer[index])
        }

        return BlockDone
    }

    func getAllocated(): (UInt64, Int64) {
        return (version, allocated.load())
    }

    func getReserved(): (UInt64, Int64) {
        return (version, reserved.load())
    }
}
func newBlock<E>(tailBlock: Block<E>, tail: AtomicReference<Block<E>>): Unit {
    if (!tailBlock.isExtended.compareAndSwap(false, true)) {
        while (true) {
            if (let Some(_) <- tailBlock.next.load()) {
                return
            }
        }
    }

    let newBlock = Block<E>(tailBlock.version + 1)
    tailBlock.next.store(newBlock)
    tail.store(newBlock)
}

/**
 * Sets 'head' to point to the next block.
 */
func nextBlock<E>(headBlock: Block<E>, head: AtomicReference<Block<E>>, tail: AtomicReference<Block<E>>): Bool {
    if (!refEq(head.load(), headBlock)) {
        return true
    }

    if (let Some(next) <- headBlock.next.load()) {
        // Waits for ongoing allocation.
        while (refEq(headBlock, tail.load())) {}
        head.compareAndSwap(headBlock, next)
        return true
    }

    return false
}
