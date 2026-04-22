import std.reflect.*

func validate(cond: Bool, message: String) {
    if (!cond) {
        throw Exception(message)
    }
}

class ReflectBridge {
    public static func entry(): Array<String> {
        try {
            leaf()
            throw Exception("unreachable")
        } catch (e: Exception) {
            let stackTrace = e.getStackTrace()
            validate(stackTrace.size >= 4, "stack trace too short: ${stackTrace.size}")

            println("PRINT_STACK_TRACE_BEGIN")
            e.printStackTrace()
            println("PRINT_STACK_TRACE_END")
            return [
                stackTrace.size.toString(),
                stackTrace[0].methodName,
                stackTrace[1].methodName,
                stackTrace[2].methodName,
                stackTrace[3].methodName,
            ]
        }
    }

    public static func leaf() {
        throw Exception("C2R nested reflect unwind validation")
    }
}

main() {
    let ti = (TypeInfo.of<ReflectBridge>() as ClassTypeInfo).getOrThrow()
    let entry = ti.getStaticFunction("entry")
    let frames = (entry.apply(ti, []) as Array<String>).getOrThrow()
    validate(frames.size >= 5, "unexpected return size: ${frames.size}")

    println("TRACE_LEN=" + frames[0])
    println("TRACE_0=" + frames[1])
    println("TRACE_1=" + frames[2])
    println("TRACE_2=" + frames[3])
    println("TRACE_3=" + frames[4])

    validate(frames[1] == "ReflectBridge::leaf()", "unexpected frame0: ${frames[1]}")
    validate(frames[2] == "ReflectBridge::entry()", "unexpected frame1: ${frames[2]}")
    validate(frames[3].contains("StaticFunctionInfo::apply"), "unexpected frame2: ${frames[3]}")
    validate(frames[4] == "main()", "unexpected frame3: ${frames[4]}")
    println("VALIDATION_OK")
}
