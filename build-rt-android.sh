#!/bin/bash

set -xe
export CANGJIE_VERSION=1.0.0
# This script builds the Cangjie runtime.
# It is adapted for the standalone cangjie_runtime_fork repository.

# Set WORKSPACE to the project root directory
export WORKSPACE=$(cd "$(dirname "$0")" && pwd)

# Environment variables for the build.
# Please adjust these paths according to your environment.
export OPENSSL_PATH=/usr/lib/x86_64-linux-gnu; # For aarch64, you might need /usr/lib/aarch64-linux-gnu/
export LD_LIBRARY_PATH=$OPENSSL_PATH:$LD_LIBRARY_PATH

# Assumes llvm-15 is used.
export PATH=/usr/lib/llvm-15/bin:$PATH;

# Set target architecture.
export ARCH=aarch64 # or aarch64

# The original script sourced an envsetup.sh from a compiler project, which is not present in this repository.
# The copy operation to a compiler directory has also been removed.

# Navigate to the runtime source directory
cd "$WORKSPACE/runtime"

# Set build configuration (debug, release, or relwithdebinfo)
export BUILD_TYPE=debug

echo "Starting Cangjie runtime build..."
python3 build.py clean;

# Run the build script
python3 build.py build -t $BUILD_TYPE \
    --target android-aarch64 \
    --target-toolchain ${ANDROID_NDK_ROOT}/toolchains/ \
    --enable-interpreter-test-support \
    -v ${CANGJIE_VERSION};

# Install the built artifacts
python3 build.py install

echo "Cangjie runtime build and installation complete."
echo "Output files are located in: $WORKSPACE/runtime/output/common"


# Update Cangjie toolchain
. "$WORKSPACE/env-android.sh"

# cp -R $WORKSPACE/runtime/output/common/linux_android31_${BUILD_TYPE}_${ARCH}/{lib,runtime} $CJNATIVE_TOOLCHAIN && echo "Cangjie runtime copied to toolchain directory: $CJNATIVE_TOOLCHAIN"
