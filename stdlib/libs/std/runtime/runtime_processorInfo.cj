/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.runtime

@When[backend == "cjnative"]
import std.fs.*

@When[backend == "cjnative"]
@Intrinsic
func startCjCPUProfiling(): Bool

@When[backend == "cjnative"]
@Intrinsic
func stopCjCPUProfiling(fd: Int32): Bool

@When[os == "Windows"]
foreign func CJ_Runtime_OpenFileForFd(path: CString): Int32

@When[os == "Windows"]
foreign func CJ_Runtime_CloseFd(fd: Int32): Int32

@FastNative
foreign func CJ_OS_ProcessorCount(): Int64

class ProfilingInfoException <: Exception {
    init(message: String) {
        super(message)
    }

    protected func getClassName(): String {
        return "ProfilingInfoException"
    }
}

@Deprecated[message: "All static Properties are converted to public functions."]
public struct ProcessorInfo {
    // Get the number of processors
    public static prop processorCount: Int64 {
        get() {
            getProcessorCount()
        }
    }
}

public func getProcessorCount(): Int64 {
    return unsafe { CJ_OS_ProcessorCount() }
}

@When[backend == "cjnative"]
public func startCPUProfiling(): Unit {
    let success = startCjCPUProfiling()
    if (!success) {
        throw ProfilingInfoException("Failed to start cpu profiling.")
    }
    return
}

@When[backend == "cjnative" && os != "Windows"]
public func stopCPUProfiling(path: Path): Unit {
    try (file = File(path, ReadWrite)) {
        let success = stopCjCPUProfiling(Int32(file.fileDescriptor.fileHandle))
        if (!success) {
            throw ProfilingInfoException("Failed to stop cpu profiling.")
        }
    }
    return
}

@When[backend == "cjnative" && os == "Windows"]
public func stopCPUProfiling(path: Path): Unit {
    let pathStr = path.toString()
    if (!validatePathString(pathStr)) {
        throw IllegalArgumentException(
            "CPU profiling path is invalid or contains forbidden characters for security reasons.")
    }

    let resolvedPath = resolvePath(path)

    try (cPath = unsafe { LibC.mallocCString(resolvedPath.toString()).asResource() }) {
        let fd = CJ_Runtime_OpenFileForFd(cPath)
        if (fd == -1) {
            throw ProfilingInfoException("Failed to stop cpu profiling.")
        }
        let success = stopCjCPUProfiling(fd)
        CJ_Runtime_CloseFd(fd)
        if (!success) {
            throw ProfilingInfoException("Failed to stop cpu profiling.")
        }
    }
    return
}
