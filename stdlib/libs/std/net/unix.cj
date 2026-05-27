/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.net

/**
 * Package-private function that checks unix domain socket address for validity (to connect or bind)
 */
func checkUnixSocketAddress(address: SocketAddress, name: String): SocketAddress {
    if (!(address is UnixSocketAddress)) {
        throw IllegalArgumentException("${name} should be a Unix Path address but got ${address.family}: ${address}")
    }
    return address
}

private func unixSocketAddress(path: ?String): ?SocketAddress {
    match (path) {
        case Some(value) => Some(UnixSocketAddress(value))
        case None => None
    }
}

private func setUnixSocketLocalAddress(impl: SocketCommon<ActualPlatformSocket>, address: SocketAddress): Unit {
    checkUnixSocketAddress(address, "localAddress")
    impl.localAddress = address
}

private func createUnixSocketCommon(
    address: SocketAddress,
    localAddress: ?SocketAddress
): SocketCommon<ActualPlatformSocket> {
    checkUnixSocketAddress(address, "address")
    let impl = SocketCommon<ActualPlatformSocket>(SocketNet.UNIX, AddressFamily.UNIX, StreamingMode)
    impl.remoteAddress = address
    if (let Some(value) <- localAddress) {
        setUnixSocketLocalAddress(impl, value)
    }
    return impl
}

private func unixSocketRemoteAddress(impl: SocketCommon<ActualPlatformSocket>): SocketAddress {
    impl.remoteAddress ?? SocketException.notYetConnected()
}

private func unixSocketLocalAddress(impl: SocketCommon<ActualPlatformSocket>): SocketAddress {
    impl.localAddress ?? SocketException.notYetConnected()
}

private func unixReadTimeout(impl: SocketCommon<ActualPlatformSocket>): ?Duration {
    impl.readTimeout
}

private func unixReadTimeout(impl: SocketCommon<ActualPlatformSocket>, timeout: ?Duration): Unit {
    impl.readTimeout = timeout?.throwIfNegative("Read timeout").toNanosecondGranularity()
}

private func unixWriteTimeout(impl: SocketCommon<ActualPlatformSocket>): ?Duration {
    impl.writeTimeout
}

private func unixWriteTimeout(impl: SocketCommon<ActualPlatformSocket>, timeout: ?Duration): Unit {
    impl.writeTimeout = timeout?.throwIfNegative("Write timeout").toNanosecondGranularity()
}

private func unixSocketRead(impl: SocketCommon<ActualPlatformSocket>, buffer: Array<Byte>): Int64 {
    impl.read(buffer)
}

private func unixSocketWrite(impl: SocketCommon<ActualPlatformSocket>, buffer: Array<Byte>): Unit {
    impl.write(buffer)
}

private func unixSocketConnect(impl: SocketCommon<ActualPlatformSocket>, timeout: ?Duration): Unit {
    impl.connect(timeout?.throwIfNegative("Timeout"))
}

private func socketClosed(impl: SocketCommon<ActualPlatformSocket>): Bool {
    impl.isClosed()
}

private func socketClose(impl: SocketCommon<ActualPlatformSocket>): Unit {
    impl.close()
}

private func unixServerLocalAddress(impl: SocketCommon<ActualPlatformSocket>): SocketAddress {
    impl.localAddress ?? SocketException.notYetBound()
}

private func createUnixServerSocketCommon(bindAt: SocketAddress): SocketCommon<ActualPlatformSocket> {
    checkUnixSocketAddress(bindAt, "bindAt")
    let impl = SocketCommon<ActualPlatformSocket>(UNIX, AddressFamily.UNIX, SocketMode.StreamingMode)
    impl.localAddress = bindAt
    return impl
}

private func unixServerBacklogSize(size: Int32): Int64 {
    Int64(size)
}

private func unixServerBind(impl: SocketCommon<ActualPlatformSocket>, backlogSize: Int32): Unit {
    impl.bind(backlogSize)
}

