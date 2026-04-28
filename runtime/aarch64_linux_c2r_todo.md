# aarch64_linux C2R Unwind Todo

## Scope

This checklist is for landing the `C2R` unwind mechanism on `aarch64_linux`.

Two different targets must not be mixed:

- `aarch64_linux` C2R prototype rollout
- Android runtime frame identification fix for `extractNativeLibs=false`

The Android crash root cause should still be handled as a separate top-priority task.

## Current State

What is already true in the tree:

- `C2RSlotData` exists in common definitions.
- `MRT_SaveTopContextToC2RStub` and `MRT_RestoreTopContextFromC2RStub` already exist.
- `x86_64_linux` has a running prototype with slot save and TLS top-context switching.

What is still missing for `aarch64_linux`:

- `C2RStub` frame layout does not reserve `C2RSlotData`.
- `aarch64_linux` `C2RStub` does not call `MRT_SaveTopContextToC2RStub`.
- `aarch64_linux` `C2RStub` does not call `MRT_RestoreTopContextFromC2RStub`.
- `C2RFrame::GetSlotData()` is only implemented for `x86_64_linux`.
- There is still no `UnwindToCallerContextFromC2RStub`.
- Existing `c2r` validation scripts are hard-coded to `linux_x86_64_cjnative`.

## P0

Goal: catch up `aarch64_linux` to the current `x86_64_linux` prototype level.

- [ ] Redesign `src/arch/aarch64_linux/CalleeSavedStub.S` C2R stub frame layout to reserve `C2RSlotData`.
- [ ] Recompute all affected stack offsets in the `aarch64_linux` C2R stub.
- [ ] Update CFI metadata after the new stub layout is introduced.
- [ ] Update frame layout comments in the `aarch64_linux` stub so they match the real stack shape.
- [ ] Insert `MRT_SaveTopContextToC2RStub` before the runtime helper call in `aarch64_linux` C2R stubs.
- [ ] Insert `MRT_RestoreTopContextFromC2RStub` after the runtime helper returns in `aarch64_linux` C2R stubs.
- [ ] Ensure argument save/restore still works after adding the new slot area.
- [ ] Add `aarch64_linux` support to `C2RFrame::GetSlotData()` in `src/Common/StackType.h`.
- [ ] Define the exact `C2RSlotData` offset for the `aarch64_linux` stub layout.
- [ ] Verify `MRT_SaveTopContextToC2RStub` writes `{pc, fa, status}` correctly with the new `aarch64` slot address.
- [ ] Verify `MRT_RestoreTopContextFromC2RStub` restores TLS unwind context correctly on helper exit.
- [ ] Confirm `StackInfo::CheckTopUnwindContextAndInit()` starts unwind directly from the C2R boundary when TLS status is risky.
- [ ] Add `aarch64_linux` runnable validation entry points instead of relying only on `linux_x86_64_cjnative`.
- [ ] Run at least one `cj -> runtime -> cj` nested validation on `aarch64_linux`.

## P1

Goal: validate that the prototype is safe across the main runtime consumers.

- [ ] Verify EH stack building still works on `aarch64_linux`.
- [ ] Verify nested catch scenarios are still correct when a throw happens inside a C2R helper path.
- [ ] Verify print-stack paths unwind from the C2R boundary instead of helper-internal runtime frames.
- [ ] Verify signal stack unwinding works with the C2R top-context switch.
- [ ] Verify GC stack scanning still sees the expected roots after the new C2R stub layout.
- [ ] Verify `C2R/N2C/C2N` nested unwind chains compress runtime helper segments as intended.
- [ ] Add or adapt validation cases for reflective call paths on `aarch64_linux`.
- [ ] Add `StackGrow` coverage; current summary explicitly says this area is not fully covered.
- [ ] Run a release-style regression sweep for the non-StackGrow stack cases on `aarch64_linux`.

## P2

Goal: move from prototype parity to the full mechanism described in the analysis.

- [ ] Implement `UnwindToCallerContextFromC2RStub` in `src/UnwindStack/UnwindContext.cpp`.
- [ ] Extend `UnwindContext::UnwindToCallerContext()` to dispatch on `FrameType::C2R_STUB`.
- [ ] Finalize the `C2R_STUB -> caller` policy for each saved status:
- [ ] `RISKY / SIGNAL_STATUS / UNKNOWN` should recover from slot data.
- [ ] Decide whether `RELIABLE` should still use machine unwind or always use slot recovery.
- [ ] Re-verify EH, GC, signal, print-stack, and nested boundary paths after custom C2R unwind is enabled.
- [ ] Reassess debugging and observability changes caused by the new visible boundary behavior.
- [ ] After `aarch64_linux` is stable, extend validation to Android mainline scenarios.

## Android Separate Track

This is not blocked on the `aarch64_linux` C2R prototype and should be tracked independently.

- [ ] Fix Android runtime frame identification for the `extractNativeLibs=false` scenario.
- [ ] Validate that ordinary runtime frames are no longer misclassified as managed frames.
- [ ] Re-run the original Android crash reproducer after the frame-identification fix.

## Exit Criteria

Prototype-complete:

- `aarch64_linux` has `C2RSlotData + TLS top-context switching`.
- Nested `cj -> runtime -> cj` unwind works.
- EH, print-stack, signal, and GC paths pass targeted validation.

Full-mechanism-complete:

- `C2R_STUB` has dedicated unwind-to-caller logic.
- `StackGrow` and nested boundary regressions pass.
- Android scenario validation is complete.
