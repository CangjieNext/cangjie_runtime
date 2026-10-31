/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */
package std.reflect

import std.collection.*

/**
 * Reflective information about Option<T>, using composed enum metadata.
 */
@When[env != "ohos"]
public class OptionTypeInfo <: TypeInfo {
    let enumInfo: EnumTypeInfo
    private let someConstructor: EnumConstructorInfo
    private let noneConstructor: EnumConstructorInfo
    private let _payloadType: TypeInfo

    private init(enumInfo: EnumTypeInfo, some: EnumConstructorInfo, none: EnumConstructorInfo) {
        super(enumInfo._info)
        this.enumInfo = enumInfo
        someConstructor = some
        noneConstructor = none
        _payloadType = some.parameters[0]
    }

    // Qualified names are already normalized by TypeInfo. Matching the complete
    // name against Some's parameter also checks that there is exactly one T.
    static func fromEnum(enumInfo: EnumTypeInfo, qualifiedName: String): TypeInfo {
        if (!qualifiedName.startsWith("Option<")) {
            return enumInfo
        }
        try {
            if (enumInfo.constructors.size != 2) {
                return enumInfo
            }
            let some = enumInfo.getConstructor("Some", argsCount: 1)
            let none = enumInfo.getConstructor("None")
            if (qualifiedName != "Option<${some.parameters[0].qualifiedName}>") {
                return enumInfo
            }
            OptionTypeInfo(enumInfo, some, none)
        } catch (e: InfoNotFoundException) {
            enumInfo
        }
    }

    /**
     * Returns the cached type information of T in Option<T>.
     */
    public prop payloadType: TypeInfo {
        get() { _payloadType }
    }

    /**
     * Returns Option type information, or throws IllegalTypeException.
     */
    public static redef func of(instance: Any): OptionTypeInfo {
        match (TypeInfo.of(instance)) {
            case oti: OptionTypeInfo => oti
            case other => throw IllegalTypeException(
                "Parameter `instance` must be an Option type instance, but got `${other.qualifiedName}`.")
        }
    }

    /**
     * Returns Option type information, or throws IllegalTypeException.
     */
    public static redef func of<T>(): OptionTypeInfo {
        match (TypeInfo.of<T>()) {
            case oti: OptionTypeInfo => oti
            case other => throw IllegalTypeException(
                "Generic type `T` must be an Option type, but got `${other.qualifiedName}`.")
        }
    }

    /**
     * Finds Option type information by qualified name.
     */
    public static redef func get(qualifiedName: String): OptionTypeInfo {
        match (TypeInfo.get(qualifiedName)) {
            case oti: OptionTypeInfo => oti
            case other => throw IllegalTypeException(
                "Expected Option type `${qualifiedName}`, but got `${other.qualifiedName}`.")
        }
    }

    /**
     * Constructs Some(payload), preserving the reflected Option<T> type.
     */
    public func constructSome(payload: Any): Any {
        let actualType = TypeInfo.of(payload)
        if (!actualType.isSubtypeOf(payloadType)) {
            throw IllegalTypeException(
                "Option payload type mismatch for `${qualifiedName}`: expected `${payloadType.qualifiedName}`, got `${actualType.qualifiedName}`.")
        }
        someConstructor.apply([payload])
    }

    /**
     * Constructs None, preserving the reflected Option<T> type.
     */
    public func constructNone(): Any {
        noneConstructor.apply([])
    }

    /**
     * Returns whether the compatible Option instance is Some.
     */
    public func isSome(instance: Any): Bool {
        associatedValues(instance).size == 1
    }

    /**
     * Returns whether the compatible Option instance is None.
     */
    public func isNone(instance: Any): Bool {
        associatedValues(instance).isEmpty()
    }

    /**
     * Returns Some(payload) or None; rejects incompatible instances and invalid metadata.
     */
    public func getPayload(instance: Any): Option<Any> {
        let values = associatedValues(instance)
        if (values.isEmpty()) {
            return None
        }
        Some(values[0])
    }

    private func associatedValues(instance: Any): ReadOnlyList<Any> {
        let (ctor, values) = enumInfo.destruct(instance)
        if ((ctor == someConstructor && values.size == 1) ||
            (ctor == noneConstructor && values.isEmpty())) {
            return values
        }
        throw ReflectException("Unexpected Option constructor or associated values for `${qualifiedName}`.")
    }
}