private func unixServerAccept(impl: SocketCommon<ActualPlatformSocket>, timeout: ?Duration): UnixSocket {
    let accepted = impl.accept(timeout) ?? SocketException.throwClosedException()
    return UnixSocket(accepted)
}

private func windowsUnixSocketReadTimeout(impl: SocketCommon<ActualPlatformSocket>): ?Duration {
    unixReadTimeout(impl)
}

private func windowsUnixSocketReadTimeout(impl: SocketCommon<ActualPlatformSocket>, timeout: ?Duration): Unit {
    unixReadTimeout(impl, timeout)
}

private func windowsUnixSocketWriteTimeout(impl: SocketCommon<ActualPlatformSocket>): ?Duration {
    unixWriteTimeout(impl)
}

private func windowsUnixSocketWriteTimeout(impl: SocketCommon<ActualPlatformSocket>, timeout: ?Duration): Unit {
    unixWriteTimeout(impl, timeout)
}

private func windowsUnixSocketSendBufferSize(impl: SocketCommon<ActualPlatformSocket>): Int64 {
    socketSendBufferSize(impl)
}

private func windowsUnixSocketSendBufferSize(impl: SocketCommon<ActualPlatformSocket>, newSize: Int64): Unit {
    socketSendBufferSize(impl, newSize)
}

private func windowsUnixSocketReceiveBufferSize(impl: SocketCommon<ActualPlatformSocket>): Int64 {
    socketReceiveBufferSize(impl)
}

private func windowsUnixSocketReceiveBufferSize(impl: SocketCommon<ActualPlatformSocket>, newSize: Int64): Unit {
    socketReceiveBufferSize(impl, newSize)
}

private func windowsUnixSocketRead(impl: SocketCommon<ActualPlatformSocket>, buffer: Array<Byte>): Int64 {
    unixSocketRead(impl, buffer)
}

private func windowsUnixSocketWrite(impl: SocketCommon<ActualPlatformSocket>, buffer: Array<Byte>): Unit {
    unixSocketWrite(impl, buffer)
}

private func windowsUnixSocketConnect(impl: SocketCommon<ActualPlatformSocket>, timeout: ?Duration): Unit {
    unixSocketConnect(impl, timeout)
}

private func windowsUnixSocketGetOption(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32,
    value: CPointer<Unit>,
    valueLength: CPointer<UIntNative>
): Unit {
    socketGetOption(impl, level, option, value, valueLength)
}

private func windowsUnixSocketSetOption(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32,
    value: CPointer<Unit>,
    valueLength: UIntNative
): Unit {
    socketSetOption(impl, level, option, value, valueLength)
}

private func windowsUnixSocketGetOptionIntNative(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32
): IntNative {
    socketGetOptionIntNative(impl, level, option)
}

private func windowsUnixSocketSetOptionIntNative(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32,
    value: IntNative
): Unit {
    socketSetOptionIntNative(impl, level, option, value)
}

private func windowsUnixSocketGetOptionBool(impl: SocketCommon<ActualPlatformSocket>, level: Int32, option: Int32): Bool {
    socketGetOptionBool(impl, level, option)
}

private func windowsUnixSocketSetOptionBool(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32,
    value: Bool
): Unit {
    socketSetOptionBool(impl, level, option, value)
}

private func windowsUnixSocketClose(impl: SocketCommon<ActualPlatformSocket>): Unit {
    socketClose(impl)
}

private func windowsUnixSocketClosed(impl: SocketCommon<ActualPlatformSocket>): Bool {
    socketClosed(impl)
}

private func windowsUnixServerSendBufferSize(impl: SocketCommon<ActualPlatformSocket>): Int64 {
    socketSendBufferSize(impl)
}

private func windowsUnixServerSendBufferSize(impl: SocketCommon<ActualPlatformSocket>, newSize: Int64): Unit {
    socketSendBufferSize(impl, newSize)
}

private func windowsUnixServerReceiveBufferSize(impl: SocketCommon<ActualPlatformSocket>): Int64 {
    socketReceiveBufferSize(impl)
}

