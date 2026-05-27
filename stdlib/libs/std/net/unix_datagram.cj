/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.net

private func createUnixDatagramSocketCommon(bindAt: SocketAddress): SocketCommon<ActualPlatformSocket> {
    checkUnixSocketAddress(bindAt, "bindAt")
    let impl = SocketCommon<ActualPlatformSocket>(SocketNet.UNIX, bindAt.family, DatagramMode)
    impl.localAddress = bindAt
    return impl
}

private func unixDatagramRemoteAddress(impl: SocketCommon<ActualPlatformSocket>): ?SocketAddress {
    impl.remoteAddress
}

private func unixDatagramLocalAddress(impl: SocketCommon<ActualPlatformSocket>): SocketAddress {
    impl.localAddress ?? SocketException.notYetBoundNeedBind()
}

private func unixReceiveTimeout(impl: SocketCommon<ActualPlatformSocket>): ?Duration {
    impl.readTimeout
}

private func unixReceiveTimeout(impl: SocketCommon<ActualPlatformSocket>, timeout: ?Duration): Unit {
    impl.readTimeout = timeout?.throwIfNegative("Receive timeout").toNanosecondGranularity()
}

private func unixSendTimeout(impl: SocketCommon<ActualPlatformSocket>): ?Duration {
    impl.writeTimeout
}

private func unixSendTimeout(impl: SocketCommon<ActualPlatformSocket>, timeout: ?Duration): Unit {
    impl.writeTimeout = timeout?.throwIfNegative("Send timeout").toNanosecondGranularity()
}

private func unixDatagramClose(impl: SocketCommon<ActualPlatformSocket>): Unit {
    impl.close()
}

private func unixDatagramClosed(impl: SocketCommon<ActualPlatformSocket>): Bool {
    impl.isClosed()
}

private func unixDatagramBind(impl: SocketCommon<ActualPlatformSocket>): Unit {
    impl.bind(None)
}

private func unixDatagramConnect(impl: SocketCommon<ActualPlatformSocket>, remote: SocketAddress): Unit {
    checkUnixSocketAddress(remote, "remote")
    try {
        impl.remoteAddress = remote
        impl.connect(None, shouldBeBound: true)
    } catch (e: Exception) {
        impl.remoteAddress = None
        throw e
    }
}

private func unixDatagramDisconnect(impl: SocketCommon<ActualPlatformSocket>): Unit {
    impl.disconnect()
}

private func unixDatagramReceiveFrom(
    impl: SocketCommon<ActualPlatformSocket>,
    buffer: Array<Byte>
): (SocketAddress, Int64) {
    impl.receiveFrom(buffer) ?? SocketException.throwClosedException()
}

private func unixDatagramSendTo(
    impl: SocketCommon<ActualPlatformSocket>,
    recipient: SocketAddress,
    payload: Array<Byte>
): Unit {
    checkUnixSocketAddress(recipient, "recipient")
    impl.send(payload, recipient)
}

private func unixDatagramSend(impl: SocketCommon<ActualPlatformSocket>, payload: Array<Byte>): Unit {
    if (OS == "macOS") {
        impl.write(payload)
    } else {
        unixDatagramSendTo(impl, impl.remoteAddress ?? SocketException.notYetConnected(), payload)
    }
}

private func unixDatagramReceive(impl: SocketCommon<ActualPlatformSocket>, buffer: Array<Byte>): Int64 {
    let _ = impl.remoteAddress ?? SocketException.notYetConnected()
    let (_, size) = unixDatagramReceiveFrom(impl, buffer)
    return size
}

private func unixDatagramSocketSendBufferSize(impl: SocketCommon<ActualPlatformSocket>): Int64 {
    socketSendBufferSize(impl)
}

private func unixDatagramSocketSendBufferSize(impl: SocketCommon<ActualPlatformSocket>, newSize: Int64): Unit {
    socketSendBufferSize(impl, newSize)
}

private func unixDatagramSocketReceiveBufferSize(impl: SocketCommon<ActualPlatformSocket>): Int64 {
    socketReceiveBufferSize(impl)
}

private func unixDatagramSocketReceiveBufferSize(impl: SocketCommon<ActualPlatformSocket>, newSize: Int64): Unit {
    socketReceiveBufferSize(impl, newSize)
}

private func unixDatagramSocketGetOption(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32,
    value: CPointer<Unit>,
    valueLength: CPointer<UIntNative>
): Unit {
    socketGetOption(impl, level, option, value, valueLength)
}

private func unixDatagramSocketSetOption(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32,
    value: CPointer<Unit>,
    valueLength: UIntNative
): Unit {
    socketSetOption(impl, level, option, value, valueLength)
}

