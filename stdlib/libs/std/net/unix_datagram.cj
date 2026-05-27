/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.net

/*
 * Represents a Unix Domain datagram socket.
 *
 * Once an instance is created, it is not yet bound so should be bound explicitly via bind().
 * Unlike UnixSocket, a UnixDatagramSocket may remain unconnected and work without pairing to any remote address handling
 * multiple peers at the same time.
 * However, a UnixDatagramSocket socket could be optionally paired via connect() to an existing bound socket file.
 * A created pairing (after connect invocation) could be terminated via disconnect().
 *
 * Unlike UDP, Unix Domain socket don't limit datagram size to 64Kib, however due to the underlying implementation and operation system
 * limitations, the maximum datagram size could be still limited to some amount.
 *
 * Instances of this type should be explicitly closed even when the bind() hasn't been invoked.
 *
 * @see DatagramSocket for more details on how do datagram sockets work.
 */
@When[os != "Windows"]
public class UnixDatagramSocket <: DatagramSocket {
    private let unixDatagramImpl: SocketCommon<ActualPlatformSocket>

    /**
     * Creates an unbound Unix datagram socket for a local path.
     *
     * @param bindAt local path that should not already exist before bind().
     */
    public init(bindAt!: String) {
        this(bindAt: UnixSocketAddress(bindAt))
    }

    /**
     * Creates an unbound Unix datagram socket for a local address.
     *
     * @param bindAt local address that should be created during bind().
     */
    public init(bindAt!: SocketAddress) {
        checkUnixSocketAddress(bindAt, "bindAt")

        unixDatagramImpl = SocketCommon(SocketNet.UNIX, bindAt.family, DatagramMode)
        unixDatagramImpl.localAddress = bindAt
    }

    /**
     * Remote address used by this Unix datagram socket, or `None` if it is unconnected.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop remoteAddress: ?SocketAddress {
        get() {
            unixDatagramImpl.remoteAddress
        }
    }

    /**
     * Local address configured for this Unix datagram socket.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop localAddress: SocketAddress {
        get() {
            unixDatagramImpl.localAddress ?? SocketException.notYetBoundNeedBind()
        }
    }

    /**
     * Receive timeout for this Unix datagram socket, or `None`.
     * The platform may round the timeout, but cancellation is never earlier than requested.
     *
     * The default value is None.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop receiveTimeout: ?Duration {
        get() {
            unixDatagramImpl.readTimeout
        }
        set(timeout) {
            unixDatagramImpl.readTimeout = timeout?.throwIfNegative("Receive timeout").toNanosecondGranularity()
        }
    }

    /**
     * Send timeout for this Unix datagram socket, or `None`.
     *
     * Very small or very large durations may be rounded to platform-supported values.
     *
     * The default value is None.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop sendTimeout: ?Duration {
        get() {
            unixDatagramImpl.writeTimeout
        }
        set(timeout) {
            unixDatagramImpl.writeTimeout = timeout?.throwIfNegative("Send timeout").toNanosecondGranularity()
        }
    }

    /**
     * Close the socket releasing all resources. All operations except for close() and isClose() are no longer available.
     * This function is reentrant.
     **/
    public override func close(): Unit {
        unixDatagramImpl.close()
    }

    /**
     * Checks whether this socket has been explicitly closed via close()
     **/
    public override func isClosed(): Bool {
        unixDatagramImpl.isClosed()
    }

    /**
     * Bind Unix datagram socket at local path.
     *
     * This operation does atomically create a socket file at the local path.
     *
     * @throws SocketException if the socket is already bound or path already exists.
     */
    public func bind(): Unit {
        unixDatagramImpl.bind(None)
    }

    /**
     * Configure the socket to only work with the specified remote peer address. To undo this action use disconnect()
     *
     * After invoking this function with a particular address, we will not be able to send message to peers other than this. We will also never receive any messages from other peers (they will be just filtered out). Invoking disconnect() reverts the filter to the initial state so it will be possible to send message to any recipients again.
     *
     * This function should be only invoked after bind()
     */
    public func connect(remotePath: String): Unit {
        connect(UnixSocketAddress(remotePath))
    }

    /**
     * Configure the socket to only work with the specified remote peer address. To undo this action use disconnect()
     *
     * After invoking this function with a particular address, we will not be able to send message to peers other than this. We will also never receive any messages from other peers (they will be just filtered out). Invoking disconnect() reverts the filter to the initial state so it will be possible to send message to any recipients again.
     *
     * This function should be only invoked after bind()
     *
     * @throws SocketException if the socket is not bound.
     * @throws SocketException if the connection cannot be established.
     */
    public func connect(remote: SocketAddress): Unit {
        checkUnixSocketAddress(remote, "remote")
        try {
            unixDatagramImpl.remoteAddress = remote
            unixDatagramImpl.connect(None, shouldBeBound: true)
        } catch (e: Exception) {
            unixDatagramImpl.remoteAddress = None
            throw e
        }
    }

