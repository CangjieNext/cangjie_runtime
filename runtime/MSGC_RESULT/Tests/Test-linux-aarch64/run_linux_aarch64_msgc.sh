#!/usr/bin/env bash

set -u

RESULT_ROOT="/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/MSGC_RESULT/Tests/Test-linux-aarch64"
TEST_ROOT="/home/lipeisheng/cangjie/cangjie_test"
SDK_ROOT="/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc"
STDX_ROOT="$SDK_ROOT/linux_aarch64_cjnative/dynamic/stdx"
FRAMEWORK_ROOT="/tmp/cangjie_test_framework.XDSApM"
FRAMEWORK_COMMIT="$(git -C "$FRAMEWORK_ROOT" rev-parse HEAD 2>/dev/null || true)"
NORMALIZER="$RESULT_ROOT/normalize_msgc_results.py"

# Use only the selected SDK in child test shells. The inherited environment on
# this host contains x86_64 library paths even though the host is aarch64.
export CANGJIE_HOME="$SDK_ROOT"
export CANGJIE_STDX_PATH="$STDX_ROOT"
export CANGJIE_TEST="$TEST_ROOT"
export PATH="$SDK_ROOT/bin:$SDK_ROOT/tools/bin:/usr/bin:/bin:/usr/local/bin"
unset LD_LIBRARY_PATH
export LD_LIBRARY_PATH="$SDK_ROOT/runtime/lib/linux_aarch64_cjnative:$SDK_ROOT/tools/lib:$STDX_ROOT"

if [ ! -x "$SDK_ROOT/bin/cjc" ]; then
    echo "Missing compiler: $SDK_ROOT/bin/cjc" >&2
    exit 2
fi
if [ ! -f "$TEST_ROOT/testsuites/HLT/configs/cjnative/linux_aarch64-linux_aarch64/basic.cfg" ]; then
    echo "Missing HLT Linux aarch64 configuration" >&2
    exit 2
fi
if [ ! -f "$TEST_ROOT/testsuites/LLT/configs/cjnative/cjnative_test_linux_aarch64.cfg" ]; then
    echo "Missing LLT Linux aarch64 configuration" >&2
    exit 2
fi
if [ ! -f "$FRAMEWORK_ROOT/main.py" ]; then
    echo "Missing Maple framework: $FRAMEWORK_ROOT/main.py" >&2
    exit 2
fi

mkdir -p "$RESULT_ROOT"

RUN_STAMP="$(date '+%Y%m%d_%H%M%S')"
RUN_ROOT="$RESULT_ROOT/run-$RUN_STAMP"
mkdir -p "$RUN_ROOT"

cat > "$RUN_ROOT/environment.txt" <<EOF
run_stamp=$RUN_STAMP
host=$(uname -a)
host_arch=$(uname -m)
sdk_root=$SDK_ROOT
sdk_cjc=$SDK_ROOT/bin/cjc
sdk_cjc_version=$("$SDK_ROOT/bin/cjc" --version 2>&1 | tr '\n' ' ')
stdx_root=$STDX_ROOT
test_root=$TEST_ROOT
framework_root=$FRAMEWORK_ROOT
framework_commit=$FRAMEWORK_COMMIT
runtime_root=/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime
gc_mode=marksweep
jobs=2
timeout_seconds=120
retry=0
EOF

printf '%s\n' "$(date '+%F %T%z') start MSGC Linux aarch64 run" | tee "$RUN_ROOT/run.log"
printf '%s\n' "framework_commit=$FRAMEWORK_COMMIT" | tee -a "$RUN_ROOT/run.log"
printf '%s\n' "stdx_root=$STDX_ROOT" | tee -a "$RUN_ROOT/run.log"

STDX_LIBS="-lstdx.unittest.data -lstdx.unittest -lstdx.encoding.json -lstdx.serialization.serialization -lstdx.serialization -lstdx.net.http -lstdx.net.tls -lstdx.net.tls.common -lstdx.net -lstdx.logger -lstdx.log -lstdx.encoding.url -lstdx.encoding.json.stream -lstdx.crypto.kit -lstdx.crypto.x509 -lstdx.crypto.crypto -lstdx.crypto.keys -lstdx.crypto.digest -lstdx.crypto.common -lstdx.encoding.hex -lstdx.encoding.base64 -lstdx.encoding -lstdx.compress.tar -lstdx.crypto -lstdx.compress -lstdx.compress.zlib -lstdx.actors -lstdx.effect -lstdx.string_intern -lstdx"
STDX_LINK="-L $STDX_ROOT $STDX_LIBS --import-path $STDX_ROOT"

