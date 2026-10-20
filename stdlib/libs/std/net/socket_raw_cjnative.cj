/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.net

import std.sync.*

/*
 * This API supplies users with the basic functionality to create and use sockets of different domains and types.
 * For example, based on this API user should be able to create sockets in following domains: AF_PACKET, AF_NETLINK,
 * and in following types: SOCK_RAW, SOCK_SEQPACKET.
 * This is a low-level API, not recommended for general and direct usage. Users are encouraged to construct their own
 * higher level APIs based on this one to access a socket of specific domain and/or type.
 * For general usage of TCP, UDP, UDS in stream or datagram mode, it's encouraged to use existing corresponding
 * Cangjie APIs rather than this one.
 * Implementation note:
 * This API is an thin interface between Cangjie code and system calls related to socket.
 * To enable users to access these system calls, the implementation makes sure that calling these wrapped cangjie API
 * will not block underlying system thread.
 */
/**
 * Raw-socket lifecycle state, encoded into a single atomic UInt32 so that
 * "start an I/O session" and "close" can be coordinated with one CAS loop,
 * mirroring the SocketState/SocketHolder protocol used by Tcp/Udp/Unix
 * sockets (socket_state.cj / socket_holder.cj):
 *
 * bit 31   : disposing (close has won the race and will release the fd)
 * bit 30   : close-requested (close() entered, fd not yet released)
 * bit 29   : bound
 * bits 0-28: number of running I/O sessions (receive/receiveFrom/send/sendTo/)
 *            accept/connect/get|setSocketOption/local|remoteAddress getters)
 *
 * close() waits (with the process-global closeCondition) until the session
 * counter drains to 0, so the fd can never be closed and reused by another
 * socket while an I/O operation is in flight on it. I/O sessions sample
 * the fd only while holding a session, so they always address the socket
 * they were started on.
 */
struct RawSocketLifecycle {
    private static const DISPOSING_MASK: UInt32 = 1u32 << 31
    private static const CLOSING_MASK: UInt32 = 1u32 << 30
    private static const BOUND_MASK: UInt32 = 1u32 << 29
    private static const COUNTER_MASK: UInt32 = 0x1FFF_FFFFu32

    RawSocketLifecycle(var value: UInt32) {}

    mut prop disposing: Bool {
        get() {
            (value & DISPOSING_MASK) != 0
        }
        set(b) {
            if (b) {
                value |= DISPOSING_MASK
            } else {
                value &= !DISPOSING_MASK
            }
        }
    }

    mut prop closing: Bool {
        get() {
            (value & (DISPOSING_MASK | CLOSING_MASK)) != 0
        }
        set(b) {
            if (b) {
                value |= CLOSING_MASK
            } else {
                value &= !CLOSING_MASK
            }
        }
    }

    mut prop bound: Bool {
        get() {
            (value & BOUND_MASK) != 0
        }
        set(b) {
            if (b) {
                value |= BOUND_MASK
            } else {
                value &= !BOUND_MASK
            }
        }
    }

    mut prop sessions: UInt32 {
        get() {
            value & COUNTER_MASK
        }
        set(v) {
            if ((v & !COUNTER_MASK) != 0) {
                throw OverflowException("RawSocket session counter overflow.")
            }
            value = (value & !COUNTER_MASK) | v
        }
    }
}

@When[backend == "cjnative"]
public class RawSocket {
    let sockImpl: RawSocketImpl
    /* Lifecycle protocol: sessions coordinate with close() through a single
     * atomic word (see RawSocketLifecycle). A non-blocking I/O operation must
     * hold a session for its whole duration; close() may only release the fd
     * after the session counter drains to zero, so the fd number cannot be
     * recycled and alias another socket while an operation is in flight. */
    private let lifecycle = AtomicUInt32(0)
    private var readTimeout_: ?Duration = None
    private var writeTimeout_: ?Duration = None
    var localAddr_: ?RawAddress = None
    var remoteAddr_: ?RawAddress = None

    prop sockFd: Int64 {
        get() {
            this.sockImpl.sockFd
        }
    }

    /* Get local socket address */
    @Deprecated[message: "Use `public prop localAddress: RawAddress` instead."]
    public prop localAddr: RawAddress {
        get() {
            throwIfClosed()
            withSession {
                localAddr_ ?? getLocalAddress()
            }
        }
    }

