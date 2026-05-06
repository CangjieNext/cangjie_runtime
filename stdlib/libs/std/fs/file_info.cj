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
 * This file defines FileInfo classes.
 */
package std.fs

import std.time.*

const FILE_INFO_NO_FOLLOW_PREFIX = "\0std.fs.FileInfo.noFollow\0"

public struct FileInfo <: Equatable<FileInfo> {
    var _path: Path

    public init(path: Path) {
        this(path, true)
    }

    public init(path: String) {
        this(Path(path), true)
    }

    public init(path: Path, followSymlink!: Bool) {
        this(path, checkPath: true, followSymlink: followSymlink)
    }

    public init(path: String, followSymlink!: Bool) {
        this(Path(path), followSymlink: followSymlink)
    }

    /**
     * @throws FSException if path is invalid
     */
    init(path: Path, isCheckPath: Bool) {
        this(path, checkPath: isCheckPath, followSymlink: true)
    }

    /**
     * @throws FSException if path is invalid
     */
    init(path: Path, checkPath!: Bool, followSymlink!: Bool = true) {
        if (checkPath && !exists(path, followSymlink: followSymlink)) {
            throw FSException("The input path `${path}` does not exist.")
        }
        let absolutePath = toAbsolutePath(path)
        _path = if (followSymlink) {
            absolutePath
        } else {
            Path(FILE_INFO_NO_FOLLOW_PREFIX + absolutePath.toString())
        }
    }

    public prop name: String {
        get() {
            realPath().fileName
        }
    }

    public prop parentDirectory: Option<FileInfo> {
        get() {
            let path = realPath()
            let parent = path.parent
            if (!parent.toString().isEmpty() && !isRootPath(path.toString())) {
                return FileInfo(parent)
            }
            return None
        }
    }

    public prop path: Path {
        get() {
            realPath()
        }
    }

    /**
     * @throws FSException if system failed to get creation time
     */
    public prop creationTime: DateTime {
        get() {
            let time = callQueryNativeFunc(CJ_FS_GetCreationTime, "Failed to get creation time.")
            return DateTime.ofEpoch(second: time, nanosecond: 0)
        }
    }

    /**
     * @throws FSException if system failed to get last access time
     */
    public prop lastAccessTime: DateTime {
        get() {
            let time = callQueryNativeFunc(CJ_FS_GetLastAccessTime, "Failed to get last access time.")
            return DateTime.ofEpoch(second: time, nanosecond: 0)
        }
    }

    /**
     * @throws FSException if system failed to get last modification time
     */
    public prop lastModificationTime: DateTime {
        get() {
            let time = callQueryNativeFunc(CJ_FS_GetLastModificationTime, "Failed to get last modification time.")
            return DateTime.ofEpoch(second: time, nanosecond: 0)
        }
    }

    /**
     * @throws FSException if memory copy failed or get directory/file size failed
     */
    public prop size: Int64 {
        get() {
            return callQueryNativeFunc(CJ_FS_PathSize, "Failed to get file size.")
        }
    }

    /**
     * @throws FSException if cPath is invalid or symbol not linked
     */
    public func isSymbolicLink(): Bool {
        return callQueryNativeFunc(CJ_FS_IsLink)
    }

    /**
     * @throws FSException if cPath is invalid
     */
    public func isRegular(): Bool {
        return callQueryNativeFunc(CJ_FS_IsFile)
    }

    /**
     * @throws FSException if cPath is invalid
     */
    public func isDirectory(): Bool {
        return callQueryNativeFunc(CJ_FS_IsDir)
    }

    /**
     * @throws FSException if cPath is invalid
     */
    public func isReadOnly(): Bool {
        return callQueryNativeFunc(CJ_FS_IsReadOnly)
    }

    public func isHidden(): Bool {
        let fileName = if (followSymlink()) {
            canonicalize(realPath()).fileName
        } else {
            realPath().fileName
        }
        if (fileName.isEmpty()) {
            throw FSException("FileName is empty.")
        }
        return fileName[0] == b'.'
    }

    /**
     * @throws FSException if cPath is invalid
     */
    public func canExecute(): Bool {
        // _wstat in implementation do not accept "\\\\?\\" as it's argument
        return callQueryNativeFunc(CJ_FS_CanExecute, shouldFixLongPath: false)
    }

    /**
     * @throws FSException if cPath is invalid
     */
    public func canRead(): Bool {
        return callQueryNativeFunc(CJ_FS_CanRead)
    }

