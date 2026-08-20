#!/usr/bin/env python3
from __future__ import annotations
import os, shutil, subprocess, sys, threading, time
from datetime import datetime
from pathlib import Path

CASE_DIR = Path(__file__).resolve().parent.parent
MSGCD = Path("/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc")
COPYD = Path("/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie")

def ev(name: str, default: str) -> str: return os.environ.get(name, default)
CFG = {key: ev(key, value) for key, value in {
    "CASE29_SEED":"0", "CASE29_ACCOUNT_COUNT":"1000", "CASE29_SUBJECT_ID":"subject-A",
    "CASE29_LIMIT":"50", "CASE29_OFFSET":"0", "CASE29_CONCURRENCY":"8",
    "CASE29_DURATION_MS":"60000", "HEAP_SIZE":"512mb", "PROCS":"",
}.items()}
def put(path: Path, text: str) -> None: path.write_text(text, encoding="utf-8")

def meta(out: Path, label: str, **extra: str) -> None:
    data = {"case":"case29_http_yinhang", "label":label, "seed":CFG["CASE29_SEED"],
      "account_count":CFG["CASE29_ACCOUNT_COUNT"], "subject_id":CFG["CASE29_SUBJECT_ID"],
      "limit":CFG["CASE29_LIMIT"], "offset":CFG["CASE29_OFFSET"],
      "concurrency":CFG["CASE29_CONCURRENCY"], "duration_ms":CFG["CASE29_DURATION_MS"],
      "heap_size":CFG["HEAP_SIZE"], "procs":CFG["PROCS"] or "host-default",
      "cpu_limit_semantics":"no CPU quota; PROCS is optional runtime-visible processor count",
      "memory_limit_semantics":"Java -Xmx / Cangjie cjHeapSize; RSS is observed, not OS-hard-capped",
      "timestamp":datetime.now().astimezone().isoformat(timespec="seconds"), **extra}
    put(out / "run_metadata.env", "".join(f"{key}={value}\n" for key, value in data.items()))

def sample(pid: int, out: Path, done: threading.Event) -> None:
    with out.open("w") as stream:
        while not done.is_set():
            values = subprocess.run(["ps","-o","rss=,%cpu=","-p",str(pid)], text=True, capture_output=True).stdout.split()
            if len(values) >= 2:
                stream.write(f"{datetime.now().astimezone().isoformat(timespec='milliseconds')},{values[0]},{values[1]}\n"); stream.flush()
            time.sleep(.2)

def execute(command: list[str], environment: dict[str,str], out: Path, stdout: str, stderr: str,
            pid_file: Path | None = None) -> int:
    with (out / stdout).open("w") as so, (out / stderr).open("w") as se:
        child = subprocess.Popen(command, env=environment, stdout=so, stderr=se)
        target_pid = child.pid
        if pid_file is not None:
            deadline = time.monotonic() + 120
            while child.poll() is None and not pid_file.exists() and time.monotonic() < deadline:
                time.sleep(.05)
            if pid_file.exists():
                try:
                    target_pid = int(pid_file.read_text().strip())
                except ValueError:
                    target_pid = child.pid
        done = threading.Event(); worker = threading.Thread(target=sample,args=(child.pid,out/"rss.csv",done),daemon=True); worker.start()
        if target_pid != child.pid:
            done.set(); worker.join(timeout=2)
            done = threading.Event()
            worker = threading.Thread(target=sample,args=(target_pid,out/"rss.csv",done),daemon=True)
            worker.start()
        rc = child.wait(); done.set(); worker.join(timeout=2); return rc

def java_side(out: Path) -> int:
    java, javac = Path(ev("JAVA_BIN","/opt/homebrew/opt/openjdk@25/bin/java")), Path(ev("JAVAC_BIN","/opt/homebrew/opt/openjdk@25/bin/javac"))
    meta(out,"Java",java_bin=str(java),javac_bin=str(javac))
    source, test = CASE_DIR/"java_benchmark/Case29MemberAccountsBenchmark.java", CASE_DIR/"java_benchmark/Case29MemberAccountsBenchmarkTest.java"
    shutil.copy2(source,out/f"{source.name}.snapshot"); shutil.copy2(test,out/f"{test.name}.snapshot")
    classes=out/"classes"; classes.mkdir()
    with (out/"compile.stdout.txt").open("w") as so, (out/"compile.stderr.txt").open("w") as se:
        rc=subprocess.run([str(javac),"--add-modules","jdk.httpserver","-d",str(classes),str(source),str(test)],stdout=so,stderr=se).returncode
    put(out/"compile.exitcode",f"{rc}\n")
    if rc: put(out/"run.exitcode",f"{rc}\n"); return rc
    with (out/"test.stdout.txt").open("w") as so, (out/"test.stderr.txt").open("w") as se:
        rc=subprocess.run([str(java),"--add-modules","jdk.httpserver","-cp",str(classes),"Case29MemberAccountsBenchmarkTest"],stdout=so,stderr=se).returncode
    put(out/"test.exitcode",f"{rc}\n")
    if rc: put(out/"run.exitcode",f"{rc}\n"); return rc
    cmd=[str(java),"--add-modules","jdk.httpserver",f"-Xmx{CFG['HEAP_SIZE'].lower().removesuffix('b')}"]
    if CFG["PROCS"]: cmd.append(f"-XX:ActiveProcessorCount={CFG['PROCS']}")
    cmd += [f"-Xlog:gc*:file={out/'gc.log'}:time,uptime,level,tags","-cp",str(classes),"Case29MemberAccountsBenchmark"]
    rc=execute(cmd,{**os.environ,**CFG,"CASE29_SERVER_PORT":"18229"},out,"stdout.txt","stderr.txt"); put(out/"run.exitcode",f"{rc}\n"); return rc

