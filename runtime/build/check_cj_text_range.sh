#!/bin/bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# Verifies the runtime text range markers in a linked binary.
#
# The runtime classifies stack frames by checking whether the frame PC falls
# into the runtime / CJThread text ranges. The ranges are bracketed by marker
# objects embedded as the first / last members of the runtime static archives
# (see os/Macos/Section.cpp and StackManager::IsRuntimeFrame); the Apple
# linker lays pulled archive members in member order. Two failures are
# possible and both are silent at runtime:
#   1. a non-cangjie object is linked in between the markers of an archive
#      (only possible if the archive was re-assembled);
#   2. archive code falls outside its range (e.g. the binary was linked by a
#      linker that does not preserve archive member order, such as lld).
# This check makes both failures loud.
#
# Usage:
#   check_cj_text_range.sh <linked-binary> <cangjie-object-or-archive> ...
#
# Pass every cangjie-provided link input, e.g.:
#   check_cj_text_range.sh App.bin \
#       cjstart.o section.o \
#       libcangjie-runtime.a libcangjie-thread.a
#
# Symbols from *.a archives must lie inside a marker bracket; symbols from the
# standalone glue objects (cjstart.o, section.o) legitimately sit outside the
# brackets (they are linked before the archives) and are only whitelisted.
#
# Intended for a dedicated Xcode Build Phase (run after Link Binary) or CI.

set -eu

if [ $# -lt 2 ]; then
    echo "usage: $0 <linked-binary> <cangjie-object-or-archive> ..." >&2
    exit 2
fi

BIN=$1
shift
[ -f "${BIN}" ] || { echo "error: ${BIN}: no such file" >&2; exit 2; }

SYMS=$(nm -n "${BIN}" 2>/dev/null) || { echo "error: nm failed on ${BIN}" >&2; exit 2; }

# Whitelist: names defined by any cangjie-provided input. ".L*" assembler-local
# labels are excluded: they mark the end of functions and may legitimately sit
# a few bytes past the markers.
TMP_NAMES=$(mktemp)
# Text symbols contributed by the archives; each of them must lie inside one
# of the marker brackets.
TMP_ARCH=$(mktemp)
trap 'rm -f "${TMP_NAMES}" "${TMP_ARCH}"' EXIT
: > "${TMP_NAMES}"
: > "${TMP_ARCH}"
for input in "$@"; do
    [ -f "${input}" ] || { echo "error: ${input}: no such file" >&2; exit 2; }
    nm --defined-only "${input}" 2>/dev/null | awk 'NF >= 3 && $3 !~ /^\.L/ {print $3}' >> "${TMP_NAMES}"
    case "${input}" in
    *.a)
        nm --defined-only "${input}" 2>/dev/null | awk '$2 ~ /^[Tt]$/ && $3 !~ /^\.L/ {print $3}' >> "${TMP_ARCH}"
        ;;
    esac
done
sort -u -o "${TMP_NAMES}" "${TMP_NAMES}"
sort -u -o "${TMP_ARCH}" "${TMP_ARCH}"

RC=0

# check_range <start-marker-name> <end-marker-name> <label>
# Prints the non-cangjie text symbols strictly inside the bracket; empty
# output means the bracket is clean. Hex addresses are compared as strings:
# nm prints fixed-width zero-padded values, so the lexicographic order
# matches the numeric one. The "" concatenations force string comparison --
# BSD awk would otherwise coerce hex digits containing "e" to scientific
# notation numbers.
check_range() {
    local start_name=$1 end_name=$2
    awk -v s="${start_name}" -v e="${end_name}" '
        $3 == s { start = $1 }
        $3 == e { end = $1 }
        END {
            if (start != "" && end != "") {
                print start, end
            }
        }
    ' <<< "${SYMS}"
}

PAIRS=$(check_range "__CJ_Text_Start" "__CJ_Text_End")
THREAD_PAIRS=$(check_range "__CJ_Thread_Text_Start" "__CJ_Thread_Text_End")

# $(...) strips trailing newlines, so concatenate with an explicit separator
# to keep each bracket on its own line.
ALL_PAIRS="${PAIRS}"
if [ -n "${THREAD_PAIRS}" ]; then
    ALL_PAIRS="${ALL_PAIRS}
${THREAD_PAIRS}"
fi

if [ -z "${PAIRS}" ]; then
    echo "error: ${BIN}: runtime range markers missing" >&2
    exit 1
fi

VIOLATIONS=""
OUTSIDE=""

# Check 1: every text symbol inside any bracket must be defined by a
# cangjie-provided input.
while read -r start_hex end_hex; do
    [ -z "${start_hex}" ] && continue
    while IFS= read -r line; do
        [ -z "${line}" ] && continue
        addr=$(printf '%s\n' "${line}" | awk '{print $1}')
        inside=$(printf '%s %s %s\n' "${addr}" "${start_hex}" "${end_hex}" |
            awk '{ print ($1 "" > $2 "" && $1 "" < $3 "") ? "y" : "n" }')
        [ "${inside}" = "y" ] || continue
        sym=$(printf '%s\n' "${line}" | awk '{print $3}')
        grep -Fxq "${sym}" "${TMP_NAMES}" || VIOLATIONS="${VIOLATIONS}${line}
"
    done <<< "${SYMS}"
done <<< "${PAIRS}"

if [ -n "${VIOLATIONS}" ]; then
    echo "error: ${BIN}: non-cangjie symbols inside a marker bracket:" >&2
    printf '%s' "${VIOLATIONS}" >&2
    RC=1
fi

# Check 2: every archive text symbol must lie inside at least one bracket.
while IFS= read -r sym; do
    [ -z "${sym}" ] && continue
    addr_hex=$(awk -v s="${sym}" '$3 == s {print $1; exit}' <<< "${SYMS}")
    [ -z "${addr_hex}" ] && continue
    found=""
    while read -r start_hex end_hex; do
        [ -z "${start_hex}" ] && continue
        inside=$(printf '%s %s %s\n' "${addr_hex}" "${start_hex}" "${end_hex}" |
            awk '{ print ($1 "" >= $2 "" && $1 "" <= $3 "") ? "y" : "n" }')
        if [ "${inside}" = "y" ]; then
            found="y"
            break
        fi
    done <<< "${ALL_PAIRS}"
    if [ "${found}" != "y" ]; then
        OUTSIDE="${OUTSIDE}${sym} at 0x${addr_hex}
"
    fi
done < "${TMP_ARCH}"

if [ -n "${OUTSIDE}" ]; then
    echo "error: ${BIN}: archive text symbols outside all marker brackets:" >&2
    printf '%s' "${OUTSIDE}" >&2
    RC=1
fi

if [ "${RC}" = "0" ]; then
    echo "ok: ${BIN}: marker brackets clean"
else
    echo "hint: the marker objects must remain the first/last members of the" >&2
    echo "cangjie runtime static archives, and the binary must be linked by a" >&2
    echo "linker that preserves archive member order (Apple ld)." >&2
fi
exit ${RC}
