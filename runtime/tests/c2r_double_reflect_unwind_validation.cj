import std.reflect.*

func validate(cond: Bool, message: String) {
    if (!cond) {
        throw Exception(message)
    }
}

class NestedReflectInner {
    public static func entry(): Array<String> {
        try {
            leaf()
            throw Exception("unreachable")
        } catch (e: Exception) {
            let stackTrace = e.getStackTrace()
            validate(stackTrace.size >= 6, "stack trace too short: ${stackTrace.size}")

            println("PRINT_STACK_TRACE_BEGIN")
            e.printStackTrace()
            println("PRINT_STACK_TRACE_END")
            return [
                stackTrace.size.toString(),
                stackTrace[0].methodName,
                stackTrace[1].methodName,
                stackTrace[2].methodName,
                stackTrace[3].methodName,
                stackTrace[4].methodName,
                stackTrace[5].methodName,
            ]
        }
    }

    public static func leaf() {
        throw Exception("C2R double nested reflect unwind validation")
    }
}

class NestedReflectOuter {
    public static func entry(): Array<String> {
        let ti = (TypeInfo.of<NestedReflectInner>() as ClassTypeInfo).getOrThrow()
        let entry = ti.getStaticFunction("entry")
        return (entry.apply(ti, []) as Array<String>).getOrThrow()
    }
}

main() {
    let ti = (TypeInfo.of<NestedReflectOuter>() as ClassTypeInfo).getOrThrow()
    let entry = ti.getStaticFunction("entry")
    let frames = (entry.apply(ti, []) as Array<String>).getOrThrow()
    validate(frames.size >= 7, "unexpected return size: ${frames.size}")

    println("TRACE_LEN=" + frames[0])
    println("TRACE_0=" + frames[1])
    println("TRACE_1=" + frames[2])
    println("TRACE_2=" + frames[3])
    println("TRACE_3=" + frames[4])
    println("TRACE_4=" + frames[5])
    println("TRACE_5=" + frames[6])

    validate(frames[1] == "NestedReflectInner::leaf()", "unexpected frame0: ${frames[1]}")
    validate(frames[2] == "NestedReflectInner::entry()", "unexpected frame1: ${frames[2]}")
    validate(frames[3].contains("StaticFunctionInfo::apply"), "unexpected frame2: ${frames[3]}")
    validate(frames[4] == "NestedReflectOuter::entry()", "unexpected frame3: ${frames[4]}")
    validate(frames[5].contains("StaticFunctionInfo::apply"), "unexpected frame4: ${frames[5]}")
    validate(frames[6] == "main()", "unexpected frame5: ${frames[6]}")
    println("VALIDATION_OK")
}
