/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

/**
 * @file
 *
 * Native FFI declarations for the file-system watcher (CJ_FS_Watcher*).
 * Consumes the C symbols produced by Task 4.
 */

package std.fs

foreign {
    // dirPath must be a directory. recursive=1 for recursive watching.
    // Returns 0 (NULL) on failure, writes error message to *errOutCstr.
    func CJ_FS_WatcherCreate(dirPath: CString, recursive: Int32, errOutCstr: CPointer<CString>): UIntNative

    // Blocks for up to timeoutMs, fills outBuf with ready events (<= cap).
    // Returns: >= 0 event count; -1 fatal error; -2 OS watch lost.
    func CJ_FS_WatcherPoll(h: UIntNative, timeoutMs: Int64,
                           outBuf: CPointer<WatchEventNative>, cap: Int64,
                           errOutCstr: CPointer<CString>): Int64

    // Destroys the watcher handle.
    func CJ_FS_WatcherDestroy(h: UIntNative): Unit
}

/**
 * Mirrors the C struct CJ_FS_WatchEvent from watcher.h.
 * Fields have defaults so the struct can be zero-initialized.
 */
@C
struct WatchEventNative {
    let kind: Int32 = 0
    let isDir: Int32 = 0
    let path: CString = CString(CPointer<UInt8>())
}

/**
 * Converts a CString to a String.  Returns an empty string when cs is null.
 */
func cstringToString(cs: CString): String {
    if (cs.isNull()) {
        return String()
    }
    return cs.toString()
}

/**
 * Maps a native event-kind integer (CJ_FS_WK_*) to the Cangjie WatchEventKind enum.
 */
func mapKind(nativeKind: Int32): WatchEventKind {
    match (nativeKind) {
        case 1 => WatchEventKind.create
        case 2 => WatchEventKind.modify
        case 3 => WatchEventKind.delete
        case 4 => WatchEventKind.renameFrom
        case 5 => WatchEventKind.renameTo
        case 6 => WatchEventKind.attributeChange
        case 7 => WatchEventKind.watchDisappeared
        case 8 => WatchEventKind.overflow
        case _ => WatchEventKind.modify // fallback; should not occur
    }
}
