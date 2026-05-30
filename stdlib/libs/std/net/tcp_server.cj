/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.net

/**
 * TCP server socket providing a way to listen for TCP incoming connections.
 *
 * Once created, could be configured via corresponding properties (e.g. reusePort) or setSocketOptionXX functions.
 *
 * To start listening, use bind() function that does bind socket on a local port and start listening for connections.
 *
 * Receiving an incoming connection is provided via accept() function that does wait for the next connection or returns immediately
 * if there is already pending connection.
 *
 * Instances of this type should be explicitly closed even when the bind() hasn't been invoked.
 */
public class TcpServerSocket <: ServerSocket {
    private let impl: SocketCommon<ActualTcpPlatformSocket>
    private var backlogSize_: Int32 = SOCKET_DEFAULT_BACKLOG

    /**
     * Local address the socket will be or is currently bound at.
     *
     * @throws SocketException is the socket is already closed.
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

    /** SO_SNDBUF hint for outgoing data; throws IllegalArgumentException for non-positive sizes. */
    public mut prop sendBufferSize: Int64 {
        get() {
            // Server sockets expose the accepted sockets' outgoing buffer hint through the listening descriptor.
            impl.getSendBufferSize()
        }
        set(newSize) {
            impl.setSendBufferSize(newSize)
        }
    }

    /** SO_RCVBUF hint for incoming data; throws IllegalArgumentException for non-positive sizes. */
    public mut prop receiveBufferSize: Int64 {
        get() {
            impl.getReceiveBufferSize()
        }
        set(newSize) {
            // The configured value is a platform hint and may be rounded by the OS.
            impl.setReceiveBufferSize(newSize)
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
            // The public type exposes backlog as Int64 while the native call stores Int32.
            Int64(backlogSize_)
        }
        set(newSize) {
            impl.checkNotBound()
            impl.checkNotClosed()
            backlogSize_ = match {
                case newSize <= 0 => throw IllegalArgumentException("BacklogSize should be positive: ${newSize}.")
                case newSize >= Int64(Int32.Max) => Int32.Max - 1
                case _ => Int32(newSize)
            }
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
     * Accept a client TCP socket, waiting for one if there are no pending connection requests.
     *
     * The OS implementation usually provides an incoming connection requests queue (called backlog),
     * so calling accept() does takes a candidate from the queue
     * or wait until we get some request if the queue is empty.
     *
     * Usually, a TCP client is waiting for connection until we get it from server.accept() function. If the server don't
     * get it in time then the client may fail to connect with error (e.g. connection timeout error). If the backlog queue is full
     * due to the missing accept() invocations then the operating system usually start rejecting new incoming connections that don't fit
     * the backlog queue capacity.
     * This fact could be used for backpressure control so if a server detects that no requests could be processed for some reason
     * then it may stop doing accept() to keep client in the queue and limit workload.
     *
     * The specified timeout is applied to accept operation
     * @throws SocketTimeoutException when the specified timeout is over
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override func accept(timeout!: ?Duration): TcpSocket {
        let accepted = impl.accept(timeout) ?? SocketException.throwClosedException()
        return TcpSocket(accepted)
    }

    /**
     * Accept a client TCP socket without applying an explicit timeout.
     *
     * This overload delegates to accept(timeout: None), so it waits until a client
     * is accepted or the server socket is closed.
     *
     * The operating system backlog queue still controls pending connection
     * capacity, and clients may time out if they wait there for too long.
     */
    public override func accept(): TcpSocket {
        accept(timeout: None)
    }

    /** Close this socket and release its resources. This function is reentrant. */
    public override func close(): Unit {
        // Closing the listener leaves already accepted TcpSocket instances independent.
        impl.close()
    }

    /** Return whether close() has already been invoked for this socket. */
    public override func isClosed(): Bool {
        impl.isClosed()
    }

    /** Read a raw TCP server socket option; valueLength is updated with the native result size. */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        unsafe { impl.getSocketOption(level, option, value, valueLength) }
    }

    /** Write a raw TCP server socket option; native setsockopt failures are reported as exceptions. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        unsafe { impl.setSocketOption(level, option, value, valueLength) }
    }

    /** Read a TCP server socket option as IntNative, requiring the native result size to match. */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        impl.getSocketOptionIntNative(level, option)
    }

    /** Write an IntNative value to a TCP server socket option. */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        impl.setSocketOptionIntNative(level, option, value)
    }

    /** Read a TCP server socket option as Bool using IntNative zero/nonzero conversion. */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        // Server socket bool options are represented by native integer values.
        impl.getSocketOptionBool(level, option)
    }

    /** Write a Bool value to a TCP server socket option using IntNative conversion. */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        impl.setSocketOptionBool(level, option, value)
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
