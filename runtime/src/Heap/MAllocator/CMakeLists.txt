# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "MSBitmapArena.cpp"
    "MSSizeClass.cpp"
    "MSFreeManager.cpp"
    "MSRegionInfo.cpp"
    "MSRegionManager.cpp"
    "MSCentralRegionPool.cpp"
    "MSAllocBuffer.cpp"
    "MSLargeObjectSpace.cpp"
    "MSRegionSpace.cpp"
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../Allocator)
add_library(MAllocator STATIC ${SRC_LIST})
