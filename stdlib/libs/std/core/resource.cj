/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * Resource interface for managing external resources that require explicit cleanup.
 *
 * Implementations should ensure that `close()` is called when the resource is no longer needed.
 * The `close()` method should be idempotent (safe to call multiple times).
 *
 * Note: Due to language restrictions, destructors (`~init()`) cannot call instance methods.
 * Therefore, callers must explicitly call `close()` to release resources, or use patterns
 * like `try-finally` to ensure cleanup:
 *
 * Example usage:
 * ```cangjie
 * let resource = MyResource()
 * try {
 *     // use resource
 * } finally {
 *     resource.close()
 * }
 * ```
 *
 * Example implementation:
 * ```cangjie
 * class MyResource <: Resource {
 *     private var _closed = false
 *     private let _handle: CPointer<UInt8>
 *
 *     init() {
 *         _handle = unsafe { LibC.malloc<UInt8>(count: 1024) }
 *     }
 *
 *     public func isClosed(): Bool {
 *         _closed
 *     }
 *
 *     public func close(): Unit {
 *         if (_closed) {
 *             return
 *         }
 *         unsafe { LibC.free(_handle) }
 *         _closed = true
 *     }
 * }
 * ```
 */
public interface Resource {
    func isClosed(): Bool
    func close(): Unit
}
