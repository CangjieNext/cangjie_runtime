#!/bin/bash

set -eo pipefail

if [[ ! -v CJNATIVE_TOOLCHAIN ]]; then
    echo "Error: variable CJNATIVE_TOOLCHAIN is unset."
    exit 1
fi
source "$CJNATIVE_TOOLCHAIN/envsetup.sh"

if [[ ! -v ANDROID_NDK_ROOT ]]; then
    echo "Error: variable ANDROID_NDK_ROOT is unset."
    echo "Set ANDROID_NDK_ROOT in env.sh first."
    exit 1
fi

if [ -z "$1" ]; then
    echo "Error: no test provided."
    echo "Usage: $0 <test_name>"
    exit 1
fi
TEST_NAME="$1"

ANDROID_API_LEVEL="${ANDROID_API_LEVEL:-31}"
ANDROID_TARGET_ARCH="${ANDROID_TARGET_ARCH:-aarch64}"

HOST_OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
HOST_ARCH="$(uname -m)"
HOST_TAG="${HOST_OS}-${HOST_ARCH}"
NDK_BIN="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG/bin"
if [ ! -d "$NDK_BIN" ]; then
    echo "Error: NDK toolchain bin directory not found: $NDK_BIN"
    exit 1
fi

if [ "$ANDROID_TARGET_ARCH" = "aarch64" ] || [ "$ANDROID_TARGET_ARCH" = "arm64" ]; then
    TARGET_TRIPLE="aarch64-linux-android"
    ASM_DIR="shared/asm/arm64"
    ARCH_INCLUDE="-I../../../src/CJThread/src/runtime/schedule/include/inner/gas/arm/arm64/"
    ANDROID_CC="${CC:-$NDK_BIN/${TARGET_TRIPLE}${ANDROID_API_LEVEL}-clang}"
    ANDROID_CXX="${CXX:-$NDK_BIN/${TARGET_TRIPLE}${ANDROID_API_LEVEL}-clang++}"
elif [ "$ANDROID_TARGET_ARCH" = "x86_64" ]; then
    TARGET_TRIPLE="x86_64-linux-android"
    ASM_DIR="shared/asm/amd64"
    ARCH_INCLUDE="-I../../../src/CJThread/src/runtime/schedule/include/inner/gas/x86/x86_64/"
    ANDROID_CC="${CC:-$NDK_BIN/${TARGET_TRIPLE}${ANDROID_API_LEVEL}-clang}"
    ANDROID_CXX="${CXX:-$NDK_BIN/${TARGET_TRIPLE}${ANDROID_API_LEVEL}-clang++}"
else
    echo "Error: unsupported ANDROID_TARGET_ARCH: $ANDROID_TARGET_ARCH"
    echo "Supported values: aarch64, x86_64"
    exit 1
fi

if [ ! -x "$ANDROID_CC" ]; then
    echo "Error: Android C compiler is not executable: $ANDROID_CC"
    exit 1
fi
if [ ! -x "$ANDROID_CXX" ]; then
    echo "Error: Android C++ compiler is not executable: $ANDROID_CXX"
    exit 1
fi

rm -rf "$TEST_NAME/out"
mkdir -p "$TEST_NAME/out"

"$ANDROID_CC" -shared -o "$TEST_NAME/out/libforeign.so" shared/foreign.c -fPIC

LIB="$TEST_NAME/lib"
rm -rf "$LIB"
mkdir -p "$LIB"

if [ "$ANDROID_TARGET_ARCH" = "aarch64" ] || [ "$ANDROID_TARGET_ARCH" = "arm64" ]; then
    "$ANDROID_CC" -x assembler-with-cpp -c "$ASM_DIR/C2I_Stub.S"          -o "$LIB/C2I_Stub.o"
    "$ANDROID_CC" -x assembler-with-cpp -c "$ASM_DIR/I2C_Stub.S"          -o "$LIB/I2C_Stub.o"
    "$ANDROID_CC" -x assembler-with-cpp -c "$ASM_DIR/I2I_Stub.S"          -o "$LIB/I2I_Stub.o"
    "$ANDROID_CC" -x assembler-with-cpp -c "$ASM_DIR/ExceptionHandling.S" -o "$LIB/ExceptionHandling.o"
elif [ "$ANDROID_TARGET_ARCH" = "x86_64" ]; then
    if ! command -v nasm >/dev/null 2>&1; then
        echo "Error: nasm is required for x86_64 interpreter asm build."
        exit 1
    fi
    nasm -f elf64 "$ASM_DIR/C2I_Stub.s"           -o "$LIB/C2I_Stub.o"
    nasm -f elf64 "$ASM_DIR/I2C_Stub.s"           -o "$LIB/I2C_Stub.o"
    nasm -f elf64 "$ASM_DIR/I2I_Stub.s"           -o "$LIB/I2I_Stub.o"
    nasm -f elf64 "$ASM_DIR/ExceptionHandling.s"  -o "$LIB/ExceptionHandling.o"
fi

CJTHREAD_INCLUDES=$(find ../../../src/CJThread/src -type d -name "include" | sed 's/^/-I/' | tr '\n' ' ')
INCLUDE_DIRS="-I../.. -I.. -I../../.. -I$TEST_NAME -Ishared -Ishared/third_party -I../../../include $CJTHREAD_INCLUDES $ARCH_INCLUDE"

for file in shared/*.cpp; do
    "$ANDROID_CXX" -fPIC $INCLUDE_DIRS -c "$file" -o "${file%.cpp}.o" -Wno-attributes -g
done
mv shared/*.o "$LIB"

for file in "$TEST_NAME"/*.cpp; do
    "$ANDROID_CXX" -fPIC $INCLUDE_DIRS -c "$file" -o "${file%.cpp}.o" -Wno-attributes -g
done
mv "$TEST_NAME"/*.o "$LIB"

ANDROID_LINK_FLAGS="-Wl,-z,max-page-size=16384 -landroid -llog"
"$ANDROID_CXX" -shared -o "$TEST_NAME/out/libinterpreter.so" "$LIB"/*.o -Wno-attributes -g -fPIC $ANDROID_LINK_FLAGS
exit 0
