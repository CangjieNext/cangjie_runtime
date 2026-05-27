/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.net

private func checkUdpSocketAddress(address: SocketAddress, name: String): SocketAddress {
    if (!(address is IPSocketAddress)) {
        throw IllegalArgumentException("${name} should be either IPv4 or IPv6 but got ${address.family}: ${address}")
    }
    return address
}

private func createUdpSocketCommon(bindAt: SocketAddress): SocketCommon<ActualPlatformSocket> {
    checkUdpSocketAddress(bindAt, "bindAt")
    let impl = SocketCommon<ActualPlatformSocket>(SocketNet.UDP, bindAt.family, DatagramMode)
    impl.localAddress = bindAt
    return impl
}

private func udpRemoteAddress(impl: SocketCommon<ActualPlatformSocket>): ?SocketAddress {
    impl.remoteAddress
}

private func udpLocalAddress(impl: SocketCommon<ActualPlatformSocket>): SocketAddress {
    impl.localAddress ?? SocketException.notYetBoundNeedBind()
}

private func udpReceiveTimeout(impl: SocketCommon<ActualPlatformSocket>): ?Duration {
    impl.readTimeout
}

private func udpReceiveTimeout(impl: SocketCommon<ActualPlatformSocket>, timeout: ?Duration): Unit {
    impl.readTimeout = timeout?.throwIfNegative("Receive timeout").toNanosecondGranularity()
}

private func udpSendTimeout(impl: SocketCommon<ActualPlatformSocket>): ?Duration {
    impl.writeTimeout
}

private func udpSendTimeout(impl: SocketCommon<ActualPlatformSocket>, timeout: ?Duration): Unit {
    impl.writeTimeout = timeout?.throwIfNegative("Send timeout").toNanosecondGranularity()
}

private func udpSocketClose(impl: SocketCommon<ActualPlatformSocket>): Unit {
    impl.close()
}

private func udpSocketClosed(impl: SocketCommon<ActualPlatformSocket>): Bool {
    impl.isClosed()
}

private func udpSocketBind(impl: SocketCommon<ActualPlatformSocket>): Unit {
    impl.bind(None)
}

/*
 * Represents a UDP datagram socket.
 *
 * Once an instance is created, it is not yet bound so should be bound explicitly via bind().
 * Unlike TCP, a UDP socket may remain unconnected and work without pairing to any remote address handling
 * multiple peers at the same time.
 * However, a UDP socket could be optionally paired via connect() that generally doesn't involve any negotiation so "connecting" to
 * a non-existing address could compelte successfully. A created pairing (after connect invocation) could be terminated via disconnect().
 *
 * UDP protocol does only allow sending and receiving datagrams at most of 64Kib long.
 *
 * Instances of this type should be explicitly closed even when the bind() hasn't been invoked.
 *
 * @see DatagramSocket for more details on how do datagram sockets work.
 */
public class UdpSocket <: DatagramSocket {
    private let impl: SocketCommon<ActualPlatformSocket>

    /**
     * Creates an unbound UDP socket ready to bind at the specified port
     *
     * The default address is 0.0.0.0 what means every IP address of current machine,
     * this is more suitable for a server that can be connected from the local network
     * using localhost address (127.0.0.1) or from outside using corresponding address.
     * Therefore, this address (0.0.0.0) shall not be used as the destination address,
     * so it is necessary to specify an address in a particular network when calling
     * `connect` or `sendTo`. In other words, needs to select the network
     * through which to communicate. Linux, however, able to resolve network on its own,
     * in case of acting with 0.0.0.0 real address resolves in network of other side
     * (source address copies to destination address or vice versa), however,
     * do not rely on it in case of a portable app because windows does not support it.
     */
    public init(bindAt!: UInt16) {
        this(bindAt: IPSocketAddress(IPv4Address.unspecified, bindAt))
    }

    /**
     * Creates an unbound UDP socket ready to bind at the specified interface/port
     */
    public init(bindAt!: SocketAddress) {
        impl = createUdpSocketCommon(bindAt)
    }

    /**
     * UDP peer selected by connect(), or `None` while this socket accepts packets from any sender.
     *
     * @throws SocketException if the socket is already closed.
     */
    public override prop remoteAddress: ?SocketAddress {
        get() {
            udpRemoteAddress(impl)
        }
    }

    /**
     * IP endpoint configured for bind(), or the endpoint currently bound by this UDP socket.
     *
     * @throws SocketException if the socket is already closed.
     */
    public override prop localAddress: SocketAddress {
        get() {
            udpLocalAddress(impl)
        }
    }

