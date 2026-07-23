#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ALLOC_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
ALLOC_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
CENTRAL_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
CENTRAL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
REGION_MANAGER_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"
REGION_SPACE_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
CANGJIE_HEADER="$ROOT_DIR/src/Cangjie.h"
SCHEDULER_SOURCE="$ROOT_DIR/src/CjScheduler.cpp"
RUNTIME_API_SOURCE="$ROOT_DIR/src/CangjieRuntimeApi.cpp"

extract_function() {
    local file="$1"
    local signature="$2"
    awk -v signature="$signature" '
        $0 == signature { capture = 1 }
        capture {
            print
            line = $0
            opens = gsub(/\{/, "{", line)
            closes = gsub(/\}/, "}", line)
            depth += opens - closes
            if (opens > 0) { saw_open = 1 }
            if (saw_open && depth == 0) { exit }
        }
    ' "$file"
}

for field in sweepAssistDebtScaled_ sweepDebtGen_; do
    if ! rg -q "$field" "$ALLOC_HEADER"; then
        echo "AllocBufferMS must own $field" >&2
        exit 1
    fi
done

if rg -q 'sweepAssistStats_|MSSweepAssistAtomicStats|SnapshotSweepAssistStats' \
    "$ALLOC_HEADER" "$ALLOC_SOURCE" "$CENTRAL_HEADER" "$CENTRAL_SOURCE"; then
    echo "P-level Sweep diagnostics must stay out of the allocation path" >&2
    exit 1
fi

active_log_line="$(rg -n 'WorkBuf\* activeLog_' "$ALLOC_HEADER" | cut -d: -f1)"
sweep_debt_line="$(rg -n 'sweepAssistDebtScaled_' "$ALLOC_HEADER" | tail -1 | cut -d: -f1)"
if [[ -z "$active_log_line" || -z "$sweep_debt_line" || "$sweep_debt_line" -le "$active_log_line" ]]; then
    echo "P-level Sweep metadata must stay after the activeLog_ ABI slot" >&2
    exit 1
fi

if ! rg -q 'offsetof\(AllocBufferMS, activeLog_\) == AllocBufferMS::kActiveLogOffset' "$ALLOC_SOURCE"; then
    echo "the AArch64/X86 activeLog_ 896-byte ABI assertion must remain" >&2
    exit 1
fi

if rg -q 'AssistSweepCredit' "$ALLOC_SOURCE" "$CENTRAL_HEADER" "$CENTRAL_SOURCE" \
    "$REGION_MANAGER_SOURCE" "$REGION_SPACE_SOURCE"; then
    echo "the old process-wide AssistSweepCredit path must be removed" >&2
    exit 1
fi

if rg -q 'ComputeAssistSweepRegions|AssistSweepForFreshRegions' "$CENTRAL_HEADER" "$CENTRAL_SOURCE"; then
    echo "legacy pressure-scaled direct Assist paths must not bypass fixed P-level batching" >&2
    exit 1
fi

if ! rg -q 'sweepRegionsPerAllocScaled_' "$CENTRAL_HEADER" ||
   ! rg -q 'MSSweepCreditPool sweepCredit_' "$CENTRAL_HEADER" ||
   ! rg -q 'MSSweepAssistResult MSSweepAssist\(' "$CENTRAL_HEADER"; then
    echo "CentralRegionPool must publish a fixed-point ratio, one-shot credit, and split P payment" >&2
    exit 1
fi

assist_body="$(extract_function "$CENTRAL_SOURCE" \
    'MSSweepAssistResult CentralRegionPool::MSSweepAssist(AllocBufferMS& owner, size_t debtRegions,')"
if [[ -z "$assist_body" ]] ||
   ! printf '%s\n' "$assist_body" | rg -q 'ComputeMSSweepAssistDirectRegions' ||
   printf '%s\n' "$assist_body" | rg -q 'ScaleMSSweepAssistRatioScaled'; then
    echo "P direct Sweep Assist must run one fixed batch after credit, independent of pressure" >&2
    exit 1
