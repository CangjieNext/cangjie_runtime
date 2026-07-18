// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "log.h"
#include "sock_impl.h"
#include "schdfd_impl.h"
#include "securec.h"
#include "domainsock.h"

#ifdef __cplusplus
extern "C" {
#endif

SOCKET DomainsockCreate(int domain, int type, int protocol, int *socketError)
{
    SOCKET sockFd;
    int ret;

    if (domain != AF_UNIX || (type != SOCK_STREAM && type != SOCK_DGRAM)) {
        *socketError = ERRNO_SOCK_ARG_INVALID;
        LOG_ERROR(ERRNO_SOCK_ARG_INVALID, "type:%d out of compliance", type);
        return -1;
    }

    // Create domain socket with overlapped support for async I/O.
    sockFd = WSASocket(domain, type, protocol, nullptr,
                       0, WSA_FLAG_OVERLAPPED);
    if (sockFd == INVALID_SOCKET) {
        *socketError = WSAGetLastError();
        LOG_ERROR(*socketError, "WSASocket failed, domain: %u, type: %d, protocol: %d", domain, type, protocol);
        return -1;
    }

    ret = SchdfdRegisterAndNetpollAdd(sockFd);
    if (ret != 0) {
        *socketError = ret;
        LOG_ERROR(ret, "domainsock SchdfdRegisterAndNetpollAdd failed, fd: %d", sockFd);
        return -1;
    }

    LOG_INFO(0, "domainsock create success, fd: %d", sockFd);
    return sockFd;
}

int DomainsockBind(SOCKET fd, const struct sockaddr *addr, socklen_t addrLen)
{
    int ret;

    if (bind(fd, addr, addrLen) == SOCKET_ERROR) {
        ret = WSAGetLastError();
        LOG_ERROR(ret, "bind failed, fd: %d, addrlen: %u", fd, addrLen);
        return ret;
    }

    return 0;
}

int DomainsockBindListen(SOCKET sockFd, const struct SockAddr *addr, int backlog)
{
    int ret;
    int sockType;
    int optLen = sizeof(int);

    ret = DomainsockBind(sockFd, reinterpret_cast<const struct sockaddr *>(addr->sockaddr), addr->addrLen);
    if (ret != 0) {
        return ret;
    }

    ret = getsockopt(sockFd, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&sockType), &optLen);
    if (ret == SOCKET_ERROR) {
        ret = WSAGetLastError();
        LOG_ERROR(ret, "getsockopt failed, fd: %d", sockFd);
        return ret;
    }

    // Only stream domain sockets should listen for incoming connections.
    if (sockType != SOCK_STREAM) {
        return 0;
    }

    // SOMAXCONN_HINT(N) means to set the backlog to N, with a setting range of (200, 65535).
    if (listen(sockFd, SOMAXCONN_HINT(backlog)) == SOCKET_ERROR) {
        ret = WSAGetLastError();
        LOG_ERROR(ret, "listen failed, fd: %d", sockFd);
        return ret;
    }

    LOG_INFO(0, "listen success, fd: %d", sockFd);
    return 0;
}

int DomainsockAcceptCoreInlock(SOCKET listenFd, struct sockaddr_storage *localAndPeerAddr,
                               DWORD addrLen, SOCKET *outFd, unsigned long long timeout)
{
    int ret;
    int acceptRet;
    SOCKET acceptFd;
    struct IocpOperation *operation;
    SchdpollEventType type = SHCDPOLL_READ;

    operation = SchdfdUpdateIocpOperationInlock(listenFd, type, nullptr, 0);
    if (operation == nullptr) {
        ret = ERRNO_SOCK_ARG_INVALID;
        LOG_ERROR(ret, "get iocp read operation failed , fd: %d", listenFd);
        return ret;
    }

    // Create a socket that supports overlapping I/O operations.
    acceptFd = WSASocket(AF_UNIX, SOCK_STREAM, 0, nullptr,
                         0, WSA_FLAG_OVERLAPPED);
    if (acceptFd == INVALID_SOCKET) {
        ret = WSAGetLastError();
        LOG_ERROR(ret, "WSASocket failed, domain: %u, protocol: %d", AF_UNIX, 0);
        return ret;
    }

    // Submit asynchronous accept operation.
    acceptRet = g_mswsockHooks.AcceptEx(listenFd, acceptFd, localAndPeerAddr, 0,
                                        addrLen, addrLen, &operation->transBytes, &operation->overlapped);
    if (acceptRet == TRUE && SchdfdUseSkipIocp()) {
        *outFd = acceptFd;
        return 0;
    }

    if (acceptRet == FALSE) {
        ret = WSAGetLastError();
        if (ret != ERROR_IO_PENDING) {
            LOG_ERROR(ret, "AcceptEx failed, fd: %d", listenFd);
            (void)closesocket(acceptFd);
            return ret;
        }
    }

    ret = SchdfdIocpWaitInlock(operation, timeout);
    if (ret != 0) {
        (void)closesocket(acceptFd);
        return ret;
    }

    *outFd = acceptFd;
    return 0;
}

