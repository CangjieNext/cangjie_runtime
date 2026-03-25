#include "log.h"

#if !defined(__ANDROID__)
FILE* logFile;
#endif

void initLogs() {
#if !defined(__ANDROID__)
    logFile = fopen("interpreter.mock.log", "w");
    if (logFile == nullptr) {
        logFile = stderr;
    }
    setbuf(logFile, nullptr);
#endif
}
