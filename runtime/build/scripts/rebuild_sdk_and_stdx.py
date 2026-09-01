#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import pathlib
import platform
import shlex
import stat
import shutil
import subprocess
import sys
import sysconfig
from dataclasses import dataclass
from typing import Iterable, Mapping


LINUX_COMPILER_LLVM_SHARED_LIBRARY_PATTERNS = (
    "libLLVM*.so*",
    "libLTO*.so*",
)
WINDOWS_COMPILER_LLVM_SHARED_LIBRARY_PATTERNS = (
    "libLLVM*.dll",
)
WINDOWS_COMPILER_LLVM_TOOL_NAMES = (
    "ld.lld.exe",
    "llc.exe",
    "llvm-ar.exe",
    "opt.exe",
)
WINDOWS_SDK_LLVM_RUNTIME_NAMES = (
    "ld.exe",
    "libgcc_s_seh-1.dll",
    "libiconv-2.dll",
    "libintl-8.dll",
    "libstdc++-6.dll",
    "libwinpthread-1.dll",
    "libzstd.dll",
    "zlib1.dll",
)
WINDOWS_COMPILER_DIAGNOSTIC_ARTIFACT_NAMES = (
    "cjc-bfd.exe",
    "cjc-debug.exe",
    "cjc-map.exe",
    "cjc-map2.exe",
    "cjc-pdatafix.exe",
    "cjc-symbols.exe",
)


@dataclass(frozen=True)
class RepoLayout:
    workspace_root: pathlib.Path
    compiler_root: pathlib.Path
    runtime_root: pathlib.Path
    stdlib_root: pathlib.Path
    stdx_root: pathlib.Path


@dataclass(frozen=True)
class HostLayout:
    os_name: str
    arch: str
    sdk_platform_name: str

    def runtime_common_name(self, build_type: str) -> str:
        return f"{self.os_name}_{build_type}_{self.arch}"


@dataclass(frozen=True)
class BuildConfig:
    compiler_root: pathlib.Path
    runtime_root: pathlib.Path
    stdlib_root: pathlib.Path
    stdx_root: pathlib.Path
    sdk_root: pathlib.Path
    build_type: str
    version: str
    jobs: int
    openssl_lib: pathlib.Path
    run_verify: bool
    dry_run: bool


@dataclass(frozen=True)
class BuildStage:
    name: str
    cwd: pathlib.Path
    command: list[str]
    envsetup: pathlib.Path | None = None


def compiler_cjnative_build_dir(config: BuildConfig) -> pathlib.Path:
    return (
        config.compiler_root
        / "build"
        / "build"
        / "third_party"
        / "cjnative-prefix"
        / "src"
        / "cjnative-build"
    )


def resolve_repo_layout(script_path: pathlib.Path) -> RepoLayout:
    runtime_root = script_path.resolve().parents[2]
    workspace_root = runtime_root.parents[1]
    return RepoLayout(
        workspace_root=workspace_root,
        compiler_root=workspace_root / "cangjie_compiler",
        runtime_root=runtime_root,
        stdlib_root=runtime_root.parent / "stdlib",
        stdx_root=workspace_root / "cangjie_stdx",
    )


def detect_host_layout(system_name: str | None = None, machine_name: str | None = None) -> HostLayout:
    system_name = (system_name or platform.system()).lower()
    machine_name = (machine_name or platform.machine()).lower()

    if system_name == "darwin":
        os_name = "darwin"
    elif system_name == "linux":
        os_name = "linux"
    elif system_name == "windows":
        os_name = "windows"
    else:
        raise ValueError(f"unsupported host system: {system_name}")

    arch_alias = {
        "arm64": "aarch64",
        "aarch64": "aarch64",
        "x86_64": "x86_64",
        "amd64": "x86_64",
        "arm": "arm",
        "arm32": "arm",
    }
    arch = arch_alias.get(machine_name)
    if arch is None:
        raise ValueError(f"unsupported host architecture: {machine_name}")

    return HostLayout(
        os_name=os_name,
        arch=arch,
        sdk_platform_name=f"{os_name}_{arch}_cjnative",
    )


