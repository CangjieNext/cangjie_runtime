# MSGC Cross-Target Lowering and Multi-Object-Format Emission Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make MSGC lowering target-independent, enable AArch64 ELF emission with correct feature/relocation gates, and add safe X86-64 phase-gated write-barrier emission for Mach-O, ELF, and supported COFF forms.

**Architecture:** Common lowering always emits executable MSGC wrappers and never selects AArch64/X86. The common AsmPrinter classifies wrapper semantics, while AArch64 and X86 target hooks decide whether they can emit a complete fast path; unsupported cases return `false` before emitting instructions and retain the wrapper slow call.

**Tech Stack:** LLVM 15 C++14, LLVM IR/MC layer, AArch64/X86 AsmPrinter, lit/FileCheck, llvm-readobj, llvm-objdump.

**Workspace note:** The source tree already contains uncommitted MSGC work. Execute in the existing workspace, preserve unrelated changes, and use scoped `git diff --check` checkpoints instead of creating commits unless the user explicitly requests commits.

**Execution status (2026-07-21):** Tasks 1-7 are implemented. The targeted LLVM build succeeded; the complete AArch64/X86 MSGC CodeGen selection passed 28/28 lit tests, and the extended GCMode/LTO/Runtime-lowering/CopyGC regression passed 39/39. COFF intentionally uses the verified wrapper fallback because the current phase byte has no stable cross-C++-ABI symbol. X86 non-C calling conventions and tail-call opcodes also use the verified wrapper fallback. SDK/runtime/stdlib builds were not run.

---

### Task 1: Make MSGC write-barrier lowering target-independent

**Files:**

- Modify: `llvm/lib/CodeGen/CJBarrierLowering.cpp`
- Test: `llvm/test/CodeGen/AArch64/CangjieGC/msgc-writebarrier-target-independent-lowering.ll`

- [x] Add a lowering regression that runs the same MSGC IR through both
  `aarch64-apple-darwin` and `x86_64-unknown-linux-gnu` and checks that
  `gcwrite*`/`gcread_generic` become the same MSGC wrapper family before the
  target hook makes an inline decision.
- [x] Run the new checks with the currently installed `llc` and confirm the
  X86 check fails because `CJBarrierLowering` still contains an AArch64 gate.
- [x] Replace `Triple::isAArch64()` conditions in
  `shouldLowerMSGCWriteBarrier`, `replaceWithMSGCWriteStruct`, and
  `replaceWithMSGCWriteGenericAsStruct` with a target-neutral ABI predicate.
  The predicate must be based only on the wrapper ABI/data layout (64-bit
  pointers), not on architecture or object format.
- [x] Preserve GC-mode and feature gates: CopyGC must still use CopyGC calls,
  and disabled MSGC lowering must retain the pre-existing runtime-call path.
- [x] Re-run both target checks and the existing CopyGC/MSGC lowering tests.

Expected lowering rule:

```cpp
bool supportsMSGCWrapperABI(const Module &M) {
  return M.getDataLayout().getPointerSizeInBits() == 64;
}

// Target architecture and object format are intentionally absent here.
return isMSGCMode(M) && EnableGCFastPath && supportsMSGCWrapperABI(M);
```

### Task 2: Split AArch64 MSGC capability predicates

**Files:**

- Modify: `llvm/lib/Target/AArch64/AArch64CangjieCallSize.h`
- Modify: `llvm/lib/Target/AArch64/AArch64CangjieCallSize.cpp`
- Modify: `llvm/include/llvm/CodeGen/CJMSGCABI.h`
- Modify: `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp`
- Test: `llvm/test/CodeGen/AArch64/cangjie-msgc-elf-capability.ll`

- [x] Add RED tests for these independent cases:
  AArch64 Mach-O remains inline-capable, AArch64 ELF `+lse` can inline a write
  barrier, AArch64 ELF `-lse` keeps the wrapper call, and an unsupported object
  format keeps the wrapper call.
- [x] Move the single `enable-msgc-inline-write-barrier` command-line option to
  common CodeGen ownership and expose one query in `CJMSGCABI.h`. Do not define
  a second option with the same spelling in the X86 target.
- [x] Introduce separate AArch64 predicates for allocation and barrier
  emission. Allocation accepts Mach-O/ELF; a write barrier additionally
  requires an atomic capability. Preserve Apple arm64 behavior and require
  `AArch64Subtarget::hasLSE()` for ELF.
- [x] Use exactly the same predicates in `getCangjieCallSizeInBytes` and the
  emitter hook so stackmap call-size accounting cannot diverge from emitted
  instruction selection.
- [x] Re-run capability tests with both `+lse` and `-lse` and verify slow paths
  still contain the original wrapper call.

Expected capability split:

```cpp
bool supportsMSGCAllocation(const Triple &TT) {
  return TT.isOSBinFormatMachO() || TT.isOSBinFormatELF();
}

bool supportsMSGCWriteBarrier(const Triple &TT,
                              const AArch64Subtarget &ST) {
  if (!supportsMSGCAllocation(TT))
    return false;
  return TT.isOSBinFormatMachO() || ST.hasLSE();
}
```

