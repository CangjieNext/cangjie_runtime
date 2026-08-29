# AArch64 GC stackmap register root fix

## 1. Issue

On Linux AArch64, `Runtime LLT/CJNative/GC/gc_async_allocation_finalizer.cj` intermittently aborted during GC root scanning with errors such as:

```text
register x21 is not recorded
wrong reg info
```

The generated stackmap was valid and described `x21`/`x22` as live register roots. The runtime decoder also correctly decoded the register numbers. The failure was that the root scanner had no address for the saved register value in `RegSlotsMap`.

## 2. Root cause

The runtime can scan a mutator from the GC thread after the mutator has entered a saferegion. In this situation, the GC thread cannot read the mutator's live CPU registers directly. The existing code only recovered register values from safepoint or stack-grow stub frames. A cjthread parked through `CJThreadMcall`, or a mutator that entered a saferegion without a safepoint stub frame in the unwind chain, had no register source registered.

On AArch64, `CJThreadContext` is the scheduler's saved register context. `CJThreadContextGet` stores `x18` through `x30` at the fixed context offsets. It is therefore a stable memory representation of the callee-saved registers while the mutator is in a saferegion. The stackmap only consumes the slots for registers explicitly present in its root map.

The LLVM stackmap producer and runtime decoder were not inconsistent, so no LLVM change was required.

## 3. Fix

The runtime changes are:

1. Add a `cjThreadContextValid` flag to `Mutator`.
2. Save the AArch64 cjthread context before `DoEnterSaferegion()` in `Mutator::EnterSaferegion()`.
3. Mark the context valid for the scheduler park path after `CJThreadMcall` has saved the context and before the mutator becomes safe.
4. Clear the flag when the mutator is resumed or reset.
5. Register the saved AArch64 `x18` through `x30` fields in `RegSlotsMap` before scanning managed frames, stack-grow frames, and recorded stack frames.

Modified source files:

- `src/Mutator/Mutator.h`
- `src/Mutator/Mutator.inline.h`
- `src/Mutator/Mutator.cpp`
- `src/UnwindStack/GcStackInfo.cpp`
- `src/UnwindStack/StackGrowStackInfo.cpp`

The exact source patch is saved as `20260829_aarch64_gc_stackmap_register_root.patch` in this directory.

## 4. Build

The cjthread static library was rebuilt first because the existing CMake configure path invokes `build/build_cjthread.sh` without the runtime source working directory. The manual command used was:

```text
cd /home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime
sh build/build_cjthread.sh -p linux_aarch64_cangjie Release STATIC cj "-DMRT_USE_MARK_SWEEP=1" /home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/output/common/linux_release_aarch64 linux_aarch64_cjnative
cmake --build CMakebuild --target cangjie-runtime -- -j4
```

The resulting runtime was copied to the SDK runtime directory used by the test framework:

```text
/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc/runtime/lib/linux_aarch64_cjnative/libcangjie-runtime.so
```

The current fixed runtime SHA256 is:

```text
9f39c2bc8d4d3018d5c24955feff1ca22448f431bdaae4a86afee8cd2dacb9be
```

The pre-fix SDK runtime is preserved at:

```text
libcangjie-runtime.so.before-fix
SHA256: a8dcbc5a4a09bd3486d4658e9317d6fb032f3ce3646d29ce1a84781dcef559f6
```

## 5. Verification

All tests below used the fixed runtime from the SDK path above, the AArch64 Linux cjnative configuration, and serial execution (`-j1`).

| Verification | Result |
| --- | --- |
| Focused `gc_async_allocation_finalizer.cj` framework run | `1 PASS / 0 FAIL` |
| Direct repeated target pressure run | `50 PASS / 0 FAIL` |
| Full Runtime LLT | `227 PASS / 4 FAIL` |
| Full Runtime LLT target case | `gc_async_allocation_finalizer.cj: PASS` |

The full Runtime LLT result is in `runtime-llt-regression/`. Compared with the pre-fix baseline (`226 PASS / 5 FAIL`), the AArch64 GC stackmap failure is eliminated. No `register x21`, `wrong reg info`, `SIGSEGV`, or `SIGABRT` signature was found in the fixed Runtime LLT logs.

The four remaining Runtime LLT failures are unchanged baseline/environment failures:

| Case | Observed reason | Baseline comparison |
| --- | --- | --- |
| `CJNative/Exception/Expection41_NewArray64.cj` | Test timeout, process returned `3` instead of expected `1` | Same timeout and return code in baseline |
| `CJNative/RuntimeApi/runtime_api_validation.cj` | `pthread_create` returned `EAGAIN` (`Resource temporarily unavailable`) | Same error in baseline |
| `CJNative/RuntimeApi/runtime_api_task_loader.cj` | `pthread_create` returned `EAGAIN` (`Resource temporarily unavailable`) | Same error in baseline |
| `CJNative/RuntimeApi/runtime_api_followup.cj` | `coStackSize` outside `[64KB, 1GB]`; `InitCJRuntime` assertion failed | Same error in baseline |

Raw focused results are in `framework-focus-final/`, and the 50-run pressure summary is in `validation-x-repeat-50/summary.txt`.

## 6. Scope and residual risk

This fix targets AArch64 callee-saved register roots that are unavailable from the unwind stack after a mutator enters a saferegion. It does not change LLVM stackmap encoding or make unsupported platform/test cases pass. The remaining full-suite failures require independent test/environment fixes.

The complete six-stage Linux regression sequence was not rerun as part of this focused bugfix verification; the recorded extended regression is Runtime LLT, the stage containing the reproducer.
