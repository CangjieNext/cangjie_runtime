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
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <poll.h>
#include <dirent.h>
#include <stdio.h>

/* Maximum events buffered per Poll call (matches WATCHER_BUFFER_CAP in watcher.cj). */
#define EVBUF_CAP 256

typedef struct {
    int  wd;
    char path[4096]; /* directory absolute/canonical path */
} WdEntry;

struct CJ_FS_Watcher {
    int     fd;         /* inotify file descriptor */
    int     recursive;
    WdEntry *dirs;      /* wd->path table */
    size_t  dirsCap;
    size_t  dirsLen;
    char    watchRoot[4096];
    size_t  rootLen;
    /* Event scratch: paths relative to root, stable until next Poll. */
    CJ_FS_WatchEvent evbuf[EVBUF_CAP];
    char            scratch[EVBUF_CAP][4096];
};

/* ---------- wd table management ---------- */

static void addWd(CJ_FS_Watcher *w, int wd, const char *path)
{
    if (w->dirsLen == w->dirsCap) {
        size_t nc = w->dirsCap == 0 ? 16 : w->dirsCap * 2;
        WdEntry *nb = (WdEntry *)malloc(nc * sizeof(WdEntry));
        if (!nb) {
            (void)close(wd);
            return;
        }
        if (w->dirs && w->dirsLen > 0) {
            size_t copyLen = w->dirsLen < nc ? w->dirsLen : nc;
            if (memcpy_s(nb, nc * sizeof(WdEntry), w->dirs, copyLen * sizeof(WdEntry)) != EOK) {
                (void)close(wd);
                free(nb);
                return;
            }
        }
        free(w->dirs);
        w->dirs   = nb;
        w->dirsCap = nc;
    }
    w->dirs[w->dirsLen].wd = wd;
    int err = snprintf_s(w->dirs[w->dirsLen].path, sizeof(w->dirs[w->dirsLen].path),
                         sizeof(w->dirs[w->dirsLen].path) - 1, "%s", path);
    if (err < 0) {
        /* ignore: path truncated but still usable */
    }
    w->dirsLen++;
}

static const char *pathOfWd(CJ_FS_Watcher *w, int wd)
{
    for (size_t i = 0; i < w->dirsLen; i++) {
        if (w->dirs[i].wd == wd) {
            return w->dirs[i].path;
        }
    }
    return NULL;
}

static void removeWd(CJ_FS_Watcher *w, int wd)
{
    for (size_t i = 0; i < w->dirsLen; i++) {
        if (w->dirs[i].wd == wd) {
            w->dirs[i] = w->dirs[w->dirsLen - 1];
            w->dirsLen--;
            return;
        }
    }
}

/* ---------- inotify mask ---------- */

static const uint32_t WATCH_MASK =
    IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVE | IN_ATTRIB |
    IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;

/* ---------- recursive tree walk ---------- */

static int addTree(CJ_FS_Watcher *w, const char *dir)
{
    int wd = inotify_add_watch(w->fd, dir, WATCH_MASK);
    if (wd < 0) {
        return -1;
    }
    addWd(w, wd, dir);

    if (!w->recursive) {
        return 0;
    }

    DIR *d = opendir(dir);
    if (!d) {
        return 0; /* not a dir or no permission -- skip silently */
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char child[4096];
        int err = snprintf_s(child, sizeof(child), sizeof(child) - 1, "%s/%s", dir, e->d_name);
        if (err < 0) {
            continue;
        }
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            addTree(w, child); /* recurse */
        }
    }
    closedir(d);
    return 0;
}

/* ---------- public API ---------- */