    /**
     * Reverts the effect of connect() function so we can send and receive to/from any address again.
     * This function makes no effect if invoked multiple times or if we invoke disconnect without connect invocation.
     *
     * @throws SocketException if the socket is not bound.
     * @throws SocketException if the connection cannot be established.
     */
    public func disconnect(): Unit {
        unixDatagramImpl.disconnect()
    }

    /**
     * Receive the next datagram into the specified buffer waiting for data if needed.
     *
     * Returns a pair of the datagram sender address and the actual size of received datagram, possibly zero
     * or a value greater than the passed buffer size.
     *
     * Unlike read in streams, this function requires a buffer of proper size (big enough),
     * otherwise a datagram that is bigger than the provided buffer will be
     * truncated and the returned datagram size will be greater that the buffer size.
     *
     * @throws SocketException if buffer is empty or if it is not possible to read the data.
     * @throws SocketTimeoutException if reading time has expired.
     */
    public override func receiveFrom(buffer: Array<Byte>): (SocketAddress, Int64) {
        unixDatagramImpl.receiveFrom(buffer) ?? SocketException.throwClosedException()
    }

    /**
     * Sends datagram of the payload to the specified recipient address.
     *
     * It also may block in this function invocation if there is not enough
     * output buffer space available for some reason. Depending on the underlying
     * implementation, it may also silently discard a datagram in this case.
     */
    public override func sendTo(recipient: SocketAddress, payload: Array<Byte>): Unit {
        checkUnixSocketAddress(recipient, "recipient")

        unixDatagramImpl.send(payload, recipient)
    }

    /**
     * Send a message with the specified payload to the peer with preconfigured address.
     * This only works if address has been specified using `connect() ` otherwise will fail immediately.
     *
     * In other aspects, it works the same as regular `sendTo(recipient,payload).
     */
    public func send(payload: Array<Byte>): Unit {
        if (OS == "macOS") {
            unixDatagramImpl.write(payload)
        } else {
            sendTo(unixDatagramImpl.remoteAddress ?? SocketException.notYetConnected(), payload)
        }
    }

    /**
     * Receive a datagram message from the preconfigured peer address.
     * This only works if the address has been specified via connect() otherwise will fail.
     * In other aspects, it works the same as regular `receiveFrom(buffer).
     */
    public func receive(buffer: Array<Byte>): Int64 {
        let _ = unixDatagramImpl.remoteAddress ?? SocketException.notYetConnected()
        let (_, size) = receiveFrom(buffer)
        return size
    }

    /**
     * SO_SNDBUF option, providing a way to specify hint for the underlying
     * native socket implementation about the desired outgoing buffer size.
     *
     * The operating system may clamp, ignore, or report advisory values for this option.
     *
     * @throws IllegalArgumentException if the specified buffer size is negative or 0.
     */
    public mut prop sendBufferSize: Int64 {
        get() {
            unixDatagramImpl.getSendBufferSize()
        }
        set(newSize) {
            unixDatagramImpl.setSendBufferSize(newSize)
        }
    }

    /**
     * SO_RCVBUF option, providing a way to specify hint for the underlying
     * native socket implementation about the desired receive buffer size.
     *
     * The operating system may clamp, ignore, or report advisory values for this option.
     *
     * @throws IllegalArgumentException if the specified buffer size is negative or 0.
     */
    public mut prop receiveBufferSize: Int64 {
        get() {
            unixDatagramImpl.getReceiveBufferSize()
        }
        set(newSize) {
            unixDatagramImpl.setReceiveBufferSize(newSize)
        }
    }

    /**
     * Read a raw Unix datagram socket option.
     *
     * Set valueLength to value's byte capacity before the call. The native call stores the
     * returned option length back through the same pointer.
     *
     * @throws SocketException if getsockopt fails.
     */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        unsafe { unixDatagramImpl.getSocketOption(level, option, value, valueLength) }
    }

    /** Write a raw Unix datagram socket option; native setsockopt failures are reported as exceptions. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        unsafe { unixDatagramImpl.setSocketOption(level, option, value, valueLength) }
    }

    /** Read a Unix datagram socket option as IntNative, requiring the native result size to match. */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        unixDatagramImpl.getSocketOptionIntNative(level, option)
    }

    /** Write an IntNative value to a Unix datagram socket option. */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        unixDatagramImpl.setSocketOptionIntNative(level, option, value)
    }

    /** Read a Unix datagram socket option as Bool using IntNative zero/nonzero conversion. */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        unixDatagramImpl.getSocketOptionBool(level, option)
    }

    /** Write a Bool value to a Unix datagram socket option using IntNative conversion. */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        unixDatagramImpl.setSocketOptionBool(level, option, value)
    }

    public override func toString(): String {
        "UnixDatagramSocket(${unixDatagramImpl.toString()})"
    }
}

