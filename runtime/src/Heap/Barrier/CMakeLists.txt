# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "Barrier.cpp"
)

# Mark-Sweep barrier sources (Step 3 will add MSIdleBarrier.cpp, MSWriteBarrier.cpp here)
if (MARKSWEEP_FLAG MATCHES 1)
    # list(APPEND SRC_LIST "MSIdleBarrier.cpp")
    # list(APPEND SRC_LIST "MSWriteBarrier.cpp")
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
add_library(Barrier STATIC ${SRC_LIST})

