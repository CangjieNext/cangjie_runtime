// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RegionFreeSlot.h"

namespace MapleRuntime {
std::array<uint8_t, RegionFreeSlot::SMALL_SIZE_THRESHOLD + 1> RegionFreeSlot::smallSizeToSlotIdx {};
}