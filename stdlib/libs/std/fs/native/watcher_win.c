/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

#define WIN32_LEAN_AND_MEAN
#include "watcher.h"

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define EVBUF_CAP  64
#define DIRBUF_LEN 4096

struct CJ_FS_Watcher {
    HANDLE hDir;
    HANDLE hEvt;              /* manual-reset event for overlapped I/O */
    OVERLAPPED ov;
    int  recursive;
    char watchRoot[MAX_PATH]; /* UTF-8 watch root (informational) */
    BYTE buf[DIRBUF_LEN];    /* ReadDirectoryChangesW read buffer */
    BOOL pending;            /* TRUE while an overlapped read is in flight */
    /* scratch path buffers (UTF-8), stable until next Poll */
    char scratch[EVBUF_CAP][MAX_PATH];
    CJ_FS_WatchEvent evbuf[EVBUF_CAP];
};

/* Convert null-terminated UTF-8 narrow string to wide (wchar_t).
 * NOTE: only safe for null-terminated C strings (e.g. dirPath in Create). */
static void widen(const char* u8, wchar_t* out, int outcap)
{
    MultiByteToWideChar(CP_UTF8, 0, u8, -1, out, outcap);
}

/* Convert a counted (NOT null-terminated) wide string to UTF-8. */
static void narrowCounted(const wchar_t* w, DWORD nameLenBytes, char* out, int outcap)
{
    int cchWide = (int)(nameLenBytes / sizeof(wchar_t));
    int n = WideCharToMultiByte(CP_UTF8, 0, w, cchWide, out, outcap, NULL, NULL);
    if (n < 0) {
        n = 0;
    }
    if (n < outcap) {
        out[n] = '\0';
    } else if (outcap > 0) {
        out[outcap - 1] = '\0';
    }
}

/* Issue (or re-arm) an overlapped ReadDirectoryChangesW call. */
static BOOL armRead(CJ_FS_Watcher* w)
{
    DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME
                 | FILE_NOTIFY_CHANGE_DIR_NAME
                 | FILE_NOTIFY_CHANGE_ATTRIBUTES
                 | FILE_NOTIFY_CHANGE_SIZE
                 | FILE_NOTIFY_CHANGE_LAST_WRITE
                 | FILE_NOTIFY_CHANGE_CREATION;
    memset(&w->ov, 0, sizeof(w->ov));
    w->ov.hEvent = w->hEvt;
    ResetEvent(w->hEvt);
    DWORD returned = 0;
    BOOL ok = ReadDirectoryChangesW(w->hDir, w->buf, DIRBUF_LEN,
                                    w->recursive ? TRUE : FALSE,
                                    filter, &returned, &w->ov, NULL);
    w->pending = ok;
    return ok;
}

/* Append a decoded event to the internal evbuf. */
static void pushEv(CJ_FS_Watcher* w, int32_t kind,
                   const wchar_t* wname, DWORD nameLenBytes, size_t* filled)
{
    if (*filled >= EVBUF_CAP) {
        return;
    }
    CJ_FS_WatchEvent* ev = &w->evbuf[*filled];
    ev->kind  = kind;
    ev->isDir = 0;  /* Win does not report isDir in notifications (spec §14) */
    if (wname) {
        /* ReadDirectoryChangesW FileName is already relative to watched dir */
        narrowCounted(wname, nameLenBytes, w->scratch[*filled], MAX_PATH);
    } else {
        w->scratch[*filled][0] = '\0';
    }
    ev->path = w->scratch[*filled];
    (*filled)++;
}

CJ_FS_Watcher* CJ_FS_WatcherCreate(const char* dirPath,
    int32_t recursive, char** errOut)
{
    CJ_FS_Watcher* w = (CJ_FS_Watcher*)calloc(1, sizeof(CJ_FS_Watcher));
    if (!w) {
        return NULL;
    }

    w->recursive = recursive;
    (void)snprintf(w->watchRoot, sizeof(w->watchRoot), "%s", dirPath);

    wchar_t wpath[MAX_PATH];
    widen(dirPath, wpath, MAX_PATH);

    w->hDir = CreateFileW(wpath,
                          FILE_LIST_DIRECTORY,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL,
                          OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                          NULL);
    if (w->hDir == INVALID_HANDLE_VALUE) {
        if (errOut) {
            *errOut = strdup("CreateFileW failed");
        }
        free(w);
        return NULL;
    }

    w->hEvt = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!w->hEvt) {
        if (errOut) {
            *errOut = strdup("CreateEvent failed");
        }
        CloseHandle(w->hDir);
        free(w);
        return NULL;
    }

    if (!armRead(w)) {
        if (errOut) {
            *errOut = strdup("ReadDirectoryChangesW failed");
        }
        CloseHandle(w->hDir);
        CloseHandle(w->hEvt);
        free(w);
        return NULL;
    }

    return w;
}

