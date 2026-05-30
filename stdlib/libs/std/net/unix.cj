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
        this.impl = SocketCommon(SocketNet.UNIX, AddressFamily.UNIX, StreamingMode)
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
     * Unix domain peer address configured for this stream socket.
     *
     * @throws SocketException if the socket has no connected peer or is already closed.
     */
    public override prop remoteAddress: SocketAddress {
        get() {
            impl.remoteAddress ?? SocketException.notYetConnected()
        }
    }

    /**
     * Unix domain local address used by this stream socket.
     *
     * @throws SocketException if the socket is already closed or no local address is available.
     */
    public override prop localAddress: SocketAddress {
        get() {
            impl.localAddress ?? SocketException.notYetConnected()
        }
    }

    /**
     * Read timeout for Unix domain stream operations, or `None` for no limit.
     * The platform may round the duration but must not cancel earlier than requested.
     *
     * The default value is None.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop readTimeout: ?Duration {
        get() {
            impl.readTimeout
        }
        set(timeout) {
            impl.readTimeout = timeout?.throwIfNegative("Read timeout").toNanosecondGranularity()
        }
    }

    /**
     * Write timeout for Unix domain stream operations, or `None` for no limit.
     *
     * Very small or very large durations may be rounded to platform-supported values.
     *
     * The default value is None.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override mut prop writeTimeout: ?Duration {
        get() {
            impl.writeTimeout
        }
        set(timeout) {
            impl.writeTimeout = timeout?.throwIfNegative("Write timeout").toNanosecondGranularity()
        }
    }

    /**
     * Unix stream SO_SNDBUF hint for outgoing bytes; rejects non-positive sizes.
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
     * Unix stream SO_RCVBUF hint for incoming bytes; rejects non-positive sizes.
     */
    public mut prop receiveBufferSize: Int64 {
        get() {
            impl.getReceiveBufferSize()
        }
        set(newSize) {
            impl.setReceiveBufferSize(newSize)
        }
    }

    public override func read(buffer: Array<Byte>): Int64 {
        impl.read(buffer)
    }

    public override func write(buffer: Array<Byte>): Unit {
        impl.write(buffer)
    }

    /**
     * Connects to the remote peer within the specified timeout.
     * If the timeout is `None`, then connection attempts will continue without time limit.
     * Please note that this function doesn't do retry so if the server peer does reject connection, we get error despite the timeout duration.
     *
     * This function also does bind first before doing connect so there is no need to invoke bind
     * Unix sockets connect through a filesystem-style socket address rather than an IP endpoint.
     *
     * @throws SocketException if the connection cannot be established.
     * @throws SocketTimeoutException if the waiting time has expired.
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public func connect(timeout!: ?Duration = None): Unit {
        // Unix domain sockets use the same timeout validation as TCP, but delegate to Unix socket storage.
        impl.connect(timeout?.throwIfNegative("Timeout"))
    }

    /**
     * Read a raw Unix socket option; valueLength is updated with the native result size.
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
     * Write a raw Unix socket option; native setsockopt failures are reported as exceptions.
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
     * Read a Unix socket option as IntNative, requiring the native result size to match.
     */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        impl.getSocketOptionIntNative(level, option)
    }

    /**
     * Write an IntNative value to a Unix socket option.
     */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        impl.setSocketOptionIntNative(level, option, value)
    }

    /**
     * Read a Unix socket option as Bool using IntNative zero/nonzero conversion.
     */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        impl.getSocketOptionBool(level, option)
    }

    /**
     * Write a Unix socket Bool option using IntNative conversion.
     */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        // Unix stream sockets route raw option writes through the shared implementation.
        impl.setSocketOptionBool(level, option, value)
    }

    /**
     * Close this socket and release its resources. This function is reentrant.
     */
    public func close(): Unit {
        impl.close()
    }

    /**
     * Return whether close() has already been invoked for this socket.
     */
    public func isClosed(): Bool {
        // The closed state is tracked by the platform socket wrapper.
        impl.isClosed()
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
        checkUnixSocketAddress(address, "localAddress")
        impl.localAddress = address
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
        this.impl = SocketCommon(SocketNet.UNIX, AddressFamily.UNIX, StreamingMode)
        this.impl.remoteAddress = address
        if (let Some(legacyLocalAddress) <- localAddress) {
            setLocalAddress(legacyLocalAddress)
        }
    }

    /**
     * Wraps a precreated socket accepted by the deprecated Windows branch.
     */
    init(impl: SocketCommon<ActualPlatformSocket>) {
        this.impl = impl
    }

    /**
     * Remote address configured for the deprecated Windows-compatible socket.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop remoteAddress: SocketAddress {
        get() {
            this.impl.remoteAddress ?? SocketException.notYetConnected()
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
            this.impl.localAddress ?? SocketException.notYetConnected()
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
            this.impl.readTimeout
        }
        set(timeout) {
            this.impl.readTimeout = timeout?.throwIfNegative("Read timeout").toNanosecondGranularity()
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
            this.impl.writeTimeout
        }
        set(timeout) {
            this.impl.writeTimeout = timeout?.throwIfNegative("Write timeout").toNanosecondGranularity()
        }
    }

    /**
     * Deprecated Unix socket SO_SNDBUF hint; rejects non-positive sizes.
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
     * Deprecated Unix socket SO_RCVBUF hint; rejects non-positive sizes.
     */
    public mut prop receiveBufferSize: Int64 {
        get() {
            this.impl.getReceiveBufferSize()
        }
        set(newSize) {
            this.impl.setReceiveBufferSize(newSize)
        }
    }

    public override func read(buffer: Array<Byte>): Int64 {
        this.impl.read(buffer)
    }

    public override func write(buffer: Array<Byte>): Unit {
        this.impl.write(buffer)
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
        this.impl.connect(timeout?.throwIfNegative("Timeout"))
    }

    /**
     * Close this socket and release its resources. This function is reentrant.
     */
    public func close(): Unit {
        this.impl.close()
    }

    /**
     * Return whether close() has already been invoked for this socket.
     */
    public func isClosed(): Bool {
        this.impl.isClosed()
    }

    public override func toString(): String {
        "UnixSocket(${this.impl.toString()})"
    }

    /**
     * Write a Bool value to a Unix socket option using IntNative conversion.
     */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        this.impl.setSocketOptionBool(level, option, value)
    }

    /**
     * Read a deprecated Unix socket Bool option using IntNative conversion.
     */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        this.impl.getSocketOptionBool(level, option)
    }

    /**
     * Read a deprecated Unix socket raw option and update valueLength with the result size.
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
     * Write a deprecated Unix socket raw option; native failures become SocketException.
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
     * Read a deprecated Unix socket IntNative option and verify the native size.
     */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        this.impl.getSocketOptionIntNative(level, option)
    }

    /**
     * Write a deprecated Unix socket IntNative option value.
     */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        this.impl.setSocketOptionIntNative(level, option, value)
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
     * Local address the socket will be or is currently bound at.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop localAddress: SocketAddress {
        get() {
            this.impl.localAddress ?? SocketException.notYetBound()
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

    /**
     * Unix server SO_SNDBUF hint for accepted streams; rejects non-positive sizes.
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
     * Unix server SO_RCVBUF hint for accepted streams; rejects non-positive sizes.
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
     * Configure incoming connections backlog size. This only works before binding socket.
     * Changing this value is not guaranteed to be actually applied since the operating system may decide to
     * change or bump it, or simply ignore.
     **/
    public mut prop backlogSize: Int64 {
        get() {
            Int64(backlogSize_)
        }
        set(newSize) {
            // Deprecated Windows branch keeps the same validation before binding.
            this.impl.checkNotBound()
            this.impl.checkNotClosed()
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
        this.impl.bind(backlogSize_)
    }

    /**
     * Accept a client socket, waiting until one is pending or the timeout expires.
     *
     * @throws SocketTimeoutException if the spcified timeout allapsed before got pending connection request
     * @throws IllegalArgumentException if the specified timeout duration is negative.
     */
    public override func accept(timeout!: ?Duration): UnixSocket {
        // The accepted platform socket is wrapped as a UnixSocket for callers.
        let accepted = this.impl.accept(timeout) ?? SocketException.throwClosedException()
        return UnixSocket(accepted)
    }

    /**
     * Accept a client socket without a timeout.
     */
    public override func accept(): UnixSocket {
        // The deprecated server overload keeps the no-timeout accept behavior.
        accept(timeout: None)
    }

    /**
     * Close this socket and release its resources. This function is reentrant.
     */
    public override func close(): Unit {
        // Close releases the server-side Unix socket descriptor held by the shared implementation.
        this.impl.close()
    }

    /**
     * Return whether close() has already been invoked for this socket.
     */
    public override func isClosed(): Bool {
        // The closed state is tracked by the shared socket implementation.
        this.impl.isClosed()
    }

    /**
     * Read a Unix socket option as IntNative, requiring the native result size to match.
     */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        this.impl.getSocketOptionIntNative(level, option)
    }

    /**
     * Write an IntNative value to a Unix socket option.
     */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        this.impl.setSocketOptionIntNative(level, option, value)
    }

    /**
     * Read a raw Unix server socket option; valueLength is updated with the native result size.
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
     * Write a Unix server raw option; native failures become SocketException.
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
     * Read a Unix socket option as Bool using IntNative zero/nonzero conversion.
     */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        this.impl.getSocketOptionBool(level, option)
    }

    /**
     * Write a Bool value to a Unix socket option using IntNative conversion.
     */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        this.impl.setSocketOptionBool(level, option, value)
    }

    public override func toString(): String {
        "UnixServerSocket(${this.impl.toString()})"
    }
}

