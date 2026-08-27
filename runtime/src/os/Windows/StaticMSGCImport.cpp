// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/MSGCFastPath.h"

namespace MapleRuntime {

#ifdef MRT_USE_MARK_SWEEP
extern "C" {
MSGCFastPathState* __imp_CJ_MRT_MSGCFastPathState =
    &CJ_MRT_MSGCFastPathState;
}
#endif

} // namespace MapleRuntime
