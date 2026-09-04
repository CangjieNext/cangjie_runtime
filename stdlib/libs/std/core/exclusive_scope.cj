/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * The execution of the input function @param fn occupies the underlying OS thread exclusively.
 * When executing @param fn, a switch from the cangjie thread stack to the OS thread stack may occur,
 * and the underlying OS thread cannot be preempted by other cangjie threads.
 * After @param fn returns (or an exception propagates out of @param fn),
 * the runtime restores the previous stack and allows preemption again.
 * Exceptions thrown inside @param fn propagate to the caller of exclusiveScope.
 *
 * @param fn The function to be executed exclusively.
 * @return The return value of @param fn.
 */
@When[env != "ohos"]
public func exclusiveScope<T>(fn: () -> T): T {
    exclusiveScopeImpl(fn)
}

@Intrinsic
func exclusiveScopeImpl<T>(fn: () -> T): T
