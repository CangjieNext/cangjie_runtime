/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

#include <stdatomic.h>
#include <stdlib.h>

#ifdef WIN32
#include <windows.h>
#else
#include <sched.h>
#include <time.h>
#endif  // WIN32

#ifdef __arm__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Watomic-alignment"
#endif

static atomic_flag g_atexitCallbackListLocker = ATOMIC_FLAG_INIT;
extern void CJ_CORE_AtExitCallbackListLock(void)
{
    int spins = 0;
    while (atomic_flag_test_and_set(&g_atexitCallbackListLocker)) {
        /* Pure spinning here would burn a core for the whole atexit teardown
         * when a thread loses the race against CJ_CORE_ExecAtexitCallbacks
         * (the flag check in the Cangjie layer happens before the lock).
         * Yield after a short busy phase so the waiter cannot starve the
         * lock holder (which may run long, blocking user callbacks). */
        if (++spins > 16) {
#ifdef WIN32
            Sleep(1);
#else
            if (spins <= 4096) {
                sched_yield();
            } else {
                struct timespec ts = {0, 100000}; /* 100us */
                nanosleep(&ts, NULL);
            }
#endif
        }
    }
}
extern void CJ_CORE_AtExitCallbackListUnlook(void)
{
    atomic_flag_clear(&g_atexitCallbackListLocker);
}

#ifdef WIN32
extern void CJ_CORE_Abort(void)
{
    TerminateProcess(GetCurrentProcess(), 1);
}
#else
extern void CJ_CORE_Abort(void)
{
    abort();
}
#endif

#ifdef __arm__
#pragma GCC diagnostic pop
#endif