def build_stage_plan(config: BuildConfig, host: HostLayout) -> list[BuildStage]:
    compiler_envsetup = config.sdk_root / "envsetup.sh"
    return [
        BuildStage(
            name="compiler",
            cwd=config.compiler_root,
            command=[
            "python3",
            "build.py",
            "build",
            "-t",
            config.build_type,
            "--product",
            "cjc",
            "--no-tests",
            "-j",
            str(config.jobs),
            ],
        ),
        BuildStage(
            name="compiler-llvm",
            cwd=compiler_cjnative_build_dir(config),
            command=[
                "cmake",
                "--build",
                ".",
                "--parallel",
                str(config.jobs),
                "--target",
                "LLVM",
                "llc",
                "LTO",
                "llvm-ar",
                "opt",
                "lld",
            ],
        ),
        BuildStage(
            name="compiler",
            cwd=config.compiler_root,
            command=[
            "python3",
            "build.py",
            "install",
            "--prefix",
            str(config.sdk_root),
            ],
        ),
        BuildStage(
            name="runtime",
            cwd=config.runtime_root,
            command=["python3", "build.py", "clean"],
        ),
        BuildStage(
            name="runtime",
            cwd=config.runtime_root,
            command=[
            "python3",
            "build.py",
            "build",
            "--target",
            "native",
            "--build-type",
            config.build_type,
            "--gc-mode",
            "marksweep",
            "-v",
            config.version,
            ],
        ),
        BuildStage(
            name="runtime",
            cwd=config.runtime_root,
            command=[
            "python3",
            "build.py",
            "install",
            "--prefix",
            str(config.runtime_root / "output" / "common"),
            ],
        ),
        BuildStage(
            name="stdlib",
            cwd=config.stdlib_root,
            command=["python3", "build.py", "clean"],
        ),
        BuildStage(
            name="stdlib",
            cwd=config.stdlib_root,
            command=[
            "python3",
            "build.py",
            "build",
            "-t",
            config.build_type,
            "--target-lib",
            str(config.runtime_root / "output"),
            "--target-lib",
            str(config.openssl_lib),
            "-j",
            str(config.jobs),
            ],
            envsetup=compiler_envsetup,
        ),
        BuildStage(
            name="stdlib",
            cwd=config.stdlib_root,
            command=[
            "python3",
            "build.py",
            "install",
            "--prefix",
            str(config.stdlib_root / "output"),
            ],
            envsetup=compiler_envsetup,
        ),
        BuildStage(
            name="stdx",
            cwd=config.stdx_root,
            command=["python3", "build.py", "clean"],
        ),
        BuildStage(
            name="stdx",
            cwd=config.stdx_root,
            command=[
            "python3",
            "build.py",
            "build",
            "-t",
            config.build_type,
            "--target-lib",
            str(config.openssl_lib),
            "--include",
            str(config.compiler_root / "include"),
            "--jobs",
            str(config.jobs),
            ],
            envsetup=compiler_envsetup,
        ),
        BuildStage(
            name="stdx",
            cwd=config.stdx_root,
            command=[
            "python3",
            "build.py",
            "install",
            "--prefix",
            str(config.stdx_root / "target"),
            ],
            envsetup=compiler_envsetup,
        ),
    ]


def build_stage_commands(config: BuildConfig, host: HostLayout) -> list[list[str]]:
    return [stage.command for stage in build_stage_plan(config, host)]


def build_copy_plan(config: BuildConfig, host: HostLayout) -> list[tuple[pathlib.Path, pathlib.Path]]:
    runtime_common_dir = (
        config.runtime_root
        / "output"
        / "common"
        / host.runtime_common_name(config.build_type)
    )
    return [
        (runtime_common_dir / "lib", config.sdk_root / "lib"),
        (runtime_common_dir / "runtime", config.sdk_root / "runtime"),
        (config.stdlib_root / "output" / "lib", config.sdk_root / "lib"),
        (config.stdlib_root / "output" / "modules", config.sdk_root / "modules"),
        (config.stdlib_root / "output" / "runtime", config.sdk_root / "runtime"),
        (
            config.stdx_root / "target" / host.sdk_platform_name,
            config.sdk_root / host.sdk_platform_name,
        ),
    ]


