# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

add_subdirectory(schdfd)
add_subdirectory(sock)
add_subdirectory(tcpsock)
add_subdirectory(udpsock)
add_subdirectory(rawsock)
set(AIO_OBJECT_TARGETS schdfd sock tcpsock udpsock rawsock)
if(${OS} STREQUAL "linux" OR ${OS} STREQUAL "macos" OR ${OS} STREQUAL "ios")
add_subdirectory(domainsock)
list(APPEND AIO_OBJECT_TARGETS domainsock)
endif()

# OHOS toolchains enable -flto globally, so the compiler emits LLVM bitcode instead of ELF objects.
# These objects are archived into libcangjie-aio.a, which is a released artifact and is
# further merged into libcangjie-std-net.a / libcangjie-std-netFFI.a. A static library has
# no link step (it is only archived by llvm-ar), so the bitcode ends up inside the released
# archives: binutils cannot read such members, and the released SDK becomes pinned to one
# exact LLVM version. This only affects OHOS builds, so apply the workaround there only.
# Note that -fsanitize=cfi requires -flto and implies -fwhole-program-vtables, so the three
# options have to be disabled together. -mbranch-protection is unaffected and still applies.
if(PLATFORM_NAME STREQUAL "ohos_arm64_cangjie" OR PLATFORM_NAME STREQUAL "ohos_arm32_cangjie" OR
    PLATFORM_NAME STREQUAL "ohos_x86_64_cangjie")
    foreach(aio_target IN LISTS AIO_OBJECT_TARGETS)
        target_compile_options(${aio_target} PRIVATE -fno-lto -fno-sanitize=cfi -fno-whole-program-vtables)
    endforeach()
endif()