private func windowsUnixServerReceiveBufferSize(impl: SocketCommon<ActualPlatformSocket>, newSize: Int64): Unit {
    socketReceiveBufferSize(impl, newSize)
}

private func windowsUnixServerGetOption(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32,
    value: CPointer<Unit>,
    valueLength: CPointer<UIntNative>
): Unit {
    socketGetOption(impl, level, option, value, valueLength)
}

private func windowsUnixServerSetOption(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32,
    value: CPointer<Unit>,
    valueLength: UIntNative
): Unit {
    socketSetOption(impl, level, option, value, valueLength)
}

private func windowsUnixServerGetOptionIntNative(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32
): IntNative {
    socketGetOptionIntNative(impl, level, option)
}

private func windowsUnixServerSetOptionIntNative(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32,
    value: IntNative
): Unit {
    socketSetOptionIntNative(impl, level, option, value)
}

private func windowsUnixServerGetOptionBool(impl: SocketCommon<ActualPlatformSocket>, level: Int32, option: Int32): Bool {
    socketGetOptionBool(impl, level, option)
}

private func windowsUnixServerSetOptionBool(
    impl: SocketCommon<ActualPlatformSocket>,
    level: Int32,
    option: Int32,
    value: Bool
): Unit {
    socketSetOptionBool(impl, level, option, value)
}

private func windowsUnixServerClose(impl: SocketCommon<ActualPlatformSocket>): Unit {
    socketClose(impl)
}

private func windowsUnixServerClosed(impl: SocketCommon<ActualPlatformSocket>): Bool {
    socketClosed(impl)
}

/*
 * Represents a Unix domain streaming socket.
 *
 * Once an instance is created, it is not yet connected so should be connected explicitly via connect().
 *
 * Instances of this type should be explicitly closed even when the connect() hasn't been invoked.
 *
 * @see StreamingSocket for more details on how do streaming sockets work.
 */
@When[os != "Windows"]
public class UnixSocket <: StreamingSocket {
    private let impl: SocketCommon<ActualPlatformSocket>

    private func setLocalAddress(address: SocketAddress) {
        checkUnixSocketAddress(address, "localAddress")
        impl.localAddress = address
    }

    /**
     * Create an unconnected Unix domain socket ready to connect to the specified socket path
     *
     * @param path to connect to
     */
    public init(path: String, localPath!: ?String = None) {
        this(UnixSocketAddress(path))
        if (let Some(_localPath) <- localPath) {
            setLocalAddress(UnixSocketAddress(_localPath))
        }
    }

    /**
     * Create an unconnected Unix domain socket ready to connect to the specified socket path
     * @param address to connect to
     */
    public init(address: SocketAddress, localAddress!: ?SocketAddress = None) {
        checkUnixSocketAddress(address, "address")
        this.impl = SocketCommon<ActualPlatformSocket>(SocketNet.UNIX, AddressFamily.UNIX, StreamingMode)
        impl.remoteAddress = address
        if (let Some(_localAddress) <- localAddress) {
            setLocalAddress(_localAddress)
        }
    }

    /**
     * Creates an internally precreated socket. See UnixServerSocket.accept.
     */
    init(impl: SocketCommon<ActualPlatformSocket>) {
        this.impl = impl
    }

    /**
     * Unix domain socket address of the connected peer.
     *
     * The value is available after a successful connect or accept and identifies the peer socket path or abstract address.
     *
     * @throws SocketException if the socket is already closed or has no connected peer.
     */
    public override prop remoteAddress: SocketAddress {
        get() {
            unixSocketRemoteAddress(impl)
        }
    }

    /**
     * Local Unix domain socket address associated with this stream socket.
     *
     * For client sockets, this may be assigned by the platform during connect when no explicit local address was provided.
     *
     * @throws SocketException if the socket is already closed or no local endpoint has been assigned.
     */
    public override prop localAddress: SocketAddress {
        get() {
            unixSocketLocalAddress(impl)
        }
    }

