// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "SysCall.h"

#include <cstdlib>
#include <unistd.h>
#if defined(_WIN64)
#include <processthreadsapi.h>
#elif defined(__APPLE__)
#include <signal.h>
#include "sys/syscall.h"
#else
#include "linux/futex.h"
#include "sys/syscall.h"
#endif

namespace MapleRuntime {
#ifndef SYS_futex
#define SYS_futex __NR_futex
#endif

#ifndef SYS_tgkill
#define SYS_tgkill __NR_tgkill
#endif

#if defined(__linux__) || defined(hongmeng)
// only support FUTEX_WAIT/FUTEX_WAKE
int Futex(const volatile int* uaddr, int op, int val)
{
    return syscall(SYS_futex, uaddr, op, val, nullptr, nullptr, 0);
}
#endif

pid_t GetTid()
{
#if defined(__BIONIC__)
    return gettid();
#elif defined(_WIN64)
    return GetCurrentThreadId();
#elif defined(__APPLE__)
    uint64_t tid;
    pthread_threadid_np(nullptr, &tid);
    return static_cast<pid_t>(tid);
#else
    return syscall(SYS_gettid);
#endif
}

int GetPid()
{
#if defined(__BIONIC__) || defined(__APPLE__)
    return getpid();
#elif defined(_WIN64)
    return GetCurrentProcessId();
#else
    return syscall(SYS_getpid);
#endif
}

int SendThreadSignal(pid_t tid, int sig)
{
#if defined(__linux__) || defined(hongmeng)
    // tgkill sends signal to a specific thread within the current process.
    return syscall(SYS_tgkill, GetPid(), tid, sig);
#elif defined(__APPLE__)
    // macOS has no tgkill equivalent, send the signal to the whole process.
    // For SIGABRT this still crashes the process and produces a core dump
    // that contains the stack of the thread holding the write lock.
    (void)tid;
    return kill(GetPid(), sig);
#else
    // Platforms without tgkill/kill (e.g. Windows), abort the process to produce
    // a crash dump that contains the stack of the thread holding the write lock.
    (void)tid;
    (void)sig;
    std::abort();
    return -1; // unreachable, keeps the compiler happy
#endif
}
} // namespace MapleRuntime
