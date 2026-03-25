#ifndef LOG_H
#define LOG_H

#include <stdio.h>

extern FILE* logFile;

void initLogs(); 

#define log(fmt, ...) \
    fprintf(logFile, fmt "\n", ##__VA_ARGS__)

#endif