def build_runtime_sdk_sync_plan(config: BuildConfig, host: HostLayout) -> list[tuple[pathlib.Path, pathlib.Path]]:
    runtime_common_dir = (
        config.runtime_root
        / "output"
        / "common"
        / host.runtime_common_name(config.build_type)
    )
    return [
        (runtime_common_dir / "lib", config.sdk_root / "lib"),
        (runtime_common_dir / "runtime", config.sdk_root / "runtime"),
    ]


def build_stdlib_sdk_sync_plan(config: BuildConfig, host: HostLayout) -> list[tuple[pathlib.Path, pathlib.Path]]:
    # Overlay the freshly-installed stdlib output into the SDK *before* stdx is
    # built. stdx compilation dlopens std macro plugins (e.g. @Derive) and
    # imports std .cjo from the SDK (CANGJIE_HOME), so the SDK must already hold
    # the just-built std here — otherwise stdx links/loads stale std artifacts.
    return [
        (config.stdlib_root / "output" / "lib", config.sdk_root / "lib"),
        (config.stdlib_root / "output" / "modules", config.sdk_root / "modules"),
        (config.stdlib_root / "output" / "runtime", config.sdk_root / "runtime"),
    ]


def build_compiler_llvm_sync_plan(
    config: BuildConfig,
    host: HostLayout,
) -> list[tuple[pathlib.Path, pathlib.Path]]:
    cjnative_build = compiler_cjnative_build_dir(config)
    llvm_sdk_root = config.sdk_root / "third_party" / "llvm"

    if host.os_name == "windows":
        build_bin = cjnative_build / "bin"
        sdk_bin = llvm_sdk_root / "bin"
        tools = [build_bin / name for name in WINDOWS_COMPILER_LLVM_TOOL_NAMES]
        shared_libs = discover_compiler_llvm_shared_libraries(
            build_bin,
            WINDOWS_COMPILER_LLVM_SHARED_LIBRARY_PATTERNS,
        )
        return [(src, sdk_bin / src.name) for src in tools + shared_libs]

    if host.os_name == "darwin":
        shared_libs = [
            cjnative_build / "lib" / "libLLVM.dylib",
            cjnative_build / "lib" / "libLTO.dylib",
        ]
    else:
        shared_libs = discover_linux_compiler_llvm_shared_libraries(
            cjnative_build / "lib"
        )

    plan: list[tuple[pathlib.Path, pathlib.Path]] = [
        (cjnative_build / "bin", llvm_sdk_root / "bin"),
    ]
    for shared_lib in shared_libs:
        plan.append((shared_lib, llvm_sdk_root / "lib" / shared_lib.name))
    return plan


def discover_linux_compiler_llvm_shared_libraries(
    lib_dir: pathlib.Path,
) -> list[pathlib.Path]:
    shared_libs: list[pathlib.Path] = []
    for pattern in LINUX_COMPILER_LLVM_SHARED_LIBRARY_PATTERNS:
        matches = sorted(
            (
                path
                for path in lib_dir.glob(pattern)
                if path.is_file() or path.is_symlink()
            ),
            key=lambda path: (path.is_symlink(), path.name),
        )
        if not matches:
            raise FileNotFoundError(
                f"compiler LLVM shared libraries do not match {pattern}: {lib_dir}"
            )
        shared_libs.extend(matches)
    return shared_libs


def discover_compiler_llvm_shared_libraries(
    lib_dir: pathlib.Path,
    patterns: tuple[str, ...],
) -> list[pathlib.Path]:
    shared_libs: list[pathlib.Path] = []
    for pattern in patterns:
        matches = sorted(path for path in lib_dir.glob(pattern) if path.is_file())
        if not matches:
            raise FileNotFoundError(
                f"compiler LLVM shared libraries do not match {pattern}: {lib_dir}"
            )
        shared_libs.extend(matches)
    return shared_libs