    /**
     * Timeout used while reading from this Unix domain stream.
     *
     * `None` leaves reads without a library-side deadline. Finite durations are normalized before reaching the platform
     * socket implementation.
     *
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop readTimeout: ?Duration {
        get() {
            unixReadTimeout(impl)
        }
        set(timeout) {
            unixReadTimeout(impl, timeout)
        }
    }

    /**
     * Timeout used while writing to this Unix domain stream.
     *
     * `None` means no library-side write deadline. Finite durations are stored using the socket timeout granularity
     * supported by the platform.
     *
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop writeTimeout: ?Duration {
        get() {
            unixWriteTimeout(impl)
        }
        set(timeout) {
            unixWriteTimeout(impl, timeout)
        }
    }

    /** Gets or sets this UnixSocket send buffer size; non-positive updates throw IllegalArgumentException. */
    public mut prop sendBufferSize: Int64 {
        get() {
            socketSendBufferSize(impl)
        }
        set(newSize) {
            socketSendBufferSize(impl, newSize)
        }
    }

    /** Gets or sets this UnixSocket receive buffer size; non-positive updates throw IllegalArgumentException. */
    public mut prop receiveBufferSize: Int64 {
        get() {
            socketReceiveBufferSize(impl)
        }
        set(newSize) {
            socketReceiveBufferSize(impl, newSize)
        }
    }

    public override func read(buffer: Array<Byte>): Int64 {
        unixSocketRead(impl, buffer)
    }

    public override func write(buffer: Array<Byte>): Unit {
        unixSocketWrite(impl, buffer)
    }

    /**
     * Connects this Unix domain stream socket to its configured peer.
     *
     * The socket is bound as part of connect when needed. Passing `None` leaves the connect attempt without a
     * library-side timeout, and failed attempts are reported directly without retry.
     *
     * @throws SocketException if the connection cannot be established.
     * @throws SocketTimeoutException if the waiting time has expired.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public func connect(timeout!: ?Duration = None): Unit {
        unixSocketConnect(impl, timeout)
    }

    /** Reads a UnixSocket raw option into `value`; failures are reported as SocketException. */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        socketGetOption(impl, level, option, value, valueLength)
    }

    /** Writes a UnixSocket raw option from `value`; failures are reported as SocketException. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        socketSetOption(impl, level, option, value, valueLength)
    }

    /** Reads a UnixSocket option represented as IntNative. */
    public func getSocketOptionIntNative(level: Int32, option: Int32): IntNative {
        socketGetOptionIntNative(impl, level, option)
    }

    /** Writes a UnixSocket option represented as IntNative. */
    public func setSocketOptionIntNative(level: Int32, option: Int32, value: IntNative): Unit {
        socketSetOptionIntNative(impl, level, option, value)
    }

    /** Reads a UnixSocket option represented as Bool. */
    public func getSocketOptionBool(level: Int32, option: Int32): Bool {
        socketGetOptionBool(impl, level, option)
    }

    /** Writes a UnixSocket option represented as Bool. */
    public func setSocketOptionBool(level: Int32, option: Int32, value: Bool): Unit {
        socketSetOptionBool(impl, level, option, value)
    }

    /**
     * Close the socket releasing all resources. All operations except for close() and isClose() are no longer available.
     * This function is reentrant.
     **/
    public func close(): Unit {
        socketClose(impl)
    }

    /**
     * Checks whether this socket has been explicitly closed via close()
     **/
    public func isClosed(): Bool {
        socketClosed(impl)
    }

    public override func toString(): String {
        "UnixSocket(${impl.toString()})"
    }
}

@When[os == "Windows"]
@Deprecated
public class UnixSocket <: StreamingSocket {
    private let impl: SocketCommon<ActualPlatformSocket>

    private func setLocalAddress(address: SocketAddress) {
        setUnixSocketLocalAddress(impl, address)
    }

    public init(path: String, localPath!: ?String = None) {
        this(UnixSocketAddress(path), localAddress: unixSocketAddress(localPath))
    }

    public init(address: SocketAddress, localAddress!: ?SocketAddress = None) {
        this.impl = createUnixSocketCommon(address, localAddress)
    }

