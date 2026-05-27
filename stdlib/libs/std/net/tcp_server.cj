/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.net

/**
 * TCP listener for accepting incoming network stream connections.
 *
 * A new instance is configurable until bind() starts listening on the selected local IP endpoint. The operating system
 * owns the pending connection queue, and accept() returns TcpSocket instances taken from that queue.
 *
 * Close instances explicitly even if bind() has not been called.
 */
public class TcpServerSocket <: ServerSocket {
    private let impl: SocketCommon<ActualTcpPlatformSocket>
    private var backlogSize_: Int32 = SOCKET_DEFAULT_BACKLOG

    /**
     * Local IP endpoint configured for this TCP listener.
     *
     * If bindAt used port zero, the actual port becomes visible after bind succeeds.
     *
     * @throws SocketException if the socket is already closed.
     */
    public override prop localAddress: SocketAddress {
        get() {
            impl.localAddress ?? SocketException.notYetBound()
        }
    }

    /**
     * Creates a TCP server socket that is not yet bound so client can't connects until we do bind()
     * bindAt specifies the local port to bind at or zero to bind at a random free local port
     */
    public init(bindAt!: UInt16) {
        this(bindAt: IPSocketAddress(IPv4Address.unspecified, bindAt))
    }

    /**
     * Creates a TCP server socket that is not yet bound so clients can't connect until we do bind()
     * bindAt specifies the local address to bind at, possibly with zero port to bind at a random free local port
     */
    public init(bindAt!: SocketAddress) {
        checkAddress(bindAt, "bindAt")

        impl = SocketCommon(SocketNet.TCP, bindAt.family, SocketMode.StreamingMode)
        impl.localAddress = bindAt

        setSocketOptionBool(SOL_SOCKET, SOCK_REUSEADDR, true)
    }

    /**
     * When binding socket, try to reuse the address even if it's already used and bound. This property configures SO_REUSEADDR.
     * This is usually makes sense when we are binding and there are still opened connections remaining from the previous socket.
     * Such old sockets are usually hanging in TIME_WAIT state and may prevent us from binding a new server socket.
     * Enabled by default
     * Behaviour of this option is system-dependant. Please consult with SO_REUSEADDR/SOCK_REUSEADDR documentation before using.
     */
    public mut prop reuseAddress: Bool {
        get() {
            impl.getSocketOptionBool(SOL_SOCKET, SOCK_REUSEADDR)
        }
        set(enabled) {
            impl.setSocketOptionBool(SOL_SOCKET, SOCK_REUSEADDR, enabled)
        }
    }

    /**
     * When binding socket to a local port, try to reuse it even if it's already used and bound.
     *
     * Please note that there are limitations on when ports could be reused. Behaviour of this option
     * is system-dependant (e.g. this option is unavailable on Windows).
     * Please consult with SO_REUSEPORT documentation before using.
     *
     * This option could be only modified before binding and will fail
     * after a successful bind() invocation.
     */
    @When[os != "Windows"]
    public mut prop reusePort: Bool {
        get() {
            impl.getSocketOptionBool(SOL_SOCKET, SOCK_REUSEPORT)
        }
        set(enabled) {
            impl.setSocketOptionBool(SOL_SOCKET, SOCK_REUSEPORT, enabled)
        }
    }

    @When[os == "Windows"]
    @Deprecated
    public mut prop reusePort: Bool {
        get() {
            impl.getSocketOptionBool(SOL_SOCKET, SOCK_REUSEPORT)
        }
        set(enabled) {
            impl.setSocketOptionBool(SOL_SOCKET, SOCK_REUSEPORT, enabled)
        }
    }

    /** Gets or sets this TcpServerSocket SO_SNDBUF size; non-positive updates throw IllegalArgumentException. */
    public mut prop sendBufferSize: Int64 {
        get() {
            socketSendBufferSize(impl)
        }
        set(newSize) {
            socketSendBufferSize(impl, newSize)
        }
    }

