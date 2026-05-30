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
    private let impl: SocketCommon<ActualPlatformSocket>

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

        impl = SocketCommon(SocketNet.UNIX, bindAt.family, DatagramMode)
        impl.localAddress = bindAt
    }

    /**
     * Remote address used by this Unix datagram socket, or `None` if it is unconnected.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop remoteAddress: ?SocketAddress {
        get() {
            impl.remoteAddress
        }
    }

    /**
     * Local address configured for this Unix datagram socket.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop localAddress: SocketAddress {
        get() {
            impl.localAddress ?? SocketException.notYetBoundNeedBind()
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
            impl.readTimeout
        }
        set(timeout) {
            impl.readTimeout = timeout?.throwIfNegative("Receive timeout").toNanosecondGranularity()
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
            impl.writeTimeout
        }
        set(timeout) {
            impl.writeTimeout = timeout?.throwIfNegative("Send timeout").toNanosecondGranularity()
        }
    }

    /**
     * Close this socket and release its resources. This function is reentrant.
     */
    public override func close(): Unit {
        // Datagram close releases any local Unix socket path binding held by the descriptor.
        impl.close()
    }

    /**
     * Return whether close() has already been invoked for this socket.
     */
    public override func isClosed(): Bool {
        impl.isClosed()
    }

    /**
     * Bind Unix datagram socket at local path.
     *
     * This operation does atomically create a socket file at the local path.
     *
     * @throws SocketException if the socket is already bound or path already exists.
     */
    public func bind(): Unit {
        impl.bind(None)
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
            impl.remoteAddress = remote
            impl.connect(None, shouldBeBound: true)
        } catch (e: Exception) {
            impl.remoteAddress = None
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
        // Unix datagram disconnect clears the peer association on the underlying socket.
        impl.disconnect()
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
     * Unix datagram sockets also return the sender address with each received packet.
     *
     * @throws SocketException if buffer is empty or if it is not possible to read the data.
     * @throws SocketTimeoutException if reading time has expired.
     */
    public override func receiveFrom(buffer: Array<Byte>): (SocketAddress, Int64) {
        // A missing native result means the socket reached the closed state.
        impl.receiveFrom(buffer) ?? SocketException.throwClosedException()
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

        impl.send(payload, recipient)
    }

    /**
     * Send a message with the specified payload to the peer with preconfigured address.
     * This only works if address has been specified using `connect() ` otherwise will fail immediately.
     *
     * In other aspects, it works the same as regular `sendTo(recipient,payload).
     */
    public func send(payload: Array<Byte>): Unit {
        if (OS == "macOS") {
            impl.write(payload)
        } else {
            sendTo(impl.remoteAddress ?? SocketException.notYetConnected(), payload)
        }
    }

    /**
     * Receive a datagram message from the preconfigured peer address.
     * This only works if the address has been specified via connect() otherwise will fail.
     * In other aspects, it works the same as regular `receiveFrom(buffer).
     */
    public func receive(buffer: Array<Byte>): Int64 {
        let _ = impl.remoteAddress ?? SocketException.notYetConnected()
        let (_, size) = receiveFrom(buffer)
        return size
    }

    /**
     * Unix datagram SO_SNDBUF hint for outgoing messages; rejects non-positive sizes.
     */
    public mut prop sendBufferSize: Int64 {
        get() {
            impl.getSendBufferSize()
        }
        set(newSize) {
            impl.setSendBufferSize(newSize)
        }
    }

    /**
     * Unix datagram SO_RCVBUF hint for incoming messages; rejects non-positive sizes.
     */
    public mut prop receiveBufferSize: Int64 {
        get() {
            impl.getReceiveBufferSize()
        }
        set(newSize) {
            impl.setReceiveBufferSize(newSize)
        }
    }

    /**
     * Read a Unix datagram Bool option using IntNative conversion.
     */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        impl.getSocketOptionBool(level, option)
    }

    /**
     * Write a Unix datagram Bool option using IntNative conversion.
     */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        impl.setSocketOptionBool(level, option, value)
    }

    /**
     * Read a Unix datagram raw option and update valueLength with the result size.
     */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        unsafe { impl.getSocketOption(level, option, value, valueLength) }
    }

    /**
     * Write a Unix datagram raw option; native failures become SocketException.
     */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        unsafe { impl.setSocketOption(level, option, value, valueLength) }
    }

    /**
     * Read a Unix datagram IntNative option and verify the native size.
     */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        impl.getSocketOptionIntNative(level, option)
    }

    /**
     * Write a Unix datagram IntNative option value.
     */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        impl.setSocketOptionIntNative(level, option, value)
    }

    public override func toString(): String {
        "UnixDatagramSocket(${impl.toString()})"
    }
}

@When[os == "Windows"]
@Deprecated
public class UnixDatagramSocket <: DatagramSocket {
    private let impl: SocketCommon<ActualPlatformSocket>

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

