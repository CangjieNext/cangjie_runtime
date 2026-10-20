#!/bin/bash
# =============================================================================
# cj_marker_order_smoke_test.sh
#
# Mach-O linker archive-member-order smoke test for the runtime text-range
# marker scheme (see ../cj_text_range_design.md).
#
# The marker scheme requires that a static archive [Start.o, rt1.o, rt2.o,
# End.o] lands in the output with Start < rt1 < rt2 < End in address order,
# so that __CJ_Text_Start / __CJ_Text_End bracket all runtime code. This
# script verifies that property empirically for a set of toolchains:
#
#   1. builds marker-bracketed archives (libbracket.a, libthread.a) plus
#      auxiliary objects,
#   2. links them under several modes (default / -all_load / -force_load /
#      -ObjC / -dead_strip / weak end marker / LTO / ld_classic),
#   3. checks with `nm -n` that the bracket holds,
#   4. runs the binary so its built-in self-check validates the bracket,
#   5. records the ld version string for every toolchain.
#
# Re-run after an Xcode upgrade:
#   ./cj_marker_order_smoke_test.sh --xcode /Applications/Xcode.app \
#                                   --xcode /Applications/Xcode26.app
#
# Exit status: 0 if every requested (toolchain, mode) passes, 1 otherwise.
# =============================================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

XCODES=()
WORKDIR=""
MODES="default,all_load,force_load,objc,dead_strip,weak,lto,late_pull"
TARGET="macos"          # macos (runnable) or ios (nm-only)
KEEP=0

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --xcode)   XCODES+=("$2"); shift 2 ;;
        --workdir) WORKDIR="$2"; shift 2 ;;
        --modes)   MODES="$2"; shift 2 ;;
        --target)  TARGET="$2"; shift 2 ;;
        --keep)    KEEP=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown option: $1" >&2; usage ;;
    esac
done

