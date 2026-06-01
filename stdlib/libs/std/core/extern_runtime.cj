package std.core

// MemberAccessException thrown if an attempt is made to access or update a non-existing member
public class MemberAccessException <: Exception {
    public MemberAccessException(message: String) {
        super(message)
    }
}

// FunctionAccessException thrown if an attempt is made to access a non-existing function
public class FunctionAccessException <: Exception {
    public FunctionAccessException(message: String) {
        super(message)
    }
}

// FunctionCallException thrown if an attempt is made to call a function with the wrong arguments
public class FunctionCallException <: Exception {
    public FunctionCallException(message: String) {
        super(message)
    }
}

// IndexAccessException thrown if an attempt is made to access or update with the wrong index, including out-of-bounds errors
public class IndexAccessException <: Exception {
    public IndexAccessException(message: String) {
        super(message)
    }
}

// ExternDynamicException thrown if a function in the foreign runtime throws a runtime exception
public class ExternDynamicException <: Exception {
    public ExternDynamicException(message: String) {
        super(message)
    }
}

// ExternConversionException thrown if data cannot be converted to or from an Extern type as required.
public class ExternConversionException <: Exception {
    public ExternConversionException(message: String) {
        super(message)
    }
}

public interface Runtime<T> where T <: Runtime<T> {
    static func memberAccess(e: Extern<T>, field: String): Extern<T>
    static func indexAccess(e: Extern<T>, arg: Any): Extern<T>

    static func memberUpdate(e: Extern<T>, field: String, value: Any) : Unit
    static func indexUpdate(e: Extern<T>, field: Any, value: Any): Unit

    static func functionCall(e: Extern<T>, args: Array<Any>): Extern<T>

    static func fromExtern<R>(h: Extern<T>): R
    static func toExtern<R>(v: R): Extern<T>
}

public struct Extern<T> where T <: Runtime<T> {
    private let payload: Any
    public Extern(payload: Any) {
        // Note: this is a normal field assignment and not a dynamic member update. Compiler
        // treats this as a special case and doesn't desugar it.
        this.payload = payload
    }
}

@Intrinsic
public func getPayload<T>(e: Extern<T>): Any where T<: Runtime<T>