fi
credit_line="$(printf '%s\n' "$assist_body" | rg -n 'sweepCredit_\.Take' | sed -n '1s/:.*//p')"
batch_line="$(printf '%s\n' "$assist_body" | rg -n 'ComputeMSSweepAssistDirectRegions' | sed -n '1s/:.*//p')"
direct_line="$(printf '%s\n' "$assist_body" | rg -n 'AssistSweep\(' | sed -n '1s/:.*//p')"
if [[ -z "$credit_line" || -z "$batch_line" || -z "$direct_line" ||
      "$credit_line" -ge "$batch_line" || "$batch_line" -ge "$direct_line" ]]; then
    echo "P Assist must claim central credit before selecting and running one direct batch" >&2
    exit 1
fi

if ! rg -q 'cjMSAssistSweepMaxRegionsPerCall", 4,' "$SCHEDULER_SOURCE" ||
   ! rg -Uq 'msAssistSweepMaxRegionsPerCall == 0 \?[[:space:]]*4 :' "$RUNTIME_API_SOURCE" ||
   ! rg -Uq 'Maximum regions swept by one MSGC P-level assist-sweep batch\.[[:space:][:print:]]*Default to 4' \
       "$CANGJIE_HEADER"; then
    echo "cjMSAssistSweepMaxRegionsPerCall must default to one four-Region P batch" >&2
    exit 1
fi

if rg -q 'allocFreshRegionsThisCycle_' "$CENTRAL_HEADER" "$CENTRAL_SOURCE"; then
    echo "allocation progress must be P-local, not a process-wide fresh-region counter" >&2
    exit 1
fi

slow_alloc="$(extract_function "$ALLOC_SOURCE" 'MAddress AllocBufferMS::SlowAlloc(uint8_t cls)')"
if [[ -z "$slow_alloc" ]]; then
    echo "failed to extract AllocBufferMS::SlowAlloc" >&2
    exit 1
fi
if ! printf '%s\n' "$slow_alloc" | awk '
    /activeRegions_\[cls\] = newRegion/ { published = NR }
    /AccumulateSweepDebt\(1, cls\)/ { charged = NR }
    END { exit !(published > 0 && charged > published) }
'; then
    echo "small allocation must add one P Sweep-debt Region after publishing the active Region" >&2
    exit 1
fi

refill="$(extract_function "$ALLOC_SOURCE" 'MSRegionInfo* AllocBufferMS::RefillFromCentral(uint8_t cls)')"
if [[ -z "$refill" ]]; then
    echo "failed to extract AllocBufferMS::RefillFromCentral" >&2
    exit 1
fi
if printf '%s\n' "$refill" | rg -q 'AccumulateSweepDebt|MSSweepAssist'; then
    echo "foreground refill must not charge or repay a P Sweep debt before the Region is published" >&2
    exit 1
fi

large_alloc="$(extract_function "$REGION_SPACE_SOURCE" \
    'MAddress RegionSpaceMS::TryAllocateLargeObjectOnce(size_t objectBytes, size_t allocSize, ThreadLocalData* local,')"
if [[ -z "$large_alloc" ]]; then
    echo "failed to extract RegionSpaceMS::TryAllocateLargeObjectOnce" >&2
    exit 1
fi
large_charge_count="$(printf '%s\n' "$large_alloc" | rg -c 'AccumulateSweepDebt\(regionBytes / MS_REGION_SIZE, kNumSizeClasses\)' || true)"
if [[ "$large_charge_count" -ne 2 ]]; then
    echo "both successful large-object paths must charge P Sweep debt exactly once" >&2
    exit 1
fi
if ! printf '%s\n' "$large_alloc" | awk '
    /RegisterLargeObjectMS\(/ { registrations++ }
    /AccumulateSweepDebt\(/ {
        charges++
        if (registrations < charges) { invalid = 1 }
    }
    END { exit !(registrations == 2 && charges == 2 && invalid == 0) }
'; then
    echo "large-object Sweep debt must be charged only after successful registration" >&2
    exit 1
fi

take_large="$(extract_function "$REGION_MANAGER_SOURCE" \
    'MSRegionInfo* MSRegionManager::TakeRegionForLarge(size_t unitCount, uintptr_t& regionStart,')"
if printf '%s\n' "$take_large" | rg -q 'AssistSweepCredit|AccumulateSweepDebt|MSSweepAssist'; then
    echo "TakeRegionForLarge must not charge failed or not-yet-successful allocations" >&2
    exit 1
fi

echo "MSGC P-level Sweep debt contract holds"