if [ ${#XCODES[@]} -eq 0 ]; then
    echo "error: pass at least one --xcode <DIR>" >&2
    usage
fi

if [ -z "$WORKDIR" ]; then
    WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/cj_marker_smoke.XXXXXX")"
fi
mkdir -p "$WORKDIR"
SUMMARY="$WORKDIR/summary.txt"
: > "$SUMMARY"

OVERALL=0

# -----------------------------------------------------------------------------
# Test sources.
#
# Symbol naming follows the runtime convention: C identifiers are written with
# one leading underscore less than the final symbol, i.e. source
# `_CJ_Text_Start` produces symbol `__CJ_Text_Start`.
# -----------------------------------------------------------------------------
write_sources() {
    local d="$1"
    mkdir -p "$d"

    # --- runtime archive members (bracket: start ... end) ---
    cat > "$d/m_start.c" <<'EOF'
__attribute__((used)) void _CJ_Text_Start(void) { }
EOF
    cat > "$d/rt1.c" <<'EOF'
__attribute__((noinline)) void cj_rt1_fn(int x) { if (x > 0) { } }
EOF
    cat > "$d/rt2.c" <<'EOF'
__attribute__((noinline)) void cj_rt2_fn(int x) { if (x > 1) { } }
EOF
    cat > "$d/m_end.c" <<'EOF'
__attribute__((used)) void _CJ_Text_End(void) { }
EOF
    # weak-variant of the end marker (weak mode)
    cat > "$d/m_end_weak.c" <<'EOF'
__attribute__((used, weak)) void _CJ_Text_End(void) { }
EOF

    # --- thread archive members (bracket: thread start ... thread end) ---
    cat > "$d/t_start.c" <<'EOF'
__attribute__((used)) void _CJ_Thread_Text_Start(void) { }
EOF
    cat > "$d/tt1.c" <<'EOF'
__attribute__((noinline)) void cjt_thread1_fn(int x) { if (x > 2) { } }
EOF
    cat > "$d/tt2.c" <<'EOF'
__attribute__((noinline)) void cjt_thread2_fn(int x) { if (x > 3) { } }
EOF
    cat > "$d/t_end.c" <<'EOF'
__attribute__((used)) void _CJ_Thread_Text_End(void) { }
EOF
    cat > "$d/t_end_weak.c" <<'EOF'
__attribute__((used, weak)) void _CJ_Thread_Text_End(void) { }
EOF

    # --- aux archive member (late-pull / Q2 test) ---
    cat > "$d/aux.c" <<'EOF'
__attribute__((noinline)) void cj_aux_fn(int x) { if (x > 4) { } }
EOF

    # --- ObjC member (-ObjC mode) ---
    cat > "$d/objc1.m" <<'EOF'
__attribute__((objc_root_class))
@interface CJSmokeObj
@end
@implementation CJSmokeObj
@end
EOF

    # --- section.o: references the markers, carries the runtime self-check ---
    # Compiled as C++ with an explicit -std=c++14 (Xcode 15.4 defaults below
    # C++11 for C++).
    cat > "$d/section.cpp" <<'EOF'
extern "C" {
void _CJ_Text_Start(void);
void _CJ_Text_End(void);
void _CJ_Thread_Text_Start(void);
void _CJ_Thread_Text_End(void);
void cj_rt1_fn(int);
void cj_rt2_fn(int);
void cjt_thread1_fn(int);
void cjt_thread2_fn(int);
}

__attribute__((noinline)) extern "C" int cj_selfcheck(void) {
    const unsigned long s  = reinterpret_cast<unsigned long>(&_CJ_Text_Start);
    const unsigned long e  = reinterpret_cast<unsigned long>(&_CJ_Text_End);
    const unsigned long r1 = reinterpret_cast<unsigned long>(&cj_rt1_fn);
    const unsigned long r2 = reinterpret_cast<unsigned long>(&cj_rt2_fn);
    const unsigned long ts = reinterpret_cast<unsigned long>(&_CJ_Thread_Text_Start);
    const unsigned long te = reinterpret_cast<unsigned long>(&_CJ_Thread_Text_End);
    const unsigned long t1 = reinterpret_cast<unsigned long>(&cjt_thread1_fn);
    const unsigned long t2 = reinterpret_cast<unsigned long>(&cjt_thread2_fn);
    if (!(s < r1 && r1 < r2 && r2 < e)) return 1;
    if (!(ts < t1 && t1 < t2 && t2 < te)) return 2;
    return 0;
}
EOF

    # --- user.o: references runtime members, defines its own function ---
    cat > "$d/user.c" <<'EOF'
extern void cj_rt1_fn(int);
extern void cj_rt2_fn(int);
extern void cjt_thread1_fn(int);
extern void cjt_thread2_fn(int);
extern int  cj_selfcheck(void);
#ifdef LATE_PULL
extern void cj_aux_fn(int);
#endif

__attribute__((noinline)) int user_fn(int x) { return x + 1; }

int main(void) {
    cj_rt1_fn(1);
    cj_rt2_fn(2);
    cjt_thread1_fn(3);
    cjt_thread2_fn(4);
    user_fn(6);
#ifdef LATE_PULL
    cj_aux_fn(5);
#endif
    return cj_selfcheck();
}
EOF
}

# -----------------------------------------------------------------------------
# Build one archive: ar cr <archive> in the given member order + ranlib.
# -----------------------------------------------------------------------------
build_archive() {
    local ar="$1" ranlib="$2"; shift 2
    local out="$1"; shift
    rm -f "$out"
    "$ar" cr "$out" "$@" || return 1
    "$ranlib" "$out" || return 1
}

# -----------------------------------------------------------------------------
# nm -n based bracket check.
#   $1 = binary, $2 = arch (or empty for host)
# Prints "OK" / "FAIL ..." and returns 0/1.
# Also records the relative position of _cj_aux_fn vs _user_fn (late pull).
# -----------------------------------------------------------------------------
bracket_check() {
    local bin="$1" arch="$2"
    local nm_args=(-n -U)
    [ -n "$arch" ] && nm_args+=(-arch "$arch")

    # nm -n lists symbols in address order. Filter the symbols of interest and
    # require the two marker brackets to appear as consecutive subsequences
    # (any foreign symbol between bracket members breaks the match).
    local syms order
    syms='^(__CJ_Text_Start|__CJ_Text_End|__CJ_Thread_Text_Start|__CJ_Thread_Text_End|_cj_rt1_fn|_cj_rt2_fn|_cjt_thread1_fn|_cjt_thread2_fn|_cj_aux_fn|_user_fn)$'
    order="$(nm ${nm_args[@]+"${nm_args[@]}"} "$bin" 2>/dev/null \
             | awk '{print $3}' | grep -E "$syms" | tr '\n' ' ')"

    for s in __CJ_Text_Start __CJ_Text_End _cj_rt1_fn _cj_rt2_fn \
             __CJ_Thread_Text_Start __CJ_Thread_Text_End \
             _cjt_thread1_fn _cjt_thread2_fn _user_fn; do
        case " $order " in
            *" $s "*) ;;
            *) echo "FAIL: missing symbol $s"; return 1 ;;
        esac
    done

    case "$order" in
        *"__CJ_Text_Start _cj_rt1_fn _cj_rt2_fn __CJ_Text_End"*) ;;
        *) echo "FAIL: bracket broken"; echo "  order: $order"; return 1 ;;
    esac
    case "$order" in
        *"__CJ_Thread_Text_Start _cjt_thread1_fn _cjt_thread2_fn __CJ_Thread_Text_End"*) ;;
        *) echo "FAIL: thread bracket broken"; echo "  order: $order"; return 1 ;;
    esac

    # auxiliary observation: position of late-pulled _cj_aux_fn relative to
    # _user_fn (present only in late_pull mode)
    local verdict="OK"
    case " $order " in
        *" _cj_aux_fn "*)
            case "$order" in
                *_cj_aux_fn*_user_fn*) verdict="OK aux_before_user" ;;
                *)                     verdict="OK aux_after_user" ;;
            esac
            ;;
    esac
    echo "$verdict"
    return 0
}