int DomainsockAccept(SOCKET inFd, SOCKET *outFd, struct SockAddr *addr, unsigned long long timeout)
{
    int ret;
    SOCKET fd;
    struct sockaddr_storage localAndPeerAddr[2];
    struct sockaddr *peerAddr;
    struct sockaddr *localAddr;
    int peerAddrLen;
    int localAddrLen;
    DWORD addrLen = sizeof(struct sockaddr_storage);
    SchdpollEventType type = SHCDPOLL_READ;

    ret = SchdfdLock(inFd, type);
    if (ret != 0) {
        return ret;
    }

    while (1) {
        ret = DomainsockAcceptCoreInlock(inFd, localAndPeerAddr, addrLen, &fd, timeout);
        if (ret != 0) {
            if (ret == WSAECONNRESET || ret == ERROR_NETNAME_DELETED) {
                continue;
            }
            SchdfdUnlock(inFd, type);
            return ret;
        } else {
            break;
        }
    }

    // After setting this option, you can use the getSockname and getPeername
    // interfaces on this socket to obtain local and remote addresses.
    ret = setsockopt(fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<char *>(&inFd), sizeof(inFd));
    if (ret == SOCKET_ERROR) {
        ret = WSAGetLastError();
        LOG_ERROR(ret, "setsockopt(SO_UPDATE_ACCEPT_CONTEXT) failed, fd: %d", fd);
        SchdfdUnlock(inFd, type);
        (void)closesocket(fd);
        return ret;
    }

    SchdfdUnlock(inFd, type);

    if (addr != nullptr && addr->sockaddr != nullptr) {
        // AcceptEx puts application data, local address, and remote address into the same buffer.
        g_mswsockHooks.GetAcceptExSockaddrs(localAndPeerAddr, 0, addrLen, addrLen,
                                            &localAddr, &localAddrLen, &peerAddr, &peerAddrLen);
        (void)memcpy_s(addr->sockaddr, peerAddrLen, peerAddr, peerAddrLen);
    }

    ret = SchdfdRegisterAndNetpollAdd(fd);
    if (ret != 0) {
        LOG_ERROR(ret, "domainsock SchdfdRegisterAndNetpollAdd failed, fd: %d", fd);
        return ret;
    }

    *outFd = fd;
    return 0;
}

