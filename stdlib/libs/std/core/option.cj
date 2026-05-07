/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

package std.core

/**
 * An optional value that either contains a value of type T (Some) or contains nothing (None).
 */
public enum Option<T> {
    Some(T)
    | None

    /**
     * Returns the contained value if this Option is Some, otherwise throws NoneValueException.
     *
     * @throws NoneValueException if the value of Option is None
     */
    @Frozen
    public func getOrThrow(): T {
        match (this) {
            case Some(v) => v
            case None => throw NoneValueException()
        }
    }

    /**
     * Returns the contained value if this Option is Some, otherwise throws the exception
     * returned by the given closure.
     *
     * @param exception a closure that produces the Exception to throw when this Option is None
     * @throws Exception the exception returned by `exception` if the value of Option is None
     */
    @Frozen
    public func getOrThrow(exception: () -> Exception): T {
        match (this) {
            case Some(v) => v
            case None => throw exception()
        }
    }

    /**
     * Returns the contained value if this Option is Some, otherwise returns the value
     * produced by the given closure.
     *
     * @param other a closure that produces the default value when this Option is None
     * @returns T the contained value, or the default value produced by `other`
     */
    @Frozen
    public func getOrDefault(other: () -> T): T {
        match (this) {
            case Some(v) => v
            case None => other()
        }
    }

    /**
     * Returns true if this Option is None.
     *
     * @returns Bool true if this Option is None, false otherwise
     */
    @Frozen
    public func isNone(): Bool {
        match (this) {
            case None => true
            case Some(_) => false
        }
    }

    /**
     * Returns true if this Option is Some.
     *
     * @returns Bool true if this Option is Some, false otherwise
     */
    @Frozen
    public func isSome(): Bool {
        match (this) {
            case Some(_) => true
            case None => false
        }
    }

    /**
     * Transforms the contained value using the given closure if this Option is Some,
     * otherwise returns None.
     *
     * @param transform a closure that transforms the contained value
     * @returns Option<R> Some containing the transformed value if this Option is Some, otherwise None
     */
    public func map<R>(transform: (T) -> R): Option<R> {
        match (this) {
            case Some(v) => Some(transform(v))
            case None => None
        }
    }

    /**
     * Returns this Option if it is Some and the predicate returns true for the contained value,
     * otherwise returns None.
     *
     * @param predicate a closure that tests the contained value
     * @returns Option<T> this Option if the predicate returns true, otherwise None
     */
    public func filter(predicate: (T) -> Bool): Option<T> {
        let v = this ?? return None
        if (predicate(v)) {
            return v
        }
        return None
    }

    /**
     * Transforms the contained value using the given closure that returns an Option,
     * and flattens the result. Returns None if this Option is None.
     *
     * @param transform a closure that transforms the contained value into an Option
     * @returns Option<R> the result of applying the transform if this Option is Some, otherwise None
     */
    public func flatMap<R>(transform: (T) -> Option<R>): Option<R> {
        match (this) {
            case Some(v) => transform(v)
            case None => None
        }
    }
}

/**
 * Executes the given action if the Option is Some.
 *
 * @param o the Option to check
 * @param action the closure to execute with the contained value
 */
public func ifSome<T>(o: Option<T>, action: (T) -> Unit): Unit {
    match (o) {
        case Some(v) => action(v)
        case None => ()
    }
}

/**
 * Executes the given action if the Option is None.
 *
 * @param o the Option to check
 * @param action the closure to execute when the Option is None
 */
public func ifNone<T>(o: Option<T>, action: () -> Unit): Unit {
    match (o) {
        case Some(v) => ()
        case None => action()
    }
}

extend<T> Option<T> <: Equatable<Option<T>> where T <: Equatable<T> {
    @Frozen
    public operator func ==(other: Option<T>): Bool {
        return match ((this, other)) {
            case (Some(v1), Some(v2)) => v1 == v2
            case (None, None) => true
            case _ => false
        }
    }

    @Frozen
    public operator func !=(other: Option<T>): Bool {
        return !(this == other)
    }
}

extend<T> Option<T> <: ToString where T <: ToString {
    public func toString(): String {
        match (this) {
            case Some(s) => "Some(${s})"
            case None => "None"
        }
    }
}

extend<T> Option<T> <: Hashable where T <: Hashable {
    public func hashCode(): Int64 {
        return match (this) {
            case Some(v) => v.hashCode()
            case None => 0
        }
    }
}

extend<T> Option<Option<T>> {
    /**
     * Flattens a nested Option. If this Option is Some(Some(value)), returns Some(value);
     * if this Option is Some(None), returns None; if this Option is None, returns None.
     *
     * @returns Option<T> the flattened Option
     */
    public func flatten(): Option<T> {
        match (this) {
            case Some(o) => o
            case None => None
        }
    }
}