    /**
     * Timeout applied to UDP receive and receiveFrom calls.
     *
     * `None` means that a read may wait indefinitely. A finite value is rounded to the socket layer granularity
     * and is never shortened below the requested duration unless the platform reports a larger failure first.
     *
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop receiveTimeout: ?Duration {
        get() {
            udpReceiveTimeout(impl)
        }
        set(timeout) {
            udpReceiveTimeout(impl, timeout)
        }
    }

    /**
     * Timeout applied to UDP send and sendTo calls.
     *
     * `None` lets the write wait without a library-side deadline. Finite durations are normalized to the
     * platform socket granularity before they are stored.
     *
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop sendTimeout: ?Duration {
        get() {
            udpSendTimeout(impl)
        }
        set(timeout) {
            udpSendTimeout(impl, timeout)
        }
    }

    /**
     * Close the socket releasing all resources. All operations except for close() and isClose() are no longer available.
     * This function is reentrant.
     **/
    public override func close(): Unit {
        udpSocketClose(impl)
    }

    /**
     * Checks whether this socket has been explicitly closed via close()
     **/
    public override func isClosed(): Bool {
        udpSocketClosed(impl)
    }

    /**
     * Bind UDP socket at local port
     */
    public func bind(): Unit {
        udpSocketBind(impl)
    }

    /**
     * Configure the socket to only work with the specified remote peer address. To undo this action use disconnect()
     * Note that the remote address should always have the same address family as local at which the socket is bound: for example, both IPv4.
     *
     * After invoking this function with a particular address, function send will use the preset address. Functions send/sendTo may throw exceptions if
     * ICMP abnormal responses are recieved. We will also never receive any messages from other peers (they will be just filtered out).
     * Invoking disconnect() reverts the filter to the initial state so it will be possible to send message to any recipients again.
     *
     * This function should be only invoked after bind()
     *
     * @throws IllegalArgumentException if remote address has wrong kind.
     * @throws SocketException if the socket is not bound.
     * @throws SocketException if the connection cannot be established.
     */
    public func connect(remote: SocketAddress): Unit {
        throwIfIPv4ZeroOnWindows(remote)
        checkUdpSocketAddress(remote, "remote")

        if (remote.family != localAddress.family) {
            throw IllegalArgumentException(
                "remote address kind (${remote.family}) should have the same address family as local (${localAddress.family})")
        }

        impl.remoteAddress = remote
        try {
            impl.connect(None, shouldBeBound: true)
        } catch (e: Exception) {
            impl.remoteAddress = None
            throw e
        }
    }

    /**
     * Clears the UDP peer filter installed by connect().
     *
     * After disconnecting, sendTo can target any compatible IP peer again and receiveFrom may return datagrams
     * from any sender accepted by the operating system. Calling this repeatedly has no additional effect.
     */
    public func disconnect(): Unit {
        impl.disconnect()
    }

    /**
     * Reads one UDP datagram into the provided buffer, waiting until data is available or the timeout expires.
     *
     * The returned pair contains the sender address and the datagram size reported by the platform. When the
     * packet is larger than the buffer, copied bytes are truncated and the reported size may exceed buffer.size.
     *
     * @throws SocketException if buffer is empty, the socket is not bound, or the socket cannot read data.
     * @throws SocketTimeoutException if reading time has expired.
     */
    public override func receiveFrom(buffer: Array<Byte>): (SocketAddress, Int64) {
        impl.receiveFrom(buffer) ?? SocketException.throwClosedException()
    }

    /**
     * Sends the payload as one UDP datagram to the specified IP recipient.
     *
     * The call may block while the socket output queue is full. Platform UDP behavior still applies, so a
     * successfully submitted datagram is not guaranteed to be delivered.
     *
     * @throws IllegalArgumentException if recipient is not compatible with the local address family.
     * @throws SocketException if payload size is larger than allowed by platform.
     * @throws SocketException if connect was called earlier and an abnormal ICMP response is reported.
     */
    public override func sendTo(recipient: SocketAddress, payload: Array<Byte>): Unit {
        throwIfIPv4ZeroOnWindows(
            recipient as IPSocketAddress ?? throw IllegalArgumentException(
                "recipient address kind (${recipient.family}) should have " +
                    "the same address family as local (${localAddress.family})"))

        if (payload.size > 65507) {
            throw SocketException("Unable to send datagram larger than 65507.")
        }
        if (recipient.family != impl.kind) {
            throw IllegalArgumentException(
                "recipient address kind (${recipient.family}) should have " +
                    "the same address family as local (${localAddress.family})")
        }

        impl.send(payload, recipient)
    }