        impl = SocketCommon(SocketNet.UNIX, bindAt.family, DatagramMode)
        impl.localAddress = bindAt
    }

    /**
     * Unix domain peer selected by connect(), or `None` while datagrams may arrive
     * from any sender.
     *
     * @throws SocketException if the socket is already closed.
     */
    public override prop remoteAddress: ?SocketAddress {
        get() {
            this.impl.remoteAddress
        }
    }

    /**
     * Unix domain path where this datagram socket is or will be bound.
     *
     * @throws SocketException if the socket is closed or no local binding is available.
     */
    public override prop localAddress: SocketAddress {
        get() {
            this.impl.localAddress ?? SocketException.notYetBoundNeedBind()
        }
    }

    /**
     * Timeout used by Unix datagram receive operations, or `None` for no limit.
     * The platform may round the value, but it must not cancel earlier than requested.
     *
     * The default value is None.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop receiveTimeout: ?Duration {
        get() {
            this.impl.readTimeout
        }
        set(timeout) {
            this.impl.readTimeout = timeout?.throwIfNegative("Receive timeout").toNanosecondGranularity()
        }
    }

    /**
     * Timeout used by Unix datagram send operations, or `None` for no limit.
     *
     * The operating system may round durations to supported timeout values.
     *
     * The default value is None.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop sendTimeout: ?Duration {
        get() {
            this.impl.writeTimeout
        }
        set(timeout) {
            this.impl.writeTimeout = timeout?.throwIfNegative("Send timeout").toNanosecondGranularity()
        }
    }

    /**
     * Close the deprecated Windows-compatible socket and release its resources.
     * This function is reentrant.
     **/
    public override func close(): Unit {
        this.impl.close()
    }

    /**
     * Checks whether this deprecated Windows-compatible socket was closed.
     **/
    public override func isClosed(): Bool {
        this.impl.isClosed()
    }

    /**
     * Bind the deprecated Windows-compatible Unix datagram socket at its local path.
     *
     * This operation atomically creates the socket file at the configured path.
     *
     * @throws SocketException if the socket is already bound or path already exists.
     */
    public func bind(): Unit {
        this.impl.bind(None)
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
            this.impl.remoteAddress = remote
            this.impl.connect(None, shouldBeBound: true)
        } catch (e: Exception) {
            this.impl.remoteAddress = None
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
        this.impl.disconnect()
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
        this.impl.receiveFrom(buffer) ?? SocketException.throwClosedException()
    }

    /**
     * Receive a datagram from the peer configured by connect().
     * Fails when no peer is configured; otherwise it follows receiveFrom(buffer) semantics.
     */
    public func receive(buffer: Array<Byte>): Int64 {
        let _ = this.impl.remoteAddress ?? SocketException.notYetConnected()
        let (_, size) = receiveFrom(buffer)
        return size
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

        this.impl.send(payload, recipient)
    }

    /**
     * Send a message with the specified payload to the peer with preconfigured address.
     * This only works if address has been specified using `connect() ` otherwise will fail immediately.
     *
     * In other aspects, it works the same as regular `sendTo(recipient,payload).
     */
    public func send(payload: Array<Byte>): Unit {
        if (OS == "macOS") {
            this.impl.write(payload)
        } else {
            sendTo(this.impl.remoteAddress ?? SocketException.notYetConnected(), payload)
        }
    }

    public override func toString(): String {
        "UnixDatagramSocket(${this.impl.toString()})"
    }

    /**
     * Deprecated Unix datagram SO_SNDBUF hint; rejects non-positive sizes.
     */
    public mut prop sendBufferSize: Int64 {
        get() {
            this.impl.getSendBufferSize()
        }
        set(newSize) {
            this.impl.setSendBufferSize(newSize)
        }
    }

    /**
     * Deprecated Unix datagram SO_RCVBUF hint; rejects non-positive sizes.
     */
    public mut prop receiveBufferSize: Int64 {
        get() {
            this.impl.getReceiveBufferSize()
        }
        set(newSize) {
            this.impl.setReceiveBufferSize(newSize)
        }
    }

    /**
     * Read a Unix datagram socket option as Bool using IntNative zero/nonzero conversion.
     */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        this.impl.getSocketOptionBool(level, option)
    }

    /**
     * Write a Bool value to a Unix datagram socket option using IntNative conversion.
     */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        this.impl.setSocketOptionBool(level, option, value)
    }

    /**
     * Read a raw Unix datagram socket option; valueLength is updated with the native result size.
     */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        unsafe { this.impl.getSocketOption(level, option, value, valueLength) }
    }

    /**
     * Write a raw Unix datagram socket option; native setsockopt failures are reported as exceptions.
     */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        unsafe { this.impl.setSocketOption(level, option, value, valueLength) }
    }

    /**
     * Read a Unix datagram socket option as IntNative, requiring the native result size to match.
     */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        this.impl.getSocketOptionIntNative(level, option)
    }

    /**
     * Write an IntNative value to a Unix datagram socket option.
     */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        this.impl.setSocketOptionIntNative(level, option, value)
    }
}