def build_runtime_relayout_plan(
    config: BuildConfig,
    host: HostLayout,
) -> list[tuple[pathlib.Path, pathlib.Path]]:
    runtime_common_root = config.runtime_root / "output" / "common"
    runtime_common_dir = runtime_common_root / host.runtime_common_name(config.build_type)
    runtime_platform_dir = pathlib.Path(host.sdk_platform_name)

    return [
        (
            runtime_common_root / "lib" / runtime_platform_dir,
            runtime_common_dir / "lib" / runtime_platform_dir,
        ),
        (
            runtime_common_root / "runtime" / "lib" / runtime_platform_dir,
            runtime_common_dir / "runtime" / "lib" / runtime_platform_dir,
        ),
    ]


def build_codesign_targets(config: BuildConfig, host: HostLayout) -> list[pathlib.Path]:
    if host.os_name != "darwin":
        return []
    return [
        config.sdk_root / "third_party" / "llvm" / "lib",
        config.sdk_root / "runtime" / "lib" / host.sdk_platform_name,
        config.sdk_root / host.sdk_platform_name,
        config.stdx_root / "target" / host.sdk_platform_name,
    ]


def wrap_with_envsetup(cmd: list[str], envsetup: pathlib.Path) -> list[str]:
    if platform.system().lower() == "windows":
        def quote(value: str) -> str:
            return "'" + value.replace("'", "''") + "'"

        command = list(cmd)
        if command and command[0] == "python3":
            command[0] = sys.executable
        ps_command = "; ".join([
            f". {quote(str(envsetup))}",
            "& " + " ".join(quote(part) for part in command),
        ])
        return [
            "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-Command", ps_command,
        ]
    shell_cmd = f'source "{envsetup}" >/dev/null 2>&1 && {shlex.join(cmd)}'
    return ["bash", "-lc", shell_cmd]


def run_command(
    cmd: list[str],
    cwd: pathlib.Path,
    dry_run: bool,
    envsetup: pathlib.Path | None = None,
    environment: dict[str, str] | None = None,
) -> None:
    effective_cmd = wrap_with_envsetup(cmd, envsetup) if envsetup else list(cmd)
    if effective_cmd and effective_cmd[0] == "python3":
        effective_cmd[0] = sys.executable
    env_note = f" envsetup={envsetup}" if envsetup else ""
    print(f"[run] cwd={cwd}{env_note} cmd={' '.join(effective_cmd)}", flush=True)
    if dry_run:
        return
    run_env = dict(os.environ) if environment is None else dict(environment)
    subprocess.run(effective_cmd, cwd=cwd, check=True, env=run_env)


def copy_tree(src: pathlib.Path, dst: pathlib.Path, dry_run: bool) -> None:
    print(f"[copy] {src} -> {dst}", flush=True)
    if dry_run:
        return
    if not src.exists():
        raise FileNotFoundError(f"copy source does not exist: {src}")
    make_path_writable(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, dst, dirs_exist_ok=True)