    /**
     * Send a message with the specified payload to the peer with preconfigured address.
     * This only works if address has been specified using `connect() ` otherwise will fail immediately.
     *
     * In other aspects, it works the same as regular `sendTo(recipient,payload).
     *
     * @throws SocketException if not connected, not bound or already closed
     * @throws SocketException if payload size is larger than allowed by platform.
     * @throws SocketException if connect was preliminary called and abnormal ICMP was received.
     */
    public func send(payload: Array<Byte>): Unit {
        if (OS == "macOS") {
            impl.write(payload)
        } else {
            sendTo(remoteAddress ?? SocketException.notYetConnected(), payload)
        }
    }

    /**
     * Receive a datagram message from the preconfigured peer address.
     * This only works if the address has been specified via connect() otherwise will fail.
     * In other aspects, it works the same as regular `receiveFrom(buffer).
     *
     * @throws SocketException if buffer is empty or if it is not possible to read the data.
     * @throws SocketException if not connected, not bound or already closed
     */
    public func receive(buffer: Array<Byte>): Int64 {
        let _ = impl.remoteAddress ?? SocketException.notYetConnected()
        let (_, size) = receiveFrom(buffer)
        return size
    }

    /**
     * When binding socket to a local port, try to reuse it even if it's already used and bound.
     *
     * Please note that there are limitations on when ports could be reused. Behaviour of this option
     * is system-dependant (e.g. this option is unavailable on Windows).
     */
    @When[os != "Windows"]
    public mut prop reusePort: Bool {
        get() {
            impl.getSocketOptionBool(SOL_SOCKET, SOCK_REUSEPORT)
        }
        set(reuse) {
            impl.setSocketOptionBool(SOL_SOCKET, SOCK_REUSEPORT, reuse)
        }
    }

    @When[os == "Windows"]
    @Deprecated
    public mut prop reusePort: Bool {
        get() {
            impl.getSocketOptionBool(SOL_SOCKET, SOCK_REUSEPORT)
        }
        set(reuse) {
            impl.setSocketOptionBool(SOL_SOCKET, SOCK_REUSEPORT, reuse)
        }
    }

    /**
     * When binding socket, try to reuse the address even if it's already used and bound. This property configures SO_REUSEADDR.
     * This is especially useful when doing multicasting. Behaviour of this option is system-dependant.
     * Please consult with SO_REUSEADDR/SOCK_REUSEADDR documentation before using.
     */
    public mut prop reuseAddress: Bool {
        get() {
            impl.getSocketOptionBool(SOL_SOCKET, SOCK_REUSEADDR)
        }
        set(reuse) {
            impl.setSocketOptionBool(SOL_SOCKET, SOCK_REUSEADDR, reuse)
        }
    }

    /** Gets or sets this UdpSocket SO_SNDBUF size; non-positive updates throw IllegalArgumentException. */
    public mut prop sendBufferSize: Int64 {
        get() {
            socketSendBufferSize(impl)
        }
        set(newSize) {
            socketSendBufferSize(impl, newSize)
        }
    }

    /** Gets or sets this UdpSocket SO_RCVBUF size; non-positive updates throw IllegalArgumentException. */
    public mut prop receiveBufferSize: Int64 {
        get() {
            socketReceiveBufferSize(impl)
        }
        set(newSize) {
            socketReceiveBufferSize(impl, newSize)
        }
    }

    /** Reads a UdpSocket raw option into `value`; failures are reported as SocketException. */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        socketGetOption(impl, level, option, value, valueLength)
    }

    /** Writes a UdpSocket raw option from `value`; failures are reported as SocketException. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        socketSetOption(impl, level, option, value, valueLength)
    }

    /** Reads a UdpSocket option represented as IntNative. */
    public func getSocketOptionIntNative(level: Int32, option: Int32): IntNative {
        socketGetOptionIntNative(impl, level, option)
    }

    /** Writes a UdpSocket option represented as IntNative. */
    public func setSocketOptionIntNative(level: Int32, option: Int32, value: IntNative): Unit {
        socketSetOptionIntNative(impl, level, option, value)
    }

    /** Reads a UdpSocket option represented as Bool. */
    public func getSocketOptionBool(level: Int32, option: Int32): Bool {
        socketGetOptionBool(impl, level, option)
    }

    /** Writes a UdpSocket option represented as Bool. */
    public func setSocketOptionBool(level: Int32, option: Int32, value: Bool): Unit {
        socketSetOptionBool(impl, level, option, value)
    }

    public override func toString(): String {
        "UdpSocket(${impl.toString()})"
    }
}
