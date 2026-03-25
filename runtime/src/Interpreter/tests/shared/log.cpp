#include "log.h"

FILE* logFile;

void initLogs() {
    logFile= fopen("interpreter.mock.log", "w");
    setbuf(logFile, NULL);
}