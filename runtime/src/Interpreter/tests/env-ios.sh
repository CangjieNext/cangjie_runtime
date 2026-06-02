export CJNATIVE_TOOLCHAIN=/Users/msharapov/tools/cjmp-sdk-mac-arm64-0.2.4-release/cjmp-tools/third_party/cangjie-ios

# Optional. Defaults to 11.0 in build-ios.sh.
# export IOS_MIN_VERSION=11.0

# iOS simulator, arm64.
export IOS_TARGET_ARCH=aarch64
export IOS_PLATFORM=simulator
export IOS_SYSROOT="$(xcrun --sdk iphonesimulator --show-sdk-path)"

# iOS device, arm64.
# export IOS_TARGET_ARCH=aarch64
# export IOS_PLATFORM=device
# export IOS_SYSROOT="$(xcrun --sdk iphoneos --show-sdk-path)"

# iOS simulator, x86_64.
# export IOS_TARGET_ARCH=x86_64
# export IOS_PLATFORM=simulator
# export IOS_SYSROOT="$(xcrun --sdk iphonesimulator --show-sdk-path)"
