/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

#ifndef CANGJIE_FS_WATCHER_H
#define CANGJIE_FS_WATCHER_H

#include <stdint.h>
#include <stddef.h>

typedef struct CJ_FS_Watcher CJ_FS_Watcher; /* opaque */

typedef struct {
    int32_t      kind;     /* CJ_FS_WK_* */
    int32_t      isDir;    /* 0/1 */
    const char*  path;     /* UTF-8, relative to watch root; points to internal scratch buffer, valid until next Poll */
} CJ_FS_WatchEvent;

enum {
    CJ_FS_WK_NONE              = 0,
    CJ_FS_WK_CREATE            = 1,
    CJ_FS_WK_MODIFY            = 2,
    CJ_FS_WK_DELETE            = 3,
    CJ_FS_WK_RENAME_FROM       = 4,
    CJ_FS_WK_RENAME_TO         = 5,
    CJ_FS_WK_ATTRIB            = 6,
    CJ_FS_WK_WATCH_DISAPPEARED = 7,
    CJ_FS_WK_OVERFLOW          = 8,
};

/* dirPath must be a directory.
 * recursive=1 for recursive watching.
 * Returns NULL on failure, writes message to *errOut. */
CJ_FS_Watcher* CJ_FS_WatcherCreate(const char* dirPath, int32_t recursive, char** errOut);

/* Blocks for up to timeoutMs, fills outBuf with ready events (<=cap).
 * Returns: >=0 event count; -1 fatal error; -2 OS unavailable. */
int64_t CJ_FS_WatcherPoll(CJ_FS_Watcher* w, int64_t timeoutMs,
                           CJ_FS_WatchEvent* outBuf, int64_t cap,
                           char** errOut);

void CJ_FS_WatcherDestroy(CJ_FS_Watcher* w);

#endif /* CANGJIE_FS_WATCHER_H */