    init(impl: SocketCommon<ActualPlatformSocket>) {
        this.impl = impl
    }

    public override prop remoteAddress: SocketAddress {
        get() {
            unixSocketRemoteAddress(impl)
        }
    }

    public override prop localAddress: SocketAddress {
        get() {
            unixSocketLocalAddress(impl)
        }
    }

    public override mut prop readTimeout: ?Duration {
        get() {
            windowsUnixSocketReadTimeout(impl)
        }
        set(timeout) {
            windowsUnixSocketReadTimeout(impl, timeout)
        }
    }

    public override mut prop writeTimeout: ?Duration {
        get() {
            windowsUnixSocketWriteTimeout(impl)
        }
        set(timeout) {
            windowsUnixSocketWriteTimeout(impl, timeout)
        }
    }

    /** Gets or sets the Windows compatibility UnixSocket send buffer size. */
    public mut prop sendBufferSize: Int64 {
        get() {
            windowsUnixSocketSendBufferSize(impl)
        }
        set(newSize) {
            windowsUnixSocketSendBufferSize(impl, newSize)
        }
    }

    /** Gets or sets the Windows compatibility UnixSocket receive buffer size. */
    public mut prop receiveBufferSize: Int64 {
        get() {
            windowsUnixSocketReceiveBufferSize(impl)
        }
        set(newSize) {
            windowsUnixSocketReceiveBufferSize(impl, newSize)
        }
    }

    public override func read(buffer: Array<Byte>): Int64 {
        windowsUnixSocketRead(impl, buffer)
    }

    public override func write(buffer: Array<Byte>): Unit {
        windowsUnixSocketWrite(impl, buffer)
    }

    public func connect(timeout!: ?Duration = None): Unit {
        windowsUnixSocketConnect(impl, timeout)
    }

    /** Reads a Windows compatibility UnixSocket raw option into `value`. */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        windowsUnixSocketGetOption(impl, level, option, value, valueLength)
    }

    /** Writes a Windows compatibility UnixSocket raw option from `value`. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        windowsUnixSocketSetOption(impl, level, option, value, valueLength)
    }

    /** Reads a Windows compatibility UnixSocket option represented as IntNative. */
    public func getSocketOptionIntNative(level: Int32, option: Int32): IntNative {
        windowsUnixSocketGetOptionIntNative(impl, level, option)
    }

    /** Writes a Windows compatibility UnixSocket option represented as IntNative. */
    public func setSocketOptionIntNative(level: Int32, option: Int32, value: IntNative): Unit {
        windowsUnixSocketSetOptionIntNative(impl, level, option, value)
    }

    /** Reads a Windows compatibility UnixSocket option represented as Bool. */
    public func getSocketOptionBool(level: Int32, option: Int32): Bool {
        windowsUnixSocketGetOptionBool(impl, level, option)
    }

    /** Writes a Windows compatibility UnixSocket option represented as Bool. */
    public func setSocketOptionBool(level: Int32, option: Int32, value: Bool): Unit {
        windowsUnixSocketSetOptionBool(impl, level, option, value)
    }

    public func close(): Unit {
        windowsUnixSocketClose(impl)
    }

    public func isClosed(): Bool {
        windowsUnixSocketClosed(impl)
    }

    public override func toString(): String {
        "UnixSocket(${impl.toString()})"
    }
}

/**
 * Unix Domain server socket providing a way to listen for incoming connections.
 *
 * Once created, could be configured via corresponding properties (e.g. reusePort) or setSocketOptionXX functions.
 *
 * To start listening, use bind() function that does bind socket on a local path and start listening for connections.
 * The local path to bind at should be a non-existing path otherwise bind() will fail.
 *
 * Receiving an incoming connection is provided via accept() function that does wait for the next connection or returns immediately
 * if there is already pending connection.
 *
 * Instances of this type should be explicitly closed even when the bind() hasn't been invoked.
 */
@When[os != "Windows"]
public class UnixServerSocket <: ServerSocket {
    private let impl: SocketCommon<ActualPlatformSocket>
    private var backlogSize_: Int32 = SOCKET_DEFAULT_BACKLOG

