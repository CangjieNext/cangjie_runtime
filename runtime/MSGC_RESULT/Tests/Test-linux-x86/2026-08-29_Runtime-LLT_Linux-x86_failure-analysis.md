# Runtime LLT Linux x86 Failure Analysis

## 1. Scope and result

Analysis target: `Runtime-LLT` executed with the MSGC SDK on Linux x86_64.

- Result directory: `/home/lipeisheng/cangjie/cangjie_runtime_CMSGC/runtime/MSGC_RESULT/Tests/Test-linux-x86/Runtime-LLT`
- Test repository: `/home/lipeisheng/cangjie/cangjie_test`
- SDK: `/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc`
- Runtime LLT summary: 231 executable cases, 227 PASS, 4 FAIL
- Structured result records: 248 = 227 PASS + 4 FAIL + 17 UNRESOLVED

The framework uses `UNRESOLVED` for cases without a valid command. In this
document, those 17 records are classified as `UNSUPPORTED` as requested.

Evidence files:

- `Runtime-LLT/summary.txt`
- `Runtime-LLT/results.json`
- `Runtime-LLT/results.xml`
- `Runtime-LLT/log/*.log`

## 2. Classification overview

| Category | Cases | Count | Direct result | Recommended handling |
| --- | --- | ---: | --- | --- |
| Runtime API test ABI mismatch | `runtime_api_followup.cj`, `runtime_api_task_loader.cj`, `runtime_api_validation.cj` | 3 | C helper aborts with return code 134 during `InitCJRuntime` | Use the canonical SDK `Cangjie.h`, or synchronize the test ABI before rerun |
| Pressure/OOM case exceeds normal LLT boundary | `Expection41_NewArray64.cj` | 1 | Execution times out after 119.4 seconds, framework return code 3 | Move to a controlled pressure/OOM suite or redesign the allocation test |
| Unsupported on Linux CJNative | 17 records | 17 | Framework status `UNRESOLVED`, no valid command | Keep out of this Linux CJNative LLT list, or run with the required platform/backend/toolchain |

No conclusion is made here that the runtime implementation itself is broken
for the three API cases. Their first failure is at the C ABI boundary before
the intended API assertions are exercised.

## 3. Category A: Runtime API test ABI mismatch

### Affected cases

- `Runtime/CJNative/RuntimeApi/runtime_api_followup.cj`
- `Runtime/CJNative/RuntimeApi/runtime_api_task_loader.cj`
- `Runtime/CJNative/RuntimeApi/runtime_api_validation.cj`

### Evidence and cause

All three C helpers include the local test header:

- `RuntimeApi/runtime_api_followup.c:20`
- `RuntimeApi/runtime_api_task_loader.c:18`
- `RuntimeApi/runtime_api_validation.c:13`

The test header defines a reduced `RuntimeParam` ABI in
`RuntimeApi/runtime_api_test.h`:

- `HeapParam` has only the legacy fields at lines 41-49.
- `GCParam` ends at `gcThreads` at lines 51-57.
- `RuntimeParam` is composed from those reduced structures at lines 69-74.

The current runtime public header has additional MSGC fields in `GCParam`,
including `scavengeGrowth`, background sweep/scavenge controls, mark budgets,
and parallel sweep controls (`runtime/src/Cangjie.h:245-400`). Therefore the
offset of `LogParam` and `ConcurrencyParam` in the test-defined structure does
not match the structure expected by the linked MSGC runtime.

The failure output is consistent across the tests:

