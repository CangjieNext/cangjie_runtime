/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

#include "watcher.h"

#include <stdlib.h>
#include <string.h>
#include <CoreServices/CoreServices.h>

#define EVBUF_CAP   64
#define PATH_MAX_LEN 4096

typedef struct {
    int32_t kind;
    int32_t isDir;
    char    path[PATH_MAX_LEN];
} QueuedEvent;

struct CJ_FS_Watcher {
    FSEventStreamRef      stream;
    dispatch_queue_t      dq;
    QueuedEvent           q[EVBUF_CAP];
    size_t                qHead;
    size_t                qTail;
    size_t                qCount;
    char                  watchRoot[PATH_MAX_LEN];
    size_t                rootLen;
    /* Per-handle output buffers -- written ONLY by Poll (worker thread).
     * The dispatch callback never touches these, so path pointers remain
     * valid after Poll returns. */
    CJ_FS_WatchEvent      outEvbuf[EVBUF_CAP];
    char                  outScratch[EVBUF_CAP][PATH_MAX_LEN];
};

/* ---- internal ring queue ------------------------------------------------ */

static void qPush(CJ_FS_Watcher* w, int32_t kind, int32_t isDir, const char* path)
{
    if (w->qCount == EVBUF_CAP) {
        return;   /* overflow: event lost -- caller uses OVERFLOW flag path */
    }
    QueuedEvent* e = &w->q[w->qTail];
    e->kind  = kind;
    e->isDir = isDir;
    (void)snprintf(e->path, sizeof(e->path), "%s", path);
    w->qTail = (w->qTail + 1) % EVBUF_CAP;
    w->qCount++;
}

static int qPop(CJ_FS_Watcher* w, CJ_FS_WatchEvent* out)
{
    if (w->qCount == 0) {
        return 0;
    }
    QueuedEvent* e = &w->q[w->qHead];
    out->kind  = e->kind;
    out->isDir = e->isDir;
    out->path  = e->path;   /* pointer into handle-internal scratch, valid until next Poll */
    w->qHead = (w->qHead + 1) % EVBUF_CAP;
    w->qCount--;
    return 1;
}

/* ---- FSEvents stream callback ------------------------------------------- */

/* Process FSEvents callbacks (internal, fewer params). */
static void processStreamEvents(CJ_FS_Watcher *w, size_t numEvents,
                                char **paths, const FSEventStreamEventFlags *eventFlags)
{
    for (size_t i = 0; i < numEvents; i++) {
        FSEventStreamEventFlags f    = eventFlags[i];
        const char*              abs  = paths[i];
        const char*              rel  = abs;

        /* Strip watchRoot prefix to produce relative path */
        if (strncmp(abs, w->watchRoot, w->rootLen) == 0) {
            rel = abs + w->rootLen;
            if (*rel == '/') {
                rel++;
            }
        }

        int32_t isDir = (f & kFSEventStreamEventFlagItemIsDir) ? 1 : 0;

        if (f & kFSEventStreamEventFlagRootChanged) {
            qPush(w, CJ_FS_WK_WATCH_DISAPPEARED, 0, "");
        } else if (f & kFSEventStreamEventFlagMustScanSubDirs) {
            qPush(w, CJ_FS_WK_OVERFLOW, 0, "");
        } else if (f & kFSEventStreamEventFlagItemCreated) {
            qPush(w, CJ_FS_WK_CREATE, isDir, rel);
        } else if (f & kFSEventStreamEventFlagItemRemoved) {
            qPush(w, CJ_FS_WK_DELETE, isDir, rel);
        } else if (f & kFSEventStreamEventFlagItemModified) {
            qPush(w, CJ_FS_WK_MODIFY, isDir, rel);
        } else if (f & kFSEventStreamEventFlagItemRenamed) {
            /* rename from/to pairing is approximate in v1 (spec §14):
             * a single rename event emits both RENAME_FROM and RENAME_TO. */
            qPush(w, CJ_FS_WK_RENAME_FROM, isDir, rel);
            qPush(w, CJ_FS_WK_RENAME_TO,   isDir, rel);
        } else {
            qPush(w, CJ_FS_WK_ATTRIB, isDir, rel);
        }
    }
}

/* G.FUN.01: system callback -- signature mandated by FSEvents API, cannot reduce parameters. */
static void streamCb(ConstFSEventStreamRef streamRef,
                     void*                  clientCallBackInfo,
                     size_t                 numEvents,
                     void*                  eventPaths,
                     const FSEventStreamEventFlags eventFlags[],
                     const FSEventStreamEventId  eventIds[])
{
    (void)streamRef;
    (void)eventIds;
    processStreamEvents((CJ_FS_Watcher*)clientCallBackInfo, numEvents,
                        (char**)eventPaths, eventFlags);
}

/* ---- watcher cleanup (extracted from Create error paths) ----------------- */

/*
 * Release resources held by a partially-initialised watcher.
 * Safe to call on a zeroed-out (calloc'd) struct.
 */
static void cleanupWatcher(CJ_FS_Watcher* w)
{
    if (!w) {
        return;
    }
    if (w->stream) {
        FSEventStreamInvalidate(w->stream);
        FSEventStreamRelease(w->stream);
    }
    if (w->dq) {
        dispatch_release(w->dq);
    }
    free(w);
}

