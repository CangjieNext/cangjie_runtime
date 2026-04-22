func validate(cond: Bool, message: String) {
    if (!cond) {
        throw Exception(message)
    }
}

func leaf() {
    throw Exception("C2R unwind validation")
}

func mid() {
    leaf()
}

main() {
    try {
        mid()
        throw Exception("unreachable")
    } catch (e: Exception) {
        let stackTrace = e.getStackTrace()
        validate(stackTrace.size >= 3, "stack trace too short: ${stackTrace.size}")

        println("TRACE_LEN=" + stackTrace.size.toString())
        println("TRACE_0=" + stackTrace[0].methodName)
        println("TRACE_1=" + stackTrace[1].methodName)
        println("TRACE_2=" + stackTrace[2].methodName)

        validate(stackTrace[0].methodName == "leaf()", "unexpected frame0: ${stackTrace[0].methodName}")
        validate(stackTrace[1].methodName == "mid()", "unexpected frame1: ${stackTrace[1].methodName}")
        validate(stackTrace[2].methodName == "main()", "unexpected frame2: ${stackTrace[2].methodName}")

        println("PRINT_STACK_TRACE_BEGIN")
        e.printStackTrace()
        println("PRINT_STACK_TRACE_END")
        println("VALIDATION_OK")
    }
}