### Task 3: Make AArch64 external symbols and relocations object-format correct

**Files:**

- Modify: `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp`
- Test: `llvm/test/CodeGen/AArch64/cangjie-msgc-elf-symbols.ll`
- Test: `llvm/test/CodeGen/AArch64/cangjie-msgc-elf-relocations.ll`

- [x] Add assembly checks proving semantic runtime names acquire the Mach-O
  leading underscore only through the target mangler, while ELF uses the
  Itanium name exactly once (for example
  `_ZN12MapleRuntime4Heap21inlineFastPathGCPhaseE`).
- [x] Add object checks for ELF external-data access and confirm the current
  direct page/pageoff form fails the expected GOT relocation checks.
- [x] Replace hard-coded Mach-O-decorated phase/heap/region symbol strings with
  semantic symbol names and obtain their `MCSymbol` through
  `Mangler::getNameWithPrefix` plus the module `DataLayout`.
- [x] Route all external runtime-data addresses through object-format-aware GOT
  page/pageoff operands. Keep local basic-block labels direct.
- [x] Apply the same mangling helper to `memcpy` and `memmove`; remove manual
  `_memcpy`/`memcpy` and `_memmove`/`memmove` branches.
- [x] Verify Mach-O assembly is unchanged semantically and ELF objects contain
  the expected GOT relocations using `llvm-readobj -r`.

The emitter must receive semantic names only:

```cpp
static constexpr StringLiteral MSGCPhaseSymbol =
    "_ZN12MapleRuntime4Heap21inlineFastPathGCPhaseE";

MCSymbol *getTargetSymbol(AsmPrinter &AP, StringRef Name) {
  SmallString<128> Mangled;
  Mangler::getNameWithPrefix(Mangled, Name, AP.getDataLayout());
  return AP.OutContext.getOrCreateSymbol(Mangled);
}
```

### Task 4: Add the X86 MSGC target hook and ref write fast path

**Files:**

- Modify: `llvm/lib/Target/X86/X86AsmPrinter.h`
- Modify: `llvm/lib/Target/X86/X86MCInstLower.cpp`
- Create: `llvm/lib/Target/X86/X86MSGCEmitter.cpp`
- Modify: `llvm/lib/Target/X86/CMakeLists.txt`
- Test: `llvm/test/CodeGen/X86/cangjie-msgc-writebarrier-fastpath.ll`

- [x] Add RED tests for X86-64 Mach-O and ELF showing that a lowered MSGC ref
  wrapper is still emitted as an unconditional call.
- [x] Declare `X86AsmPrinter::emitMccWriteBarrierFastPath` and a private MSGC
  emitter entry in `X86AsmPrinter.h`.
- [x] Implement the hook bridge in `X86MCInstLower.cpp`: classify the wrapper,
  lower the slow-call target operand with the existing `X86MCInstLower`, then
  invoke the dedicated emitter. This keeps symbol lowering in the established
  target path and avoids exposing the anonymous lowerer class.
- [x] Add `X86MSGCEmitter.cpp` to CMake. Every emitted instruction must go
  through `EmitAndCountInstruction` so stackmap shadow accounting remains
  correct.
- [x] Emit a phase-gated ref/static-ref path for SysV and Win64 register ABIs:
  load the phase through the format-appropriate indirection, branch to the
  unchanged wrapper call when phase is non-idle, and in idle phase store the
  complete raw 64-bit reference value to the destination field.
- [x] Return `false` before emitting any instruction if target format, ABI,
  symbol access, or wrapper kind is unsupported. Never partially emit and then
  fall back through the common AsmPrinter.
- [x] Verify Mach-O/ELF assembly contains a phase check, a fast store, a join
  label, and the original wrapper call only on the slow branch.

Target hook shape:

```cpp
bool X86AsmPrinter::emitMccWriteBarrierFastPath(
    const MachineInstr *MI, const MachineOperand &MOSym,
    unsigned Opcode) {
  auto Kind = msgc::classifyMSGCWriteBarrier(
      cast<const Function>(MOSym.getGlobal())->getName());
  if (Kind == msgc::MSGCWriteBarrierKind::None)
    return false;

  X86MCInstLower Lowerer(*MI->getMF(), *this);
  Optional<MCOperand> SlowTarget = Lowerer.LowerMachineOperand(MI, MOSym);
  return SlowTarget &&
         emitMccWriteBarrierForMSGC(MI, *SlowTarget, Kind, Opcode);
}
```

### Task 5: Add X86 struct and array-copy idle paths

**Files:**

- Modify: `llvm/lib/Target/X86/X86MSGCEmitter.cpp`
- Test: `llvm/test/CodeGen/X86/cangjie-msgc-writebarrier-copy-fastpath.ll`

- [x] Add RED tests for `Struct`, `StaticStruct`, `ArrayCopyRef`, and
  `ArrayCopyStruct` wrappers under the SysV register ABI.
- [x] Centralize wrapper-kind-to-argument-register mapping; do not duplicate
  SysV register choices inside individual instruction sequences.
