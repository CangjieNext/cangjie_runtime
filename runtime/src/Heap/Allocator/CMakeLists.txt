# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "Allocator.cpp"
    "MemMap.cpp"
    "CartesianTree.cpp"
)

if (NOT MARKSWEEP_FLAG MATCHES 1)
    list(APPEND SRC_LIST "RegionManager.cpp")
    list(APPEND SRC_LIST "RegionSpace.cpp")
    list(APPEND SRC_LIST "AllocBuffer.cpp")
else()
    # MSGC allocator implementations belong to Allocator as well.  Keeping
    # them in this target ensures both the shared Runtime and the static
    # libcangjie-runtime archive contain the complete Region-scan allocator.
    list(APPEND SRC_LIST
        "../MAllocator/MSBitmapArena.cpp"
        "../MAllocator/MSScanClass.cpp"
        "../MAllocator/MSSizeClass.cpp"
        "../MAllocator/MSFreeManager.cpp"
        "../MAllocator/MSRegionInfo.cpp"
        "../MAllocator/MSRegionManager.cpp"
        "../MAllocator/MSCentralRegionPool.cpp"
        "../MAllocator/MSSweepEpochCoordinator.cpp"
        "../MAllocator/MSAllocBuffer.cpp"
        "../MAllocator/MSRegionSpace.cpp"
    )
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
# The MSGC implementation sources reside in ../MAllocator and include
# AllocUtil.h from this directory.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_library(Allocator STATIC ${SRC_LIST})
