/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * This source file is part of the Cangjie project, licensed under Apache-2.0
 * with Runtime Library Exception.
 *
 * See https://cangjie-lang.cn/pages/LICENSE for license information.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "securec.h"

#ifdef WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <pthread.h>
#endif // WIN32

typedef struct StructureString {
    char* buffer;
    size_t length;
} StructureString;

#define INITIAL_BUFFER_SIZE 512
#define WIN_EOF 0x1A
#define STR_END '\0'

// Line-oriented and byte-array reads share a cache to preserve the order of
// bytes prefetched by either path.
static char g_stdinBuffer[INITIAL_BUFFER_SIZE];
static size_t g_stdinBufferStart = 0;
static size_t g_stdinBufferEnd = 0;
static bool g_stdinTextEof = false;

#ifdef WIN32
static SRWLOCK g_stdinLock = SRWLOCK_INIT;

static void LockStdin(void)
{
    AcquireSRWLockExclusive(&g_stdinLock);
}

static void UnlockStdin(void)
{
    ReleaseSRWLockExclusive(&g_stdinLock);
}
#else
static pthread_mutex_t g_stdinLock = PTHREAD_MUTEX_INITIALIZER;

static void LockStdin(void)
{
    (void)pthread_mutex_lock(&g_stdinLock);
}

static void UnlockStdin(void)
{
    (void)pthread_mutex_unlock(&g_stdinLock);
}
#endif

static int64_t ReadStdinBytes(char* buffer, size_t maxLen)
{
#ifdef WIN32
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdIn == NULL || hStdIn == INVALID_HANDLE_VALUE) {
        return -1;
    }

    DWORD numOfBytesToRead = maxLen > MAXDWORD ? MAXDWORD : (DWORD)maxLen;
    DWORD numOfBytesRead = 0;
    BOOL success = ReadFile(hStdIn, buffer, numOfBytesToRead, &numOfBytesRead, NULL);
    if (!success) {
        DWORD error = GetLastError();
        if (error == ERROR_MORE_DATA) {
            // Message-mode pipes can deliver a valid prefix without completing the message.
            return (int64_t)numOfBytesRead;
        }
        if (error == ERROR_BROKEN_PIPE) {
            return 0;
        }
        return -1;
    }
    return (int64_t)numOfBytesRead;
#else
    // Preserve prompt visibility for the default line-buffered terminal streams.
    // Do not flush fully buffered redirected output as a side effect of input.
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        (void)fflush(stdout);
    }
    ssize_t readSize;
    do {
        readSize = read(STDIN_FILENO, buffer, maxLen);
    } while (readSize < 0 && errno == EINTR);
    return (int64_t)readSize;
#endif
}

static int ReadStdinByte(void)
{
    if (g_stdinBufferStart == g_stdinBufferEnd) {
        int64_t readSize = ReadStdinBytes(g_stdinBuffer, sizeof(g_stdinBuffer));
        if (readSize <= 0) {
            // Preserve stdio EOF persistence for the line-reading path.
            // Transient read failures do not set the persistent EOF state.
            g_stdinTextEof = readSize == 0;
            return EOF;
        }
        g_stdinBufferStart = 0;
        g_stdinBufferEnd = (size_t)readSize;
    }

    return (unsigned char)g_stdinBuffer[g_stdinBufferStart++];
}

static int ReadStdinTextByte(void)
{
    if (g_stdinTextEof) {
        return EOF;
    }
#ifdef WIN32
    // Match the default Windows text-input semantics of getchar.
    // Apply text conversion only to line reads; byte-array reads use raw bytes.
    int c = ReadStdinByte();
    if (c == WIN_EOF) {
        g_stdinTextEof = true;
        return EOF;
    }
    if (c == '\r') {
        int next = ReadStdinByte();
        if (next == '\n') {
            return '\n';
        }
        if (next != EOF) {
            // Restore the lookahead byte to the cache, including after a refill.
            --g_stdinBufferStart;
        }
    }
    return c;
#else
    return ReadStdinByte();
#endif
}

/**
 * @brief Reads a line of text from the console or from redirected stdin until newline or EOF.
 *
 * This function reads input from the console, storing it in a dynamically allocated
 * buffer within a `StructureString` object. If the buffer reaches its initial capacity,
 * it is automatically resized to accommodate additional characters.
 * The function stops reading when it encounters a newline character or the end of the file.
 *
 * @return StructureString*
 * - Returns a pointer to a `StructureString` containing the read input.
 * - If an error occurs (e.g., memory allocation failure) or no data is read, it returns NULL.
 */