- [x] For struct/static-struct wrappers, branch to the slow wrapper when the GC
  phase is non-idle; otherwise call target-mangled `memcpy` with destination,
  source, and byte count in the platform C ABI registers.
- [x] For array-copy wrappers, use the same phase rule and emit target-mangled
  `memmove` in idle phase because source/destination ranges may overlap.
- [x] Preserve the wrapper call at the slow label and converge both paths at an
  explicit join label. Record the Cangjie stackmap once for the complete
  expanded sequence, matching existing target-emitter conventions.
- [x] Run checks for both Darwin and Linux triples, including symbol spelling
  (`_memcpy` on Mach-O and `memcpy` on ELF).

Idle-path contract:

```text
load runtime phase
phase != IDLE  -> call original CJ_MCC_*_Fast_MSGC wrapper
phase == IDLE  -> store / memcpy / memmove
both paths     -> join and continue
```

### Task 6: Add Win64 ABI gating and fallback tests

**Files:**

- Modify: `llvm/lib/Target/X86/X86MSGCEmitter.cpp`
- Test: `llvm/test/CodeGen/X86/cangjie-msgc-writebarrier-win64.ll`

- [x] Add COFF tests for ref/static-ref and copy wrappers before changing the
  emitter. The expected baseline is a safe wrapper call for every kind.
- [x] Implement COFF phase-symbol access only through the supported import or
  reference-pointer form already used by X86 MC lowering. Do not synthesize an
  ELF GOTPCREL expression for COFF.
- [x] Enable Win64 ref/static-ref only when phase loading and register mapping
  are complete (`RCX/RDX/R8` as applicable).
- [x] Keep Win64 struct/static-struct/array-copy wrappers on the slow call until
  their stack-argument offsets and unwind implications are proven. The hook
  must return `false` before emitting anything for these kinds.
- [x] Verify generated COFF objects with `llvm-readobj --symbols --relocations`
  and confirm unsupported kinds remain link-compatible runtime calls.
- [x] Add negative tests for X86-32 and unsupported object formats; lowering
  may produce wrappers only for the supported 64-bit ABI, and backend emission
  must never crash on a manually authored wrapper.

Initial support matrix after this task:

| Target | Ref/static-ref | Struct/array copy | Allocation |
|---|---|---|---|
| X86-64 Mach-O | phase-gated fast path | phase-gated fast path | wrapper |
| X86-64 ELF | phase-gated fast path | phase-gated fast path | wrapper |
| X86-64 COFF | phase-gated when COFF indirection is valid | wrapper | wrapper |
| X86-32/other | wrapper | wrapper | wrapper |

### Task 7: Run targeted regression and update maintained knowledge

**Files:**

- Modify: `MSGC_RESULT/Implementation/Step47_MSGC跨目标Lowering与MachO_ELF_COFF发码扩展设计.md`
- Modify: `/Users/lipeisheng/.codex/skills/cangjie-llvm-skill/references/gc-barriers.md`
- Modify: `/Users/lipeisheng/.codex/skills/cangjie-llvm-skill/references/file-index.md`

- [x] Build only LLVM tools required by this change:

  ```bash
  ninja -C /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build \
    llc FileCheck llvm-readobj llvm-objdump
  ```

- [x] Run every new test directly with build-tree tools, then run the existing
  `AArch64/CangjieGC`, AArch64 MSGC, and X86 Cangjie fast-path tests affected by
  the refactor. Do not start SDK, runtime dylib, stdlib, or stdx builds.
- [x] Inspect Mach-O, ELF, and COFF assembly/object output for phase-symbol and
  libc-call spelling and relocation kinds. A passing textual test is not
  sufficient if an external-data relocation was changed.
- [x] Run scoped source hygiene checks:

  ```bash
  git diff --check -- \
    llvm/include/llvm/CodeGen/CJMSGCABI.h \
    llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp \
    llvm/lib/CodeGen/CJBarrierLowering.cpp \
    llvm/lib/Target/AArch64 llvm/lib/Target/X86 \
    llvm/test/CodeGen/AArch64 llvm/test/CodeGen/X86
  ```

- [x] Update Step47's support matrix and implementation status with only
  verified behavior, including any deliberate COFF fallback.
- [x] Update `gc-barriers.md` with the new ownership boundary (common lowering,
  target capability, final MC emission), and `file-index.md` with
  `X86MSGCEmitter.cpp` and the AArch64 capability/symbol helpers.
- [x] Review the final diff against the user's requested boundary: no
  architecture/object-format selection in common lowering; AArch64/X86
  differences only in target final emission/call-size accounting; no SDK
  build artifacts changed.

## Completion criteria

- Common MSGC lowering contains no AArch64, X86, Mach-O, ELF, or COFF checks.
- AArch64 Mach-O remains functional; AArch64 ELF uses valid symbols and
  relocations, with LSE-gated barrier emission.
- X86-64 Mach-O/ELF have safe phase-gated idle write paths; COFF either has a
  verified ref fast path or deliberately preserves the wrapper call.
- Every unsupported target/kind falls back before target instructions are
  emitted.
- LLVM targeted build/tests pass; SDK build remains intentionally deferred.
