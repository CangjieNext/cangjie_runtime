// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_QUEUE_POLICY_H
#define MRT_MS_MARK_QUEUE_POLICY_H

namespace MapleRuntime {

inline bool ShouldEnqueueMSMarkedObject(bool newlyMarked, bool hasRefField)
{
    return newlyMarked && hasRefField;
}

} // namespace MapleRuntime

#endif // MRT_MS_MARK_QUEUE_POLICY_H
