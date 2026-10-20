#!/bin/bash

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

# Generate a Mach-O -order_file that pins the Cangjie runtime / cjthread
# text layout to the archive member order (the marker-bracket invariant of
# the cj_text_range scheme, see runtime/cj_text_range_design.md).
#
# Usage:
#   gen_cj_order_file.sh [--output FILE] <libcangjie-runtime.a> <libcangjie-thread.a>
#
# Example (paths as shipped by the iOS/macOS runtime SDK):
#   gen_cj_order_file.sh --output cangjie.order \
#       libcangjie-runtime.a libcangjie-thread.a
#
# The glue objects (cjstart.o, section.o) are deliberately NOT inputs. Their
# code has no ordering requirement: unlisted symbols fall outside the marker
# brackets on every linker (end of the output section on lld, link-line
# position on Apple ld), and the initializer execution order is governed by
# the link line (__mod_init_func), which no linker reorders here.
#
# The generated file lists every defined __TEXT,__text symbol of the given
# inputs, one bare symbol per line, in the order the inputs were given.
# Linker behavior (verified on the Xcode 15.4 toolchain; lld from lld/MachO
# sources):
#   - Apple ld (classic and new): listed symbols are laid out in line order;
#     unlisted symbols keep their link-line/archive-member position. The new
#     ld ignores entries of the form "archive.a(member.o):symbol" and entries
#     with a cpu prefix ("arm64:symbol"), so only bare symbol lines pin
#     archive members there.
#   - lld-MachO: listed symbols get decreasing layout priorities; unlisted
#     ones (priority 0) land at the end of the output section.
# With every runtime/cjthread text symbol listed between the marker symbols,
# all three linkers keep the marker bracket intact regardless of archive
# member order or pull order.
#
# What is load-bearing vs not (verified on the Xcode 15.4 toolchain):
#   - REQUIRED: _CJ_Text_Start as the first line, _CJ_Thread_Text_End as the
#     last line, and every runtime/cjthread text symbol listed in between.
#     A marker-only file (lds-style boundary symbols) breaks the bracket:
#     listed symbols sort ahead of unlisted ones, so _CJ_Text_End lands
#     before the runtime code and the range collapses to empty.
#   - NOT required: the order among runtime/thread symbols. Shuffling the
#     inner lines keeps the bracket intact; IsRuntimeFrame only needs the
#     bracket. The member-order output is a deterministic choice, not a
#     constraint.
#
# Duplicate symbol names (e.g. identical static function names in two
# members) are emitted once, at the first occurrence. This may interleave
# code inside the bracket on some linkers; the bracket itself stays intact.
#
# The generator must run against the archives as built on the build machine
# (whose member order the CMake wiring controls), not against rebuilt or
# reassembled copies.

set -eu

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
}

OUTPUT=""
VERBOSE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --output) OUTPUT="$2"; shift 2 ;;
        -v|--verbose) VERBOSE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --)       shift; break ;;
        -*)       echo "gen_cj_order_file.sh: unknown option: $1" >&2; usage; exit 2 ;;
        *)        break ;;
    esac
done
if [ $# -lt 1 ]; then
    usage
    exit 2
fi
for input in "$@"; do
    if [ ! -f "$input" ]; then
        echo "gen_cj_order_file.sh: input not found: $input" >&2
        exit 1
    fi
    case "$input" in
        *.a) ;;
        *)
            echo "gen_cj_order_file.sh: only static archives (.a) are accepted, got: $input" >&2
            echo "  (glue objects like cjstart.o/section.o must not be listed; see header comment)" >&2
            exit 1
            ;;
    esac
done

TMPDIR_OF="$(mktemp -d "${TMPDIR:-/tmp}/cjorder.XXXXXX")"
trap 'rm -rf "$TMPDIR_OF"' EXIT

# Parse `nm -m` output. Symbol lines look like:
#   0000000000000000 (__TEXT,__text) external [no dead strip] __CJ_Text_Start
# Archive member headers look like:
#   start.o:
# Emit "<member>\t<name>" for every defined __TEXT,__text symbol, skipping
# compiler-generated local labels (ltmp*).
AWK_PROG='
/^[0-9A-Fa-f]+ \(__TEXT,__text\)/ {
    name = $NF
    if (name !~ /^ltmp[0-9]*$/)
        print member "\t" name
    next
}
/:$/ && NF == 1 && $0 !~ /\(/ { member = substr($0, 1, length($0) - 1) }
'

: > "$TMPDIR_OF/all"
for input in "$@"; do
    ar t "$input" | sed 's/[[:space:]]*$//' > "$TMPDIR_OF/members"
    nm -m "$input" | awk "$AWK_PROG" > "$TMPDIR_OF/syms"
    # Keep only symbols whose member still exists in the archive TOC
    # (nm may surface members from fat slices differently).
    awk -F '\t' '
        NR == FNR { want[$0] = 1; next }
        want[$1] { print }
    ' "$TMPDIR_OF/members" "$TMPDIR_OF/syms" >> "$TMPDIR_OF/all"
    # The runtime archives must contain native Mach-O objects (no LTO). An
    # input without a single __TEXT,__text symbol means bitcode members or a
    # wrong input; fail loudly instead of emitting a broken order file.
    if [ ! -s "$TMPDIR_OF/syms" ]; then
        echo "gen_cj_order_file.sh: no __TEXT,__text symbol found in $input" >&2
        echo "  (bitcode/LTO member, or not a valid Mach-O object/archive?)" >&2
        exit 1
    fi
done

awk -F '\t' -v verbose="$VERBOSE" '
    {
        if (!seen[$2]++) {
            order[++n] = $2
        } else {
            dup[++nd] = $2
        }
    }
    END {
        for (i = 1; i <= n; i++)
            print order[i]
        # Duplicate names are expected (see header comment): emitted once at
        # the first occurrence. Details are diagnostic output, printed only
        # with --verbose so SDK packaging terminals stay clean.
        if (nd > 0 && verbose) {
            printf("gen_cj_order_file.sh: %d duplicate symbol name(s), pinned at first occurrence:", nd) > "/dev/stderr"
            for (i = 1; i <= nd; i++)
                printf(" %s", dup[i]) > "/dev/stderr"
            printf("\n") > "/dev/stderr"
        }
        if (verbose)
            printf("gen_cj_order_file.sh: %d symbol(s) emitted\n", n) > "/dev/stderr"
    }
' "$TMPDIR_OF/all" > "$TMPDIR_OF/body"

if [ -n "$OUTPUT" ]; then
    {
        echo "# Generated by runtime/build/gen_cj_order_file.sh. Do not edit."
        echo "# Inputs (link-line order): $*"
        cat "$TMPDIR_OF/body"
    } > "$OUTPUT"
else
    cat "$TMPDIR_OF/body"
fi
