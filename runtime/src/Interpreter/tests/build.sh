#!/bin/bash

set -eo pipefail

if [[ ! -v CJNATIVE_TOOLCHAIN ]]; then
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

if [ -z "$CXX" ]; then
    CXX="g++"
fi

rm -rf "$TEST_NAME/out"
mkdir -p "$TEST_NAME/out"

$CXX -shared -o "$TEST_NAME/out/libforeign.so" shared/foreign.c -fPIC

cjc --int-overflow wrapping -Woff unused shared/common.cj -O2 --output-type staticlib --save-temps "$TEST_NAME/out/" -o "$TEST_NAME/out/common.a"
cjc --int-overflow wrapping -Woff unused "$TEST_NAME/test.cj" "$TEST_NAME/out/common.a" -O2 --link-option=-export-dynamic --save-temps "$TEST_NAME/out/" --import-path="$TEST_NAME/out" --output-type exe -o "$TEST_NAME/out/test" -L "$TEST_NAME/out/" -lforeign

export LIB="$TEST_NAME/lib"
rm -rf $LIB
mkdir -p $LIB

ARCH=$(uname -m)
if [ "$ARCH" = "x86_64" ]; then
    export ASM_DIR="shared/asm/amd64"
    nasm -f elf64 $ASM_DIR/C2I_Stub.s         -o $LIB/C2I_Stub.o
    nasm -f elf64 $ASM_DIR/I2C_Stub.s         -o $LIB/I2C_Stub.o
    nasm -f elf64 $ASM_DIR/I2I_Stub.s         -o $LIB/I2I_Stub.o
    nasm -f elf64 $ASM_DIR/ExceptionHandling.s -o $LIB/ExceptionHandling.o
    ARCH_INCLUDE="-I../../../src/CJThread/src/runtime/schedule/include/inner/gas/x86/x86_64/"
elif [ "$ARCH" = "aarch64" ]; then
    export ASM_DIR="shared/asm/arm64"
    ${CC:-gcc} -x assembler-with-cpp -c $ASM_DIR/C2I_Stub.S         -o $LIB/C2I_Stub.o
    ${CC:-gcc} -x assembler-with-cpp -c $ASM_DIR/I2C_Stub.S         -o $LIB/I2C_Stub.o
    ${CC:-gcc} -x assembler-with-cpp -c $ASM_DIR/I2I_Stub.S         -o $LIB/I2I_Stub.o
    ${CC:-gcc} -x assembler-with-cpp -c $ASM_DIR/ExceptionHandling.S -o $LIB/ExceptionHandling.o
    ARCH_INCLUDE="-I../../../src/CJThread/src/runtime/schedule/include/inner/gas/arm/arm64/"
else
    echo "Error: unsupported architecture: $ARCH"
    exit 1
fi

CJTHREAD_INCLUDES=$(find ../../../src/CJThread/src -type d -name "include" | sed 's/^/-I/' | tr '\n' ' ')
export INCLUDE_DIRS="-I../.. -I.. -I../../.. -I$TEST_NAME -Ishared -Ishared/third_party -I../../../include $CJTHREAD_INCLUDES $ARCH_INCLUDE"

for file in shared/*.cpp;
do
	$CXX -fPIC $INCLUDE_DIRS -c $file -o ${file%.cpp}.o -Wno-attributes -g
done
mv shared/*.o $LIB

for file in $TEST_NAME/*.cpp;
do
        $CXX -fPIC $INCLUDE_DIRS -c $file -o ${file%.cpp}.o -Wno-attributes -g
done
mv $TEST_NAME/*.o $LIB

$CXX -shared -o "$TEST_NAME/out/libcbcengine.so" $LIB/*.o -Wno-attributes -g -fPIC
