#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ENV_SOURCE="$ROOT_DIR/src/Base/EnvParam.cpp"

if rg -q 'bool CheckLowerBound\(size_t ' "$ENV_SOURCE" &&
   rg -q 'bool CheckLowerBound\(uint64_t ' "$ENV_SOURCE"; then
    echo 'size_t and uint64_t must not be separate overloads because they alias on Linux AArch64' >&2
    exit 1
fi

if ! rg -Uq 'template <typename T>[[:space:]]*bool CheckLowerBound\(T value, T minValue, EnvBoundKind minBound\)' \
    "$ENV_SOURCE"; then
    echo 'EnvParam lower-bound checks must use one type-alias-safe numeric template' >&2
    exit 1
fi

if ! rg -Uq 'template <typename T>[[:space:]]*bool CheckUpperBound\(T value, T maxValue, EnvBoundKind maxBound\)' \
    "$ENV_SOURCE"; then
    echo 'EnvParam upper-bound checks must use one type-alias-safe numeric template' >&2
    exit 1
fi

echo 'EnvParam platform type-alias contract holds'
