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
    private let unixImpl: SocketCommon<ActualPlatformSocket>

    private func setLocalAddress(address: SocketAddress) {
        checkUnixSocketAddress(address, "localAddress")
        unixImpl.localAddress = address
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
        this.unixImpl = SocketCommon(SocketNet.UNIX, AddressFamily.UNIX, StreamingMode)
        unixImpl.remoteAddress = address
        if (let Some(_localAddress) <- localAddress) {
            setLocalAddress(_localAddress)
        }
    }

    /**
     * Creates an internally precreated socket. See UnixServerSocket.accept.
     */
    init(unixImpl: SocketCommon<ActualPlatformSocket>) {
        this.unixImpl = unixImpl
    }

    /**
     * Remote address the socket will be or is currently connected to.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop remoteAddress: SocketAddress {
        get() {
            unixImpl.remoteAddress ?? SocketException.notYetConnected()
        }
    }

    /**
     * Local address the socket will be or currently is bound at.
     *
     * @throws SocketException is the socket is already closed
     * or no local address is available (local address was not provided during creation and the socket is not connected).
     */
    public override prop localAddress: SocketAddress {
        get() {
            unixImpl.localAddress ?? SocketException.notYetConnected()
        }
    }

    /**
     * Read operation time limit or `None` for infinite read attempts.
     * The value specified here is actually the minimal amount of time before a read operation cancelled.
     * The actual time is not guaranteed but it will be never cancelled earlier than the specified timeout value.
     * If the duration is too big than it can be bumped to the infinite. When it's too small then if will be bumped to the minimal clock granularity.
     *
     * The default value is None.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop readTimeout: ?Duration {
        get() {
            unixImpl.readTimeout
        }
        set(timeout) {
            unixImpl.readTimeout = timeout?.throwIfNegative("Read timeout").toNanosecondGranularity()
        }
    }

    /**
     * Write operation time limit or `None` for infinite read attempts.
     *
     * The value specified here is actually the minimal amount of time before a write operation cancelled.
     * The actual time is not guaranteed but it will be never cancelled earlier than the specified timeout value.
     * If the duration is too big than it can be bumped to the infinite. When it's too small then if will be bumped to the minimal clock granularity.
     *
     * The default value is None.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop writeTimeout: ?Duration {
        get() {
            unixImpl.writeTimeout
        }
        set(timeout) {
            unixImpl.writeTimeout = timeout?.throwIfNegative("Write timeout").toNanosecondGranularity()
        }
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
            unixImpl.getSendBufferSize()
        }
        set(newSize) {
            unixImpl.setSendBufferSize(newSize)
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
            unixImpl.getReceiveBufferSize()
        }
        set(newSize) {
            unixImpl.setReceiveBufferSize(newSize)
        }
    }

    public override func read(buffer: Array<Byte>): Int64 {
        unixImpl.read(buffer)
    }

    public override func write(buffer: Array<Byte>): Unit {
        unixImpl.write(buffer)
    }

    /**
     * Connects to the remote peer within the specified timeout.
     * If the timeout is `None`, then connection attempts will continue without time limit.
     * Please note that this function doesn't do retry so if the server peer does reject connection, we get error despite the timeout duration.
     *
     * This function also does bind first before doing connect so there is no need to invoke bind
     *
     * @throws SocketException if the connection cannot be established.
     * @throws SocketTimeoutException if the waiting time has expired.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public func connect(timeout!: ?Duration = None): Unit {
        unixImpl.connect(timeout?.throwIfNegative("Timeout"))
    }

    /**
     * Read a raw Unix socket option.
     *
     * valueLength must point to the value buffer size on entry and receives the result size
     * written by the native call.
     *
     * @throws SocketException if getsockopt fails.
     */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        unsafe { unixImpl.getSocketOption(level, option, value, valueLength) }
    }

    /** Write a raw Unix socket option; native setsockopt failures are reported as exceptions. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        unsafe { unixImpl.setSocketOption(level, option, value, valueLength) }
    }

    /** Read a Unix socket option as IntNative, requiring the native result size to match. */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        unixImpl.getSocketOptionIntNative(level, option)
    }

    /** Write an IntNative value to a Unix socket option. */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        unixImpl.setSocketOptionIntNative(level, option, value)
    }

    /** Read a Unix socket option as Bool using IntNative zero/nonzero conversion. */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        unixImpl.getSocketOptionBool(level, option)
    }

    /** Write a Bool value to a Unix socket option using IntNative conversion. */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        unixImpl.setSocketOptionBool(level, option, value)
    }

    /**
     * Close the socket releasing all resources. All operations except for close() and isClose() are no longer available.
     * This function is reentrant.
     **/
    public func close(): Unit {
        unixImpl.close()
    }

    /**
     * Checks whether this socket has been explicitly closed via close()
     **/
    public func isClosed(): Bool {
        unixImpl.isClosed()
    }

    public override func toString(): String {
        "UnixSocket(${unixImpl.toString()})"
    }
}

