#include "log.h"

#if !defined(__ANDROID__) && !INTERPRETER_TEST_IOS_LOG
FILE* logFile;
#endif

void initLogs() {
#if !defined(__ANDROID__) && !INTERPRETER_TEST_IOS_LOG
    logFile = fopen("interpreter.mock.log", "w");
    if (logFile == nullptr) {
        logFile = stderr;
    }
    setbuf(logFile, nullptr);
#endif
}
