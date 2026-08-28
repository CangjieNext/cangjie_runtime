#!/usr/bin/env bash
# Bash entry point for the Windows case25 runner.
#
# Use this from Git Bash, MSYS2, Cygwin, or WSL.  Benchmark configuration is
# deliberately the same environment-variable interface as run_compare_linux.sh;
# the Windows-specific implementation remains in run_compare_windows.ps1.
set -euo pipefail

case_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ps_script="$case_dir/run_compare_windows.ps1"

if (($# != 0)); then
    echo "[ERROR] positional arguments are not supported; configure the runner with environment variables." >&2
    exit 2
fi
if [[ ! -f "$ps_script" ]]; then
    echo "[ERROR] Windows runner not found: $ps_script" >&2
    exit 1
fi

to_windows_path() {
    local value="$1"
    case "$value" in
        [[:alpha:]]:[\\/]*|\\\\*)
            printf '%s\n' "$value"
            ;;
        *)
            if command -v cygpath >/dev/null 2>&1; then
                cygpath -aw "$value"
            elif command -v wslpath >/dev/null 2>&1; then
                wslpath -w "$value"
            else
                # This is useful when Bash receives an already usable Windows
                # path variant that was not matched above.
                printf '%s\n' "$value"
            fi
            ;;
    esac
}

# These variables are paths in the shared Linux/Windows command interface.
# Convert only explicit overrides; the PowerShell runner owns its Windows
# defaults and must retain them unchanged.
path_variables=(
    MSGC_SDK_ROOT MSGC_STDX_ROOT
    COPYGC_PACKAGE_ROOT COPYGC_SDK_ROOT COPYGC_STDX_ROOT
    GO_BIN PYTHON_BIN JAVA_HOME
)
for name in "${path_variables[@]}"; do
    if [[ -n "${!name:-}" ]]; then
        converted="$(to_windows_path "${!name}")"
        printf -v "$name" '%s' "$converted"
        export "$name"
    fi
done

# Linux accepts JAVA_BIN/JAVAC_BIN.  The unchanged PowerShell runner discovers
# both programs through JAVA_HOME, so derive it from either executable when
# needed.  A supplied JAVA_HOME remains authoritative.
java_executable="${JAVA_BIN:-${JAVAC_BIN:-}}"
java_executable_name="java"
[[ -z "${JAVA_BIN:-}" && -n "${JAVAC_BIN:-}" ]] && java_executable_name="javac"
if [[ -n "$java_executable" && -z "${JAVA_HOME:-}" ]]; then
    java_bin="$(to_windows_path "$java_executable")"
    case "$java_bin" in
        *\\bin\\java.exe) JAVA_HOME="${java_bin%\\bin\\java.exe}" ;;
        *\\bin\\javac.exe) JAVA_HOME="${java_bin%\\bin\\javac.exe}" ;;
        */bin/java.exe) JAVA_HOME="${java_bin%/bin/java.exe}" ;;
        */bin/javac.exe) JAVA_HOME="${java_bin%/bin/javac.exe}" ;;
        *)
            echo "[ERROR] $java_executable_name executable must be under a JDK bin directory: $java_executable" >&2
            exit 2
            ;;
    esac
    export JAVA_HOME
elif [[ -n "$java_executable" && -n "${JAVA_HOME:-}" ]]; then
    echo "[WARN] JAVA_HOME is set; JAVA_BIN/JAVAC_BIN are ignored by the Windows PowerShell runner." >&2
fi

# Windows has no taskset equivalent in the existing PowerShell runner.  PROCS
# still limits the JVM's ActiveProcessorCount and Cangjie processor count.
if [[ -n "${JAVA_CPUSET:-}" ]]; then
    echo "[WARN] JAVA_CPUSET is Linux-only and is ignored on Windows; use PROCS instead." >&2
fi

powershell_bin="${POWERSHELL_BIN:-}"
if [[ -z "$powershell_bin" ]]; then
    if command -v pwsh.exe >/dev/null 2>&1; then
        powershell_bin="$(command -v pwsh.exe)"
    elif command -v powershell.exe >/dev/null 2>&1; then
        powershell_bin="$(command -v powershell.exe)"
    else
        echo "[ERROR] pwsh.exe or powershell.exe is required to run the Windows benchmark." >&2
        exit 1
    fi
fi

ps_script="$(to_windows_path "$ps_script")"
echo "[INFO] delegating to: $ps_script"
exec env MSYS_NO_PATHCONV=1 "$powershell_bin" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "$ps_script"