    /**
     * @throws FSException if cPath is invalid
     */
    public func canWrite(): Bool {
        return callQueryNativeFunc(CJ_FS_CanWrite)
    }

    /**
     * @throws FSException if cPath is invalid
     * @throws FSException if operation not permitted on Windows
     * Processes running as a privileged user may bypass this permission setting.
     */
    public func setExecutable(executable: Bool): Bool {
        if (isSelfSymbolicLink()) {
            return canExecute()
        }
        return callSetterNativeFunc(CJ_FS_SetExecutable, executable)
    }

    /**
     * @throws FSException if cPath is invalid
     * @throws FSException if operation not permitted on Windows
     * Processes running as a privileged user may bypass this permission setting.
     */
    public func setReadable(readable: Bool): Bool {
        if (isSelfSymbolicLink()) {
            return canRead()
        }
        return callSetterNativeFunc(CJ_FS_SetReadable, readable)
    }

    /**
     * @throws FSException if cPath is invalid
     * @throws FSException if operation not permitted on Windows
     */
    public func setWritable(writable: Bool): Bool {
        // _wstat in implementation do not accept "\\\\?\\" as it's argument
        if (isSelfSymbolicLink()) {
            return canWrite()
        }
        return callSetterNativeFunc(CJ_FS_SetWritable, writable, shouldFixLongPath: false)
    }

    private func isSelfSymbolicLink(): Bool {
        return !followSymlink() && callQueryNativeFunc(CJ_FS_IsLink)
    }

    /**
     * @throws FSException if cPath is invalid
     */
    private func callQueryNativeFunc(nativeFunction: CFunc<(CString, Bool) -> Int8>, shouldFixLongPath!: Bool = true): Bool {
        unsafe {
            let queryPath = realPath()
            let pathStr = if (shouldFixLongPath) {
                fixLongPath(queryPath.toString())
            } else {
                queryPath.toString()
            }
            let cPath = LibC.mallocCString(pathStr)
            var ret: Int8 = nativeFunction(cPath, followSymlink())
            LibC.free(cPath)
            if (ret < 0) {
                throw FSException("Native function error return ${ret}.")
            }
            return ret > 0
        }
    }

    /**
     * @throws FSException if cPath is invalid
     * @throws FSException if operation not permitted on Windows
     */
    private func callSetterNativeFunc(nativeFunction: CFunc<(CString, Bool) -> Int8>, flag: Bool,
        shouldFixLongPath!: Bool = true): Bool {
        unsafe {
            let queryPath = realPath()
            let pathStr = if (shouldFixLongPath) {
                fixLongPath(queryPath.toString())
            } else {
                queryPath.toString()
            }
            let cPath = LibC.mallocCString(pathStr)
            let ret: Int8 = nativeFunction(cPath, flag)
            LibC.free(cPath)
            if (ret == -1) {
                throw FSException("Native function error return ${ret}.")
            } else if (ret == -4) {
                throw FSException("Operation not permitted!")
            } else if (ret == -2) {
                throw FSException("Invalid argument!")
            }
            return ret > 0
        }
    }

    /**
     * @throws FSException if `nativeFunction` returns negative error code
     */
    private func callQueryNativeFunc(nativeFunction: CFunc<(CString, Bool) -> Int64>, msg: String,
        shouldFixLongPath!: Bool = true): Int64 {
        unsafe {
            let queryPath = realPath()
            let pathStr = if (shouldFixLongPath) {
                fixLongPath(queryPath.toString())
            } else {
                queryPath.toString()
            }
            let cPath: CString = LibC.mallocCString(pathStr)
            let ret: Int64 = nativeFunction(cPath, followSymlink())
            LibC.free(cPath)
            if (ret < 0) {
                throw FSException("${msg} Native function return ${ret}.")
            }
            return ret
        }
    }

    public operator func ==(other: FileInfo): Bool {
        return realPath() == other.path
    }

    private func followSymlink(): Bool {
        return !_path.toString().startsWith(FILE_INFO_NO_FOLLOW_PREFIX)
    }

    private func realPath(): Path {
        let path = _path.toString()
        if (path.startsWith(FILE_INFO_NO_FOLLOW_PREFIX)) {
            return Path(path.removePrefix(FILE_INFO_NO_FOLLOW_PREFIX))
        }
        return _path
    }
}