@When[os == "Windows"]
@Deprecated
public class UnixSocket <: StreamingSocket {
    private let legacyImpl: SocketCommon<ActualPlatformSocket>

    private func setLocalAddress(address: SocketAddress) {
        checkUnixSocketAddress(address, "localAddress")
        legacyImpl.localAddress = address
    }

    /**
     * Create a deprecated Windows-compatible Unix domain socket for the target path.
     *
     * @param path remote path used when connect() is invoked.
     */
    public init(path: String, localPath!: ?String = None) {
        this(UnixSocketAddress(path))
        if (let Some(legacyLocalPath) <- localPath) {
            setLocalAddress(UnixSocketAddress(legacyLocalPath))
        }
    }

    /**
     * Create a deprecated Windows-compatible Unix domain socket for the target address.
     *
     * @param address remote address used when connect() is invoked.
     */
    public init(address: SocketAddress, localAddress!: ?SocketAddress = None) {
        checkUnixSocketAddress(address, "address")
        this.legacyImpl = SocketCommon(SocketNet.UNIX, AddressFamily.UNIX, StreamingMode)
        legacyImpl.remoteAddress = address
        if (let Some(legacyLocalAddress) <- localAddress) {
            setLocalAddress(legacyLocalAddress)
        }
    }

    /**
     * Wraps a precreated socket accepted by the deprecated Windows branch.
     */
    init(legacyImpl: SocketCommon<ActualPlatformSocket>) {
        this.legacyImpl = legacyImpl
    }

    /**
     * Remote address configured for the deprecated Windows-compatible socket.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop remoteAddress: SocketAddress {
        get() {
            legacyImpl.remoteAddress ?? SocketException.notYetConnected()
        }
    }

    /**
     * Local address configured for the deprecated Windows-compatible socket.
     *
     * @throws SocketException is the socket is already closed
     * or no local address is available (local address was not provided during creation and the socket is not connected).
     */
    public override prop localAddress: SocketAddress {
        get() {
            legacyImpl.localAddress ?? SocketException.notYetConnected()
        }
    }

    /**
     * Read operation timeout for the deprecated Windows-compatible Unix socket, or `None` for no limit.
     * Cancellation is never earlier than the requested duration, though the platform may round the value.
     *
     * The default value is None.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop readTimeout: ?Duration {
        get() {
            legacyImpl.readTimeout
        }
        set(timeout) {
            legacyImpl.readTimeout = timeout?.throwIfNegative("Read timeout").toNanosecondGranularity()
        }
    }

    /**
     * Write operation timeout for the deprecated Windows-compatible Unix socket, or `None` for no limit.
     *
     * The platform may round very small or very large durations to supported timeout values.
     *
     * The default value is None.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop writeTimeout: ?Duration {
        get() {
            legacyImpl.writeTimeout
        }
        set(timeout) {
            legacyImpl.writeTimeout = timeout?.throwIfNegative("Write timeout").toNanosecondGranularity()
        }
    }

    /**
     * SO_SNDBUF hint used by the deprecated Windows-compatible Unix socket.
     *
     * The operating system may clamp, ignore, or report advisory values for this option.
     *
     * @throws IllegalArgumentException if the specified buffer size is negative or 0.
     */
    public mut prop sendBufferSize: Int64 {
        get() {
            legacyImpl.getSendBufferSize()
        }
        set(newSize) {
            legacyImpl.setSendBufferSize(newSize)
        }
    }