@When[os == "Windows"]
@Deprecated
public class UnixDatagramSocket <: DatagramSocket {
    private let legacyDatagramImpl: SocketCommon<ActualPlatformSocket>

    /**
     * Creates an unbound Unix datagram socket ready to bind at the specified path
     *
     * @param bindAt local path (should be a non-existing path that will be created during bind)
     */
    public init(bindAt!: String) {
        this(bindAt: UnixSocketAddress(bindAt))
    }

    /**
     * Creates an unbound Unix datagram socket ready to bind at the specified address
     *
     * @param bindAt local path (should be a non-existing path that will be created during bind)
     */
    public init(bindAt!: SocketAddress) {
        checkUnixSocketAddress(bindAt, "bindAt")

        legacyDatagramImpl = SocketCommon(SocketNet.UNIX, bindAt.family, DatagramMode)
        legacyDatagramImpl.localAddress = bindAt
    }

    /**
     * Remote address the socket is connected to or `None` if the socket is unconnected.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop remoteAddress: ?SocketAddress {
        get() {
            legacyDatagramImpl.remoteAddress
        }
    }

    /**
     * Local address the socket will be or is currently bound at.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop localAddress: SocketAddress {
        get() {
            legacyDatagramImpl.localAddress ?? SocketException.notYetBoundNeedBind()
        }
    }

    /**
     * Receive/ReceiveFrom operation time limit or `None` for infinite read attempts.
     * The value specified here is actually the minimal amount of time before a read operation cancelled.
     * The actual time is not guaranteed but it will be never cancelled earlier than the specified timeout value.
     * If the duration is too big than it can be bumped to the infinite. When it's too small then if will be bumped to the minimal clock granularity.
     *
     * The default value is None.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop receiveTimeout: ?Duration {
        get() {
            legacyDatagramImpl.readTimeout
        }
        set(timeout) {
            legacyDatagramImpl.readTimeout = timeout?.throwIfNegative("Receive timeout").toNanosecondGranularity()
        }
    }

    /**
     * Send/SendTo operation time limit or `None` for infinite read attempts.
     *
     * The value specified here is actually the minimal amount of time before a write operation cancelled.
     * The actual time is not guaranteed but it will be never cancelled earlier than the specified timeout value.
     * If the duration is too big than it can be bumped to the infinite. When it's too small then if will be bumped to the minimal clock granularity.
     *
     * The default value is None.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop sendTimeout: ?Duration {
        get() {
            legacyDatagramImpl.writeTimeout
        }
        set(timeout) {
            legacyDatagramImpl.writeTimeout = timeout?.throwIfNegative("Send timeout").toNanosecondGranularity()
        }
    }

    /**
     * Close the deprecated Windows-compatible socket and release its resources.
     * This function is reentrant.
     **/
    public override func close(): Unit {
        legacyDatagramImpl.close()
    }

    /**
     * Checks whether this deprecated Windows-compatible socket was closed.
     **/
    public override func isClosed(): Bool {
        legacyDatagramImpl.isClosed()
    }

    /**
     * Bind the deprecated Windows-compatible Unix datagram socket at its local path.
     *
     * This operation atomically creates the socket file at the configured path.
     *
     * @throws SocketException if the socket is already bound or path already exists.
     */
    public func bind(): Unit {
        legacyDatagramImpl.bind(None)
    }

    /**
     * Restrict this deprecated Windows-compatible socket to a remote peer path.
     *
     * Use disconnect() to restore sending to and receiving from any peer. Invoke this only after bind().
     */
    public func connect(remotePath: String): Unit {
        connect(UnixSocketAddress(remotePath))
    }

    /**
     * Restrict this deprecated Windows-compatible socket to a remote peer address.
     *
     * Messages from other peers are filtered out until disconnect() restores the unconnected state.
     * Invoke this only after bind().
     *
     * @throws SocketException if the socket is not bound.
     * @throws SocketException if the connection cannot be established.
     */
    public func connect(remote: SocketAddress): Unit {
        checkUnixSocketAddress(remote, "remote")
        try {
            legacyDatagramImpl.remoteAddress = remote
            legacyDatagramImpl.connect(None, shouldBeBound: true)
        } catch (e: Exception) {
            legacyDatagramImpl.remoteAddress = None
            throw e
        }
    }

