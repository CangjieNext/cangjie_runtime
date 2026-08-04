// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_RSET_UPDATE_BARRIER_H
#define MRT_RSET_UPDATE_BARRIER_H

#include "RememberSet.h"

namespace MapleRuntime {

class BaseObject;

class RSetUpdateBarrier {
public:
    RSetUpdateBarrier() : rset_(RememberSet::Instance()) {}
    ~RSetUpdateBarrier() = default;

    void WriteReference(BaseObject* obj, BaseObject* field, BaseObject* target);

private:
    RememberSet& rset_;
};

} // namespace MapleRuntime

#endif // MRT_RSET_UPDATE_BARRIER_H