private func unixDatagramSocketGetOptionIntNative(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32
): IntNative {
    socketGetOptionIntNative(impl, level, option)
}

private func unixDatagramSocketSetOptionIntNative(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32,
    value: IntNative
): Unit {
    socketSetOptionIntNative(impl, level, option, value)
}

private func unixDatagramSocketGetOptionBool(impl: SocketCommon<ActualPlatformSocket>, level: Int32, option: Int32): Bool {
    socketGetOptionBool(impl, level, option)
}

private func unixDatagramSocketSetOptionBool(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32,
    value: Bool
): Unit {
    socketSetOptionBool(impl, level, option, value)
}

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

        impl = SocketCommon<ActualPlatformSocket>(SocketNet.UNIX, bindAt.family, DatagramMode)
        impl.localAddress = bindAt
    }

    /**
     * Unix domain datagram peer selected by connect(), or `None` when no peer is pinned.
     *
     * @throws SocketException if the socket is already closed.
     */
    public override prop remoteAddress: ?SocketAddress {
        get() {
            impl.remoteAddress
        }
    }

    /**
     * Local Unix socket address configured for bind(), or the address already bound by this socket.
     *
     * @throws SocketException if the socket is already closed.
     */
    public override prop localAddress: SocketAddress {
        get() {
            impl.localAddress ?? SocketException.notYetBoundNeedBind()
        }
    }

    /**
     * Timeout used while receiving Unix domain datagrams.
     *
     * `None` keeps receive operations blocking without a library deadline. Finite durations are normalized to
     * platform granularity before being passed to the socket implementation.
     *
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
     * Timeout used while sending Unix domain datagrams.
     *
     * `None` means writes do not have a library-side deadline. A finite duration is stored using the nearest
     * supported socket timeout granularity.
     *
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
     * Close the socket releasing all resources. All operations except for close() and isClose() are no longer available.
     * This function is reentrant.
     **/
    public override func close(): Unit {
        impl.close()
    }

    /**
     * Checks whether this socket has been explicitly closed via close()
     **/
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
     * Removes the Unix datagram peer association created by connect().
     *
     * Once disconnected, the socket can sendTo different Unix socket addresses again and receiveFrom is no
     * longer restricted to the previously connected peer.
     *
     * @throws SocketException if the socket is not bound.
     * @throws SocketException if the connection cannot be established.
     */
    public func disconnect(): Unit {
        impl.disconnect()
    }

    /**
     * Reads one Unix domain datagram into the provided buffer.
     *
     * The result contains the sender socket address and the datagram size reported by the platform. If the
     * datagram does not fit, copied bytes are truncated and the reported size may be larger than buffer.size.
     *
     * @throws SocketException if buffer is empty or if the socket cannot read data.
     * @throws SocketTimeoutException if reading time has expired.
     */
    public override func receiveFrom(buffer: Array<Byte>): (SocketAddress, Int64) {
        impl.receiveFrom(buffer) ?? SocketException.throwClosedException()
    }

    /**
     * Sends the payload as one Unix domain datagram to the specified recipient socket address.
     *
     * The call validates the recipient as a UnixSocketAddress and then delegates to the platform socket. It may
     * block while the local output queue has no available space.
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

    /** Gets or sets this UnixDatagramSocket send buffer size; non-positive updates throw IllegalArgumentException. */
    public mut prop sendBufferSize: Int64 {
        get() {
            unixDatagramSocketSendBufferSize(impl)
        }
        set(newSize) {
            unixDatagramSocketSendBufferSize(impl, newSize)
        }
    }

    /** Gets or sets this UnixDatagramSocket receive buffer size; non-positive updates throw IllegalArgumentException. */
    public mut prop receiveBufferSize: Int64 {
        get() {
            unixDatagramSocketReceiveBufferSize(impl)
        }
        set(newSize) {
            unixDatagramSocketReceiveBufferSize(impl, newSize)
        }
    }

    /** Reads a UnixDatagramSocket raw option into `value`; failures are reported as SocketException. */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        unixDatagramSocketGetOption(impl, level, option, value, valueLength)
    }

    /** Writes a UnixDatagramSocket raw option from `value`; failures are reported as SocketException. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        unixDatagramSocketSetOption(impl, level, option, value, valueLength)
    }

    /** Reads a UnixDatagramSocket option represented as IntNative. */
    public func getSocketOptionIntNative(level: Int32, option: Int32): IntNative {
        unixDatagramSocketGetOptionIntNative(impl, level, option)
    }

    /** Writes a UnixDatagramSocket option represented as IntNative. */
    public func setSocketOptionIntNative(level: Int32, option: Int32, value: IntNative): Unit {
        unixDatagramSocketSetOptionIntNative(impl, level, option, value)
    }

    /** Reads a UnixDatagramSocket option represented as Bool. */
    public func getSocketOptionBool(level: Int32, option: Int32): Bool {
        unixDatagramSocketGetOptionBool(impl, level, option)
    }

    /** Writes a UnixDatagramSocket option represented as Bool. */
    public func setSocketOptionBool(level: Int32, option: Int32, value: Bool): Unit {
        unixDatagramSocketSetOptionBool(impl, level, option, value)
    }

    public override func toString(): String {
        "UnixDatagramSocket(${impl.toString()})"
    }
}

