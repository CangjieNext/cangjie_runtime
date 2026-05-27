/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.net

public struct SocketOptions {
    @Deprecated[message: "Use `public static const SOCKET: Int32` in `public struct OptionLevel` instead."]
    public static const SOL_SOCKET: Int32 = SOL_SOCKET

    @Deprecated[message: "Use `public static const TCP: Int32` in `public struct OptionLevel` instead."]
    public static const IPPROTO_TCP: Int32 = IPPROTO_TCP

    @Deprecated[message: "Use `public static const UDP: Int32` in `public struct OptionLevel` instead."]
    public static const IPPROTO_UDP: Int32 = IPPROTO_UDP

    public static const SO_KEEPALIVE: Int32 = SOCK_KEEPALIVE
    public static const TCP_NODELAY: Int32 = SOCK_TCP_NODELAY
    public static const TCP_QUICKACK: Int32 = SOCK_TCP_QUICKACK

    public static const SO_LINGER: Int32 = SOCK_LINGER
    public static const SO_SNDBUF: Int32 = SOCK_SNDBUF
    public static const SO_RCVBUF: Int32 = SOCK_RCVBUF

    public static const SO_REUSEADDR: Int32 = SOCK_REUSEADDR
    public static const SO_REUSEPORT: Int32 = SOCK_REUSEPORT
    public static const SO_BINDTODEVICE: Int32 = SOCK_BINDTODEVICE
}

func socketSendBufferSize<NS>(impl: SocketCommon<NS>): Int64 where NS <: PlatformSocket<NS> {
    Int64(impl.getSocketOptionIntNative(SOL_SOCKET, SOCK_SNDBUF))
}

func socketSendBufferSize<NS>(impl: SocketCommon<NS>, newSize: Int64): Unit where NS <: PlatformSocket<NS> {
    if (newSize <= 0) {
        throw IllegalArgumentException("Buffer size should be positive, got ${newSize}.")
    }
    impl.setSocketOptionIntNative(SOL_SOCKET, SOCK_SNDBUF, IntNative(newSize))
}

func socketReceiveBufferSize<NS>(impl: SocketCommon<NS>): Int64 where NS <: PlatformSocket<NS> {
    Int64(impl.getSocketOptionIntNative(SOL_SOCKET, SOCK_RCVBUF))
}

func socketReceiveBufferSize<NS>(impl: SocketCommon<NS>, newSize: Int64): Unit where NS <: PlatformSocket<NS> {
    if (newSize <= 0) {
        throw IllegalArgumentException("Buffer size should be positive, got ${newSize}.")
    }
    impl.setSocketOptionIntNative(SOL_SOCKET, SOCK_RCVBUF, IntNative(newSize))
}

func checkedSocketBacklogSize<NS>(impl: SocketCommon<NS>, newSize: Int64): Int32 where NS <: PlatformSocket<NS> {
    impl.checkNotBound()
    impl.checkNotClosed()
    return match {
        case newSize <= 0 => throw IllegalArgumentException("BacklogSize should be positive: ${newSize}.")
        case newSize >= Int64(Int32.Max) => Int32.Max - 1
        case _ => Int32(newSize)
    }
}

func socketGetOption<NS>(
    impl: SocketCommon<NS>,
    level: Int32,
    option: Int32,
    value: CPointer<Unit>,
    valueLength: CPointer<UIntNative>
): Unit where NS <: PlatformSocket<NS> {
    unsafe { impl.getSocketOption(level, option, value, valueLength) }
}

func socketSetOption<NS>(
    impl: SocketCommon<NS>,
    level: Int32,
    option: Int32,
    value: CPointer<Unit>,
    valueLength: UIntNative
): Unit where NS <: PlatformSocket<NS> {
    unsafe { impl.setSocketOption(level, option, value, valueLength) }
}

func socketGetOptionIntNative<NS>(
    impl: SocketCommon<NS>,
    level: Int32,
    option: Int32
): IntNative where NS <: PlatformSocket<NS> {
    impl.getSocketOptionIntNative(level, option)
}

func socketSetOptionIntNative<NS>(
    impl: SocketCommon<NS>,
    level: Int32,
    option: Int32,
    value: IntNative
): Unit where NS <: PlatformSocket<NS> {
    impl.setSocketOptionIntNative(level, option, value)
}

func socketGetOptionBool<NS>(
    impl: SocketCommon<NS>,
    level: Int32,
    option: Int32
): Bool where NS <: PlatformSocket<NS> {
    impl.getSocketOptionBool(level, option)
}

func socketSetOptionBool<NS>(
    impl: SocketCommon<NS>,
    level: Int32,
    option: Int32,
    value: Bool
): Unit where NS <: PlatformSocket<NS> {
    impl.setSocketOptionBool(level, option, value)
}
