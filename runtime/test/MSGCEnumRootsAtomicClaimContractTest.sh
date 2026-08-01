#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Mutator/MutatorManager.h"
SOURCE="$ROOT_DIR/src/Mutator/MutatorManager.cpp"

rg -q 'kMSEnumGCClaimProbeLimit = 256' "$SOURCE"
rg -q 'std::atomic<EnumTransitionState> state' "$HEADER"
rg -q 'std::atomic<size_t> claimCursor' "$HEADER"
rg -q 'std::atomic<size_t> inFlight' "$HEADER"
rg -q 'std::unique_ptr<EnumTransitionEntry\[\]> entries' "$HEADER"

claim="$(sed -n '/size_t MutatorManager::ClaimMSEnumTransitionBatch/,/^}/p' "$SOURCE")"
printf '%s\n' "$claim" | rg -q 'claimCursor\.fetch_add'
printf '%s\n' "$claim" | rg -q 'compare_exchange_strong'
printf '%s\n' "$claim" | rg -q 'probeLimit'
printf '%s\n' "$claim" | rg -q 'requestedProbeLimit'
printf '%s\n' "$claim" | rg -q 'active\.load\(std::memory_order_acquire\)'
if printf '%s\n' "$claim" | rg -q 'lock_guard|unique_lock|visited < entryCount'; then
    echo 'EnumRoots claim path must use bounded lock-free ticket probing' >&2
    exit 1
fi

complete="$(sed -n '/void MutatorManager::CompleteMSEnumTransitionBatch/,/^}/p' "$SOURCE")"
printf '%s\n' "$complete" | rg -q 'state\.store'
printf '%s\n' "$complete" | rg -q 'inFlight\.fetch_sub'
if printf '%s\n' "$complete" | rg -q 'lock_guard|unique_lock'; then
    echo 'EnumRoots completion path must not serialize every batch' >&2
    exit 1
fi

echo 'MSGC EnumRoots transition uses atomic entry claims and bounded ticket probing'