    /* Get local socket address */
    public prop localAddress: RawAddress {
        get() {
            throwIfClosed()
            withSession {
                localAddr_ ?? getLocalAddress()
            }
        }
    }

    /* Get remote socket address */
    @Deprecated[message: "Use `public prop remoteAddress: RawAddress` instead."]
    public prop remoteAddr: RawAddress {
        get() {
            throwIfClosed()
            withSession {
                remoteAddr_ ?? getPeerAddress()
            }
        }
    }

    /* Get remote socket address */
    public prop remoteAddress: RawAddress {
        get() {
            throwIfClosed()
            withSession {
                remoteAddr_ ?? getPeerAddress()
            }
        }
    }

    /* Get or set timeout for socket read */
    public mut prop readTimeout: ?Duration {
        get() {
            throwIfClosed()
            this.readTimeout_
        }
        set(v) {
            throwIfClosed()
            this.readTimeout_ = v?.throwIfNegative("Read timeout")
        }
    }

    /* Get or set timeout for socket write */
    public mut prop writeTimeout: ?Duration {
        get() {
            throwIfClosed()
            this.writeTimeout_
        }
        set(v) {
            throwIfClosed()
            this.writeTimeout_ = v?.throwIfNegative("Write timeout")
        }
    }

    private init(fd: Int64) {
        this.sockImpl = RawSocketImpl(fd)
    }

    /**
     * Create a socket with specific domain, type and protocol.
     *
     * @param domain The socket communication domain defined in struct SocketDomain.
     * @param sockType The socket type defined in struct SocketType.
     * @param protocol The socket protocol defined in struct ProtocolType.
     *
     * @throws SocketException if fail to create socket.
     */
    public init(domain: SocketDomain, `type`: SocketType, protocol: ProtocolType) {
        unsafe {
            let cNet = LibC.mallocCString("raw")
            var sockFd = CJ_MRT_SockCreate(domain.val, `type`.val, protocol.val, cNet)
            LibC.free(cNet)
            if (sockFd < 0) {
                socketProcessErrno(ErrnoLabel.CreateSock)
            }
            this.sockImpl = RawSocketImpl(sockFd)
        }
    }

    /**
     * Assign an address to the socket.
     *
     * @param addr The socket address stored in byte array.
     *
     * @throws SocketException if fail to bind address.
     */
    public func bind(addr: RawAddress): Unit {
        throwIfClosed()
        match (localAddr_) {
            case Some(_) => throw SocketException("Failed to bind: socket already has bound address.")
            case None =>
                withSession {
                    this.sockImpl.bind(addr)
                    this.localAddr_ = getLocalAddress()
                    var state = RawSocketLifecycle(lifecycle.load())
                    state.bound = true
                    lifecycle.store(state.value)
                }
        }
    }

    /**
     * Mark the socket as a passive socket to accept incoming connection.
     *
     * @param backlog The maximum length of the pending connections queue.
     *
     * @throws SocketException if fail to listen.
     */
    public func listen(backlog: Int32): Unit {
        throwIfClosed()
        withSession {
            let sockFd = unsafe { CJ_MRT_SockListen(this.sockFd, backlog) }
            if (sockFd < 0) {
                socketProcessErrno(ErrnoLabel.Listen)
            }
            this.sockImpl.sockFd = sockFd
        }
    }

    /**
     * Accept a connecting socket, waiting for one if there are no pending connection requests within the specified timeout.
     * If the timeout is `None`, then acception attempts will continue without time limit.
     * If the timeout less than or equal to Duration.Zero, it will timeout immediately.
     *
     * @param timeout The maxium waiting duration for getting a connection.
     * @return RawSocket for communication.
     *
     * @throws IllegalArgumentException if timeout is negative.
     * @throws SocketTimeoutException if the specified timeout ellapsed before any connection request were made.
     * @throws SocketException when other error occurs.
     */
    public func accept(timeout!: ?Duration = None): RawSocket {
        let val = getTimeout(timeout)
        throwIfClosed()
        let size = try {
            this.localAddress.addr.size
        } catch (e: SocketException) {
            throw SocketException("Failed to accept: ${e.message}")
        }
        return withSession {
            let remoteAddr = Array<Byte>(size, repeat: 0)
            let (handle, addrPtr) = getAddrPointer(remoteAddr)
            let sockFd = if (val < 0) {
                unsafe { CJ_MRT_SockAccept(this.sockFd, addrPtr) }
            } else {
                unsafe { CJ_MRT_SockAcceptTimeout(this.sockFd, addrPtr, UInt64(val)) }
            }
            releaseAddrPointer(handle, addrPtr)
            if (sockFd < 0) {
                socketProcessErrno(ErrnoLabel.Accept)
            }
            let connSock = RawSocket(sockFd)
            connSock.localAddr_ = this.localAddr_
            connSock.remoteAddr_ = RawAddress(remoteAddr)
            return connSock
        }
    }