def copy_file(src: pathlib.Path, dst: pathlib.Path, dry_run: bool) -> None:
    print(f"[copy] {src} -> {dst}", flush=True)
    if dry_run:
        return
    if not src.is_file() and not src.is_symlink():
        raise FileNotFoundError(f"copy source file does not exist: {src}")
    make_path_writable(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    source_is_symlink = src.is_symlink()
    if dst.is_symlink() or (source_is_symlink and dst.exists()):
        dst.unlink()
    shutil.copy2(src, dst, follow_symlinks=not source_is_symlink)


def make_path_writable(path: pathlib.Path) -> None:
    if not path.exists():
        return
    if path.is_symlink():
        return

    def add_user_write(target: pathlib.Path) -> None:
        mode = target.stat().st_mode
        extra_bits = stat.S_IWUSR
        if target.is_dir():
            extra_bits |= stat.S_IXUSR
        target.chmod(mode | extra_bits)

    add_user_write(path)
    if path.is_dir():
        for child in path.rglob("*"):
            if child.is_symlink():
                continue
            add_user_write(child)


def repair_runtime_output_layout(config: BuildConfig, host: HostLayout, dry_run: bool) -> None:
    for src, dst in build_runtime_relayout_plan(config, host):
        copy_tree(src, dst, dry_run=dry_run)


def sync_runtime_into_sdk(config: BuildConfig, host: HostLayout, dry_run: bool) -> None:
    for src, dst in build_runtime_sdk_sync_plan(config, host):
        copy_tree(src, dst, dry_run=dry_run)


def sync_stdlib_into_sdk(config: BuildConfig, host: HostLayout, dry_run: bool) -> None:
    for src, dst in build_stdlib_sdk_sync_plan(config, host):
        copy_tree(src, dst, dry_run=dry_run)


def cleanup_windows_compiler_diagnostic_artifacts(
    config: BuildConfig,
    host: HostLayout,
    dry_run: bool,
) -> None:
    if host.os_name != "windows":
        return

    bin_dirs = (
        config.sdk_root / "bin",
        config.compiler_root / "build" / "build" / "bin",
    )
    for bin_dir in bin_dirs:
        for name in WINDOWS_COMPILER_DIAGNOSTIC_ARTIFACT_NAMES:
            path = bin_dir / name
            if not path.is_file():
                continue
            print(f"[remove diagnostic artifact] {path}", flush=True)
            if not dry_run:
                make_path_writable(path)
                path.unlink()


def sync_compiler_llvm_into_sdk(config: BuildConfig, host: HostLayout, dry_run: bool) -> None:
    if host.os_name == "windows":
        cleanup_windows_compiler_diagnostic_artifacts(config, host, dry_run)
        if dry_run:
            build_bin = compiler_cjnative_build_dir(config) / "bin"
            sdk_bin = config.sdk_root / "third_party" / "llvm" / "bin"
            for name in WINDOWS_COMPILER_LLVM_TOOL_NAMES:
                print(f"[copy] {build_bin / name} -> {sdk_bin / name}", flush=True)
            for pattern in WINDOWS_COMPILER_LLVM_SHARED_LIBRARY_PATTERNS:
                print(f"[copy] {build_bin / pattern} -> {sdk_bin}", flush=True)
            return

        plan = build_compiler_llvm_sync_plan(config, host)
        for src, dst in plan:
            copy_file(src, dst, dry_run=dry_run)

        sdk_bin = config.sdk_root / "third_party" / "llvm" / "bin"
        required_names = {
            *(dst.name for _, dst in plan),
            *WINDOWS_SDK_LLVM_RUNTIME_NAMES,
        }
        if not dry_run:
            missing = sorted(name for name in required_names if not (sdk_bin / name).is_file())
            if missing:
                raise FileNotFoundError(
                    "Windows SDK LLVM runtime files were not installed: " + ", ".join(missing)
                )

        if sdk_bin.exists():
            for path in sorted(sdk_bin.iterdir()):
                if not path.is_file() or path.name in required_names:
                    continue
                print(f"[remove] {path}", flush=True)
                if not dry_run:
                    make_path_writable(path)
                    path.unlink()

        llvm_lib = config.sdk_root / "third_party" / "llvm" / "lib"
        for pattern in ("libLLVM*.dll", "libLTO*.dll"):
            for path in sorted(llvm_lib.glob(pattern)) if llvm_lib.exists() else []:
                print(f"[remove] {path}", flush=True)
                if not dry_run:
                    make_path_writable(path)
                    path.unlink()
        return

    if host.os_name == "linux" and dry_run:
        cjnative_build = compiler_cjnative_build_dir(config)
        llvm_sdk_root = config.sdk_root / "third_party" / "llvm"
        copy_tree(cjnative_build / "bin", llvm_sdk_root / "bin", dry_run=True)
        for pattern in LINUX_COMPILER_LLVM_SHARED_LIBRARY_PATTERNS:
            print(
                f"[copy] {cjnative_build / 'lib' / pattern} -> "
                f"{llvm_sdk_root / 'lib'}",
                flush=True,
            )
        return

    for src, dst in build_compiler_llvm_sync_plan(config, host):
        if src.is_dir() or src.name == "bin":
            copy_tree(src, dst, dry_run=dry_run)
        else:
            copy_file(src, dst, dry_run=dry_run)


def iter_dylibs(root: pathlib.Path) -> Iterable[pathlib.Path]:
    if not root.exists():
        return []
    return sorted(path for path in root.rglob("*.dylib") if path.is_file())


def codesign_tree(root: pathlib.Path, dry_run: bool) -> None:
    dylibs = list(iter_dylibs(root))
    if not dylibs:
        print(f"[codesign] skip empty tree: {root}", flush=True)
        return
    for dylib in dylibs:
        cmd = ["codesign", "-f", "-s", "-", str(dylib)]
        print(f"[codesign] {' '.join(cmd)}", flush=True)
        if not dry_run:
            subprocess.run(cmd, check=True)


def verify_outputs(config: BuildConfig, host: HostLayout) -> None:
    envsetup = config.sdk_root / "envsetup.sh"
    stdx_dir = config.sdk_root / host.sdk_platform_name / "dynamic" / "stdx"
    runtime_dir = config.sdk_root / "runtime" / "lib" / host.sdk_platform_name

    if not envsetup.is_file():
        raise FileNotFoundError(f"missing SDK envsetup: {envsetup}")
    if not stdx_dir.is_dir():
        raise FileNotFoundError(f"missing SDK stdx dir: {stdx_dir}")
    if not runtime_dir.is_dir():
        raise FileNotFoundError(f"missing SDK runtime dir: {runtime_dir}")

    clean_env = {
        "HOME": os.environ.get("HOME", ""),
        "TERM": os.environ.get("TERM", "xterm"),
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin",
    }
    probe = (
        f'source "{envsetup}" >/dev/null 2>&1 && '
        f'cjc -v && '
        f'test -d "{stdx_dir}"'
    )
    cmd = ["bash", "-c", probe]
    print(f"[verify] {' '.join(cmd)}", flush=True)
    if not config.dry_run:
        subprocess.run(cmd, check=True, env=clean_env)


def resolve_default_openssl_lib(
    *,
    system_name: str | None = None,
    machine_name: str | None = None,
    multiarch_name: str | None = None,
    env: Mapping[str, str] | None = None,
) -> pathlib.Path:
    system_name = (system_name or platform.system()).lower()
    machine_name = (machine_name or platform.machine()).lower()
    env = os.environ if env is None else env
    candidates: list[pathlib.Path] = []

    def add_openssl_path(value: str) -> None:
        path = pathlib.Path(value).expanduser()
        candidates.append(path)
        if path.name.lower() != "lib":
            candidates.append(path / "lib")

    for env_name in ("OPENSSL_PATH", "OPENSSL_ROOT_DIR"):
        env_value = env.get(env_name)
        if env_value:
            add_openssl_path(env_value)

    if system_name == "linux":
        detected_multiarch = multiarch_name or sysconfig.get_config_var("MULTIARCH")
        fallback_multiarch = {
            "aarch64": "aarch64-linux-gnu",
            "arm64": "aarch64-linux-gnu",
            "x86_64": "x86_64-linux-gnu",
            "amd64": "x86_64-linux-gnu",
            "arm": "arm-linux-gnueabihf",
            "arm32": "arm-linux-gnueabihf",
        }.get(machine_name)
        multiarch_names = [
            name
            for name in (detected_multiarch, fallback_multiarch)
            if name
        ]
        for name in dict.fromkeys(multiarch_names):
            candidates.extend(
                [
                    pathlib.Path("/usr/local/lib") / name,
                    pathlib.Path("/usr/lib") / name,
                    pathlib.Path("/lib") / name,
                ]
            )
        candidates.extend(
            [
                pathlib.Path("/usr/local/lib64"),
                pathlib.Path("/usr/lib64"),
                pathlib.Path("/lib64"),
                pathlib.Path("/usr/local/lib"),
                pathlib.Path("/usr/lib"),
                pathlib.Path("/lib"),
            ]
        )
    elif system_name == "windows":
        gcc = shutil.which("gcc")
        if gcc:
            candidates.append(pathlib.Path(gcc).resolve().parent.parent / "lib")
        for env_name in ("CANGJIE_MINGW_PATH", "MINGW_PATH", "MSYS2_MINGW64"):
            value = env.get(env_name)
            if value:
                mingw_root = pathlib.Path(value).expanduser()
                if mingw_root.name.lower() == "bin":
                    mingw_root = mingw_root.parent
                candidates.append(mingw_root / "lib")
        candidates.extend(
            [
                pathlib.Path("C:/software/msys64/mingw64/lib"),
                pathlib.Path("C:/msys64/mingw64/lib"),
                pathlib.Path("C:/software/msys64/ucrt64/lib"),
                pathlib.Path("C:/msys64/ucrt64/lib"),
            ]
        )
    else:
        candidates.extend(
            [
                pathlib.Path("/opt/homebrew/opt/openssl@3/lib"),
                pathlib.Path("/opt/homebrew/Cellar/openssl@3/3.6.1/lib"),
                pathlib.Path("/usr/local/opt/openssl@3/lib"),
                pathlib.Path("/usr/local/opt/openssl/lib"),
            ]
        )

    for candidate in candidates:
        if system_name == "windows":
            has_openssl = all(
                (candidate / name).is_file()
                for name in ("libcrypto.dll.a", "libssl.dll.a")
            )
            if candidate.is_dir() and has_openssl:
                return candidate
        elif candidate.is_dir():
            return candidate
    raise FileNotFoundError(
        "unable to locate OpenSSL lib dir; set OPENSSL_PATH or pass --openssl-lib"
    )


def parse_args() -> BuildConfig:
    script_path = pathlib.Path(__file__).resolve()
    layout = resolve_repo_layout(script_path)

    parser = argparse.ArgumentParser(
        description="Incrementally rebuild cjc, then rebuild MSGC SDK and stdx."
    )
    parser.add_argument("--compiler-root", default=str(layout.compiler_root))
    parser.add_argument("--runtime-root", default=str(layout.runtime_root))
    parser.add_argument("--stdlib-root", default=str(layout.stdlib_root))
    parser.add_argument("--stdx-root", default=str(layout.stdx_root))
    parser.add_argument("--sdk-root", default=str(layout.compiler_root / "output"))
    parser.add_argument(
        "--build-type",
        choices=["release", "debug", "relwithdebinfo"],
        default="release",
    )
    parser.add_argument(
        "--version",
        default=os.environ.get("CANGJIE_VERSION", "0.0.1"),
    )
    parser.add_argument("--jobs", type=int, default=max(os.cpu_count() or 1, 1))
    parser.add_argument("--openssl-lib", default=str(resolve_default_openssl_lib()))
    parser.add_argument("--skip-verify", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    return BuildConfig(
        compiler_root=pathlib.Path(args.compiler_root).resolve(),
        runtime_root=pathlib.Path(args.runtime_root).resolve(),
        stdlib_root=pathlib.Path(args.stdlib_root).resolve(),
        stdx_root=pathlib.Path(args.stdx_root).resolve(),
        sdk_root=pathlib.Path(args.sdk_root).resolve(),
        build_type=args.build_type,
        version=args.version,
        jobs=args.jobs,
        openssl_lib=pathlib.Path(args.openssl_lib).resolve(),
        run_verify=not args.skip_verify,
        dry_run=args.dry_run,
    )


def ensure_paths_exist(config: BuildConfig) -> None:
    required = {
        "compiler_root": config.compiler_root,
        "runtime_root": config.runtime_root,
        "stdlib_root": config.stdlib_root,
        "stdx_root": config.stdx_root,
        "openssl_lib": config.openssl_lib,
    }
    for name, path in required.items():
        if not path.exists():
            raise FileNotFoundError(f"{name} does not exist: {path}")


def main() -> int:
    try:
        config = parse_args()
        ensure_paths_exist(config)
        host = detect_host_layout()
    except Exception as exc:
        print(f"[error] {exc}", file=sys.stderr)
        return 1

    try:
        for stage in build_stage_plan(config, host):
            print(f"\n== {stage.name} ==", flush=True)
            run_command(
                stage.command,
                cwd=stage.cwd,
                dry_run=config.dry_run,
                envsetup=stage.envsetup,
            )
            if (
                stage.name == "compiler"
                and stage.cwd == config.compiler_root
                and stage.command[0:3] == ["python3", "build.py", "install"]
            ):
                print("\n== sync compiler llvm into SDK ==", flush=True)
                sync_compiler_llvm_into_sdk(config, host, dry_run=config.dry_run)
                if host.os_name == "darwin":
                    print("\n== codesign compiler llvm dylibs ==", flush=True)
                    codesign_tree(
                        config.sdk_root / "third_party" / "llvm" / "lib",
                        dry_run=config.dry_run,
                    )
            if (
                stage.name == "runtime"
                and stage.cwd == config.runtime_root
                and stage.command[0:3] == ["python3", "build.py", "install"]
            ):
                print("\n== repair runtime output layout ==", flush=True)
                repair_runtime_output_layout(config, host, dry_run=config.dry_run)
                print("\n== sync runtime into SDK ==", flush=True)
                sync_runtime_into_sdk(config, host, dry_run=config.dry_run)
                if host.os_name == "darwin":
                    print("\n== codesign runtime dylibs ==", flush=True)
                    codesign_tree(
                        config.sdk_root / "runtime" / "lib" / host.sdk_platform_name,
                        dry_run=config.dry_run,
                    )
            if (
                stage.name == "stdlib"
                and stage.cwd == config.stdlib_root
                and stage.command[0:3] == ["python3", "build.py", "install"]
            ):
                # Must run before the stdx stage: stdx compilation dlopens std
                # macro plugins and imports std .cjo from the SDK, so the SDK has
                # to hold the just-built std at this point, not only after the
                # final overlay.
                print("\n== sync stdlib into SDK ==", flush=True)
                sync_stdlib_into_sdk(config, host, dry_run=config.dry_run)
                if host.os_name == "darwin":
                    print("\n== codesign stdlib dylibs ==", flush=True)
                    codesign_tree(
                        config.sdk_root / "runtime" / "lib" / host.sdk_platform_name,
                        dry_run=config.dry_run,
                    )

        print("\n== overlay outputs into SDK ==", flush=True)
        for src, dst in build_copy_plan(config, host):
            copy_tree(src, dst, dry_run=config.dry_run)

        print("\n== codesign dylibs ==", flush=True)
        for root in build_codesign_targets(config, host):
            codesign_tree(root, dry_run=config.dry_run)

        if config.run_verify:
            print("\n== verify SDK ==", flush=True)
            verify_outputs(config, host)
    except subprocess.CalledProcessError as exc:
        print(f"[error] command failed with exit code {exc.returncode}: {exc.cmd}", file=sys.stderr)
        return exc.returncode
    except Exception as exc:
        print(f"[error] {exc}", file=sys.stderr)
        return 1

    stdx_dir = config.sdk_root / host.sdk_platform_name / "dynamic" / "stdx"
    print("\nBuild completed.", flush=True)
    print(f"SDK root : {config.sdk_root}", flush=True)
    print(f"STDX dir : {stdx_dir}", flush=True)
    print(f"Source   : {config.sdk_root / 'envsetup.sh'}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