/* ---- public API --------------------------------------------------------- */

CJ_FS_Watcher* CJ_FS_WatcherCreate(const char* dirPath, int32_t recursive, char** errOut)
{
    /* FSEvents is inherently recursive; non-recursive is approximated by
     * Cangjie-side path filtering (accept()), not handled here. */
    (void)recursive;

    CJ_FS_Watcher* w = (CJ_FS_Watcher*)calloc(1, sizeof(CJ_FS_Watcher));
    if (!w) {
        if (errOut) {
            *errOut = strdup("out of memory");
        }
        return NULL;
    }

    (void)snprintf(w->watchRoot, sizeof(w->watchRoot), "%s", dirPath);
    w->rootLen = strlen(w->watchRoot);

    /* Build the CFArray of paths to watch (single root) */
    CFStringRef pathStr = CFStringCreateWithCString(NULL, dirPath, kCFStringEncodingUTF8);
    if (!pathStr) {
        if (errOut) {
            *errOut = strdup("CFStringCreateWithCString failed");
        }
        free(w);
        return NULL;
    }
    CFArrayRef pathsToWatch = CFArrayCreate(NULL, (const void**)&pathStr, 1, NULL);
    if (!pathsToWatch) {
        if (errOut) {
            *errOut = strdup("CFArrayCreate failed");
        }
        CFRelease(pathStr);
        free(w);
        return NULL;
    }

    /* Create FSEvents stream */
    FSEventStreamContext ctx   = { 0, w, NULL, NULL, NULL };
    FSEventStreamCreateFlags flags =
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer;

    w->stream = FSEventStreamCreate(
        NULL,
        streamCb,
        &ctx,
        pathsToWatch,
        kFSEventStreamSinceEventId,   /* start from now */
        0.0,                           /* latency = 0, fire immediately */
        flags);

    CFRelease(pathsToWatch);
    CFRelease(pathStr);

    if (!w->stream) {
        if (errOut) {
            *errOut = strdup("FSEventStreamCreate failed");
        }
        free(w);
        return NULL;
    }

    /* Attach stream to a serial dispatch queue */
    w->dq = dispatch_queue_create("cj.fs.watcher", DISPATCH_QUEUE_SERIAL);
    if (!w->dq) {
        if (errOut) {
            *errOut = strdup("dispatch_queue_create failed");
        }
        cleanupWatcher(w);
        return NULL;
    }

    FSEventStreamSetDispatchQueue(w->stream, w->dq);

    if (!FSEventStreamStart(w->stream)) {
        if (errOut) {
            *errOut = strdup("FSEventStreamStart failed");
        }
        cleanupWatcher(w);
        return NULL;
    }

    return w;
}

void CJ_FS_WatcherDestroy(CJ_FS_Watcher* w)
{
    if (!w) {
        return;
    }
    if (w->stream) {
        FSEventStreamStop(w->stream);
    }
    /* cleanupWatcher releases stream and dq without calling Stop */
    w->stream = NULL;
    dispatch_queue_t savedDq = w->dq;
    w->dq = NULL;
    if (savedDq) {
        dispatch_release(savedDq);
    }
    free(w);
}

/* ---------- Poll ---------- */

/*
 * Drain one event from the ring queue into the output buffer.
 * Returns 1 if an event was drained, 0 if the queue was empty.
 * Caller holds the serial dispatch queue lock (via dispatch_sync).
 */
static int drainOneEvent(CJ_FS_Watcher* w, int64_t outIdx)
{
    CJ_FS_WatchEvent tmp;
    if (!qPop(w, &tmp)) {
        return 0;
    }
    w->outEvbuf[outIdx].kind  = tmp.kind;
    w->outEvbuf[outIdx].isDir = tmp.isDir;
    (void)snprintf(w->outScratch[outIdx], PATH_MAX_LEN, "%s", tmp.path);
    w->outEvbuf[outIdx].path  = w->outScratch[outIdx];
    return 1;
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

    /*
     * timeoutMs is not used directly: the Cangjie worker's ~200 ms poll loop
     * provides the polling cadence. This is acceptable for FSEvents which
     * delivers events asynchronously via the serial dispatch queue.
     */
    (void)timeoutMs;

    /*
     * Drain the internal ring queue on the dispatch thread so that all
     * previously-queued callbacks have completed before we read the ring.
     * dispatch_sync serialises qPop with qPush (which runs inside streamCb
     * on the same serial queue), eliminating the data race.  Each event's
     * path is deep-copied into w->outScratch so the dispatch callback can
     * never overwrite it after Poll returns.
     */
    __block int64_t out = 0;
    dispatch_sync(w->dq, ^{
        while (out < cap && drainOneEvent(w, out)) {
            out++;
        }
    });

    /* Copy handle-local output into caller's buffer.
     * outBuf[k].path now points into w->outScratch[k] (worker-owned memory). */
    for (int64_t k = 0; k < out && k < cap; k++) {
        outBuf[k] = w->outEvbuf[k];
    }

    return out;   /* >=0 event count; -1 reserved for fatal error (null watcher handled above) */
}