run_stage() {
    local stage="$1"
    local cfg="$2"
    local test_path="$3"
    local test_list="${4:-}"
    local compiler_value="$5"
    local stage_root="$RUN_ROOT/$stage"
    local framework_log="$stage_root/framework.stdout.log"
    local framework_rc=0
    local -a command_args

    mkdir -p "$stage_root/log" "$stage_root/temp"
    command_args=(
        python3 "$FRAMEWORK_ROOT/main.py"
        --test_cfg "$cfg"
        --temp_dir "$stage_root/temp"
        --log_dir "$stage_root/log"
        --output "$stage_root/results.txt"
        --json_output "$stage_root/results.json"
        --xml_output "$stage_root/results.xml"
        --timeout 120
        --directory_structure normal
        --debug
        --progress no_flush_progress
        -j 2
        -C "compiler=$compiler_value"
        -C "import-cangjie-stdx=$STDX_LINK"
        -C "cangjie-stdx-list=$STDX_LIBS"
    )
    if [ -n "$test_list" ]; then
        command_args+=(--test_list "$test_list")
    fi
    command_args+=("$test_path")

    printf '%s\n' "[$stage] START $(date '+%F %T%z')" | tee -a "$RUN_ROOT/run.log"
    printf '%q ' "${command_args[@]}" > "$stage_root/command.sh"
    printf '\n' >> "$stage_root/command.sh"

    "${command_args[@]}" > "$framework_log" 2>&1 || framework_rc=$?
    printf '%s\n' "[$stage] FRAMEWORK_RC=$framework_rc END $(date '+%F %T%z')" | tee -a "$RUN_ROOT/run.log"

    if [ -f "$stage_root/results.json" ]; then
        python3 "$NORMALIZER" "$stage_root/results.json" "$stage_root" "$stage" || true
    else
        printf '%s\n' "framework did not create results.json; rc=$framework_rc" > "$stage_root/normalization.error"
    fi
}

run_stage \
    "01-runtime-llt" \
    "$TEST_ROOT/testsuites/LLT/configs/cjnative/cjnative_test_linux_aarch64.cfg" \
    "$TEST_ROOT/testsuites/LLT/Runtime" \
    "/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/MSGC_RESULT/Tests/Test-mac/2026-08-04_msgc_llt_runtime_safe_testlist.txt" \
    "cjc --gc-mode=marksweep -j1"

run_stage \
    "02-runtime-hlt" \
    "$TEST_ROOT/testsuites/HLT/configs/cjnative/linux_aarch64-linux_aarch64/basic.cfg" \
    "$TEST_ROOT/testsuites/HLT/Runtime" \
    "/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/MSGC_RESULT/Tests/Test-mac/2026-08-04_msgc_hlt_runtime_safe_testlist.txt" \
    "cjc --gc-mode=marksweep"

run_stage \
    "03-stdlib-llt" \
    "$TEST_ROOT/testsuites/LLT/configs/cjnative/cjnative_test_linux_aarch64.cfg" \
    "$TEST_ROOT/testsuites/LLT/API" \
    "$RESULT_ROOT/llt_api_testlist.txt" \
    "cjc --gc-mode=marksweep -j1"

run_stage \
    "04-stdlib-hlt" \
    "$TEST_ROOT/testsuites/HLT/configs/cjnative/linux_aarch64-linux_aarch64/basic.cfg" \
    "$TEST_ROOT/testsuites/HLT/API" \
    "" \
    "cjc --gc-mode=marksweep"

run_stage \
    "05-compiler-llt" \
    "$TEST_ROOT/testsuites/LLT/configs/cjnative/cjnative_test_linux_aarch64.cfg" \
    "$TEST_ROOT/testsuites/LLT/compiler" \
    "$RESULT_ROOT/llt_compiler_testlist.txt" \
    "cjc --gc-mode=marksweep -j1"

run_stage \
    "06-compiler-hlt" \
    "$TEST_ROOT/testsuites/HLT/configs/cjnative/linux_aarch64-linux_aarch64/basic.cfg" \
    "$TEST_ROOT/testsuites/HLT/compiler" \
    "" \
    "cjc --gc-mode=marksweep"

printf '%s\n' "$(date '+%F %T%z') all six stages completed" | tee -a "$RUN_ROOT/run.log"
printf 'RESULT_ROOT=%s\n' "$RESULT_ROOT"
printf 'RUN_ROOT=%s\n' "$RUN_ROOT"