    /**
     * SO_RCVBUF hint used by the deprecated Windows-compatible Unix socket.
     *
     * The operating system may clamp, ignore, or report advisory values for this option.
     *
     * @throws IllegalArgumentException if the specified buffer size is negative or 0.
     */
    public mut prop receiveBufferSize: Int64 {
        get() {
            legacyImpl.getReceiveBufferSize()
        }
        set(newSize) {
            legacyImpl.setReceiveBufferSize(newSize)
        }
    }

    public override func read(buffer: Array<Byte>): Int64 {
        legacyImpl.read(buffer)
    }

    public override func write(buffer: Array<Byte>): Unit {
        legacyImpl.write(buffer)
    }

    /**
     * Connects this deprecated Windows-compatible socket to the configured peer.
     * `None` keeps the connection attempt without a time limit; failures are not retried.
     * The socket binds first when a local address was supplied.
     *
     * @throws SocketException if the connection cannot be established.
     * @throws SocketTimeoutException if the waiting time has expired.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public func connect(timeout!: ?Duration = None): Unit {
        legacyImpl.connect(timeout?.throwIfNegative("Timeout"))
    }

    /**
     * Read a raw Unix socket option.
     *
     * The valueLength pointer is both input and output: pass the capacity of value before
     * calling, then read back the actual option length.
     *
     * @throws SocketException if getsockopt fails.
     */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        unsafe { legacyImpl.getSocketOption(level, option, value, valueLength) }
    }

    /** Write a raw Unix socket option; native setsockopt failures are reported as exceptions. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        unsafe { legacyImpl.setSocketOption(level, option, value, valueLength) }
    }

    /** Read a Unix socket option as IntNative, requiring the native result size to match. */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        legacyImpl.getSocketOptionIntNative(level, option)
    }

    /** Write an IntNative value to a Unix socket option. */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        legacyImpl.setSocketOptionIntNative(level, option, value)
    }

    /** Read a Unix socket option as Bool using IntNative zero/nonzero conversion. */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        legacyImpl.getSocketOptionBool(level, option)
    }

    /** Write a Bool value to a Unix socket option using IntNative conversion. */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        legacyImpl.setSocketOptionBool(level, option, value)
    }

    /**
     * Close the socket releasing all resources. All operations except for close() and isClose() are no longer available.
     * This function is reentrant.
     **/
    public func close(): Unit {
        legacyImpl.close()
    }

    /**
     * Checks whether this socket has been explicitly closed via close()
     **/
    public func isClosed(): Bool {
        legacyImpl.isClosed()
    }

    public override func toString(): String {
        "UnixSocket(${legacyImpl.toString()})"
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
    private let unixServerImpl: SocketCommon<ActualPlatformSocket>
    private var backlogSize_: Int32 = SOCKET_DEFAULT_BACKLOG

    /**
     * Local address the socket will be or is currently bound at.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop localAddress: SocketAddress {
        get() {
            unixServerImpl.localAddress ?? SocketException.notYetBound()
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

        let unixServerImpl = SocketCommon<ActualPlatformSocket>(UNIX, AddressFamily.UNIX, SocketMode.StreamingMode)
        unixServerImpl.localAddress = bindAt
        this.unixServerImpl = unixServerImpl
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
            unixServerImpl.getSendBufferSize()
        }
        set(newSize) {
            unixServerImpl.setSendBufferSize(newSize)
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
            unixServerImpl.getReceiveBufferSize()
        }
        set(newSize) {
            unixServerImpl.setReceiveBufferSize(newSize)
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
            unixServerImpl.checkNotBound()
            unixServerImpl.checkNotClosed()
            backlogSize_ = match {
                case newSize <= 0 => throw IllegalArgumentException("BacklogSize should be positive: ${newSize}.")
                case newSize >= Int64(Int32.Max) => Int32.Max - 1
                case _ => Int32(newSize)
            }
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
        unixServerImpl.bind(backlogSize_)
    }

    /**
     * Accept a client socket, waiting until one is pending or the timeout expires.
     *
     * @throws SocketTimeoutException if the spcified timeout allapsed before got pending connection request
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override func accept(timeout!: ?Duration): UnixSocket {
        let accepted = unixServerImpl.accept(timeout) ?? SocketException.throwClosedException()
        return UnixSocket(accepted)
    }

    /** Accept a client socket without a timeout. */
    public override func accept(): UnixSocket {
        accept(timeout: None)
    }

    /**
     * Close the socket releasing all resources. All operations except for close() and isClose() are no longer available.
     * This function is reentrant.
     **/
    public override func close(): Unit {
        unixServerImpl.close()
    }

    /**
     * Checks whether this socket has been explicitly closed via close()
     **/
    public override func isClosed(): Bool {
        unixServerImpl.isClosed()
    }

    /**
     * Read a raw Unix server socket option.
     *
     * Initialize valueLength with the size of value before calling. It is updated with the
     * native result length before the function returns.
     *
     * @throws SocketException if getsockopt fails.
     */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        unsafe { unixServerImpl.getSocketOption(level, option, value, valueLength) }
    }

    /** Write a raw Unix socket option; native setsockopt failures are reported as exceptions. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        unsafe { unixServerImpl.setSocketOption(level, option, value, valueLength) }
    }

    /** Read a Unix socket option as IntNative, requiring the native result size to match. */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        unixServerImpl.getSocketOptionIntNative(level, option)
    }

    /** Write an IntNative value to a Unix socket option. */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        unixServerImpl.setSocketOptionIntNative(level, option, value)
    }

    /** Read a Unix socket option as Bool using IntNative zero/nonzero conversion. */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        unixServerImpl.getSocketOptionBool(level, option)
    }

    /** Write a Bool value to a Unix socket option using IntNative conversion. */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        unixServerImpl.setSocketOptionBool(level, option, value)
    }

    public override func toString(): String {
        "UnixServerSocket(${unixServerImpl.toString()})"
    }
}

