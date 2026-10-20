# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
#
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# Fails if LIB dynamically exports one of the process/console FFI entry points
# that were duplicated across std shared libraries (issue #948). These
# CJ_OS_* / CJ_CONSOLE_* symbols come from the libcangjie-std-processFFI /
# cangjie-std-envFFI archives; env.so, process.so and console.so each embed
# them for internal use only, and libstdFFI.so is the official dlopen entry
# point exporting them. If any std shared library exports them too, the
# dynamic linker may bind another DSO's references to the wrong copy.
#
# Expected -D:
#   LIB  path to the shared library to check
#   NM   optional nm / llvm-nm executable

if(NOT DEFINED LIB OR "${LIB}" STREQUAL "")
    message(FATAL_ERROR "CheckCJExportsHidden: LIB is not set")
endif()
if(NOT EXISTS "${LIB}")
    message(FATAL_ERROR "CheckCJExportsHidden: library not found: ${LIB}")
endif()

set(_nm_candidates)
if(DEFINED NM AND NOT "${NM}" STREQUAL "")
    list(APPEND _nm_candidates "${NM}")
endif()
list(APPEND _nm_candidates nm llvm-nm)

set(_nm_out "")
set(_nm_ok FALSE)
foreach(_nm ${_nm_candidates})
    # Darwin nm: -gU lists global defined symbols (exports).
    execute_process(
        COMMAND ${_nm} -gU "${LIB}"
        OUTPUT_VARIABLE _nm_out
        ERROR_VARIABLE _nm_err
        RESULT_VARIABLE _nm_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_nm_rc EQUAL 0 AND NOT "${_nm_out}" STREQUAL "")
        set(_nm_ok TRUE)
        break()
    endif()
    # ELF nm: -D --defined-only lists dynamic symbol table definitions.
    execute_process(
        COMMAND ${_nm} -D --defined-only "${LIB}"
        OUTPUT_VARIABLE _nm_out
        ERROR_VARIABLE _nm_err
        RESULT_VARIABLE _nm_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_nm_rc EQUAL 0 AND NOT "${_nm_out}" STREQUAL "")
        set(_nm_ok TRUE)
        break()
    endif()
endforeach()

if(NOT _nm_ok)
    execute_process(
        COMMAND readelf --dyn-syms -W "${LIB}"
        OUTPUT_VARIABLE _nm_out
        ERROR_VARIABLE _nm_err
        RESULT_VARIABLE _nm_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _nm_rc EQUAL 0)
        message(FATAL_ERROR
            "CheckCJExportsHidden: cannot read dynamic symbols of ${LIB}: ${_nm_err}")
    endif()
endif()

# The exact set of symbols duplicated across std-env / std-process /
# std-console before the fix. Kept explicit so new CJ_* FFI entries do not
# silently widen this check, and so the regression this guards stays visible.
set(_duplicated_cj_symbols
    CJ_CONSOLE_Flush
    CJ_CONSOLE_Write
    CJ_OS_CloseFile
    CJ_OS_CloseProcessHandle
    CJ_OS_CreateProcessStartInfo
    CJ_OS_FileRead
    CJ_OS_FileWrite
    CJ_OS_FreeProcessInfo
    CJ_OS_FreeProcessRtnData
    CJ_OS_FreeProcessStartInfo
    CJ_OS_GetCurrentPid
    CJ_OS_GetProcessAliveStatus
    CJ_OS_GetProcessHandle
    CJ_OS_GetProcessInfoByPid
    CJ_OS_GetStartTimeFromUnixEpoch
    CJ_OS_GetSystemTime
    CJ_OS_GetUserTime
    CJ_OS_HomeDir
    CJ_OS_StartProcess
    CJ_OS_Terminate
    CJ_OS_WaitSubProcessExit)

# Split nm output into lines; each symbol line ends with the symbol name
# ("... T _CJ_OS_HomeDir" on Mach-O, "... CJ_OS_HomeDir" on ELF).
string(REPLACE "\n" ";" _nm_lines "${_nm_out}")

foreach(_sym ${_duplicated_cj_symbols})
    foreach(_line ${_nm_lines})
        string(STRIP "${_line}" _stripped)
        if(_stripped MATCHES "_?${_sym}$")
            # Skip undefined references (U); only exported definitions
            # ("T"/"S"/"D"/...) are duplicates.
            string(FIND "${_stripped}" " U " _undef_pos)
            if(_undef_pos EQUAL -1)
                message(FATAL_ERROR
                    "CheckCJExportsHidden: ${LIB} exports duplicated "
                    "process/console FFI symbol ${_stripped}. These CJ_OS_* / "
                    "CJ_CONSOLE_* symbols must stay hidden; only libstdFFI "
                    "exports them officially (dlopen entry point). Link the "
                    "FFI archives with --exclude-libs (ELF) or -load_hidden "
                    "(Darwin).")
            endif()
        endif()
    endforeach()
endforeach()