    /**
     * Connect the socket to the address specified by addr within the specified timeout.
     * If the timeout is `None`, then acception attempts will continue without time limit.
     *
     * @param addr The peer socket address.
     * @param timeout The maxium duration for connection.
     *
     * @throws IllegalArgumentException if timeout is negative.
     * @throws SocketTimeoutException if the specified timeout ellapsed before acception was made.
     * @throws SocketException when other error occurs.
     */
    public func connect(addr: RawAddress, timeout!: ?Duration = None): Unit {
        throwIfClosed()
        let val = getTimeout(timeout)
        // connect will not try to connect when timeout == 0
        if (val == 0) {
            throw SocketTimeoutException("Failed to connect: connect timeout.")
        }
        withSession {
            this.sockImpl.connect(addr, val)
            this.localAddr_ = getLocalAddress()
        }
    }

    /* Close the socket.
     *
     * Concurrent-safe shutdown (CR-10). The old implementation released the
     * fd immediately (`isClosed` check-then-act with no synchronization), so
     * an in-flight receive()/send() on another thread could keep issuing
     * recv()/wait() calls with the stale handle after close() returned; once
     * the OS recycled the fd number for a new socket, that stale handle
     * aliased the new socket (cross-socket datagram theft or a permanent
     * hang). The fix:
     *
     *  1. every fd-touching operation runs inside a session
     *     (withSession, see enterSession/leaveSession);
     *  2. close() first marks the socket closing, which makes every
     *     subsequent session-enter fail with "already closed" - a stale
     *     handle can therefore never start another recv/wait;
     *  3. then the fd is released through the runtime's own close protocol
     *     (CJ_MRT_SockClose => SchdfdDeregister), whose reference-counted
     *     deferred-close wakes and fails any thread parked in a
     *     runtime-level wait on this fd before the underlying fd is closed.
     *
     * A session whose wait is parked when close() runs is woken by the
     * runtime's deregister protocol and returns an error; its next session
     * attempt is rejected. A recv() already executing against the fd keeps
     * using the old kernel file object (it observes this socket's data or
     * an error), so the fd number can never alias a socket that a stale
     * handle still targets. */
    public func close(): Unit {
        /* Win the close race exactly once: mark closing first so no new
         * session can start (this is what stops stale-handle reuse). */
        while (true) {
            let before = RawSocketLifecycle(lifecycle.load())
            if (before.closing) {
                return
            }
            var next = before
            next.closing = true
            if (lifecycle.compareAndSwap(before.value, next.value)) {
                break
            }
        }

        /* Release the fd through the runtime's deferred-close protocol. */
        this.sockImpl.releaseFd()

        /* Publish completion. */
        while (true) {
            let before = RawSocketLifecycle(lifecycle.load())
            var next = before
            next.disposing = true
            if (lifecycle.compareAndSwap(before.value, next.value)) {
                break
            }
        }
    }

    /**
     * Transmit a message specified by buffer to another socket.
     *
     * @param addr The peer address.
     * @param buffer The array data to be send.
     * @param flag The flag of how to invoke this func.
     *
     * @throws SocketTimeoutException when timeout.
     * @throws SocketException when other error occurs.
     */
    public func sendTo(addr: RawAddress, buffer: Array<Byte>, flags: Int32): Unit {
        throwIfClosed()
        let timeout = this.writeTimeout_?.toNanoseconds() ?? -1
        return withSession {
            sockImpl.sendTo(addr.addr, buffer, flags, timeout)
        }
    }

    /**
     * Receive a message and store it in buffer.
     *
     * @param buffer The buffer for message to be stored in.
     * @param flag The flag of how to invoke this func.
     * @return tuple of peer address and the number of bytes sent.
     *
     * @throws SocketTimeoutException when timeout.
     * @throws SocketException when other error occurs.
     */
    public func receiveFrom(buffer: Array<Byte>, flags: Int32): (RawAddress, Int64) {
        throwIfClosed()
        let timeout = this.readTimeout_?.toNanoseconds() ?? -1
        let size = try {
            this.localAddress.addr.size
        } catch (e: SocketException) {
            throw SocketException("Failed to receive data: ${e.message}")
        }
        return withSession {
            sockImpl.receiveFrom(size, buffer, flags, timeout)
        }
    }

