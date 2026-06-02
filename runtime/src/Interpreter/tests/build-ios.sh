#!/bin/bash

set -eo pipefail

if [ -z "${CJNATIVE_TOOLCHAIN+x}" ]; then
    echo "Error: variable CJNATIVE_TOOLCHAIN is unset."
    exit 1
fi
source "$CJNATIVE_TOOLCHAIN/envsetup.sh"

if [ -z "$1" ]; then
    echo "Error: no test provided."
    echo "Usage: $0 <test_name>"
    exit 1
fi
TEST_NAME="$1"

IOS_TARGET_ARCH="${IOS_TARGET_ARCH:-aarch64}"
IOS_MIN_VERSION="${IOS_MIN_VERSION:-11.0}"
IOS_PLATFORM="${IOS_PLATFORM:-}"
IOS_CC="${CC:-clang}"
IOS_CXX="${CXX:-clang++}"

if [ -z "$IOS_PLATFORM" ] && [ -n "$IOS_SYSROOT" ]; then
    case "$IOS_SYSROOT" in
        *iPhoneSimulator*|*iphonesimulator*)
            IOS_PLATFORM="simulator"
            ;;
        *iPhoneOS*|*iphoneos*)
            IOS_PLATFORM="device"
            ;;
    esac
fi

if [ -z "$IOS_PLATFORM" ]; then
    if [ "$IOS_TARGET_ARCH" = "x86_64" ]; then
        IOS_PLATFORM="simulator"
    else
        IOS_PLATFORM="device"
    fi
fi

case "$IOS_PLATFORM" in
    device|iphoneos)
        IOS_PLATFORM="device"
        IOS_SDK="iphoneos"
        ;;
    simulator|iphonesimulator|sim)
        IOS_PLATFORM="simulator"
        IOS_SDK="iphonesimulator"
        ;;
    *)
        echo "Error: unsupported IOS_PLATFORM: $IOS_PLATFORM"
        echo "Supported values: device, simulator"
        exit 1
        ;;
esac

if [ -z "$IOS_SYSROOT" ] && command -v xcrun >/dev/null 2>&1; then
    IOS_SYSROOT="$(xcrun --sdk "$IOS_SDK" --show-sdk-path)"
fi

if [ "$IOS_TARGET_ARCH" = "aarch64" ] || [ "$IOS_TARGET_ARCH" = "arm64" ]; then
    if [ "$IOS_PLATFORM" = "simulator" ]; then
        TARGET_TRIPLE="${IOS_TARGET_TRIPLE:-arm64-apple-ios${IOS_MIN_VERSION}-simulator}"
    else
        TARGET_TRIPLE="${IOS_TARGET_TRIPLE:-arm64-apple-ios${IOS_MIN_VERSION}}"
    fi
    ASM_DIR="shared/asm/arm64_ios"
    ARCH_INCLUDE="-I../../../src/CJThread/src/runtime/schedule/include/inner/gas/arm/arm64/"
elif [ "$IOS_TARGET_ARCH" = "x86_64" ]; then
    if [ "$IOS_PLATFORM" != "simulator" ]; then
        echo "Error: x86_64 is supported only for iOS simulator builds."
        exit 1
    fi
    TARGET_TRIPLE="${IOS_TARGET_TRIPLE:-x86_64-apple-ios${IOS_MIN_VERSION}-simulator}"
    ASM_DIR="shared/asm/amd64_ios"
    ARCH_INCLUDE="-I../../../src/CJThread/src/runtime/schedule/include/inner/gas/x86/x86_64/"
else
    echo "Error: unsupported IOS_TARGET_ARCH: $IOS_TARGET_ARCH"
    echo "Supported values: aarch64, arm64, x86_64"
    exit 1
fi

TARGET_FLAGS=(-target "$TARGET_TRIPLE")
if [ -n "$IOS_SYSROOT" ]; then
    TARGET_FLAGS+=(-isysroot "$IOS_SYSROOT")
fi

echo "iOS build configuration:"
echo "  platform: $IOS_PLATFORM"
echo "  arch: $IOS_TARGET_ARCH"
echo "  target: $TARGET_TRIPLE"
echo "  sdk: $IOS_SDK"
echo "  sysroot: ${IOS_SYSROOT:-<default>}"

rm -rf "$TEST_NAME/out"
mkdir -p "$TEST_NAME/out"

"$IOS_CC" "${TARGET_FLAGS[@]}" -dynamiclib -o "$TEST_NAME/out/libforeign.dylib" shared/foreign.c -fPIC $IOS_CFLAGS

LIB="$TEST_NAME/lib"
rm -rf "$LIB"
mkdir -p "$LIB"

if [ "$IOS_TARGET_ARCH" = "aarch64" ] || [ "$IOS_TARGET_ARCH" = "arm64" ]; then
    "$IOS_CC" "${TARGET_FLAGS[@]}" -x assembler-with-cpp -c "$ASM_DIR/C2I_Stub.S"          -o "$LIB/C2I_Stub.o"
    "$IOS_CC" "${TARGET_FLAGS[@]}" -x assembler-with-cpp -c "$ASM_DIR/I2C_Stub.S"          -o "$LIB/I2C_Stub.o"
    "$IOS_CC" "${TARGET_FLAGS[@]}" -x assembler-with-cpp -c "$ASM_DIR/I2I_Stub.S"          -o "$LIB/I2I_Stub.o"
    "$IOS_CC" "${TARGET_FLAGS[@]}" -x assembler-with-cpp -c "$ASM_DIR/ExceptionHandling.S" -o "$LIB/ExceptionHandling.o"
elif [ "$IOS_TARGET_ARCH" = "x86_64" ]; then
    if ! command -v nasm >/dev/null 2>&1; then
        echo "Error: nasm is required for x86_64 iOS interpreter asm build."
        exit 1
    fi
    nasm -f macho64 "$ASM_DIR/C2I_Stub.s"          -o "$LIB/C2I_Stub.o"
    nasm -f macho64 "$ASM_DIR/I2C_Stub.s"          -o "$LIB/I2C_Stub.o"
    nasm -f macho64 "$ASM_DIR/I2I_Stub.s"          -o "$LIB/I2I_Stub.o"
    nasm -f macho64 "$ASM_DIR/ExceptionHandling.s" -o "$LIB/ExceptionHandling.o"
fi

CJTHREAD_INCLUDES=$(find ../../../src/CJThread/src -type d -name "include" | sed 's/^/-I/' | tr '\n' ' ')
INCLUDE_DIRS="-I../.. -I.. -I../../.. -I$TEST_NAME -Ishared -Ishared/third_party -I../../../include $CJTHREAD_INCLUDES $ARCH_INCLUDE"

for file in shared/*.cpp; do
    "$IOS_CXX" "${TARGET_FLAGS[@]}" -fPIC $INCLUDE_DIRS -c "$file" -o "${file%.cpp}.o" -Wno-attributes -g $IOS_CXXFLAGS
done
mv shared/*.o "$LIB"

for file in "$TEST_NAME"/*.cpp; do
    "$IOS_CXX" "${TARGET_FLAGS[@]}" -fPIC $INCLUDE_DIRS -c "$file" -o "${file%.cpp}.o" -Wno-attributes -g $IOS_CXXFLAGS
done
mv "$TEST_NAME"/*.o "$LIB"

"$IOS_CXX" "${TARGET_FLAGS[@]}" -dynamiclib -o "$TEST_NAME/out/libcangjie-interpreter.dylib" "$LIB"/*.o \
    -Wno-attributes -g -fPIC $IOS_LDFLAGS