void CJ_FS_WatcherDestroy(CJ_FS_Watcher* w)
{
    if (!w) {
        return;
    }
    if (w->pending) {
        CancelIo(w->hDir);
    }
    if (w->hDir != INVALID_HANDLE_VALUE) {
        CloseHandle(w->hDir);
    }
    if (w->hEvt) {
        CloseHandle(w->hEvt);
    }
    free(w);
}

int64_t CJ_FS_WatcherPoll(CJ_FS_Watcher* w, int64_t timeoutMs,
    CJ_FS_WatchEvent* outBuf, int64_t cap, char** errOut)
{
    if (!w) {
        if (errOut) {
            *errOut = strdup("null watcher");
        }
        return -1;
    }
    if (timeoutMs < 0) {
        if (errOut) {
            *errOut = strdup("CJ_FS_WatcherPoll: timeout must be >= 0");
        }
        return -1;
    }

    DWORD wait = WaitForSingleObject(w->hEvt, (DWORD)timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        return 0;
    }
    if (wait != WAIT_OBJECT_0) {
        if (errOut) {
            *errOut = strdup("WaitForSingleObject failed");
        }
        return -1;
    }

    DWORD bytes = 0;
    if (!GetOverlappedResult(w->hDir, &w->ov, &bytes, FALSE)) {
        DWORD err = GetLastError();
        if (err == ERROR_NOTIFY_ENUM_DIR) {
            armRead(w);
            return 0;  /* buffer overflow; events lost, try again */
        }
        if (errOut) {
            *errOut = strdup("GetOverlappedResult failed");
        }
        return -1;
    }
    w->pending = FALSE;

    /* Decode the FILE_NOTIFY_INFORMATION chain. */
    size_t filled = 0;
    BYTE* p = w->buf;
    BYTE* end = w->buf + bytes;
    for (;;) {
        if (p + sizeof(FILE_NOTIFY_INFORMATION) {
            > end) { break; }
        }
        FILE_NOTIFY_INFORMATION* fni = (FILE_NOTIFY_INFORMATION*)p;
        /* NOTE: FILE_NOTIFY_CHANGE_ATTRIBUTES is in the filter but maps to
         * MODIFY (not a separate ATTRIB kind) — deliberate v1 behavior. */
        switch (fni->Action) {
            case FILE_ACTION_ADDED:
                pushEv(w, CJ_FS_WK_CREATE, fni->FileName, fni->FileNameLength, &filled);
                break;
            case FILE_ACTION_REMOVED:
                pushEv(w, CJ_FS_WK_DELETE, fni->FileName, fni->FileNameLength, &filled);
                break;
            case FILE_ACTION_MODIFIED:
                pushEv(w, CJ_FS_WK_MODIFY, fni->FileName, fni->FileNameLength, &filled);
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                pushEv(w, CJ_FS_WK_RENAME_FROM, fni->FileName, fni->FileNameLength, &filled);
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                pushEv(w, CJ_FS_WK_RENAME_TO, fni->FileName, fni->FileNameLength, &filled);
                break;
            default:
                break;
        }
        if (fni->NextEntryOffset == 0) {
            break;
        }
        p += fni->NextEntryOffset;
        if (p >= end) {
            break;
        }
    }

    /* Re-arm for next cycle.
     * armRead only rewrites w->buf and w->ov; outBuf[].path points into
     * w->scratch which is untouched until next Poll decode, so pointers
     * remain valid until the next Poll call. */
    armRead(w);

    /* Copy decoded events to caller buffer (up to cap). */
    int64_t out = 0;
    for (size_t k = 0; k < filled && (int64_t)k < cap; k++) {
        outBuf[out++] = w->evbuf[k];
    }
    return out;
}
