/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

#include <stdint.h>

#define UUID_OFFSET (24)
extern int32_t GetUUIDFromTypeInfoPointer(const void *ptr)
{
    return *(int32_t*)(ptr + UUID_OFFSET);
}