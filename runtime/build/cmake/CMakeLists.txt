# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(SECURE_CFLAG_FOR_SHARED_LIBRARY "-fstack-protector-all")

if (WINDOWS_FLAG MATCHES 1)
    set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
    set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
    set(WARNING_FLAGS "-w")
    set(SECURE_CFLAG_FOR_SHARED_LIBRARY "-static ${SECURE_CFLAG_FOR_SHARED_LIBRARY} -Wl,--exclude-libs,libssp.a")
endif()

set(STRIP_FLAG "-s")

set(CMAKE_C_FLAGS "${SECURE_CFLAG_FOR_SHARED_LIBRARY} ${WARNING_FLAGS}")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g")
set(CMAKE_C_FLAGS_RELEASE "-D_FORTIFY_SOURCE=2 -O2")
set(CMAKE_C_FLAGS_DEBUG "-O0 -g")
 
if (WINDOWS_FLAG MATCHES 1)
    # MSVCRT already provides most of secure functions,
    # so only files listed below need to be compiled from boundscheck library,
    # to avoid "multiple definition" error.
    # Refer to offiical manual of boundscheck library for more information.
    list(APPEND SRC_FILE src/memset_s.c src/snprintf_s.c src/vsnprintf_s.c src/secureprintoutput_a.c)
else()
    aux_source_directory(./src SRC_FILE)
endif()

if (OHOS_FLAG MATCHES 1) 
    add_compile_options("--target=aarch64-linux-ohos")
    add_link_options("--target=aarch64-linux-ohos")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --sysroot=${OHOS_ROOT}/out/${OHOS_SDK_OUT_PATH}/obj/third_party/musl")
elseif (OHOS_FLAG MATCHES 2)
    add_compile_options("--target=x86_64-linux-ohos")
    add_link_options("--target=x86_64-linux-ohos")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --sysroot=${OHOS_ROOT}/out/${OHOS_SDK_OUT_PATH}/obj/third_party/musl")
endif()

# Build shared library.
add_library(boundscheck SHARED ${SRC_FILE})
target_include_directories(boundscheck PRIVATE ./include)
target_link_options(boundscheck PRIVATE ${SECURE_LDFLAG_FOR_SHARED_LIBRARY} ${STRIP_FLAG})