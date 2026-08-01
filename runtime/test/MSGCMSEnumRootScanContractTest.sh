#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MUTATOR="$ROOT_DIR/src/Mutator/Mutator.cpp"

body="$(sed -n '/inline void Mutator::GcPhaseEnum/,/^}/p' "$MUTATOR")"

printf '%s\n' "$body" | rg -q 'AllocBufferMS\* rootBuffer = AllocBufferMS::GetOrCreateAllocBufferMS\(\)'
printf '%s\n' "$body" | rg -q 'rootBuffer->PushRoot\(obj\)'
printf '%s\n' "$body" | rg -q 'unique_ptr<StackObjectTraversal>'
printf '%s\n' "$body" | rg -q 'if \(stackTraversal == nullptr\)'
rg -q 'unordered_set<BaseObject\*>' "$MUTATOR"
rg -q 'vector<BaseObject\*>' "$MUTATOR"

echo 'MSGC mutator root scan caches its sink and lazily handles stack objects'
