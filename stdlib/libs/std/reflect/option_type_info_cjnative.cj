/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */
package std.reflect

/**
 * Reflective information about Option<T> types.
 *
 * Option is represented by enum metadata at runtime. This type keeps the enum
 * capabilities from EnumTypeInfo and exposes Option-specific helpers so callers
 * do not need to depend on Option's constructor layout.
 */
@When[env != "ohos"]
public class OptionTypeInfo <: EnumTypeInfo {
    var _valueType: ?TypeInfo = None

    init(cp: CPointer<Unit>) {
        super(cp)
    }

    /**
     * Returns the type info of Option<T>'s T.
     */
    public prop valueType: TypeInfo {
        get() {
            match (_valueType) {
                case Some(res) => res
                case None =>
                    let typeArgs = getTypeArgs(_info)
                    if (typeArgs.isNull()) {
                        throw InfoNotFoundException("Option value type for `${qualifiedName}` not found.")
                    }
                    let valueTypePointer = unsafe { CPointer<CPointer<Unit>>(typeArgs).read() }
                    if (valueTypePointer.isNull()) {
                        throw InfoNotFoundException("Option value type for `${qualifiedName}` not found.")
                    }
                    let res = TypeInfo.getOrCreate(valueTypePointer)
                    _valueType = res
                    res
            }
        }
    }

    /**
     * Returns the OptionTypeInfo of the runtime type of the given Option instance.
     */
    public static redef func of(instance: Any): OptionTypeInfo {
        match (TypeInfo.of(instance)) {
            case oti: OptionTypeInfo => oti
            case other => throw IllegalTypeException(
                "Parameter `instance` must be an Option type instance, but got `${other.qualifiedName}`.")
        }
    }

    /**
     * Returns the OptionTypeInfo of Option type T.
     */
    public static redef func of<T>(): OptionTypeInfo {
        match (TypeInfo.of<T>()) {
            case oti: OptionTypeInfo => oti
            case other => throw IllegalTypeException(
                "Generic type `T` must be an Option type, but got `${other.qualifiedName}`.")
        }
    }

    /**
     * Returns the OptionTypeInfo for a qualified Option type name.
     */
    public static redef func get(qualifiedName: String): OptionTypeInfo {
        match (TypeInfo.get(qualifiedName)) {
            case oti: OptionTypeInfo => oti
            case other => throw IllegalTypeException(
                "Expected Option type `${qualifiedName}`, but got `${other.qualifiedName}`.")
        }
    }

    /**
     * Constructs Some(value) for this Option<T>.
     */
    public func constructSome(value: Any): Any {
        let actualType = TypeInfo.of(value)
        if (!actualType.isSubtypeOf(valueType)) {
            throw IllegalTypeException(
                "Option Some value type mismatch for ${qualifiedName}: expected ${valueType.qualifiedName}, got ${actualType.qualifiedName}.")
        }
        getConstructor("Some", argsCount: 1).apply(Array<Any>(1, repeat: value))
    }

    /**
     * Constructs None for this Option<T>.
     */
    public func constructNone(): Any {
        getConstructor("None").apply(Array<Any>())
    }

    /**
     * Returns true when the instance is Some(_).
     */
    public func isSome(instance: Any): Bool {
        constructorOf(instance).name == "Some"
    }

    /**
     * Returns true when the instance is None.
     */
    public func isNone(instance: Any): Bool {
        constructorOf(instance).name == "None"
    }

    /**
     * Destructs Option<T> into Option<Any>.
     */
    public func getValue(instance: Any): Option<Any> {
        let (ctor, values) = destruct(instance)
        if (ctor.name == "None") {
            return Option<Any>.None
        }
        if (values.size != 1) {
            throw ReflectException(
                "Unexpected Option constructor `${ctor.name}` value count: expected 1, got ${values.size}.")
        }
        Option<Any>.Some(values[0])
    }

    private func constructorOf(instance: Any): EnumConstructorInfo {
        let (ctor, _) = destruct(instance)
        ctor
    }
}