@When[os == "Windows"]
@Deprecated
public class UnixServerSocket <: ServerSocket {
    private let legacyServerImpl: SocketCommon<ActualPlatformSocket>
    private var backlogSize_: Int32 = SOCKET_DEFAULT_BACKLOG

    /**
     * Local address configured for the deprecated Windows-compatible server socket.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop localAddress: SocketAddress {
        get() {
            legacyServerImpl.localAddress ?? SocketException.notYetBound()
        }
    }

    /**
     * Creates a deprecated Windows-compatible Unix server socket for a local path.
     *
     * @param bindAt path used when bind() is invoked.
     **/
    public init(bindAt!: String) {
        this(bindAt: UnixSocketAddress(bindAt))
    }

    /**
     * Creates a deprecated Windows-compatible Unix server socket for a local address.
     *
     * @param bindAt address used when bind() is invoked.
     **/
    public init(bindAt!: SocketAddress) {
        checkUnixSocketAddress(bindAt, "bindAt")

        let legacyServerImpl = SocketCommon<ActualPlatformSocket>(UNIX, AddressFamily.UNIX, SocketMode.StreamingMode)
        legacyServerImpl.localAddress = bindAt
        this.legacyServerImpl = legacyServerImpl
    }

    /**
     * SO_SNDBUF hint used by the deprecated Windows-compatible Unix server socket.
     *
     * The operating system may clamp, ignore, or report advisory values for this option.
     *
     * @throws IllegalArgumentException if the specified buffer size is negative or 0.
     */
    public mut prop sendBufferSize: Int64 {
        get() {
            legacyServerImpl.getSendBufferSize()
        }
        set(newSize) {
            legacyServerImpl.setSendBufferSize(newSize)
        }
    }