CJ_FS_Watcher *CJ_FS_WatcherCreate(const char *dirPath, int32_t recursive, char **errOut)
{
    CJ_FS_Watcher *w = (CJ_FS_Watcher *)calloc(1, sizeof(CJ_FS_Watcher));
    if (!w) {
        if (errOut) {
            *errOut = strdup("out of memory");
        }
        return NULL;
    }

    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd < 0) {
        if (errOut) {
            *errOut = strdup("inotify_init1 failed");
        }
        free(w);
        return NULL;
    }

    w->recursive = recursive;
    int err = snprintf_s(w->watchRoot, sizeof(w->watchRoot),
                         sizeof(w->watchRoot) - 1, "%s", dirPath);
    if (err < 0) {
        if (errOut) {
            *errOut = strdup("watchRoot path too long");
        }
        (void)close(w->fd);
        free(w);
        return NULL;
    }
    w->rootLen = strlen(w->watchRoot);

    if (addTree(w, dirPath) != 0) {
        if (errOut) {
            *errOut = strdup("inotify_add_watch failed (check max_user_watches / path)");
        }
        (void)close(w->fd);
        free(w->dirs);
        free(w);
        return NULL;
    }

    return w;
}

void CJ_FS_WatcherDestroy(CJ_FS_Watcher *w)
{
    if (!w) {
        return;
    }
    if (w->fd >= 0) {
        (void)close(w->fd);
    }
    free(w->dirs);
    free(w);
}

/* ---------- helpers for Poll ---------- */

/*
 * Convert an absolute path (base/name) to a path relative to watchRoot.
 * Writes result into `out` (capacity `outcap`), returns `out`.
 */
static const char *relOf(CJ_FS_Watcher *w, const char *base,
                         const char *name, char *out, size_t outcap)
{
    char abs[4096];
    int err = snprintf_s(abs, sizeof(abs), sizeof(abs) - 1, "%s/%s", base, name);
    if (err < 0) {
        out[0] = '\0';
        return out;
    }
    const char *rel = abs;
    if (strncmp(abs, w->watchRoot, w->rootLen) == 0) {
        rel = abs + w->rootLen;
        if (*rel == '/') {
            rel++;
        }
    }
    err = snprintf_s(out, outcap, outcap - 1, "%s", rel);
    if (err < 0) {
        out[0] = '\0';
    }
    return out;
}

/*
 * Append one event to the internal buffer.
 */
static void pushEvent(CJ_FS_Watcher *w, int32_t kind, const char *relPath,
                      int32_t isDir, size_t *out)
{
    if (*out >= EVBUF_CAP) {
        return; /* buffer full -- drop event */
    }

    CJ_FS_WatchEvent *ev = &w->evbuf[*out];
    ev->kind  = kind;
    ev->isDir = isDir;

    if (relPath != NULL) {
        int err = snprintf_s(w->scratch[*out], sizeof(w->scratch[*out]),
                              sizeof(w->scratch[*out]) - 1, "%s", relPath);
        if (err < 0) {
            w->scratch[*out][0] = '\0';
        }
        ev->path = w->scratch[*out];
    } else {
        /* watchDisappeared / overflow: empty path */
        w->scratch[*out][0] = '\0';
        ev->path = w->scratch[*out];
    }
    (*out)++;
}

/* ---------- event kind lookup table ---------- */

typedef struct {
    uint32_t mask;
    int32_t  kind;
} MaskToKind;

static const MaskToKind g_maskKindTable[] = {
    { IN_CREATE,              CJ_FS_WK_CREATE            },
    { IN_MODIFY,              CJ_FS_WK_MODIFY            },
    { IN_DELETE,              CJ_FS_WK_DELETE            },
    { IN_MOVED_FROM,          CJ_FS_WK_RENAME_FROM       },
    { IN_MOVED_TO,            CJ_FS_WK_RENAME_TO         },
    { IN_ATTRIB,              CJ_FS_WK_ATTRIB            },
    { IN_DELETE_SELF | IN_MOVE_SELF, CJ_FS_WK_WATCH_DISAPPEARED },
};
#define MASK_KIND_COUNT (sizeof(g_maskKindTable) / sizeof(g_maskKindTable[0]))

/* ---------- event processing (extracted from Poll) ---------- */