@When[os == "Windows"]
@Deprecated
public class UnixServerSocket <: ServerSocket {
    private let impl: SocketCommon<ActualPlatformSocket>
    private var backlogSize_: Int32 = SOCKET_DEFAULT_BACKLOG

    /**
     * Local address configured for the deprecated Windows-compatible server socket.
     *
     * @throws SocketException is the socket is already closed.
     */
    public override prop localAddress: SocketAddress {
        get() {
            this.impl.localAddress ?? SocketException.notYetBound()
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

        let impl = SocketCommon<ActualPlatformSocket>(UNIX, AddressFamily.UNIX, SocketMode.StreamingMode)
        impl.localAddress = bindAt
        this.impl = impl
    }

    /**
     * Deprecated Unix server SO_SNDBUF hint; rejects non-positive sizes.
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
     * Deprecated Unix server SO_RCVBUF hint; rejects non-positive sizes.
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
     * Configure the incoming connection backlog before this deprecated server socket is bound.
     * The operating system may clamp the requested value or ignore it.
     **/
    public mut prop backlogSize: Int64 {
        get() {
            Int64(backlogSize_)
        }
        set(newSize) {
            this.impl.checkNotBound()
            this.impl.checkNotClosed()
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
        this.impl.bind(backlogSize_)
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
        let accepted = this.impl.accept(timeout) ?? SocketException.throwClosedException()
        return UnixSocket(accepted)
    }

    /**
     * Accept a client socket from the deprecated Windows-compatible server without a timeout.
     */
    public override func accept(): UnixSocket {
        accept(timeout: None)
    }

    /**
     * Close this socket and release its resources. This function is reentrant.
     */
    public override func close(): Unit {
        // Deprecated server sockets delegate close state to the same implementation object.
        this.impl.close()
    }

    /**
     * Return whether close() has already been invoked for this socket.
     */
    public override func isClosed(): Bool {
        // Keep the deprecated branch consistent with the primary Unix server socket.
        this.impl.isClosed()
    }

    /**
     * Read a deprecated Unix server raw option and update valueLength with the result size.
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
     * Write a raw Unix socket option; native setsockopt failures are reported as exceptions.
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
     * Read a deprecated Unix server IntNative option and verify the native size.
     */
    public func getSocketOptionIntNative(
        level: Int32,
        option: Int32
    ): IntNative {
        this.impl.getSocketOptionIntNative(level, option)
    }

    /**
     * Write a deprecated Unix server IntNative option value.
     */
    public func setSocketOptionIntNative(
        level: Int32,
        option: Int32,
        value: IntNative
    ): Unit {
        this.impl.setSocketOptionIntNative(level, option, value)
    }

    /**
     * Read a deprecated Unix server Bool option using IntNative conversion.
     */
    public func getSocketOptionBool(
        level: Int32,
        option: Int32
    ): Bool {
        this.impl.getSocketOptionBool(level, option)
    }

    /**
     * Write a deprecated Unix server Bool option using IntNative conversion.
     */
    public func setSocketOptionBool(
        level: Int32,
        option: Int32,
        value: Bool
    ): Unit {
        this.impl.setSocketOptionBool(level, option, value)
    }

    public override func toString(): String {
        "UnixServerSocket(${this.impl.toString()})"
    }
}