    /** Gets or sets this TcpServerSocket SO_RCVBUF size; non-positive updates throw IllegalArgumentException. */
    public mut prop receiveBufferSize: Int64 {
        get() {
            socketReceiveBufferSize(impl)
        }
        set(newSize) {
            socketReceiveBufferSize(impl, newSize)
        }
    }

    /**
     * Network interface name to bind to.
     * This option is a hint for the operating system that may decide to ignore the value
     * or reject an attempt to configure it.
     */
    public mut prop bindToDevice: ?String {
        get() {
            impl.bindToDevice
        }
        set(newDevice) {
            impl.bindToDevice = newDevice
        }
    }

    /**
     * Configure incoming connections backlog size. This only works before binding socket.
     * Changing this value is not guaranteed to be actually applied since the operating system may decide to
     * change or bump it, or simply ignore.
     **/
    public mut prop backlogSize: Int64 {
        get() {
            Int64(backlogSize_)
        }
        set(newSize) {
            backlogSize_ = checkedSocketBacklogSize(impl, newSize)
        }
    }

    /**
     * Bind TCP socket on a local port. Depending on [reusePort] and [reuseAddress] flag, it may fail if the port is already occupied
     * or when there are connections remaining from the previously bound socket.
     * This function also does listen just after binding creating an incoming connections queue that could be accessed via "accept()" function.
     */
    public override func bind(): Unit {
        impl.bind(backlogSize_)
    }

    /**
     * Accepts the next TCP client socket, waiting up to the specified timeout.
     *
     * The listening backlog is consumed one connection at a time. If no accepted connection is ready, this call waits
     * until a client is available or the timeout expires.
     *
     * @throws SocketTimeoutException when the specified timeout is over.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override func accept(timeout!: ?Duration): TcpSocket {
        let accepted = impl.accept(timeout) ?? SocketException.throwClosedException()
        return TcpSocket(accepted)
    }

    /**
     * Accepts the next TCP client socket without a library-side timeout.
     *
     * When the backlog queue is empty, this call waits until the platform reports a pending connection.
     */
    public override func accept(): TcpSocket {
        accept(timeout: None)
    }

    /**
     * Closes this TCP listener and releases the underlying socket resource.
     *
     * Calling close more than once is allowed; after closing, operations other than close and isClosed are unavailable.
     **/
    public override func close(): Unit {
        impl.close()
    }

    /**
     * Returns whether this TCP listener has been explicitly closed.
     **/
    public override func isClosed(): Bool {
        impl.isClosed()
    }

    /** Reads a TcpServerSocket raw option into `value`; failures are reported as SocketException. */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        socketGetOption(impl, level, option, value, valueLength)
    }

    /** Writes a TcpServerSocket raw option from `value`; failures are reported as SocketException. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        socketSetOption(impl, level, option, value, valueLength)
    }

    /** Reads a TcpServerSocket option represented as IntNative. */
    public func getSocketOptionIntNative(level: Int32, option: Int32): IntNative {
        socketGetOptionIntNative(impl, level, option)
    }

    /** Writes a TcpServerSocket option represented as IntNative. */
    public func setSocketOptionIntNative(level: Int32, option: Int32, value: IntNative): Unit {
        socketSetOptionIntNative(impl, level, option, value)
    }

    /** Reads a TcpServerSocket option represented as Bool. */
    public func getSocketOptionBool(level: Int32, option: Int32): Bool {
        socketGetOptionBool(impl, level, option)
    }

    /** Writes a TcpServerSocket option represented as Bool. */
    public func setSocketOptionBool(level: Int32, option: Int32, value: Bool): Unit {
        socketSetOptionBool(impl, level, option, value)
    }

    public override func toString(): String {
        "TcpServerSocket(${impl.toString()})"
    }

    private static func checkAddress(address: SocketAddress, name: String): SocketAddress {
        if (!(address is IPSocketAddress)) {
            throw IllegalArgumentException(
                "${name} should be either IPv4 or IPv6 but got ${address.family}: ${address}")
        }
        return address
    }
}
