# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(SRC_LIST
    "Allocator.cpp"
    "MemMap.cpp"
    "RegionSpace.cpp"
    "RegionManager.cpp"
    "CartesianTree.cpp"
)

if (MARKSWEEP_FLAG MATCHES 1)
    list(APPEND SRC_LIST
        "SizeClass.cpp"
        "MSRegionInfo.cpp"
        "MSRegionManager.cpp"
        "CentralRegionPool.cpp"
        "AllocBufferMS.cpp"
        "LargeObjectSpace.cpp"
    )
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../)
add_library(Allocator STATIC ${SRC_LIST})