def cangjie_side(label: str, sdk: Path, stdx_root: Path, mode: str, port: int, out: Path) -> int:
    stdx=stdx_root/"dynamic/stdx"; source_dir=CASE_DIR/"cangjie_benchmark"; source, common=source_dir/"member_accounts_benchmark.cj",source_dir/"member_accounts_common.cj"
    shutil.copy2(source,out/f"{source.name}.snapshot"); shutil.copy2(common,out/f"{common.name}.snapshot")
    meta(out,label,sdk_root=str(sdk),stdx_dir=str(stdx),cjc_gc_mode=mode)
    args='-O2 "$SRC/member_accounts_benchmark.cj" "$SRC/member_accounts_common.cj"'
    if mode: args += f" --gc-mode={mode}"
    script=f'''
source "$SDK/envsetup.sh" >"$OUT/envsetup.stdout.txt" 2>"$OUT/envsetup.stderr.txt"
rc=$?
echo "$rc" >"$OUT/envsetup.exitcode"
[ "$rc" -eq 0 ] || exit "$rc"
export DYLD_LIBRARY_PATH="$STDX:$DYLD_LIBRARY_PATH"
cd "$OUT"
cjc {args} --import-path "$STDX" -L "$STDX" --link-option "$STDX/libstdx.net.http.dylib" --link-option "$STDX/libstdx.encoding.url.dylib" -o benchmark >compile.stdout.txt 2>compile.stderr.txt
rc=$?
echo "$rc" >compile.exitcode
[ "$rc" -eq 0 ] || exit "$rc"
export cjHeapSize="$HEAP_SIZE"
if [ -n "$PROCS" ]; then export cjProcessorNum="$PROCS"; fi
CASE29_SERVER_PORT="$PORT" MRT_REPORT="$OUT/gc.log" ./benchmark &
bench_pid=$!
echo "$bench_pid" >benchmark.pid
wait "$bench_pid"
rc=$?
echo "$rc" >run.exitcode
exit "$rc"
'''
    clean={"HOME":os.environ.get("HOME",""),"TERM":os.environ.get("TERM","xterm"),"PATH":"/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin",**CFG,"SDK":str(sdk),"STDX":str(stdx),"OUT":str(out),"SRC":str(source_dir),"PORT":str(port)}
    rc=execute(["bash","-c",script],clean,out,"stdout.txt","stderr.txt",out/"benchmark.pid")
    if not (out/"run.exitcode").exists(): put(out/"run.exitcode",f"{rc}\n")
    return rc

def main() -> int:
    stamp=datetime.now().strftime("%Y%m%d_%H%M%S")
    root=CASE_DIR/"result"/f"{stamp}_seed{CFG['CASE29_SEED']}_a{CFG['CASE29_ACCOUNT_COUNT']}_c{CFG['CASE29_CONCURRENCY']}_Procs{CFG['PROCS'] or 'Auto'}_compare"
    java_out,msgc_out,copy_out=root/"java",root/"msgc",root/"copygc"
    for out in (java_out,msgc_out,copy_out): out.mkdir(parents=True)
    java_rc=java_side(java_out)
    msgc=Path(ev("MSGC_SDK_ROOT",str(MSGCD)))
    msgc_rc=cangjie_side("MSGC",msgc,Path(ev("MSGC_STDX_ROOT",str(msgc/"darwin_aarch64_cjnative"))),"marksweep",18230,msgc_out)
    copy=Path(ev("COPYGC_SDK_ROOT",str(COPYD)))
    copy_rc=cangjie_side("CopyGC",copy,Path(ev("COPYGC_STDX_ROOT",str(copy.parent/"darwin_aarch64_cjnative"))),"",18231,copy_out)
    analyzer=Path(ev("COMPARE_RESULTS_PY",str(CASE_DIR.parent/"case25_http_complex_json_stream/script/compare_results.py")))
    if analyzer.is_file():
        cache=CASE_DIR/"result/.mplconfig"; cache.mkdir(exist_ok=True)
        subprocess.run([sys.executable,str(analyzer),"--run",f"Java={java_out}","--run",f"MSGC={msgc_out}","--run",f"CopyGC={copy_out}","--output",str(root/"compare_report.md")],env={**os.environ,"MPLCONFIGDIR":str(cache)})
    print(f"[DONE] compare_dir={root}")
    return 0 if not (java_rc or msgc_rc or copy_rc) else 1
if __name__ == "__main__": raise SystemExit(main())
