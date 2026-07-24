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
        WdEntry *nb = (WdEntry *)realloc(w->dirs, nc * sizeof(WdEntry));
        if (!nb) {
            close(wd); /* best-effort: cannot track this wd */
            return;
        }
        w->dirs   = nb;
        w->dirsCap = nc;
    }
    w->dirs[w->dirsLen].wd = wd;
    snprintf(w->dirs[w->dirsLen].path, sizeof(w->dirs[w->dirsLen].path), "%s", path);
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
        snprintf(child, sizeof(child), "%s/%s", dir, e->d_name);
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
    snprintf(w->watchRoot, sizeof(w->watchRoot), "%s", dirPath);
    w->rootLen = strlen(w->watchRoot);

    if (addTree(w, dirPath) != 0) {
        if (errOut) {
            *errOut = strdup("inotify_add_watch failed (check max_user_watches / path)");
        }
        close(w->fd);
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
        close(w->fd);
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
    snprintf(abs, sizeof(abs), "%s/%s", base, name);
    const char *rel = abs;
    if (strncmp(abs, w->watchRoot, w->rootLen) == 0) {
        rel = abs + w->rootLen;
        if (*rel == '/') {
            rel++;
        }
    }
    snprintf(out, outcap, "%s", rel);
    return out;
}

/*
 * Append one event to the internal buffer.
 */
static void pushEvent(CJ_FS_Watcher *w, int32_t kind, const char *base,
                      const char *name, int32_t isDir, size_t *out)
{
    if (*out >= EVBUF_CAP) {
        return; /* buffer full -- drop event */
    }

    CJ_FS_WatchEvent *ev = &w->evbuf[*out];
    ev->kind  = kind;
    ev->isDir = isDir;

    if (name != NULL) {
        relOf(w, base, name, w->scratch[*out], sizeof(w->scratch[*out]));
        ev->path = w->scratch[*out];
    } else {
        /* watchDisappeared / overflow: empty path */
        w->scratch[*out][0] = '\0';
        ev->path = w->scratch[*out];
    }
    (*out)++;
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
            const char *base = pathOfWd(w, ie->wd);

            if (ie->mask & IN_Q_OVERFLOW) {
                pushEvent(w, CJ_FS_WK_OVERFLOW, NULL, NULL, 0, &filled);
            } else if (ie->mask & IN_IGNORED) {
                removeWd(w, ie->wd);
            } else if (base != NULL) {
                int32_t isDir = (ie->mask & IN_ISDIR) ? 1 : 0;

                if (ie->mask & IN_CREATE) {
                    pushEvent(w, CJ_FS_WK_CREATE, base, ie->name, isDir, &filled);
                    /* Lazily add a watch for newly-created subdirectories */
                    if (w->recursive && isDir && ie->len > 0) {
                        char child[4096];
                        snprintf(child, sizeof(child), "%s/%s", base, ie->name);
                        int nwd = inotify_add_watch(w->fd, child, WATCH_MASK);
                        if (nwd >= 0) {
                            addWd(w, nwd, child);
                        }
                    }
                } else if (ie->mask & IN_MODIFY) {
                    pushEvent(w, CJ_FS_WK_MODIFY, base, ie->name, isDir, &filled);
                } else if (ie->mask & IN_DELETE) {
                    pushEvent(w, CJ_FS_WK_DELETE, base, ie->name, isDir, &filled);
                } else if (ie->mask & IN_MOVED_FROM) {
                    pushEvent(w, CJ_FS_WK_RENAME_FROM, base, ie->name, isDir, &filled);
                } else if (ie->mask & IN_MOVED_TO) {
                    pushEvent(w, CJ_FS_WK_RENAME_TO, base, ie->name, isDir, &filled);
                } else if (ie->mask & IN_ATTRIB) {
                    pushEvent(w, CJ_FS_WK_ATTRIB, base, ie->name, isDir, &filled);
                } else if (ie->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                    pushEvent(w, CJ_FS_WK_WATCH_DISAPPEARED, base, ie->name, isDir, &filled);
                }
            }
            /* Advance to the next event in the read buffer */
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