static StructureString* ConsoleReadLine(void)
{
    StructureString* str = (StructureString*)malloc(sizeof(StructureString));
    if (str == NULL) {
        return NULL;
    }

    str->buffer = (char*)malloc(INITIAL_BUFFER_SIZE * sizeof(char));
    if (str->buffer == NULL) {
        free(str);
        return NULL;
    }

    size_t bufferSize = INITIAL_BUFFER_SIZE;
    str->length = 0;
    (void)memset_s(str->buffer, bufferSize * sizeof(char), 0, bufferSize * sizeof(char));

    // Read characters from the console until EOF or \n is encountered
    int c;
    while ((c = ReadStdinTextByte()) != EOF) {
        if (str->length >= bufferSize) {
            size_t newBufferSize = bufferSize * 2;
            if (newBufferSize >= SECUREC_MEM_MAX_LEN) {
                break;
            }

            char* newBuffer = (char*)malloc(newBufferSize * sizeof(char));
            if (newBuffer == NULL) {
                str->length = 0; // Length reset to zero indicates an error
                break;
            }

            (void)memset_s(newBuffer, newBufferSize * sizeof(char), 0, newBufferSize * sizeof(char));
            if (memcpy_s(newBuffer, newBufferSize * sizeof(char), str->buffer, str->length * sizeof(char)) != EOK) {
                str->length = 0;
                free(newBuffer);
                break;
            }
            free(str->buffer);

            str->buffer = newBuffer;
            bufferSize = newBufferSize;
        }

        str->buffer[str->length++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    // If no data was read or error happend, clean up and return NULL
    if (str->length == 0) {
        free(str->buffer);
        free(str);
        return NULL;
    }

    return str;
}

extern int64_t CJ_CONSOLE_ReadBytes(char* buffer, size_t maxLen)
{
    if (maxLen == 0) {
        return 0;
    }

    LockStdin();
    // Return cached bytes without additional reads to fill the destination buffer.
    size_t cachedSize = g_stdinBufferEnd - g_stdinBufferStart;
    if (cachedSize > 0) {
        size_t readSize = cachedSize < maxLen ? cachedSize : maxLen;
        if (memcpy_s(buffer, maxLen, g_stdinBuffer + g_stdinBufferStart, readSize) != EOK) {
            UnlockStdin();
            return -1;
        }
        g_stdinBufferStart += readSize;
        if (g_stdinBufferStart == g_stdinBufferEnd) {
            g_stdinBufferStart = 0;
            g_stdinBufferEnd = 0;
        }
        UnlockStdin();
        return (int64_t)readSize;
    }

    int64_t readSize = ReadStdinBytes(buffer, maxLen);
    UnlockStdin();
    return readSize;
}

#ifdef WIN32
static char* Wchar2Char(const wchar_t* wstr, DWORD size, size_t* length)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, size, NULL, 0, NULL, NULL);
    if (len == 0) {
        return NULL;
    }
    *length = len;
    len += 1;
    char* cstr = (char*)malloc(sizeof(char) * len);
    if (cstr == NULL) {
        return NULL;
    }
    (void)memset_s(cstr, len * sizeof(char), 0, len * sizeof(char));

    WideCharToMultiByte(CP_UTF8, 0, wstr, size, cstr, len, NULL, NULL);

    // cstr will be freed on cangjie side
    return cstr;
}

/**
 * @brief Reads wide character input (UTF-16) from the console and converts it to UTF-8.
 *
 * This function reads UTF-16 input from the console using the specified console handle.
 * It supports Windows environments where input is retrieved using `ReadConsoleW`.
 * The read data is stored in a buffer that grows dynamically if needed. Once the data
 * is fully read, it is converted from wide characters (UTF-16) to UTF-8 and stored in
 * a `StructureString` structure.
 *
 * @param hStdin The handle to the console input (stdin).
 *
 * @return StructureString*
 * - Returns a pointer to a `StructureString` containing the converted UTF-8 string.
 * - Returns NULL if an error occurs (e.g., memory allocation failure) or if no data is read.
 */