/*
 * Resolve the event kind for a non-special inotify mask.
 * Returns 1 if a matching kind was pushed, 0 if no match.
 */
static int resolveEventKind(CJ_FS_Watcher *w, struct inotify_event *ie,
                            const char *relPath, int32_t isDir, size_t *filled)
{
    for (size_t i = 0; i < MASK_KIND_COUNT; i++) {
        if (ie->mask & g_maskKindTable[i].mask) {
            pushEvent(w, g_maskKindTable[i].kind, relPath, isDir, filled);
            return 1;
        }
    }
    return 0;
}

/*
 * Process a single inotify_event: decode its mask, push into w->evbuf,
 * and handle recursive subdirectory addition on CREATE.
 */
static void processInotifyEvent(CJ_FS_Watcher *w, struct inotify_event *ie,
                                size_t *filled)
{
    const char *base = pathOfWd(w, ie->wd);

    /* Handle overflow -- no wd needed */
    if (ie->mask & IN_Q_OVERFLOW) {
        pushEvent(w, CJ_FS_WK_OVERFLOW, NULL, 0, filled);
        return;
    }

    /* Handle ignored watch -- just remove tracking entry */
    if (ie->mask & IN_IGNORED) {
        removeWd(w, ie->wd);
        return;
    }

    /* If no known base path, nothing to do */
    if (base == NULL) {
        return;
    }

    int32_t isDir = (ie->mask & IN_ISDIR) ? 1 : 0;
    char relPath[4096];
    relOf(w, base, ie->name, relPath, sizeof(relPath));

    /* Push the event via lookup table */
    if (resolveEventKind(w, ie, relPath, isDir, filled)) {
        /* Lazily add a watch for newly-created subdirectories */
        if ((ie->mask & IN_CREATE) && w->recursive && isDir && ie->len > 0) {
            char child[4096];
            int err = snprintf_s(child, sizeof(child), sizeof(child) - 1,
                                 "%s/%s", base, ie->name);
            if (err >= 0) {
                int nwd = inotify_add_watch(w->fd, child, WATCH_MASK);
                if (nwd >= 0) {
                    addWd(w, nwd, child);
                }
            }
        }
    }
}

/* ---------- Poll ---------- */

int64_t CJ_FS_WatcherPoll(CJ_FS_Watcher *w, int64_t timeoutMs,
    CJ_FS_WatchEvent *outBuf, int64_t cap, char **errOut)
{
    if (!w) {
        if (errOut) {
            *errOut = strdup("null watcher");
        }
        return -1;
    }

    struct pollfd pfd;
    pfd.fd      = w->fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    int pr = poll(&pfd, 1, (int)timeoutMs);
    if (pr < 0) {
        if (errno == EINTR) {
            return 0; /* interrupted -- not fatal */
        }
        if (errOut) {
            *errOut = strdup("poll error");
        }
        return -1;
    }
    if (pr == 0) {
        return 0; /* timeout, no events */
    }

    /*
     * Read all available events from the non-blocking inotify fd.
     * Each inotify_event is variable-length: fixed header + ie->len bytes of name.
     */
    char rdBuf[16384];
    size_t filled = 0;

    while (1) {
        ssize_t n = read(w->fd, rdBuf, sizeof(rdBuf));
        if (n <= 0) {
            break; /* EAGAIN or error */
        }

        size_t i = 0;
        while (i < (size_t)n) {
            struct inotify_event *ie = (struct inotify_event *)(rdBuf + i);
            processInotifyEvent(w, ie, &filled);
            i += sizeof(struct inotify_event) + ie->len;
        }
    }

    /* Copy from internal buffer to caller-supplied outBuf. */
    int64_t out = 0;
    size_t limit = (size_t)cap < filled ? (size_t)cap : filled;
    for (size_t k = 0; k < limit; k++) {
        outBuf[out++] = w->evbuf[k];
    }
    return out;
}
