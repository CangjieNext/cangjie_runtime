# Windows stackmap / exception-unwind register fix

## Scope

This record covers the Windows HLT failures reporting unrecorded RBX or RSI
while walking a stack-grow frame.

## Root-cause analysis

The earlier Windows HLT result contained 17 fatal failures: 8 reported
unrecorded RBX and 9 reported unrecorded RSI.  The fatal check is in
StackGrowStackInfo.cpp.  It is reached while the runtime is walking a
stack-grow frame; suppressing it would make the collector scan with missing
relocation roots, so it is not a safe fix.

The stack-grow ABI records all saved nonvolatile registers before inspecting
the managed caller.  The compiler emits a stack map after CJ_MCC_StackGrowStub,
and the Windows stub saves the registers in the slots described by
StackMapX86.h.  The investigation found two Windows runtime correctness bugs
on that path:

1. WinModuleManager treated the PE runtime-function EndAddress as inclusive.
   PE specifies an exclusive end.  A PC exactly at a function boundary could
   therefore select the preceding function and its stack map.
2. Both the stack-grow and safepoint return stubs loaded the saved R15 slot
   into RAX instead of R15.  The restored nonvolatile-register state was
   consequently corrupt after returning from either stub.

The original HLT configuration also did not guarantee that the program loaded
the MSGC DLL on Windows: it only set LD_LIBRARY_PATH, which Windows DLL lookup
does not use.  It also omitted --gc-mode marksweep from the CJC options.  A
manual Cangjie recursion compiled without that flag stalled; the same source
compiled with MSGC mode completed immediately.  These environment defects
made the old failures unsuitable for attributing to one source line alone.

## Changes

Runtime:

1. src/os/Windows/WinModuleManager.h now checks
   startAddress <= rvaPc < endAddress.
2. src/arch/x86_64_windows/StackGrowStub.S restores -112(%rbp) into R15.
3. src/arch/x86_64_windows/HandleSafepointStub.S makes the same R15 restore.

Windows cjnative HLT configuration:

1. basic.cfg, O2.cfg and static_O2.cfg prepend the SDK runtime directory to
   PATH in run, run_pkg and run_macro; run_stdx also prepends the stdx
   directory.  This selects the intended runtime DLL on Windows.
2. The same three configurations pass --gc-mode marksweep through cmp_opt
   and compilation_options, so every newly built Windows test matches the
   MSGC runtime.
3. gc_export_004.info uses the Windows command type log.txt rather than the
   unavailable Unix command cat log.txt.

## Validation

The runtime was rebuilt and installed with:

    python build/scripts/rebuild_msgc_runtime_sdk.py --runtime-root C:\cangjie\cangjie_runtime_CMSGC\runtime --sdk-root C:\cangjie\cangjie_compiler_CMSGC\output\msgc --build-type release --version 0.0.1 --skip-clean

The installed SDK DLL is
output/msgc/runtime/lib/windows_x86_64_cjnative/libcangjie-runtime.dll
(SHA-256 D9A889ED82FD24F50A05603697661BC903D3A6BDC01E6B334513FF54757F8256).
llvm-objdump confirms both affected instructions now read:

    movq -0x70(%rbp), %r15

Focused executions, all compiled after installing the runtime:

| Check | Result |
| --- | --- |
| 5,000-level recursive Cangjie stack-growth test, cjStackSize=128kb, --gc-mode marksweep | PASS, printed 2500 and exited 0 in about 0.7 s |
| Existing Windows PE metadata EXE and Cangjie DLL-host regression | PASS, exit 0; reported metadataSize=318848 and dllMetadataSize=37100 |
| gc_export_004 HLT under the updated basic.cfg | The executable compiled with --gc-mode marksweep, ran multiple MSGC root scans and mark cycles, and emitted no missing-RBX/RSI fatal error |

The two HLT result JSON files are:

* stackmap_fix_gc_export_004_msgc.json (before the Windows log-reader fix)
* stackmap_fix_gc_export_004_msgc_after_test_adapter.json (after the fix)

The focused recursion source and its freshly built executable are retained in
this directory as stackgrow_register_restore_regression.cj and .exe.

## Remaining test-adaptation work

gc_export_004 remains a framework FAIL, but not because of the repaired
stackmap path.  Its assertions expect CopyGC-style parameter-report lines
(for example "Heap size: 4096(KB)").  MSGC instead writes [MS-GC] configuration
and collection diagnostics.  The case intentionally supplies invalid
cjAllocationWaitTime=0 and cjGarbageThreshold=-1.05 to test fallback behavior;
MSGC reports them as invalid and continues with defaults.

To make this case a passing MSGC HLT, create MSGC-specific expected patterns
for the [MS-GC] log format and preserve its invalid-input/default-fallback
intent.  Do not weaken the stack-grow missing-register fatal check.
