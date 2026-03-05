# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# MSBarrier — Mark-Sweep barrier implementations
# Step 0: empty shell. Step 3 will add MSIdleBarrier and MSMarkBarrier sources.

set(SRC_LIST)

if (SRC_LIST)
    add_library(MSBarrier STATIC ${SRC_LIST})
endif()