    /**
     * Transmit a message specified by buffer to another socket. This api is only used for connected socket.
     *
     * @param buffer The array data to be send.
     * @param flag The flag of how to invoke this func.
     *
     * @throws SocketTimeoutException when timeout.
     * @throws SocketException when other error occurs.
     */
    public func send(buffer: Array<Byte>, flags: Int32): Unit {
        throwIfClosed()
        let timeout = this.writeTimeout_?.toNanoseconds() ?? -1
        return withSession {
            sockImpl.send(buffer, flags, timeout)
        }
    }

    /**
     * Receive a message and store it in buffer. This api is only used for connected socket.
     *
     * @param buffer The buffer for message to be stored in.
     * @param flag The flag of how to invoke this func.
     * @return the number of bytes sent.
     *
     * @throws SocketTimeoutException when timeout.
     * @throws SocketException when other error occurs.
     */
    public func receive(buffer: Array<Byte>, flags: Int32): Int64 {
        throwIfClosed()
        let timeout = this.readTimeout_?.toNanoseconds() ?? -1
        return withSession {
            sockImpl.receive(buffer, flags, timeout)
        }
    }

    /**
     * Set socket option with value specified by @p value in byte array form.
     *
     * @param level Socket option level defined in struct OptionLevel or Int32 value.
     * @param option Socket option name defined in struct OptionName or Int32 value.
     * @param value The value of option name in CPointer form.
     * @param len The length of option name struct.

     * @throws SocketException if fail to set socket option.
     */
    public unsafe func setSocketOption(level: Int32, option: Int32, value: CPointer<Byte>, len: Int32): Unit {
        throwIfClosed()
        withSession {
            let ret = CJ_SockOptionSet(this.sockFd, level, option, CPointer<Unit>(value), len)
            if (ret != 0) {
                let message = formatError(CJ_SockErrnoGet())
                throw SocketException("Failed to set socket option '${option}': ${message}.")
            }
        }
    }

    /**
     * Get socket option value.
     *
     * @param level Socket option level defined in struct OptionLevel or Int32 value.
     * @param option Socket option name defined in struct OptionName or Int32 value.
     * @param value The value of option name to be stored in.
     * @param len The length of value to be stored in.
     *
     * @throws SocketException if fail to get socket option.
     */
    public unsafe func getSocketOption(level: Int32, option: Int32, value: CPointer<Byte>, len: CPointer<Int32>): Unit {
        throwIfClosed()
        withSession {
            let ret = CJ_SockOptionGet(this.sockFd, level, option, CPointer<Unit>(value), len)
            if (ret != 0) {
                let message = formatError(CJ_SockErrnoGet())
                throw SocketException("Failed to get socket option '${option}': ${message}.")
            }
        }
    }

    private func throwIfClosed(): Unit {
        if (isDisposed()) {
            SocketException.throwClosedException()
        }
    }

    /** True once close() has started (the fd will be released; no new sessions). */
    private func isDisposed(): Bool {
        let state = RawSocketLifecycle(lifecycle.load())
        return state.closing
    }

    /*
     * Session protocol, shared by all fd-touching operations:
     *
     * enterSession() CAS-increments the session counter and fails (returns
     * false) if close() has already been requested. leaveSession()
     * decrements the counter and, when it drains to zero, wakes a pending
     * close() so it can release the fd.
     */
    private func enterSession(): Bool {
        while (true) {
            let before = RawSocketLifecycle(lifecycle.load())
            if (before.closing) {
                return false
            }
            var next = before
            next.sessions += 1
            if (lifecycle.compareAndSwap(before.value, next.value)) {
                return true
            }
        }
        return false
    }

    @OverflowWrapping
    private func leaveSession(): Unit {
        while (true) {
            let before = RawSocketLifecycle(lifecycle.load())
            var next = before
            next.sessions -= 1
            if (lifecycle.compareAndSwap(before.value, next.value)) {
                break
            }
        }
        /* The closing flag is re-checked on the next session-enter; there
         * is no close-side waiter to notify in this protocol. */
    }

