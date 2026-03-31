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
private const MAX_PATH_LENGTH: Int64 = 4096

@When[backend == "cjnative"]
func validatePathString(pathStr: String): Bool {
    if (pathStr.isEmpty() || pathStr.size > MAX_PATH_LENGTH) {
        return false
    }
    if (pathStr.contains("\0")) {
        return false
    }
    if (pathStr.contains("..") || pathStr.contains("~")) {
        return false
    }
    return true
}

@When[backend == "cjnative"]
func resolvePath(path: Path): Path {
    let absolutePath = if (path.isAbsolute()) {
        path
    } else {
        canonicalize(Path(".")).join(path)
    }
    
    let resolvedPath = try {
        let parentDir = absolutePath.parent
        if (exists(parentDir)) {
            canonicalize(parentDir).join(absolutePath.fileName)
        } else {
            absolutePath
        }
    } catch (e: Exception) {
        absolutePath
    }
    
    return resolvedPath
}