static StructureString* ConsoleReadWideChars(HANDLE hStdin)
{
    wchar_t* buffer = (wchar_t*)malloc(INITIAL_BUFFER_SIZE * sizeof(wchar_t));
    if (buffer == NULL) {
        return NULL;
    }
    DWORD bufferSize = INITIAL_BUFFER_SIZE;
    (void)memset_s(buffer, bufferSize * sizeof(wchar_t), 0, bufferSize * sizeof(wchar_t));
    DWORD totalReadlen = 0;
    DWORD spareSize = bufferSize;
    DWORD readlen = 0;
    wchar_t* pReadStart = buffer;

    while (ReadConsoleW(hStdin, pReadStart, spareSize, &readlen, NULL)) {
        wchar_t* eof_pos = wcschr(buffer, WIN_EOF);
        if (eof_pos != NULL) {
            *eof_pos = STR_END;
            readlen = eof_pos - buffer;
            totalReadlen += readlen;
            break;
        }
        totalReadlen += readlen;
        if (buffer[totalReadlen - 1] == L'\n') {
            break;
        }

        DWORD newBufferSize = bufferSize * 2; // 2: double the buffer

        if (newBufferSize >= SECUREC_MEM_MAX_LEN) {
            break;
        }
        bufferSize = newBufferSize;
        wchar_t* newbuffer = (wchar_t*)malloc(bufferSize * sizeof(wchar_t));
        if (newbuffer == NULL) {
            break;
        }

        (void)memset_s(newbuffer, bufferSize * sizeof(wchar_t), 0, bufferSize * sizeof(wchar_t));
        if (memcpy_s(newbuffer, bufferSize * sizeof(wchar_t), buffer, totalReadlen * sizeof(wchar_t)) != EOK) {
            free(newbuffer);
            break;
        }

        free(buffer);
        buffer = newbuffer;
        pReadStart = buffer + totalReadlen; // totalReadlen always less then buffer size
        spareSize = bufferSize - totalReadlen;
    }

    if (totalReadlen == 0) {
        free(buffer);
        return NULL;
    }

    size_t resultLength = 0;
    char* result = Wchar2Char(buffer, totalReadlen, &resultLength);
    // Free the buffer
    free(buffer);

    StructureString* str = (StructureString*)malloc(sizeof(StructureString));
    if (str == NULL) {
        free(result);
        return NULL;
    }
    str->buffer = result;
    str->length = resultLength;
    return str; // str and str->buffer will be freed by cangjie
}
#endif

#ifdef WIN32
/**
 * @brief Determines the input method for reading from the console or redirected file input on Windows.
 *
 * This function checks if the input handle (`STD_INPUT_HANDLE`) is associated with a console or
 * a character device. If so, it reads input as wide characters (UTF-16) using `ConsoleReadWideChars`.
 * Otherwise, it uses `ConsoleReadLine` to read from a redirected file (or non-console) input.
 *
 * This approach is typically used to handle both console input and file redirection on Windows platforms.
 *
 * @return StructureString*
 * - Returns a pointer to a `StructureString` containing the read input data in UTF-8.
 * - Returns NULL if an error occurs or no data is read.
 */
extern StructureString* CJ_CONSOLE_Readline(void)
{
    LockStdin();
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    StructureString* result;
    if (GetFileType(hStdIn) == FILE_TYPE_CHAR) {
        // a character file, typically an LPT device or a console
        result = ConsoleReadWideChars(hStdIn);
    } else {
        result = ConsoleReadLine();
    }
    UnlockStdin();
    return result;
}

extern bool CJ_CONSOLE_IsStdinCharDevice(void)
{
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    return hStdIn != NULL && hStdIn != INVALID_HANDLE_VALUE && GetFileType(hStdIn) == FILE_TYPE_CHAR;
}

extern void CJ_CONSOLE_ClearstdInerr(void)
{
    // do nothing in windows system
    return;
}

#else
// not WIN32
extern StructureString* CJ_CONSOLE_Readline(void)
{
    LockStdin();
    StructureString* result = ConsoleReadLine();
    // Callers clear the input state after a null result. Reset the native EOF
    // state here so their FastNative cleanup does not wait on the input lock.
    if (result == NULL) {
        g_stdinTextEof = false;
    }
    UnlockStdin();
    return result;
}

extern void CJ_CONSOLE_ClearstdInerr(void)
{
    clearerr(stdin);
}
#endif // WIN32