    /* Runs `block` under a session; throws "already closed" when the socket
     * is closing/closed, mirroring the previous throwIfClosed() behaviour. */
    private func withSession<R>(block: () -> R): R {
        if (!enterSession()) {
            SocketException.throwClosedException()
        }
        try {
            return block()
        } finally {
            leaveSession()
        }
    }

    private func getLocalAddress(): RawAddress {
        throwIfClosed()
        let addr = getAddress(this.sockFd, localAddrGet, "local")
        let localAddr = unsafe { addr.toRawAddress() }
        unsafe { addr.free() }
        return localAddr
    }

    private func getPeerAddress(): RawAddress {
        throwIfClosed()
        let addr = getAddress(this.sockFd, peerAddrGet, "remote")
        let peerAddr = unsafe { addr.toRawAddress() }
        unsafe { addr.free() }
        return peerAddr
    }
}

@When[os != "Windows" && backend == "cjnative"]
class RawSocketImpl {
    var sockFd: Int64
    /* CR-10: set by RawSocket.close() before the fd is released. The
     * receive/receiveFrom internal loops check this after every EAGAIN
     * wait so a call racing with close() cannot keep issuing recv() with
     * a stale (possibly fd-reused) handle. */
    let closed: AtomicBool = AtomicBool(false)

    init(sockFd: Int64) {
        this.sockFd = sockFd
    }

    func bind(addr: RawAddress): Unit {
        let (handle, addrPtr) = getAddrPointer(addr.addr)
        let sockFd = unsafe { CJ_MRT_SockBind(this.sockFd, addrPtr, 0) }
        releaseAddrPointer(handle, addrPtr)
        if (sockFd < 0) {
            socketProcessErrno(ErrnoLabel.Bind)
        }
        this.sockFd = sockFd
    }

    func connect(addr: RawAddress, timeout: Int64): Unit {
        let (handle, remotePtr) = getAddrPointer(addr.addr)
        let localPtr = CPointer<SockAddr>()
        let sockFd = if (timeout < 0) {
            unsafe { CJ_MRT_SockConnect(this.sockFd, localPtr, remotePtr) }
        } else {
            unsafe { CJ_MRT_SockConnectTimeout(this.sockFd, localPtr, remotePtr, UInt64(timeout)) }
        }
        releaseAddrPointer(handle, remotePtr)
        if (sockFd < 0) {
            socketProcessErrno(ErrnoLabel.Connect)
        }
        this.sockFd = sockFd
    }

    func sendTo(addr: Array<Byte>, buffer: Array<Byte>, flags: Int32, timeout: Int64): Unit {
        let totalSize = buffer.size
        var totalSent: Int64 = 0
        while (totalSent < totalSize) {
            let sizeToSend: Int64 = getOnceSendSize(totalSize - totalSent)
            var sentSize: Int32 = 0
            unsafe {
                let (handle, addrPtr) = getAddrPointer(addr)
                let bufCp = acquireArrayRawData(buffer)
                sentSize = CJ_MRT_SockSendtoNonBlock(
                    this.sockFd,
                    bufCp.pointer + totalSent,
                    UInt32(sizeToSend),
                    flags,
                    addrPtr
                )
                releaseArrayRawData(bufCp)
                releaseAddrPointer(handle, addrPtr)
                if (sentSize > 0) {
                    totalSent += Int64(sentSize)
                    continue
                } else if (sentSize == 0) {
                    throw SocketException("The socket is closed.")
                } else {
                    processSendResult(timeout)
                }
            }
        }
    }

    func send(buffer: Array<Byte>, flags: Int32, timeout: Int64): Unit {
        let bytesToWrite = buffer.size
        var bytesWritten: Int64 = 0
        while (bytesWritten < bytesToWrite) {
            let currentChunk: Int64 = getOnceSendSize(bytesToWrite - bytesWritten)
            var writeResult: Int32 = 0
            unsafe {
                let bufCp = acquireArrayRawData(buffer)
                writeResult = CJ_MRT_SockSendNonBlock(
                    sockFd,
                    bufCp.pointer + bytesWritten,
                    UInt32(currentChunk),
                    flags
                )
                releaseArrayRawData(bufCp)
                if (writeResult > 0) {
                    bytesWritten += Int64(writeResult)
                    continue
                } else if (writeResult == 0) {
                    throw SocketException("The socket is closed.")
                } else {
                    processSendResult(timeout)
                }
            }
        }
    }