# -----------------------------------------------------------------------------
# Run one (toolchain, mode) pair. Returns 0 on pass.
# -----------------------------------------------------------------------------
run_mode() {
    local cc="$1" ar="$2" ranlib="$3" srcdir="$4" mode="$5" tag="$6" run="$7"
    local out="$srcdir/$mode"
    mkdir -p "$out"

    local weak=0 lto=0 late=0 link_extra=()
    case "$mode" in
        default)    ;;
        all_load)   link_extra+=(-Wl,-all_load) ;;
        force_load) link_extra+=("-Wl,-force_load,$srcdir/libbracket.a" "-Wl,-force_load,$srcdir/libthread.a") ;;
        objc)       link_extra+=(-Wl,-ObjC) ;;
        dead_strip) link_extra+=(-Wl,-dead_strip) ;;
        weak)       weak=1 ;;
        lto)        lto=1 ;;
        late_pull)  late=1 ;;
        ld_classic) link_extra+=(-Wl,-ld_classic) ;;
        *) echo "SKIP: unknown mode $mode"; return 2 ;;
    esac

    local flto=()
    [ $lto -eq 1 ] && flto=(-flto)
    local late_def=()
    [ $late -eq 1 ] && late_def=(-DLATE_PULL)

    # ---- archives (member order is the whole point) ----
    local end_m="m_end.o" t_end_m="t_end.o"
    if [ $weak -eq 1 ]; then end_m="m_end_weak.o"; t_end_m="t_end_weak.o"; fi
    build_archive "$ar" "$ranlib" "$srcdir/libbracket.a" \
        "$srcdir/m_start.o" "$srcdir/rt1.o" "$srcdir/rt2.o" "$srcdir/$end_m" || { echo "FAIL: ar"; return 1; }
    build_archive "$ar" "$ranlib" "$srcdir/libthread.a" \
        "$srcdir/t_start.o" "$srcdir/tt1.o" "$srcdir/tt2.o" "$srcdir/$t_end_m" || { echo "FAIL: ar"; return 1; }
    build_archive "$ar" "$ranlib" "$srcdir/libaux.a" \
        "$srcdir/aux.o" "$srcdir/objc1.o" || { echo "FAIL: ar"; return 1; }

    # ---- link line: mirrors the runtime scheme ----
    #   section.o (references runtime markers) ... libaux.a (before user.o) ...
    #   user.o (references runtime members) ... libbracket.a libthread.a
    #   -u forces the thread markers (mirrors runtime CMakeLists.txt).
    local link_args=(
        ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"}
        -std=c++14
        ${flto[@]+"${flto[@]}"}
        "$srcdir/section.o"
        "$srcdir/libaux.a"
        "$srcdir/user.o"
        "$srcdir/libbracket.a"
        "$srcdir/libthread.a"
        -Wl,-u,__CJ_Thread_Text_Start
        -Wl,-u,__CJ_Thread_Text_End
        -lobjc
        ${link_extra[@]+"${link_extra[@]}"}
        -o "$out/test"
    )

    # compile self-check with explicit -std=c++14
    if ! "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -std=c++14 ${flto[@]+"${flto[@]}"} -c "$srcdir/section.cpp" -o "$srcdir/section.o" 2> "$out/compile_section.log"; then
        echo "FAIL: compile section.cpp"; cat "$out/compile_section.log"; return 1
    fi
    if ! "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} ${late_def[@]+"${late_def[@]}"} ${flto[@]+"${flto[@]}"} -c "$srcdir/user.c" -o "$srcdir/user.o" 2> "$out/compile_user.log"; then
        echo "FAIL: compile user.c"; cat "$out/compile_user.log"; return 1
    fi

    if ! "$cc" "${link_args[@]}" 2> "$out/link.log"; then
        echo "$tag|$mode|check=FAIL:link|run=SKIP" >> "$SUMMARY"
        echo "FAIL: link"; sed -n '1,8p' "$out/link.log"
        return 1
    fi

    # capture the archive-member trace (evidence of which members were pulled)
    if [ "$mode" = "default" ]; then
        LD_TRACE_ARCHIVES=1 "$cc" "${link_args[@]}" -o "$out/test_trace" \
            > "$out/ld_trace.log" 2>&1 || true
    fi

    # ---- check ----
    local arch="" run_ok="SKIP"
    case "$TARGET" in
        macos) arch="" ;;
        ios)   arch="arm64" ;;
    esac
    local check
    check="$(bracket_check "$out/test" "$arch")"
    local check_rc=$?

    if [ "$run" = "yes" ]; then
        if "$out/test"; then run_ok="PASS"; else run_ok="FAIL($?)"; fi
    fi

    echo "$tag|$mode|check=$check|run=$run_ok" >> "$SUMMARY"
    [ "$check" = "OK" ] && [ "$run_ok" != "FAIL"* ] && return 0 || return 1
}