int DomainsockConnect(SOCKET fd, const struct sockaddr *addr, socklen_t addrLen, unsigned long long timeout)
{
    int ret;
    int connectRet;
    SchdpollEventType type = SHCDPOLL_WRITE;
    struct IocpOperation *operation;

    // For stream sockets, use ConnectEx for async connect.
    // For datagram sockets, use regular connect.
    if (addr->sa_family == AF_UNIX && addrLen > 0) {
        // Check socket type - use ConnectEx for stream sockets
        int sockType;
        int optLen = sizeof(int);
        ret = getsockopt(fd, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&sockType), &optLen);
        if (ret == SOCKET_ERROR) {
            ret = WSAGetLastError();
            LOG_ERROR(ret, "getsockopt failed, fd: %d", fd);
            return ret;
        }

        if (sockType == SOCK_STREAM) {
            ret = SchdfdLock(fd, type);
            if (ret != 0) {
                return ret;
            }

            operation = SchdfdUpdateIocpOperationInlock(fd, type, nullptr, 0);
            if (operation == nullptr) {
                ret = ERRNO_SOCK_ARG_INVALID;
                LOG_ERROR(ret, "get iocp write operation failed , fd: %d", fd);
                SchdfdUnlock(fd, type);
                return ret;
            }

            // Submit asynchronous connection operation.
            // ConnectEx requires fd to call bind beforehand.
            connectRet = g_mswsockHooks.ConnectEx(fd, addr, addrLen, nullptr, 0, nullptr, &operation->overlapped);
            if (connectRet == TRUE && SchdfdUseSkipIocp()) {
                ret = setsockopt(fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT,
                                 reinterpret_cast<char *>(&fd), sizeof(fd));
                if (ret == SOCKET_ERROR) {
                    ret = WSAGetLastError();
                    LOG_ERROR(ret, "setsockopt(SO_UPDATE_CONNECT_CONTEXT) failed, fd: %d", fd);
                }
                SchdfdUnlock(fd, type);
                return ret;
            }

            if (connectRet == FALSE) {
                ret = WSAGetLastError();
                if (ret != ERROR_IO_PENDING) {
                    LOG_ERROR(ret, "ConnectEx failed, fd: %d", fd);
                    SchdfdUnlock(fd, type);
                    return ret;
                }
            }

            ret = SchdfdIocpWaitInlock(operation, timeout);
            if (ret != 0) {
                SchdfdUnlock(fd, type);
                return ret;
            }

            ret = setsockopt(fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT,
                             reinterpret_cast<char *>(&fd), sizeof(fd));
            if (ret == SOCKET_ERROR) {
                ret = WSAGetLastError();
                LOG_ERROR(ret, "setsockopt(SO_UPDATE_CONNECT_CONTEXT) failed, fd: %d", fd);
                SchdfdUnlock(fd, type);
                return ret;
            }

            SchdfdUnlock(fd, type);
            return 0;
        }
    }

    // For datagram sockets or fallback, use regular connect.
    ret = connect(fd, addr, addrLen);
    if (ret == SOCKET_ERROR) {
        ret = WSAGetLastError();
        LOG_ERROR(ret, "connect failed, fd: %d", fd);
        return ret;
    }

    return 0;
}

int DomainsockBindConnect(SOCKET connFd, struct SockAddr *local,
                          struct SockAddr *peer, unsigned long long timeout)
{
    int ret;
    struct SockAddr localDefaultAddr;
    struct sockaddr_un localAddr;
    struct SockAddr *bindAddr = local;

    if (bindAddr == nullptr || bindAddr->sockaddr == nullptr) {
        ret = memset_s(&localAddr, sizeof(struct sockaddr_un), 0, sizeof(struct sockaddr_un));
        if (ret != 0) {
            return ret;
        }
        localAddr.sun_family = AF_UNIX;
        localDefaultAddr.sockaddr = reinterpret_cast<struct sockaddr_storage *>(&localAddr);
        localDefaultAddr.addrLen = static_cast<socklen_t>(sizeof(struct sockaddr_un));
        bindAddr = &localDefaultAddr;
    }

    ret = DomainsockBind(connFd, reinterpret_cast<const struct sockaddr *>(bindAddr->sockaddr), bindAddr->addrLen);
    if (ret != 0) {
        LOG_ERROR(ret, "DomainsockBind failed");
        return ret;
    }

    ret = DomainsockConnect(connFd, reinterpret_cast<const struct sockaddr *>(peer->sockaddr),
                            peer->addrLen, timeout);
    if (ret != 0) {
        LOG_ERROR(ret, "DomainsockConnect failed");
        return ret;
    }

    return 0;
}

int DomainsockDisconnect(SignedSocket connFd)
{
    struct sockaddr_storage peerAddr;
    int ret;
    int sockType;
    int optLen = sizeof(int);

    ret = getsockopt(connFd, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&sockType), &optLen);
    if (ret == SOCKET_ERROR) {
        ret = WSAGetLastError();
        LOG_ERROR(ret, "getsockopt failed, fd: %d", connFd);
        return ret;
    }

    // Only domain sockets for datagrams can perform a disconnect operation.
    if (sockType != SOCK_DGRAM) {
        LOG_ERROR(ERRNO_SOCK_ARG_INVALID, "unix socket type isn't SOCK_DGRAM, fd: %d", connFd);
        return ERRNO_SOCK_ARG_INVALID;
    }

    ret = memset_s(&peerAddr, sizeof(struct sockaddr_storage), 0, sizeof(struct sockaddr_storage));
    if (ret != 0) {
        return ret;
    }
    peerAddr.ss_family = AF_UNSPEC;

    ret = connect(connFd, reinterpret_cast<const struct sockaddr *>(&peerAddr), sizeof(struct sockaddr_storage));
    if (ret == SOCKET_ERROR) {
        ret = WSAGetLastError();
        LOG_ERROR(ret, "connect(AF_UNSPEC) failed, fd: %d", connFd);
        return ret;
    }

    return 0;
}

#ifdef __cplusplus
}
#endif
