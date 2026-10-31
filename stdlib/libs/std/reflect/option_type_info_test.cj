/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */
package std.reflect

import std.unittest.*
import std.unittest.testmacro.*

@When[env != "ohos"]
enum OptionLookalike<T> {
    LookalikeSome(T) | LookalikeNone
}

@When[env != "ohos"]
@Test
func optionRejectsNonOptionTypes() {
    @Assert(TypeInfo.of<OptionLookalike<Int64>>() is EnumTypeInfo)
    @AssertThrows[IllegalTypeException](OptionTypeInfo.of<OptionLookalike<Int64>>())
    @AssertThrows[IllegalTypeException](OptionTypeInfo.of(42i64))
    @AssertThrows[IllegalTypeException](OptionTypeInfo.get("Int64"))
}

@When[env != "ohos"]
@Test
func optionRequiresConstructorMetadata() {
    let enumInfo = EnumTypeInfo.of<Option<Int64>>()
    if (enumInfo.constructors.isEmpty()) {
        @Assert(TypeInfo.of<Option<Int64>>() is EnumTypeInfo)
        @AssertThrows[IllegalTypeException](OptionTypeInfo.of<Option<Int64>>())
        @AssertThrows[IllegalTypeException](OptionTypeInfo.of(Option<Int64>.Some(1)))
        @AssertThrows[IllegalTypeException](OptionTypeInfo.get("Option<Int64>"))
    } else {
        @Assert(TypeInfo.of<Option<Int64>>() is OptionTypeInfo)
    }
}

@When[env != "ohos"]
@Test
func optionConstructsTypedValues() {
    let info = OptionTypeInfo.of<Option<Int64>>()
    let some = info.constructSome(42i64)
    let none = info.constructNone()
    @Assert((some as Option<Int64>).getOrThrow(), Option<Int64>.Some(42i64))
    @Assert((none as Option<Int64>).getOrThrow(), Option<Int64>.None)
    @Assert(info.isSome(some))
    @Assert(!info.isNone(some))
    @Assert(info.isNone(none))
    @Assert(!info.isSome(none))
    @Assert((info.getPayload(some).getOrThrow() as Int64).getOrThrow(), 42i64)
    @Assert(info.getPayload(none).isNone())
    @AssertThrows[IllegalTypeException](info.constructSome("wrong"))
    @AssertThrows[IllegalTypeException](info.getPayload(Option<String>.Some("wrong")))
    @AssertThrows[IllegalTypeException](info.isSome(42i64))
    @AssertThrows[IllegalTypeException](info.isNone(Option<String>.None))
}

@When[env != "ohos"]
@Test
func optionConstructsReferencePayload() {
    let info = OptionTypeInfo.get("std.core.Option<String>")
    let some = info.constructSome("payload")
    @Assert((some as Option<String>).getOrThrow(), Option<String>.Some("payload"))
    @Assert((info.getPayload(some).getOrThrow() as String).getOrThrow(), "payload")
    @Assert(info.isNone(info.constructNone()))
}

@When[env != "ohos"]
@Test
func optionPreservesNestedNonePayload() {
    let info = OptionTypeInfo.of<Option<Option<String>>>()
    let inner = Option<String>.None
    let outer = info.constructSome(inner)
    let payload = info.getPayload(outer)
    @Assert(payload.isSome())
    @Assert((payload.getOrThrow() as Option<String>).getOrThrow(), inner)
    @Assert(info.payloadType, TypeInfo.of<Option<String>>())
    @Assert((info.constructNone() as Option<Option<String>>).getOrThrow().isNone())
}

@When[env != "ohos"]
@Test
func optionNoneHasNoAssociatedValues() {
    let valueInfo = EnumTypeInfo.of<Option<Int64>>()
    let referenceInfo = EnumTypeInfo.of<Option<String>>()
    for (info in [valueInfo, referenceInfo]) {
        let none = info.getConstructor("None")
        @Assert(none.parameters.isEmpty())
        let instance = none.apply([])
        let (ctor, values) = info.destruct(instance)
        @Assert(ctor, none)
        @Assert(values.isEmpty())
        @Assert(OptionTypeInfo.of(instance).isNone(instance))
    }
    @Assert((valueInfo.construct("None", []) as Option<Int64>).getOrThrow().isNone())
    @Assert((referenceInfo.construct("None", []) as Option<String>).getOrThrow().isNone())
}

@When[env != "ohos"]
@Test
func optionRetainsComposedEnumView() {
    let info = OptionTypeInfo.of<Option<Int64>>()
    let typeInfo: TypeInfo = info
    let some = info.constructSome(7i64)
    let enumView = EnumTypeInfo.of(some)
    @Assert(!(typeInfo is EnumTypeInfo))
    @Assert(OptionTypeInfo.of(some), info)
    @Assert(OptionTypeInfo.get(info.qualifiedName), info)
    @Assert(OptionTypeInfo.get("std.core.Option<Int64>"), info)
    @Assert(info.payloadType, TypeInfo.of<Int64>())
    @Assert(enumView, EnumTypeInfo.of<Option<Int64>>())
    @Assert(enumView, EnumTypeInfo.get(info.qualifiedName))
    let ctor = enumView.getConstructor("Some", argsCount: 1)
    @Assert(ctor.enumTypeInfo, enumView)
    @Assert((ctor.getAssociatedValues(some)[0] as Int64).getOrThrow(), 7i64)
    @Assert((enumView.construct("Some(Int64)", [9i64]) as Option<Int64>).getOrThrow(), Option<Int64>.Some(9i64))
}
