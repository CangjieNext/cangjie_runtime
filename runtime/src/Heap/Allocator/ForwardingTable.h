// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FORWARDING_TABLE_H
#define MRT_FORWARDING_TABLE_H

#include <unordered_map>
#include "Common/BaseObject.h"
namespace MapleRuntime {
using ForwardingHashTable = std::unordered_map<BaseObject*, BaseObject*>;
struct ForwardingTable {
    BaseObject* GetRoute(BaseObject*) const {}
    void SetForwardingAddr(BaseObject*, BaseObject*) {}
    // WIP: This container should be optimized or replaced with runtime implementation.
    ForwardingHashTable hashTable;
};

}
#endif // MRT_FORWARDING_TABLE_H