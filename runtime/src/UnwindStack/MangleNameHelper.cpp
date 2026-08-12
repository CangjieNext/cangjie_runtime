// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "MangleNameHelper.h"

#include <cctype>

#include "Utils/Demangler.h"

namespace MapleRuntime {
namespace {
// 判断 name 是否是合法的仓颉 mangling 名字。
// 仓颉 mangling 名字以 "_C" 开头（MANGLE_CANGJIE_PREFIX），或属于已知特殊前缀：
//   - "_CV"  wrapped function
//   - "$lambda."  lambda 表达式
//   - 含 "$real" 后缀  C 函数包装（cfuncInCJ$real）
// 非此类前缀的名字（如运行时入口符号 user.main/cj_entry$/rt$、混淆后的单字符符号等）
// 不是合法 mangling 名字，demangler 无法解析，应直接保留原样。
bool IsCangjieManglingName(const CString& name)
{
    if (name.IsEmpty()) {
        return false;
    }
    // 标准仓颉 mangling 前缀 "_C"
    if (name.Find("_C") == 0) {
        return true;
    }
    // wrapped function 前缀 "_CV"（已被上面的 "_C" 覆盖，这里显式列出以示清晰）
    if (name.Find("_CV") == 0) {
        return true;
    }
    // lambda 前缀 "$lambda."
    if (name.Find("$lambda.") != -1) {
        return true;
    }
    // C 函数包装后缀 "$real"
    if (name.Find("$real") != -1) {
        return true;
    }
    return false;
}
} // namespace

CString MangleNameHelper::RemoveGenericTypeName(const MapleRuntime::CString& rawName)
{
    MapleRuntime::CString name;
    int preBracketNum = 0;
    for (size_t i = 0; i < rawName.Length(); ++i) {
        if (preBracketNum == 0) {
            if (rawName[i] == '<') {
                ++preBracketNum;
            }
            name.Append(CString(rawName[i]));
        } else {
            if (rawName[i] == '<') {
                ++preBracketNum;
            } else if (rawName[i] == '>' && --preBracketNum == 0) {
                name.Append("...>");
            }
        }
    }
    return name;
}

void MangleNameHelper::Demangle()
{
    std::function<MapleRuntime::CString(const MapleRuntime::CString&)> genericTypefilter;
    if (stackTraceFormat == StackTraceFormatFlag::ALL) {
        genericTypefilter = [](const MapleRuntime::CString& str) { return str; };
    } else {
        genericTypefilter = [](const MapleRuntime::CString& str) { return RemoveGenericTypeName(str); };
    }
    const char POSTFIX_WITHOUT_TI[] = "$withoutTI";
    if (mangleName.EndsWith(POSTFIX_WITHOUT_TI)) {
        mangleName.Truncate(mangleName.Length() - strlen(POSTFIX_WITHOUT_TI));
    }
    // 仓颉 mangling 名字以 "_C" 开头（MANGLE_CANGJIE_PREFIX），或属于已知特殊前缀
    // （$lambda. / cfuncInCJ$real / _CV）。非此类前缀的名字（如运行时入口符号
    // user.main/cj_entry$/rt$、混淆后的单字符符号等）不是合法 mangling 名字，
    // 直接保留原样，避免 demangler 解析失败导致栈帧信息丢失或打印噪声错误日志。
    if (!IsCangjieManglingName(mangleName)) {
        methodName = mangleName;
        className = "";
        demangleName = mangleName;
        return;
    }
    auto demangler = CreateNativeDemangler(mangleName.Str(), "::", genericTypefilter);
    auto functionDemangleInfo = demangler.Demangle();
    if (!functionDemangleInfo.IsValid()) {
        methodName = mangleName;
        className = "";
        demangleName = mangleName;
        return;
    }
    methodName = functionDemangleInfo.GetFullName(demangler.ScopeResolution());
    className = functionDemangleInfo.GetPkgName();
    demangleName = className + (className.Length() > 0 ? "." : "") + methodName;
}

CString MangleNameHelper::GetSimpleClassName() const
{
    auto demangler = CreateNativeDemangler(mangleName.Str());
    return demangler.DemangleClassType();
}

bool MangleNameHelper::IsNeedFilt() const
{
    if (mangleName.IsEmpty()) {
        return false;
    }

    for (const auto& filtMangleName : filtMangleNameSet) {
        if (mangleName.Find(filtMangleName.Str()) == 0) {
            return true;
        }
    }
    return false;
}
} // namespace MapleRuntime