    func receiveFrom(size: Int64, buffer: Array<Byte>, flags: Int32, timeout: Int64): (RawAddress, Int64) {
        var recvSize: Int32 = 0
        let addrArr = Array<Byte>(size, repeat: 0)
        while (true) {
            /* CR-10: same stale-handle guard as receive(), before recv(). */
            if (closed.load()) {
                SocketException.throwClosedException()
            }
            unsafe {
                let (handle, addrPtr) = getAddrPointer(addrArr)
                let bufCp = acquireArrayRawData(buffer)
                recvSize = CJ_MRT_SockRecvfromNonBlock(sockFd, bufCp.pointer, UInt32(buffer.size), flags, addrPtr)
                releaseArrayRawData(bufCp)
                releaseAddrPointer(handle, addrPtr)
                if (recvSize > 0) {
                    break
                } else if (recvSize == 0) {
                    return (RawAddress(addrArr), 0)
                } else {
                    processRecvResult(timeout)
                }
            }
        }
        return (RawAddress(addrArr), Int64(recvSize))
    }

    func receive(buffer: Array<Byte>, flags: Int32, timeout: Int64): Int64 {
        var recvSize: Int32 = 0
        while (true) {
            /* CR-10: the fd may already have been closed and recycled by a
             * new socket while we waited in a previous iteration; check
             * BEFORE every recv() so the stale handle can never read
             * another socket's queue. */
            if (closed.load()) {
                SocketException.throwClosedException()
            }
            unsafe {
                let bufCp = acquireArrayRawData(buffer)
                recvSize = CJ_MRT_SockRecvNonBlock(sockFd, bufCp.pointer, UInt32(buffer.size), flags)
                releaseArrayRawData(bufCp)
                if (recvSize > 0) {
                    break
                } else if (recvSize == 0) {
                    return 0
                } else {
                    processRecvResult(timeout)
                }
            }
        }
        return Int64(recvSize)
    }

    func processSendResult(timeout: Int64): Unit {
        unsafe {
            if (CJ_SockErrnoGet() != ERRNO_SOCK_EAGAIN) {
                socketProcessErrno(ErrnoLabel.Write)
            } else {
                let waitCode = if (timeout == -1) {
                    CJ_MRT_SockWaitSend(this.sockFd)
                } else {
                    CJ_MRT_SockWaitSendTimeout(this.sockFd, UInt64(timeout))
                }
                if (waitCode != 0) {
                    socketProcessErrno(ErrnoLabel.Write)
                }
            }
        }
    }

    func processRecvResult(timeout: Int64): Unit {
        unsafe {
            if (CJ_SockErrnoGet() != ERRNO_SOCK_EAGAIN) {
                socketProcessErrno(ErrnoLabel.Read)
            } else {
                let waitCode = if (timeout == -1) {
                    CJ_MRT_SockWaitRecv(this.sockFd)
                } else {
                    CJ_MRT_SockWaitRecvTimeout(this.sockFd, UInt64(timeout))
                }
                if (waitCode != 0) {
                    socketProcessErrno(ErrnoLabel.Read)
                }
            }
        }
    }

    func releaseFd(): Unit {
        /* CR-10: flip the impl-level closed flag BEFORE the fd leaves this
         * process, so in-flight receive loops stop re-issuing recv() with
         * the handle once the fd is closed and potentially reused. */
        closed.store(true)
        unsafe { CJ_MRT_SockClose(this.sockFd) }
    }
}

@When[os == "Windows" && backend == "cjnative"]
class RawSocketImpl {
    var sockFd: Int64
    var isBound: Bool = false
    /* CR-10: see the non-Windows RawSocketImpl comment. */
    let closed: AtomicBool = AtomicBool(false)
    protected var socketBufferPtr: CPointer<SocketBuffer> = CPointer<SocketBuffer>()
    var bufferInitialized: AtomicBool = AtomicBool(false)

    init(sockFd: Int64) {
        this.sockFd = sockFd
    }

    func bufferInit() {
        if (bufferInitialized.compareAndSwap(false, true)) {
            let readBufferSize = SOCK_READ_BUFFER_SIZE
            let writeBufferSize = SOCK_WRITE_BUFFER_SIZE
            socketBufferPtr = unsafe { CJ_SOCKET_BufferInit(this.sockFd, readBufferSize, writeBufferSize) }
            if (socketBufferPtr.isNull()) {
                throw SocketException("Failed to initialize socket read and write buffer.")
            }
        }
    }