```text
RuntimeParam.coParam.coStackSize must be in range [64KB, 1GB].
... Assertion `InitCJRuntime(&param) == E_OK' failed.
```

The test passes a zero-initialized `RuntimeParam`, and the current runtime
explicitly permits zero `coStackSize` and resolves it to a default value:

- validation condition allowing zero: `runtime/src/CangjieRuntimeApi.cpp:91-99`
- default resolution for zero: `runtime/src/CangjieRuntimeApi.cpp:363-367`

This means the observed range error is caused by the mismatched structure
layout, not by the documented zero-as-default behavior.

### Case-specific evidence

- `runtime_api_followup.c`: first `InitCJRuntime` assertion at line 74.
- `runtime_api_task_loader.c`: first `InitCJRuntime` assertion at line 46.
- `runtime_api_validation.c`: the validation process aborts at line 80 after
  its invalid-configuration checks have already been attempted.
- Logs: `Runtime-LLT/log/runtime_api_followup_cj_*.log`,
  `runtime_api_task_loader_cj_*.log`, and `runtime_api_validation_cj_*.log`.

### Resolution

1. Change the C helpers to include the SDK/runtime public `Cangjie.h` used by
   the linked `libcangjie-runtime.so`.
2. Remove the duplicated declarations from `runtime_api_test.h`, retaining
   only test-only declarations that are absent from the public header.
3. If a private test header must be retained, make its structures byte-for-byte
   identical to the SDK header and update it whenever `RuntimeParam` changes.
4. Recompile all three `.cj` and C helper combinations with the same SDK, then
   rerun these three cases. Do not reuse binaries produced against an older
   runtime/header ABI.

The preferred fix is to consume the canonical public header rather than
maintain a second ABI definition in the test repository.

## 4. Category B: Pressure/OOM case outside normal LLT boundary

### Affected case

`Runtime/CJNative/Exception/Expection41_NewArray64.cj`

### Evidence and cause

The test allocates:

```cj
let array = Array<Int64>(2147483648) {i => 1}
```

The element payload alone is approximately 16 GiB. The test source expects
`Out of memory` for the non-O2 path (`Expection41_NewArray64.cj:7-11`). In the
Linux x86 run:

- compilation passed;
- execution produced no output;
- execution reached the 120-second framework limit;
- framework return code was 3 (`TimeOut`), while the expected process result
  was 1.

Evidence: `Runtime-LLT/log/Expection41_NewArray64_cj_*.log`, with the recorded
run time of 119.4 seconds and `stderr: TimeOut`.

### Resolution

1. Classify this as a pressure/OOM test rather than a regular Runtime LLT
   smoke case.
2. Run it in a dedicated pressure suite with an explicit memory limit, a
   bounded timeout, and a documented expected OOM behavior.
3. Prefer a smaller deterministic allocation or a direct bounded allocation
   API test if the purpose is only to validate exception handling.
4. Do not increase the normal LLT timeout or treat a 16 GiB allocation timeout
   as an ordinary Runtime LLT regression.

If the case must remain in Linux LLT, the test must be changed so that the
runtime reaches the intended OOM path within the suite timeout and the
expected output/exit-code contract matches the actual test mode.

## 5. Category C: Unsupported cases

The 17 framework `UNRESOLVED` records all contain
`No valid command statement was found.` They are classified as `UNSUPPORTED`
here because the current Linux CJNative LLT configuration has no executable
command for them.

### C1. Android or Apple platform-only cases: 3

- `Runtime/CJNative/DFX/excepetion_handle_on_android.cj`
- `Runtime/CJNative/DFX/hilog_cjrt_test.cj`
- `Runtime/CJNative/DFX/trace_cjrt_test.cj`

Their directives are restricted to Android, or Android/iOS. They require
`logcat`, `hilog`, `atrace`, `xcrun`, or an Apple/Android device environment.

Resolution: exclude them from Linux CJNative LLT and execute them in the
corresponding platform test job with the required device/toolchain.

### C2. Debug-option/IR inspection cases: 6

- `Runtime/CJNative/debuginfo_array_type.cj`
- `Runtime/CJNative/debuginfo_c_func_call.cj`
- `Runtime/CJNative/debuginfo_class_this.cj`
- `Runtime/CJNative/debuginfo_global_init_mangleName.cj`
- `Runtime/CJNative/fast-math-option.cj`
- `Runtime/CJNative/internal_variable.cj`

Their executable directives are guarded by `DEBUG_OPTION` and use
`%frontendCompiler`/IR output checks. The normal Linux CJNative LLT
configuration does not provide a matching command for this condition.

Resolution: run them in a debug-option/frontend IR test job, or exclude them
from the normal Runtime LLT list. They should not be reported as runtime
execution failures when that toolchain mode is absent.

### C3. Source fragments without an executable directive: 2

- `Runtime/CJNative/global_init/test2.cj`
- `Runtime/CJNative/global_init/test3.cj`

These files contain declarations only and have no `EXEC`, `RUN-EXEC`, or
equivalent framework command. They are not standalone test cases under the
current framework.

Resolution: keep them as dependencies/fragments and remove them from the
standalone case scan, or add complete test directives and an entry-point test
if they are intended to be independently executable.

### C4. LLVM-only interop cases: 6

- `Runtime/CJNative/interop/interop_advance_01.cj`
- `Runtime/CJNative/interop/interop_advance_02.cj`
- `Runtime/CJNative/interop/interop_advance_03.cj`
- `Runtime/CJNative/interop/interop_advance_04.cj`
- `Runtime/CJNative/interop/interop_advance_05.cj`
- `Runtime/CJNative/interop/interop_advance_06.cj`

Their directives are guarded by `Linux and LLVM`, while this run uses the
CJNative condition. No matching command is selected for the CJNative backend.

Resolution: run them in the LLVM backend test job, or exclude them from the
CJNative Runtime LLT list. Do not classify them as a Linux platform failure;
the unsupported dimension is the backend.

## 6. Recommended follow-up order

1. Fix the Runtime API test header/ABI and rerun the three API cases.
2. Move `Expection41_NewArray64.cj` to a controlled pressure/OOM list, then
   rerun it under explicit memory and timeout limits.
3. Maintain separate platform/backend/debug-option lists for the 17
   unsupported records.
4. Keep the raw framework result files as the execution record and use this
   document as the categorized failure index.