    /**
     * SO_RCVBUF hint used by the deprecated Windows-compatible Unix server socket.
     *
     * The operating system may clamp, ignore, or report advisory values for this option.
     *
     * @throws IllegalArgumentException if the specified buffer size is negative or 0.
     */
    public mut prop receiveBufferSize: Int64 {
        get() {
            legacyServerImpl.getReceiveBufferSize()
        }
        set(newSize) {
            legacyServerImpl.setReceiveBufferSize(newSize)
        }
    }

    /**
     * Configure the incoming connection backlog before this deprecated server socket is bound.
     * The operating system may clamp the requested value or ignore it.
     **/
    public mut prop backlogSize: Int64 {
        get() {
            Int64(backlogSize_)
        }
        set(newSize) {
            legacyServerImpl.checkNotBound()
            legacyServerImpl.checkNotClosed()
            backlogSize_ = match {
                case newSize <= 0 => throw IllegalArgumentException("BacklogSize should be positive: ${newSize}.")
                case newSize >= Int64(Int32.Max) => Int32.Max - 1
                case _ => Int32(newSize)
            }
        }
    }

    /**
     * Bind the deprecated Windows-compatible streaming Unix server socket.
     *
     * The call creates the socket file and starts listening with the configured backlog.
     */
    public override func bind(): Unit {
        legacyServerImpl.bind(backlogSize_)
    }

    /**
     * Accept a client socket from the deprecated Windows-compatible server branch.
     *
     * The call waits until the platform reports a pending request or the timeout expires.
     *
     * @throws SocketTimeoutException if the spcified timeout allapsed before got pending connection request
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override func accept(timeout!: ?Duration): UnixSocket {
        let accepted = legacyServerImpl.accept(timeout) ?? SocketException.throwClosedException()
        return UnixSocket(accepted)
    }

    /** Accept a client socket from the deprecated Windows-compatible server without a timeout. */
    public override func accept(): UnixSocket {
        accept(timeout: None)
    }

    /**
     * Close the socket releasing all resources. All operations except for close() and isClose() are no longer available.
     * This function is reentrant.
     **/
    public override func close(): Unit {
        legacyServerImpl.close()
    }

    /**
     * Checks whether this socket has been explicitly closed via close()
     **/
    public override func isClosed(): Bool {
        legacyServerImpl.isClosed()
    }

    /**
     * Read a raw Unix server socket option.
     *
     * valueLength carries the buffer size into getsockopt and receives the number of bytes
     * produced by the native call.
     *
     * @throws SocketException if getsockopt fails.
     */
    public func getSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: CPointer<UIntNative>
    ): Unit {
        unsafe { legacyServerImpl.getSocketOption(level, option, value, valueLength) }
    }

    /** Write a raw Unix socket option; native setsockopt failures are reported as exceptions. */
    public func setSocketOption(
        level: Int32,
        option: Int32,
        value: CPointer<Unit>,
        valueLength: UIntNative
    ): Unit {
        unsafe { legacyServerImpl.setSocketOption(level, option, value, valueLength) }
    }

    /** Read a Unix socket option as IntNative, requiring the native result size to match. */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        legacyServerImpl.getSocketOptionIntNative(level, option)
    }

    /** Write an IntNative value to a Unix socket option. */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        legacyServerImpl.setSocketOptionIntNative(level, option, value)
    }

    /** Read a Unix socket option as Bool using IntNative zero/nonzero conversion. */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        legacyServerImpl.getSocketOptionBool(level, option)
    }

    /** Write a Bool value to a Unix socket option using IntNative conversion. */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        legacyServerImpl.setSocketOptionBool(level, option, value)
    }

    public override func toString(): String {
        "UnixServerSocket(${legacyServerImpl.toString()})"
    }
}
