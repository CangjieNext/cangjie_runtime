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

# These objects are archived into libcangjie-aio.a, which is a released artifact and is
# further merged into libcangjie-std-net.a / libcangjie-std-netFFI.a. A static library has
# no link step (it is only archived by llvm-ar), so a toolchain that enables -flto globally
# (the OHOS one does) would leave LLVM bitcode inside those released archives instead of
# object code: binutils cannot read it, and it pins the released SDK to one exact LLVM
# version. Keep the aio objects free of LTO so real object files are always emitted.
# Note that -fsanitize=cfi requires -flto and implies -fwhole-program-vtables, so the three
# options have to be disabled together. -mbranch-protection is unaffected and still applies.
foreach(aio_target IN LISTS AIO_OBJECT_TARGETS)
    target_compile_options(${aio_target} PRIVATE -fno-lto -fno-sanitize=cfi -fno-whole-program-vtables)
endforeach()