    /**
     * Unix domain address where this server socket is configured to listen.
     *
     * The value names the local socket path or abstract address used when bind() creates the listening socket.
     *
     * @throws SocketException if the socket is already closed.
     */
    public override prop localAddress: SocketAddress {
        get() {
            impl.localAddress ?? SocketException.notYetBound()
        }
    }

    /**
     * Creates an anbound Unix server streaming socket configured to bind at the specified path
     * @param bindAt path for the unix server socket
     **/
    public init(bindAt!: String) {
        this(bindAt: UnixSocketAddress(bindAt))
    }

    /**
     * Creates an anbound Unix server streaming socket configured to bind at the specified path
     * @param bindAt path for the unix server socket
     **/
    public init(bindAt!: SocketAddress) {
        checkUnixSocketAddress(bindAt, "bindAt")

        let impl = SocketCommon<ActualPlatformSocket>(UNIX, AddressFamily.UNIX, SocketMode.StreamingMode)
        impl.localAddress = bindAt
        this.impl = impl
    }

    /** Gets or sets this UnixServerSocket send buffer size; non-positive updates throw IllegalArgumentException. */
    public mut prop sendBufferSize: Int64 {
        get() {
            socketSendBufferSize(impl)
        }
        set(newSize) {
            socketSendBufferSize(impl, newSize)
        }
    }

    /** Gets or sets this UnixServerSocket receive buffer size; non-positive updates throw IllegalArgumentException. */
    public mut prop receiveBufferSize: Int64 {
        get() {
            socketReceiveBufferSize(impl)
        }
        set(newSize) {
            socketReceiveBufferSize(impl, newSize)
        }
    }

    /** Configures incoming UNIX connection backlog size before bind(). */
    public mut prop backlogSize: Int64 {
        get() {
            Int64(backlogSize_)
        }
        set(newSize) {
            backlogSize_ = checkedSocketBacklogSize(impl, newSize)
        }
    }

    /**
     * Bind a streaming UNIX domain socket.
     *
     * This function also does listen just after binding creating an incoming connections queue that could be accessed via "accept()" function.
     *
     * This operation does atomically create a socket file at the local path.
     * If the path is already existing then bind() fails with SocketException.
     */
    public override func bind(): Unit {
        impl.bind(backlogSize_)
    }

    /**
     * Accepts the next Unix domain stream connection, waiting up to the specified timeout.
     *
     * The platform listening queue is consumed one connection at a time. If the queue is empty, the call waits until a
     * peer connects or the timeout expires.
     *
     * @throws SocketTimeoutException if the specified timeout elapsed before a connection became available.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override func accept(timeout!: ?Duration): UnixSocket {
        let accepted = impl.accept(timeout) ?? SocketException.throwClosedException()
        return UnixSocket(accepted)
    }

    /**
     * Accepts the next Unix domain stream connection without a library-side timeout.
     *
     * If the listening queue is empty, this call waits until the platform reports a new peer connection.
     */
    public override func accept(): UnixSocket {
        accept(timeout: None)
    }

    /**
     * Closes the Unix server socket and releases the listening descriptor.
     *
     * Calling close more than once is allowed; after closing, operations other than close and isClosed are unavailable.
     **/
    public override func close(): Unit {
        impl.close()
    }

    /**
     * Returns whether this Unix server socket has been explicitly closed.
     **/
    public override func isClosed(): Bool {
        impl.isClosed()
    }