@When[os == "Windows"]
@Deprecated
public class UnixDatagramSocket <: DatagramSocket {
    private let impl: SocketCommon<ActualPlatformSocket>

    public init(bindAt!: String) {
        this(bindAt: UnixSocketAddress(bindAt))
    }

    public init(bindAt!: SocketAddress) {
        impl = createUnixDatagramSocketCommon(bindAt)
    }

    public override prop remoteAddress: ?SocketAddress {
        get() {
            unixDatagramRemoteAddress(impl)
        }
    }

    public override prop localAddress: SocketAddress {
        get() {
            unixDatagramLocalAddress(impl)
        }
    }

    public override mut prop receiveTimeout: ?Duration {
        get() {
            unixReceiveTimeout(impl)
        }
        set(timeout) {
            unixReceiveTimeout(impl, timeout)
        }
    }

    public override mut prop sendTimeout: ?Duration {
        get() {
            unixSendTimeout(impl)
        }
        set(timeout) {
            unixSendTimeout(impl, timeout)
        }
    }

    public override func close(): Unit {
        unixDatagramClose(impl)
    }

    public override func isClosed(): Bool {
        unixDatagramClosed(impl)
    }

    public func bind(): Unit {
        unixDatagramBind(impl)
    }

    public func connect(remotePath: String): Unit {
        connect(UnixSocketAddress(remotePath))
    }

    public func connect(remote: SocketAddress): Unit {
        unixDatagramConnect(impl, remote)
    }

    public func disconnect(): Unit {
        unixDatagramDisconnect(impl)
    }

    public override func receiveFrom(buffer: Array<Byte>): (SocketAddress, Int64) {
        unixDatagramReceiveFrom(impl, buffer)
    }

    public override func sendTo(recipient: SocketAddress, payload: Array<Byte>): Unit {
        unixDatagramSendTo(impl, recipient, payload)
    }

    public func send(payload: Array<Byte>): Unit {
        unixDatagramSend(impl, payload)
    }

    public func receive(buffer: Array<Byte>): Int64 {
        unixDatagramReceive(impl, buffer)
    }

    /** Gets or sets the Windows compatibility UnixDatagramSocket send buffer size. */
    public mut prop sendBufferSize: Int64 {
        get() {
            socketSendBufferSize(impl)
        }
        set(newSize) {
            socketSendBufferSize(impl, newSize)
        }
    }

    /** Gets or sets the Windows compatibility UnixDatagramSocket receive buffer size. */
    public mut prop receiveBufferSize: Int64 {
        get() {
            socketReceiveBufferSize(impl)
        }
        set(newSize) {
            socketReceiveBufferSize(impl, newSize)
        }
    }

    /** Reads a Windows compatibility UnixDatagramSocket raw option into `value`. */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        socketGetOption(impl, level, option, value, valueLength)
    }

    /** Writes a Windows compatibility UnixDatagramSocket raw option from `value`. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        socketSetOption(impl, level, option, value, valueLength)
    }

    /** Reads a Windows compatibility UnixDatagramSocket option represented as IntNative. */
    public func getSocketOptionIntNative(level: Int32, option: Int32): IntNative {
        socketGetOptionIntNative(impl, level, option)
    }

    /** Writes a Windows compatibility UnixDatagramSocket option represented as IntNative. */
    public func setSocketOptionIntNative(level: Int32, option: Int32, value: IntNative): Unit {
        socketSetOptionIntNative(impl, level, option, value)
    }

    /** Reads a Windows compatibility UnixDatagramSocket option represented as Bool. */
    public func getSocketOptionBool(level: Int32, option: Int32): Bool {
        socketGetOptionBool(impl, level, option)
    }

    /** Writes a Windows compatibility UnixDatagramSocket option represented as Bool. */
    public func setSocketOptionBool(level: Int32, option: Int32, value: Bool): Unit {
        socketSetOptionBool(impl, level, option, value)
    }

    public override func toString(): String {
        "UnixDatagramSocket(${impl.toString()})"
    }
}