    func bind(addr: RawAddress): Unit {
        let (handle, addrPtr) = getAddrPointer(addr.addr)
        let sockFd = unsafe { CJ_MRT_SockBind(this.sockFd, addrPtr, 0) }
        releaseAddrPointer(handle, addrPtr)
        if (sockFd < 0) {
            socketProcessErrno(ErrnoLabel.Bind)
        }
        this.sockFd = sockFd
        this.isBound = true
    }

    func connect(addr: RawAddress, timeout: Int64) {
        let (remoteHd, remotePtr) = getAddrPointer(addr.addr)
        let af = Int16(addr.addr[0])
        let sockTypeVal = try {
            getSocketOptionInt32(this.sockFd, SOL_SOCKET, SOCK_TYPE)
        } catch (e: SocketException) {
            releaseAddrPointer(remoteHd, remotePtr)
            throw SocketException("Failed to connect: ${e.message}.")
        }
        let (localHd, localPtr) = if (!this.isBound && isConnectionOriented(sockTypeVal)) {
            let localArr: Array<Byte>
            match {
                case af == AF_INET =>
                    localArr = Array<Byte>(IPV4_ADDR_LEN, repeat: 0)
                    localArr[0] = UInt8(AF_INET)
                case af == AF_INET6 =>
                    localArr = Array<Byte>(IPV6_ADDR_LEN, repeat: 0)
                    localArr[0] = UInt8(AF_INET6)
                case _ =>
                    releaseAddrPointer(remoteHd, remotePtr)
                    throw SocketException("Failed to connect: unsupported address family '${af}'.")
            }
            getAddrPointer(localArr)
        } else {
            (CPointerHandle<Byte>(), CPointer<SockAddr>())
        }
        let sockFd = if (timeout < 0) {
            unsafe { CJ_MRT_SockConnect(this.sockFd, localPtr, remotePtr) }
        } else {
            unsafe { CJ_MRT_SockConnectTimeout(this.sockFd, localPtr, remotePtr, UInt64(timeout)) }
        }
        if (!localPtr.isNull()) {
            releaseAddrPointer(localHd, localPtr)
        }
        releaseAddrPointer(remoteHd, remotePtr)
        if (sockFd < 0) {
            socketProcessErrno(ErrnoLabel.Connect)
        }
        this.sockFd = sockFd
    }

    func sendTo(addr: Array<Byte>, buffer: Array<Byte>, flags: Int32, timeout: Int64): Unit {
        this.bufferInit()
        var copyLen: Int32 = 0
        unsafe {
            let bufCp = acquireArrayRawData(buffer)
            // bufCp.pointer copy to socketBufferPtr
            copyLen = CJ_SOCKET_BufferWCopy(
                this.socketBufferPtr,
                bufCp.pointer,
                Int64(SOCK_WRITE_BUFFER_SIZE),
                Int32(buffer.size)
            )
            releaseArrayRawData(bufCp)
        }
        if (copyLen == 0) {
            throw SocketException("The socket is closed.")
        } else if (copyLen < 0) {
            throw SocketException("Failed to write data: native socket copy data error.")
        } else {
            unsafe {
                let (handle, addrPtr) = getAddrPointer(addr)
                let writeLen = CJ_SOCKET_BufferSendto(this.socketBufferPtr, 0, Int32(buffer.size), timeout, addrPtr,
                    flags)
                releaseAddrPointer(handle, addrPtr)
                if (writeLen < 0) {
                    socketProcessErrno(ErrnoLabel.Write)
                }
            }
        }
    }

    func send(buffer: Array<Byte>, flags: Int32, timeout: Int64): Unit {
        let writeSize = buffer.size
        var writeToBufferSize: Int64 = 0
        bufferInit()
        while (writeToBufferSize < writeSize) {
            let bufferOnceSize: Int64 = getOnceSendSize(writeSize - writeToBufferSize)
            var copyLen: Int32 = 0
            unsafe {
                let bufCp = acquireArrayRawData(buffer)
                // bufCp.pointer copy to socketBufferPtr
                copyLen = CJ_SOCKET_BufferWCopy(
                    this.socketBufferPtr,
                    bufCp.pointer + writeToBufferSize,
                    Int64(SOCK_WRITE_BUFFER_SIZE),
                    Int32(bufferOnceSize)
                )
                releaseArrayRawData(bufCp)
            }
            if (copyLen == 0) {
                throw SocketException("The socket is closed.")
            } else if (copyLen < 0) {
                throw SocketException("Failed to write data: native socket copy data error.")
            }
            writeToBufferSize += bufferOnceSize
            var bufferSentSize: Int64 = 0
            while (bufferSentSize < bufferOnceSize) {
                let writeLen = unsafe {
                    CJ_SOCKET_BufferWrite(
                        this.socketBufferPtr,
                        UIntNative(bufferSentSize),
                        Int32(bufferOnceSize - bufferSentSize),
                        timeout,
                        flags
                    )
                }
                if (writeLen == 0) {
                    throw SocketException("The socket is closed.")
                } else if (writeLen < 0) {
                    socketProcessErrno(ErrnoLabel.Write)
                }
                bufferSentSize += Int64(writeLen)
            }
        }
    }