    /**
     * Clear the peer restriction set by connect().
     * Calling this repeatedly, or before connect(), has no effect.
     *
     * @throws SocketException if the socket is not bound.
     * @throws SocketException if the connection cannot be established.
     */
    public func disconnect(): Unit {
        legacyDatagramImpl.disconnect()
    }

    /**
     * Receive the next datagram for this deprecated Windows-compatible socket.
     *
     * Returns a pair of the datagram sender address and the actual size of received datagram, possibly zero
     * or a value greater than the passed buffer size.
     *
     * Unlike read in streams, this function requires a buffer of proper size (big enough),
     * otherwise a datagram that is bigger than the provided buffer will be
     * truncated and the returned datagram size will be greater that the buffer size.
     *
     * @throws SocketException if buffer is empty or if it is not possible to read the data.
     * @throws SocketTimeoutException if reading time has expired.
     */
    public override func receiveFrom(buffer: Array<Byte>): (SocketAddress, Int64) {
        legacyDatagramImpl.receiveFrom(buffer) ?? SocketException.throwClosedException()
    }

    /**
     * Send one datagram from this deprecated Windows-compatible socket to the recipient.
     *
     * It also may block in this function invocation if there is not enough
     * output buffer space available for some reason. Depending on the underlying
     * implementation, it may also silently discard a datagram in this case.
     */
    public override func sendTo(recipient: SocketAddress, payload: Array<Byte>): Unit {
        checkUnixSocketAddress(recipient, "recipient")

        legacyDatagramImpl.send(payload, recipient)
    }

    /**
     * Send a message with the specified payload to the peer with preconfigured address.
     * This only works if address has been specified using `connect() ` otherwise will fail immediately.
     *
     * In other aspects, it works the same as regular `sendTo(recipient,payload).
     */
    public func send(payload: Array<Byte>): Unit {
        if (OS == "macOS") {
            legacyDatagramImpl.write(payload)
        } else {
            sendTo(legacyDatagramImpl.remoteAddress ?? SocketException.notYetConnected(), payload)
        }
    }

    /**
     * Receive a datagram from the peer configured by connect().
     * Fails when no peer is configured; otherwise it follows receiveFrom(buffer) semantics.
     */
    public func receive(buffer: Array<Byte>): Int64 {
        let _ = legacyDatagramImpl.remoteAddress ?? SocketException.notYetConnected()
        let (_, size) = receiveFrom(buffer)
        return size
    }

    /**
     * SO_SNDBUF hint used by the deprecated Windows-compatible Unix datagram socket.
     *
     * The operating system may clamp, ignore, or report advisory values for this option.
     *
     * @throws IllegalArgumentException if the specified buffer size is negative or 0.
     */
    public mut prop sendBufferSize: Int64 {
        get() {
            legacyDatagramImpl.getSendBufferSize()
        }
        set(newSize) {
            legacyDatagramImpl.setSendBufferSize(newSize)
        }
    }

    /**
     * SO_RCVBUF hint used by the deprecated Windows-compatible Unix datagram socket.
     *
     * The operating system may clamp, ignore, or report advisory values for this option.
     *
     * @throws IllegalArgumentException if the specified buffer size is negative or 0.
     */
    public mut prop receiveBufferSize: Int64 {
        get() {
            legacyDatagramImpl.getReceiveBufferSize()
        }
        set(newSize) {
            legacyDatagramImpl.setReceiveBufferSize(newSize)
        }
    }

    /**
     * Read a raw Unix datagram socket option.
     *
     * valueLength is updated in place: it starts as the value buffer size and ends as the
     * result length reported by getsockopt.
     *
     * @throws SocketException if getsockopt fails.
     */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        unsafe { legacyDatagramImpl.getSocketOption(level, option, value, valueLength) }
    }

    /** Write a raw Unix datagram socket option; native setsockopt failures are reported as exceptions. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        unsafe { legacyDatagramImpl.setSocketOption(level, option, value, valueLength) }
    }

    /** Read a Unix datagram socket option as IntNative, requiring the native result size to match. */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        legacyDatagramImpl.getSocketOptionIntNative(level, option)
    }

    /** Write an IntNative value to a Unix datagram socket option. */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        legacyDatagramImpl.setSocketOptionIntNative(level, option, value)
    }

    /** Read a Unix datagram socket option as Bool using IntNative zero/nonzero conversion. */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        legacyDatagramImpl.getSocketOptionBool(level, option)
    }

    /** Write a Bool value to a Unix datagram socket option using IntNative conversion. */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        legacyDatagramImpl.setSocketOptionBool(level, option, value)
    }

    public override func toString(): String {
        "UnixDatagramSocket(${legacyDatagramImpl.toString()})"
    }
}