    /** Reads a UnixServerSocket raw option into `value`; failures are reported as SocketException. */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        socketGetOption(impl, level, option, value, valueLength)
    }

    /** Writes a UnixServerSocket raw option from `value`; failures are reported as SocketException. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        socketSetOption(impl, level, option, value, valueLength)
    }

    /** Reads a UnixServerSocket option represented as IntNative. */
    public func getSocketOptionIntNative(level: Int32, option: Int32): IntNative {
        socketGetOptionIntNative(impl, level, option)
    }

    /** Writes a UnixServerSocket option represented as IntNative. */
    public func setSocketOptionIntNative(level: Int32, option: Int32, value: IntNative): Unit {
        socketSetOptionIntNative(impl, level, option, value)
    }

    /** Reads a UnixServerSocket option represented as Bool. */
    public func getSocketOptionBool(level: Int32, option: Int32): Bool {
        socketGetOptionBool(impl, level, option)
    }

    /** Writes a UnixServerSocket option represented as Bool. */
    public func setSocketOptionBool(level: Int32, option: Int32, value: Bool): Unit {
        socketSetOptionBool(impl, level, option, value)
    }

    public override func toString(): String {
        "UnixServerSocket(${impl.toString()})"
    }
}

@When[os == "Windows"]
@Deprecated
public class UnixServerSocket <: ServerSocket {
    private let impl: SocketCommon<ActualPlatformSocket>
    private var backlogSize_: Int32 = SOCKET_DEFAULT_BACKLOG

    public override prop localAddress: SocketAddress {
        get() {
            unixServerLocalAddress(impl)
        }
    }

    public init(bindAt!: String) {
        this(bindAt: UnixSocketAddress(bindAt))
    }

    public init(bindAt!: SocketAddress) {
        this.impl = createUnixServerSocketCommon(bindAt)
    }

    /** Gets or sets the Windows compatibility UnixServerSocket send buffer size. */
    public mut prop sendBufferSize: Int64 {
        get() {
            windowsUnixServerSendBufferSize(impl)
        }
        set(newSize) {
            windowsUnixServerSendBufferSize(impl, newSize)
        }
    }

    /** Gets or sets the Windows compatibility UnixServerSocket receive buffer size. */
    public mut prop receiveBufferSize: Int64 {
        get() {
            windowsUnixServerReceiveBufferSize(impl)
        }
        set(newSize) {
            windowsUnixServerReceiveBufferSize(impl, newSize)
        }
    }

    public mut prop backlogSize: Int64 {
        get() {
            unixServerBacklogSize(backlogSize_)
        }
        set(newSize) {
            backlogSize_ = checkedSocketBacklogSize(impl, newSize)
        }
    }

    public override func bind(): Unit {
        unixServerBind(impl, backlogSize_)
    }

    public override func accept(timeout!: ?Duration): UnixSocket {
        unixServerAccept(impl, timeout)
    }

    public override func accept(): UnixSocket {
        accept(timeout: None)
    }

    public override func close(): Unit {
        windowsUnixServerClose(impl)
    }

    public override func isClosed(): Bool {
        windowsUnixServerClosed(impl)
    }

    /** Reads a Windows compatibility UnixServerSocket raw option into `value`. */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        windowsUnixServerGetOption(impl, level, option, value, valueLength)
    }

    /** Writes a Windows compatibility UnixServerSocket raw option from `value`. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        windowsUnixServerSetOption(impl, level, option, value, valueLength)
    }

    /** Reads a Windows compatibility UnixServerSocket option represented as IntNative. */
    public func getSocketOptionIntNative(level: Int32, option: Int32): IntNative {
        windowsUnixServerGetOptionIntNative(impl, level, option)
    }

    /** Writes a Windows compatibility UnixServerSocket option represented as IntNative. */
    public func setSocketOptionIntNative(level: Int32, option: Int32, value: IntNative): Unit {
        windowsUnixServerSetOptionIntNative(impl, level, option, value)
    }

    /** Reads a Windows compatibility UnixServerSocket option represented as Bool. */
    public func getSocketOptionBool(level: Int32, option: Int32): Bool {
        windowsUnixServerGetOptionBool(impl, level, option)
    }

    /** Writes a Windows compatibility UnixServerSocket option represented as Bool. */
    public func setSocketOptionBool(level: Int32, option: Int32, value: Bool): Unit {
        windowsUnixServerSetOptionBool(impl, level, option, value)
    }

    public override func toString(): String {
        "UnixServerSocket(${impl.toString()})"
    }
}
