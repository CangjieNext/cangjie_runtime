/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

#ifndef IO_POOL_FFI_H
#define IO_POOL_FFI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create a single OS-level worker thread (pthread) for blocking IO.
 * The worker entry immediately calls back into the Cangjie-side
 * CJ_IOPool_WorkerLoop() function. Called on-demand by
 * IOPool.ensureWorkerAvailable() (lazy spawn / dynamic scaling).
 *
 * retval 0  on success
 * retval -1 on pthread_create failure
 */
int32_t CJ_IOPool_CreateWorker(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IO_POOL_FFI_H */
