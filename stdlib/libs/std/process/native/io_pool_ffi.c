/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

#include <pthread.h>
#include <stdlib.h>
#include "io_pool_ffi.h"

/* @C function defined on the Cangjie side: the worker main loop. */
extern void CJ_IOPool_WorkerLoop(void);

/* Worker thread entry: immediately enters the Cangjie worker main loop.
 * All subsequent scheduling logic lives on the Cangjie side. */
static void* IoWorkerEntry(void* arg)
{
    (void)arg;
    CJ_IOPool_WorkerLoop();
    return NULL;
}

extern int32_t CJ_IOPool_CreateWorker(void)
{
    pthread_t tid;
    int ret = pthread_create(&tid, NULL, IoWorkerEntry, NULL);
    if (ret == 0) {
        /* detach: worker threads do not need to be joined. */
        pthread_detach(tid);
        return 0;
    }
    return -1;
}