# =============================================================================
# Main loop over toolchains.
# =============================================================================
for xcode in "${XCODES[@]}"; do
    [ -d "$xcode" ] || { echo "error: no such Xcode: $xcode" >&2; OVERALL=1; continue; }
    export DEVELOPER_DIR="$xcode"

    local_tag="$(basename "$xcode")"
    tc_dir="$WORKDIR/$local_tag"
    mkdir -p "$tc_dir"

    xcode_ver="$(xcodebuild -version 2>/dev/null | tr '\n' ' ')"
    ld_ver="$(xcrun ld -v 2>&1 | head -1)"
    clang_ver="$(xcrun clang --version | head -1)"
    sdkroot="$(xcrun --show-sdk-path)"

    {
        echo "=============================================================="
        echo "toolchain: $xcode"
        echo "  xcodebuild: $xcode_ver"
        echo "  ld:         $ld_ver"
        echo "  clang:      $clang_ver"
        echo "  sdk:        $sdkroot"
    } | tee "$tc_dir/toolchain.txt"

    srcdir="$tc_dir/src"
    write_sources "$srcdir"

    cc="$(xcrun -f clang)"
    ar="$(xcrun -f ar)"
    ranlib="$(xcrun -f ranlib)"

    case "$TARGET" in
        macos) SDKNAME=macosx    ; TRIPLE=arm64-apple-macos12.0     ; run="yes" ;;
        ios)   SDKNAME=iphoneos  ; TRIPLE=arm64-apple-ios12.0.0     ; run="no"  ;;
        *) echo "error: bad --target $TARGET" >&2; exit 2 ;;
    esac
    SDKROOT_ARG="$(xcrun --sdk "$SDKNAME" --show-sdk-path)"
    TARGET_FLAG=(-target "$TRIPLE" -isysroot "$SDKROOT_ARG")

    # compile plain (non-LTO, non-late) objects once per toolchain
    "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -c "$srcdir/m_start.c" -o "$srcdir/m_start.o" || exit 1
    "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -c "$srcdir/rt1.c" -o "$srcdir/rt1.o" || exit 1
    "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -c "$srcdir/rt2.c" -o "$srcdir/rt2.o" || exit 1
    "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -c "$srcdir/m_end.c" -o "$srcdir/m_end.o" || exit 1
    "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -c "$srcdir/m_end_weak.c" -o "$srcdir/m_end_weak.o" || exit 1
    "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -c "$srcdir/t_start.c" -o "$srcdir/t_start.o" || exit 1
    "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -c "$srcdir/tt1.c" -o "$srcdir/tt1.o" || exit 1
    "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -c "$srcdir/tt2.c" -o "$srcdir/tt2.o" || exit 1
    "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -c "$srcdir/t_end.c" -o "$srcdir/t_end.o" || exit 1
    "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -c "$srcdir/t_end_weak.c" -o "$srcdir/t_end_weak.o" || exit 1
    "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -c "$srcdir/aux.c" -o "$srcdir/aux.o" || exit 1
    "$cc" ${TARGET_FLAG[@]+"${TARGET_FLAG[@]}"} -c "$srcdir/objc1.m" -o "$srcdir/objc1.o" || exit 1

    IFS=',' read -r -a mode_list <<< "$MODES"
    for mode in ${mode_list[@]+"${mode_list[@]}"}; do
        # ld_classic only exists on toolchains that still ship it
        if [ "$mode" = "ld_classic" ]; then
            if ! xcrun ld -ld_classic -v >/dev/null 2>&1; then
                echo "$local_tag|ld_classic|SKIP:not supported by this ld" >> "$SUMMARY"
                continue
            fi
        fi
        # LTO on ld_classic is exercised through the same link path; skip for ios
        echo "--- $local_tag / $mode ---"
        run_mode "$cc" "$ar" "$ranlib" "$srcdir" "$mode" "$local_tag" "$run" || OVERALL=1
    done

    # keep an nm listing of the default-mode binary as evidence
    if [ -f "$srcdir/default/test" ]; then
        nm -n -U "$srcdir/default/test" > "$tc_dir/default_nm.txt" 2>/dev/null || true
    fi
    if [ -f "$srcdir/lto/test" ]; then
        nm -n -U "$srcdir/lto/test" > "$tc_dir/lto_nm.txt" 2>/dev/null || true
    fi
done

echo
echo "==================== SUMMARY ($WORKDIR/summary.txt) ===================="
cat "$SUMMARY"
echo "========================================================================"

if [ $KEEP -eq 0 ]; then
    rm -rf "$WORKDIR"
else
    echo "workdir kept: $WORKDIR"
fi

exit $OVERALL