    func receiveFrom(size: Int64, buffer: Array<Byte>, flags: Int32, timeout: Int64): (RawAddress, Int64) {
        this.bufferInit()
        let (address, readLen) = unsafe {
            var addr = SockAddr()
            try {
                let r = CJ_SOCKET_BufferRecvFrom(
                    this.socketBufferPtr,
                    0,
                    Int32(buffer.size),
                    timeout,
                    inout addr,
                    flags
                )
                if (r < 0) {
                    socketProcessErrno(ErrnoLabel.Read)
                }
                // Can not get address if timeout
                (addr.toRawAddress(), r)
            } finally {
                addr.free()
            }
        }
        var copyLen: Int32 = 0
        unsafe {
            let bufCp = acquireArrayRawData(buffer)
            // socketBufferPtr copy to bufCp.pointer
            copyLen = CJ_SOCKET_BufferRCopy(this.socketBufferPtr, bufCp.pointer, buffer.size, readLen)
            releaseArrayRawData(bufCp)
        }
        return (address, Int64(readLen))
    }

    func receive(buffer: Array<Byte>, flags: Int32, timeout: Int64): Int64 {
        bufferInit()
        let readLen: Int32 = unsafe { CJ_SOCKET_BufferRead(this.socketBufferPtr, 0, Int32(buffer.size), timeout, flags) }
        if (readLen < 0) {
            socketProcessErrno(ErrnoLabel.Read)
        }
        if (readLen == 0) {
            return 0
        } else {
            var copyLen: Int32 = 0
            unsafe {
                let bufCp = acquireArrayRawData(buffer)
                // socketBufferPtr copy to bufCp.pointer
                copyLen = CJ_SOCKET_BufferRCopy(this.socketBufferPtr, bufCp.pointer, buffer.size, readLen)
                releaseArrayRawData(bufCp)
            }
            if (copyLen == 0) {
                return 0
            } else if (copyLen < 0) {
                throw SocketException("Failed to read data: native socket copy data error.")
            }
            return Int64(copyLen)
        }
    }

    func isConnectionOriented(sockType: Int32): Bool {
        return sockType == SOCK_STREAM || sockType == SOCK_RDM || sockType == SOCK_SEQPACKET
    }

    func releaseFd(): Unit {
        match (socketBufferPtr.isNull()) {
            case true => unsafe { CJ_MRT_SockClose(sockFd) }
            case false => unsafe { CJ_SOCKET_BufferClose(socketBufferPtr, sockFd) } // does CJ_MRT_SockClose inside
        }
    }
}

@When[os == "Windows"]
func getSocketOptionInt32(sockFd: Int64, level: Int32, option: Int32): Int32 {
    unsafe {
        var sockType: CPointer<Int32> = LibC.malloc<Int32>()
        if (sockType.isNull()) {
            throw SocketException("Failed to allocate memory.")
        }
        sockType.write(0)
        var len: Int32 = 4
        let ret = CJ_SockOptionGet(sockFd, level, option, CPointer<Unit>(sockType), inout len)
        if (ret != 0) {
            LibC.free(sockType)
            let errno = CJ_SockErrnoGet()
            let message = if (errno != 0) {
                formatError(errno)
            } else {
                "Socket option get failed with unknown error"
            }
            throw SocketException(message)
        }
        let value = sockType.read()
        LibC.free(sockType)
        return value
    }
}

func getOnceSendSize(size: Int64): Int64 {
    if (size < Int64(SOCK_WRITE_BUFFER_SIZE)) {
        size
    } else {
        Int64(SOCK_WRITE_BUFFER_SIZE)
    }
}
