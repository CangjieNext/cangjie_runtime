#!/usr/bin/env python3

from __future__ import annotations

import argparse
import datetime
import hashlib
import os
import pathlib
import platform
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
from collections.abc import Mapping, Sequence
from dataclasses import dataclass


DEFAULT_MSGC_SDK_ROOT_DARWIN = pathlib.Path(
    "/Users/lipeisheng/code/cangjie/cangjie/"
    "cangjie_compiler_region_mark/output/msgc"
)

DEFAULT_MSGC_SDK_ROOT_LINUX = pathlib.Path(
    "/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc"
)
DEFAULT_MSGC_SDK_ROOT_WINDOWS = pathlib.Path(
    r"C:\cangjie\cangjie_compiler_CMSGC\output\msgc"
)


@dataclass(frozen=True)
class RepoLayout:
    workspace_root: pathlib.Path
    compiler_root: pathlib.Path
    runtime_root: pathlib.Path


@dataclass(frozen=True)
class HostLayout:
    os_name: str
    arch: str
    sdk_platform_name: str

    def runtime_common_name(self, build_type: str) -> str:
        return f"{self.os_name}_{build_type}_{self.arch}"


@dataclass(frozen=True)
class BuildConfig:
    runtime_root: pathlib.Path
    sdk_root: pathlib.Path
    build_type: str
    version: str
    skip_clean: bool
    verify_output: bool
    dry_run: bool


@dataclass(frozen=True)
class BuildStage:
    cwd: pathlib.Path
    command: list[str]


@dataclass(frozen=True)
class VerificationItem:
    label: str
    compare_mode: str
    source: pathlib.Path
    installed: pathlib.Path


DARWIN_BUILD_TOOL_DIRS = (
    pathlib.Path("/opt/homebrew/opt/llvm@16/bin"),
    pathlib.Path("/opt/homebrew/opt/m4/bin"),
    pathlib.Path("/opt/homebrew/bin"),
    pathlib.Path("/usr/local/bin"),
)
WINDOWS_BUILD_TOOL_DIRS = (
    pathlib.Path(r"C:\software\msys64\mingw64\bin"),
    pathlib.Path(r"C:\msys64\mingw64\bin"),
    pathlib.Path(r"C:\software\msys64\ucrt64\bin"),
    pathlib.Path(r"C:\msys64\ucrt64\bin"),
    pathlib.Path(r"C:\software\msys64\clang64\bin"),
    pathlib.Path(r"C:\msys64\clang64\bin"),
    pathlib.Path(r"C:\Program Files\LLVM\bin"),
    pathlib.Path(r"C:\software\CMake\bin"),
    pathlib.Path(r"C:\Program Files\CMake\bin"),
    pathlib.Path(r"C:\software\Git\cmd"),
    pathlib.Path(r"C:\software\Git\usr\bin"),
    pathlib.Path(r"C:\Program Files\Git\cmd"),
    pathlib.Path(r"C:\Program Files\Git\usr\bin"),
)
WINDOWS_TOOLCHAIN_ENV_NAMES = (
    "CANGJIE_MINGW_PATH",
    "MINGW_PATH",
    "MSYS2_MINGW64",
)
WINDOWS_MSYS_ROOT_ENV_NAMES = ("MSYS2_ROOT", "MSYS2_HOME")
WINDOWS_TOOLCHAIN_NAMES = ("mingw64", "ucrt64", "clang64")
WINDOWS_REQUIRED_TOOLS = (
    "cmake",
    "gcc",
    "g++",
    "mingw32-make",
    "ninja",
    "patch",
    "git",
)
WINDOWS_PACKAGE_PREFIXES = {
    "mingw64": "mingw-w64-x86_64",
    "ucrt64": "mingw-w64-ucrt-x86_64",
    "clang64": "mingw-w64-clang-x86_64",
}


def _normalise_windows_toolchain_root(value: str | pathlib.Path) -> pathlib.Path:
    path = pathlib.Path(value).expanduser()
    return path.parent if path.name.lower() == "bin" else path


def _windows_toolchain_candidates(environment: Mapping[str, str]) -> list[pathlib.Path]:
    candidates: list[pathlib.Path] = []

    for name in WINDOWS_TOOLCHAIN_ENV_NAMES:
        value = environment.get(name)
        if value:
            candidates.append(_normalise_windows_toolchain_root(value))

    for name in WINDOWS_MSYS_ROOT_ENV_NAMES:
        value = environment.get(name)
        if not value:
            continue
        msys_root = pathlib.Path(value).expanduser()
        candidates.extend(msys_root / toolchain for toolchain in WINDOWS_TOOLCHAIN_NAMES)

    for tool_dir in WINDOWS_BUILD_TOOL_DIRS:
        if tool_dir.name.lower() == "bin" and tool_dir.parent.name.lower() in WINDOWS_TOOLCHAIN_NAMES:
            candidates.append(tool_dir.parent)

    for tool in ("x86_64-w64-mingw32-gcc", "gcc"):
        executable = shutil.which(tool, path=environment.get("PATH", ""))
        if executable:
            candidates.append(pathlib.Path(executable).resolve().parent.parent)

    return list(dict.fromkeys(candidates))


def _windows_toolchain_is_usable(root: pathlib.Path) -> bool:
    bin_dir = root / "bin"
    return all(
        any((bin_dir / name).is_file() for name in names)
        for names in (
            ("x86_64-w64-mingw32-gcc.exe", "x86_64-w64-mingw32-gcc"),
            ("x86_64-w64-mingw32-g++.exe", "x86_64-w64-mingw32-g++"),
        )
    )


def resolve_windows_toolchain_root(
    environment: Mapping[str, str] | None = None,
    *,
    require_tools: bool = True,
) -> pathlib.Path:
    environment = os.environ if environment is None else environment
    candidates = _windows_toolchain_candidates(environment)
    for candidate in candidates:
        if _windows_toolchain_is_usable(candidate):
            return candidate
    if not require_tools:
        for candidate in candidates:
            if candidate.is_dir():
                return candidate
    raise FileNotFoundError(
        "unable to locate a usable MinGW toolchain; set "
        "CANGJIE_MINGW_PATH to its mingw64/ucrt64 root"
    )


def _windows_pacman(
    toolchain_root: pathlib.Path,
    environment: Mapping[str, str],
) -> pathlib.Path | None:
    candidates = [toolchain_root.parent / "usr" / "bin" / "pacman.exe"]
    for name in WINDOWS_MSYS_ROOT_ENV_NAMES:
        value = environment.get(name)
        if value:
            candidates.append(pathlib.Path(value) / "usr" / "bin" / "pacman.exe")
    candidates.extend(
        [
            pathlib.Path(r"C:\software\msys64\usr\bin\pacman.exe"),
            pathlib.Path(r"C:\msys64\usr\bin\pacman.exe"),
        ]
    )
    pacman = shutil.which("pacman", path=environment.get("PATH", ""))
    if pacman:
        candidates.append(pathlib.Path(pacman))
    return next((candidate for candidate in dict.fromkeys(candidates) if candidate.is_file()), None)


def _windows_openssl_lib(
    toolchain_root: pathlib.Path,
    environment: Mapping[str, str],
) -> pathlib.Path | None:
    candidates: list[pathlib.Path] = []
    for name in ("OPENSSL_ROOT_DIR", "OPENSSL_PATH"):
        value = environment.get(name)
        if not value:
            continue
        path = pathlib.Path(value).expanduser()
        candidates.append(path if path.name.lower() == "lib" else path / "lib")
        candidates.append(path)
    candidates.append(toolchain_root / "lib")
    for candidate in dict.fromkeys(candidates):
        if candidate.is_dir() and all(
            (candidate / name).is_file()
            for name in ("libcrypto.dll.a", "libssl.dll.a")
        ):
            return candidate
    return None


def _install_windows_packages(
    toolchain_root: pathlib.Path,
    packages: list[str],
    environment: Mapping[str, str],
) -> None:
    if not packages:
        return
    pacman = _windows_pacman(toolchain_root, environment)
    if pacman is None:
        raise FileNotFoundError(
            "missing Windows build dependencies and pacman was not found; "
            "install MSYS2 or set MSYS2_ROOT/MSYS2_HOME"
        )
    command = [str(pacman), "-S", "--needed", "--noconfirm", *packages]
    print(f"[install] {shlex.join(command)}", flush=True)
    subprocess.run(command, check=True, env=dict(environment))


def build_subprocess_path(
    host: HostLayout,
    current_path: str,
    darwin_tool_dirs: list[pathlib.Path] | tuple[pathlib.Path, ...] | None = None,
    environment: Mapping[str, str] | None = None,
) -> str:
    tool_dirs = darwin_tool_dirs if darwin_tool_dirs is not None else DARWIN_BUILD_TOOL_DIRS
    path_entries: list[str] = []
    if host.os_name == "darwin":
        path_entries.extend(str(path) for path in tool_dirs if path.is_dir())
    elif host.os_name == "windows":
        environment = os.environ if environment is None else environment
        try:
            toolchain_root = resolve_windows_toolchain_root(
                environment,
                require_tools=False,
            )
            path_entries.extend(
                [
                    str(toolchain_root / "bin"),
                    str(toolchain_root.parent / "usr" / "bin"),
                ]
            )
        except FileNotFoundError:
            pass
        path_entries.extend(str(path) for path in WINDOWS_BUILD_TOOL_DIRS if path.is_dir())
    path_entries.extend(entry for entry in current_path.split(os.pathsep) if entry)
    return os.pathsep.join(dict.fromkeys(path_entries))


def build_subprocess_environment(
    host: HostLayout,
    validate_tools: bool = True,
    install_missing: bool = True,
) -> dict[str, str]:
    environment = os.environ.copy()
    environment["PATH"] = build_subprocess_path(
        host,
        environment.get("PATH", ""),
        environment=environment,
    )
    if host.os_name == "windows":
        toolchain_root = resolve_windows_toolchain_root(environment)
        environment["PATH"] = build_subprocess_path(
            host,
            environment.get("PATH", ""),
            environment=environment,
        )
        # Do not let a compiler environment prepared for the cross-build leak
        # into the native CMake configure or its nested cjthread configure.
        environment.pop("CC", None)
        environment.pop("CXX", None)
        environment.pop("CFLAGS", None)
        environment.pop("CXXFLAGS", None)
        environment["CMAKE_C_COMPILER"] = "gcc"
        environment["CMAKE_CXX_COMPILER"] = "g++"
        # CMake fetches FlatBuffers with a nested Git process.  Windows Git
        # rejects the repository's :Zone.Identifier names unless NTFS
        # protection is disabled for this build process.
        environment["GIT_CONFIG_COUNT"] = "1"
        environment["GIT_CONFIG_KEY_0"] = "core.protectNTFS"
        environment["GIT_CONFIG_VALUE_0"] = "false"
    if not validate_tools:
        return environment

    if host.os_name == "windows":
        required_tools = WINDOWS_REQUIRED_TOOLS
        missing_tools = [
            tool
            for tool in required_tools
            if shutil.which(tool, path=environment["PATH"]) is None
        ]
        missing_packages: list[str] = []
        package_prefix = WINDOWS_PACKAGE_PREFIXES.get(toolchain_root.name.lower())
        if package_prefix is None:
            raise ValueError(
                f"unsupported MinGW toolchain layout: {toolchain_root}; "
                "expected mingw64, ucrt64, or clang64"
            )
        package_for_tool = {
            "mingw32-make": f"{package_prefix}-make",
            "ninja": f"{package_prefix}-ninja",
        }
        missing_packages.extend(
            package_for_tool[tool]
            for tool in missing_tools
            if tool in package_for_tool
        )
        if _windows_openssl_lib(toolchain_root, environment) is None:
            missing_packages.append(f"{package_prefix}-openssl")
        missing_packages.extend(
            tool for tool in missing_tools if tool in {"git", "patch"}
        )
        if missing_packages:
            if not install_missing:
                raise FileNotFoundError(
                    "required Windows build dependencies are not available in PATH: "
                    + ", ".join(missing_tools)
                )
            _install_windows_packages(
                toolchain_root,
                list(dict.fromkeys(missing_packages)),
                environment,
            )
            environment["PATH"] = build_subprocess_path(
                host,
                environment.get("PATH", ""),
                environment=environment,
            )
            missing_tools = [
                tool
                for tool in required_tools
                if shutil.which(tool, path=environment["PATH"]) is None
            ]
        if _windows_openssl_lib(toolchain_root, environment) is None:
            raise FileNotFoundError(
                "OpenSSL MinGW libraries were not found under "
                f"{toolchain_root / 'lib'}"
            )
        environment["OPENSSL_ROOT_DIR"] = str(toolchain_root)
    else:
        required_tools = ("cmake", "make", "clang", "clang++")
    missing_tools = [
        tool for tool in required_tools if shutil.which(tool, path=environment["PATH"]) is None
    ]
    if missing_tools:
        raise FileNotFoundError(
            "required runtime build tools are not available in PATH: " + ", ".join(missing_tools)
        )
    for tool in required_tools:
        print(f"[info] {tool}={shutil.which(tool, path=environment['PATH'])}", flush=True)
    return environment


def prepare_windows_build_environment(
    host: HostLayout,
    *,
    validate_tools: bool = True,
    install_missing: bool = True,
) -> dict[str, str]:
    if host.os_name != "windows":
        return os.environ.copy()
    environment = build_subprocess_environment(
        host,
        validate_tools=validate_tools,
        install_missing=install_missing,
    )
    for name in ("CC", "CXX", "CFLAGS", "CXXFLAGS"):
        os.environ.pop(name, None)
    os.environ.update(
        {
            "PATH": environment["PATH"],
            "CMAKE_C_COMPILER": environment["CMAKE_C_COMPILER"],
            "CMAKE_CXX_COMPILER": environment["CMAKE_CXX_COMPILER"],
            "GIT_CONFIG_COUNT": environment["GIT_CONFIG_COUNT"],
            "GIT_CONFIG_KEY_0": environment["GIT_CONFIG_KEY_0"],
            "GIT_CONFIG_VALUE_0": environment["GIT_CONFIG_VALUE_0"],
        }
    )
    if "OPENSSL_ROOT_DIR" in environment:
        os.environ["OPENSSL_ROOT_DIR"] = environment["OPENSSL_ROOT_DIR"]
    return environment


def resolve_repo_layout(script_path: pathlib.Path) -> RepoLayout:
    runtime_root = script_path.resolve().parents[2]
    workspace_root = runtime_root.parents[1]
    return RepoLayout(
        workspace_root=workspace_root,
        compiler_root=workspace_root / "cangjie_compiler",
        runtime_root=runtime_root,
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


def default_msgc_sdk_root(host: HostLayout) -> pathlib.Path:
    if host.os_name == "darwin":
        return DEFAULT_MSGC_SDK_ROOT_DARWIN
    if host.os_name == "linux":
        return DEFAULT_MSGC_SDK_ROOT_LINUX
    if host.os_name == "windows":
        return DEFAULT_MSGC_SDK_ROOT_WINDOWS
    raise ValueError(f"unsupported host system for MSGC SDK: {host.os_name}")


def build_stage_plan(config: BuildConfig, host: HostLayout) -> list[BuildStage]:
    stages: list[BuildStage] = []
    if not config.skip_clean:
        stages.append(
            BuildStage(
                cwd=config.runtime_root,
                command=[sys.executable, "build.py", "clean"],
            )
        )
    runtime_target = "windows-x86_64" if host.os_name == "windows" else "native"
    runtime_build_command = [
        sys.executable,
        "build.py",
        "build",
        "--target",
        runtime_target,
        "--build-type",
        config.build_type,
        "--gc-mode",
        "marksweep",
        "-v",
        config.version,
    ]
    if host.os_name == "windows":
        # ``native`` leaves WINDOWS_FLAG disabled.  The explicit Windows
        # target also requires the MinGW root, even when building on Windows.
        runtime_build_command.extend(
            ["--target-toolchain", str(resolve_windows_toolchain_root())]
        )
    stages.extend(
        [
            BuildStage(
                cwd=config.runtime_root,
                command=runtime_build_command,
            ),
            BuildStage(
                cwd=config.runtime_root,
                command=[
                    sys.executable,
                    "build.py",
                    "install",
                    "--prefix",
                    str(config.runtime_root / "output" / "common"),
                ],
            ),
        ]
    )
    return stages


def runtime_static_artifact_names(host: HostLayout) -> list[str]:
    artifacts = ["cjstart.o", "libcangjie-aio.a"]
    if host.os_name == "windows":
        artifacts.append("libcangjie-thread.a")
    if host.os_name == "darwin":
        artifacts.append("section.o")
    artifacts.append("libcangjie-runtime.a")
    return artifacts


def runtime_shared_library_suffix(host: HostLayout) -> str:
    if host.os_name == "windows":
        return ".dll"
    return ".dylib" if host.os_name == "darwin" else ".so"


def build_runtime_relayout_plan(
    config: BuildConfig,
    host: HostLayout,
) -> list[tuple[pathlib.Path, pathlib.Path]]:
    runtime_common_root = config.runtime_root / "output" / "common"
    runtime_common_dir = runtime_common_root / host.runtime_common_name(config.build_type)
    platform_dir = host.sdk_platform_name
    static_root = runtime_common_root / "lib" / platform_dir
    static_output = runtime_common_dir / "lib" / platform_dir
    shared_root = runtime_common_root / "runtime" / "lib" / platform_dir
    shared_output = runtime_common_dir / "runtime" / "lib" / platform_dir
    suffix = runtime_shared_library_suffix(host)

    relayout: list[tuple[pathlib.Path, pathlib.Path]] = []
    for artifact in runtime_static_artifact_names(host):
        # build.py installs all runtime archives in the common prefix. Some
        # older cjthread builds may also leave an archive in the build-type
        # directory, so use it only when the common-prefix artifact is absent.
        source = static_root / artifact
        if not source.is_file():
            source = runtime_common_dir / "lib" / platform_dir / artifact
        destination = static_output / artifact
        if source != destination:
            relayout.append((source, destination))

    return relayout + [
        (shared_root / f"libcangjie-runtime{suffix}", shared_output / f"libcangjie-runtime{suffix}"),
        (shared_root / f"libcangjie-trace{suffix}", shared_output / f"libcangjie-trace{suffix}"),
        *(
            [(shared_root / "libboundscheck.dll", shared_output / "libboundscheck.dll")]
            if host.os_name == "windows"
            else []
        ),
    ]


def build_copy_plan(
    config: BuildConfig,
    host: HostLayout,
) -> list[tuple[pathlib.Path, pathlib.Path]]:
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


def build_codesign_targets(config: BuildConfig, host: HostLayout) -> list[pathlib.Path]:
    if host.os_name != "darwin":
        return []
    return [config.sdk_root / "runtime" / "lib" / host.sdk_platform_name]
def build_verification_plan(config: BuildConfig, host: HostLayout) -> list[VerificationItem]:
    runtime_common_dir = (
        config.runtime_root
        / "output"
        / "common"
        / host.runtime_common_name(config.build_type)
    )
    platform_dir = host.sdk_platform_name
    static_root = runtime_common_dir / "lib" / platform_dir
    installed_static_root = config.sdk_root / "lib" / platform_dir
    shared_root = runtime_common_dir / "runtime" / "lib" / platform_dir
    installed_shared_root = config.sdk_root / "runtime" / "lib" / platform_dir
    items = [
        VerificationItem(
            label=artifact,
            compare_mode="sha256",
            source=static_root / artifact,
            installed=installed_static_root / artifact,
        )
        for artifact in runtime_static_artifact_names(host)
    ]
    suffix = runtime_shared_library_suffix(host)
    shared_artifacts = [f"libcangjie-runtime{suffix}", f"libcangjie-trace{suffix}"]
    if host.os_name == "windows":
        shared_artifacts.append("libboundscheck.dll")
    for artifact in shared_artifacts:
        items.append(
            VerificationItem(
                label=artifact,
                compare_mode="symbols",
                source=shared_root / artifact,
                installed=installed_shared_root / artifact,
            )
        )
    return items


def run_command(
    cmd: list[str],
    cwd: pathlib.Path,
    dry_run: bool,
    environment: dict[str, str] | None = None,
) -> None:
    print(f"[run] cwd={cwd} cmd={shlex.join(cmd)}", flush=True)
    if dry_run:
        return
    subprocess.run(cmd, cwd=cwd, check=True, env=environment)


def make_path_writable(path: pathlib.Path) -> None:
    if not path.exists() or path.is_symlink():
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


def copy_file(src: pathlib.Path, dst: pathlib.Path, dry_run: bool) -> None:
    print(f"[copy] {src} -> {dst}", flush=True)
    if dry_run:
        return
    if not src.is_file():
        raise FileNotFoundError(f"copy source file does not exist: {src}")
    make_path_writable(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def copy_tree(src: pathlib.Path, dst: pathlib.Path, dry_run: bool) -> None:
    print(f"[copy] {src} -> {dst}", flush=True)
    if dry_run:
        return
    if not src.exists():
        raise FileNotFoundError(f"copy source does not exist: {src}")
    make_path_writable(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, dst, dirs_exist_ok=True)


def iter_dylibs(root: pathlib.Path) -> list[pathlib.Path]:
    if not root.exists():
        return []
    return sorted(path for path in root.rglob("*.dylib") if path.is_file())


def codesign_tree(root: pathlib.Path, dry_run: bool) -> None:
    dylibs = iter_dylibs(root)
    if not dylibs:
        print(f"[codesign] skip empty tree: {root}", flush=True)
        return
    for dylib in dylibs:
        cmd = ["codesign", "-f", "-s", "-", str(dylib)]
        print(f"[codesign] {shlex.join(cmd)}", flush=True)
        if not dry_run:
            subprocess.run(cmd, check=True)


def repair_runtime_output_layout(config: BuildConfig, host: HostLayout, dry_run: bool) -> None:
    for src, dst in build_runtime_relayout_plan(config, host):
        copy_file(src, dst, dry_run=dry_run)


def repair_windows_msvcrt_boundscheck(
    runtime_root: pathlib.Path,
    build_type: str,
    environment: Mapping[str, str],
    dry_run: bool,
    extra_targets: Sequence[pathlib.Path] = (),
) -> None:
    """Rebuild boundscheck with the MSVCRT-compatible vsnprintf_s export.

    The upstream secure-CMake target disables ``vsnprintf_s`` by default, but
    the MinGW MSVCRT runtime links ``libcangjie-runtime.dll`` against that
    symbol.  Rebuilding this small DLL here keeps the source tree untouched
    and makes the generated runtime usable by stdlib/stdx on Windows.
    """
    source_root = runtime_root / "third_party" / "third_party_bounds_checking_function"
    source_files = [
        source_root / "src" / name
        for name in (
            "memset_s.c",
            "snprintf_s.c",
            "vsnprintf_s.c",
            "secureprintoutput_a.c",
        )
    ]
    if not all(path.is_file() for path in source_files):
        missing = [str(path) for path in source_files if not path.is_file()]
        raise FileNotFoundError(
            "cannot rebuild Windows boundscheck; missing sources:\n" + "\n".join(missing)
        )

    runtime_common_root = runtime_root / "output" / "common"
    platform_dir = "windows_x86_64_cjnative"
    target_files = [
        runtime_common_root / "runtime" / "lib" / platform_dir / "libboundscheck.dll",
        runtime_common_root
        / f"windows_{build_type}_x86_64"
        / "runtime"
        / "lib"
        / platform_dir
        / "libboundscheck.dll",
        *extra_targets,
    ]
    if dry_run:
        print(
            "[repair] rebuild Windows libboundscheck.dll with "
            "SECUREC_ENABLE_VSNPRINTF=1",
            flush=True,
        )
        return

    compiler = shutil.which(
        "x86_64-w64-mingw32-gcc",
        path=environment.get("PATH", ""),
    ) or shutil.which("gcc", path=environment.get("PATH", ""))
    if compiler is None:
        raise FileNotFoundError(
            "cannot rebuild Windows boundscheck; MinGW gcc is not available"
        )

    include_dirs = [source_root / "include"]
    generated_include = runtime_root / "output" / "temp" / "include"
    if generated_include.is_dir():
        include_dirs.append(generated_include)

    command = [
        compiler,
        "-shared",
        "-static",
        "-w",
        "-O2",
        "-D_FORTIFY_SOURCE=2",
        "-DSECUREC_ENABLE_VSNPRINTF=1",
        *(f"-I{path}" for path in include_dirs),
        *(str(path) for path in source_files),
        "-o",
    ]
    with tempfile.TemporaryDirectory(prefix="cangjie-boundscheck-") as temp_dir:
        output = pathlib.Path(temp_dir) / "libboundscheck.dll"
        command.append(str(output))
        command.extend(
            [
                "-lkernel32",
                "-luser32",
                "-lgdi32",
                "-lwinspool",
                "-lshell32",
                "-lole32",
                "-loleaut32",
                "-luuid",
                "-lcomdlg32",
                "-ladvapi32",
            ]
        )
        print(f"[repair] {shlex.join(command)}", flush=True)
        subprocess.run(command, cwd=runtime_root, check=True, env=dict(environment))
        existing_targets = [path for path in target_files if path.is_file()]
        if not existing_targets:
            raise FileNotFoundError(
                "runtime install did not produce libboundscheck.dll under "
                f"{runtime_common_root}"
            )
        for target in existing_targets:
            copy_file(output, target, dry_run=False)


def verify_outputs(config: BuildConfig, host: HostLayout) -> None:
    required_paths = [item.installed for item in build_verification_plan(config, host)]
    missing = [str(path) for path in required_paths if not path.exists()]
    if missing:
        raise FileNotFoundError("missing installed runtime outputs:\n" + "\n".join(missing))


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect_symbol_names(path: pathlib.Path) -> list[str]:
    completed = subprocess.run(
        ["nm", "-P", str(path)],
        check=True,
        capture_output=True,
        text=True,
    )
    symbols: set[str] = set()
    for line in completed.stdout.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        symbols.add(stripped.split()[0])
    return sorted(symbols)


def verify_codesign(path: pathlib.Path) -> str:
    completed = subprocess.run(
        ["codesign", "-vv", str(path)],
        check=True,
        capture_output=True,
        text=True,
    )
    stderr = completed.stderr.strip()
    return stderr if stderr else "codesign -vv passed"


def format_file_stat(path: pathlib.Path) -> str:
    stat_result = path.stat()
    timestamp = datetime.datetime.fromtimestamp(stat_result.st_mtime).isoformat(timespec="seconds")
    return f"size={stat_result.st_size} mtime={timestamp}"


def verify_installed_runtime_contents(config: BuildConfig, host: HostLayout) -> None:
    for item in build_verification_plan(config, host):
        source_sha = sha256_file(item.source)
        installed_sha = sha256_file(item.installed)
        exact_match = source_sha == installed_sha

        if item.compare_mode == "sha256":
            if not exact_match:
                raise RuntimeError(
                    f"{item.label} sha256 mismatch:\n"
                    f"source={item.source} {source_sha}\n"
                    f"installed={item.installed} {installed_sha}"
                )
            print(
                f"[verify] {item.label}: sha256 match {source_sha[:16]} "
                f"source({format_file_stat(item.source)}) installed({format_file_stat(item.installed)})",
                flush=True,
            )
            continue

        if exact_match:
            print(
                f"[verify] {item.label}: exact bytes match {source_sha[:16]} "
                f"source({format_file_stat(item.source)}) installed({format_file_stat(item.installed)})",
                flush=True,
            )
        else:
            if host.os_name != "darwin":
                raise RuntimeError(
                    f"{item.label} unexpected binary mismatch on {host.os_name}:\n"
                    f"source={item.source} {source_sha}\n"
                    f"installed={item.installed} {installed_sha}"
                )
            if collect_symbol_names(item.source) != collect_symbol_names(item.installed):
                raise RuntimeError(
                    f"{item.label} symbol mismatch after install:\n"
                    f"source={item.source}\n"
                    f"installed={item.installed}"
                )
            print(
                f"[verify] {item.label}: bytes changed after codesign "
                f"(source {source_sha[:16]} != installed {installed_sha[:16]}), "
                f"but symbol set matches and install is accepted; "
                f"source({format_file_stat(item.source)}) installed({format_file_stat(item.installed)})",
                flush=True,
            )

        if host.os_name == "darwin":
            codesign_status = verify_codesign(item.installed)
            print(f"[verify] {item.label}: {codesign_status}", flush=True)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Rebuild MSGC runtime and install it into "
            "the host platform MSGC output SDK"
        ),
    )
    parser.add_argument(
        "--runtime-root",
        type=pathlib.Path,
        default=None,
        help="path to runtime repo root; defaults to this script's repo",
    )
    parser.add_argument(
        "--sdk-root",
        type=pathlib.Path,
        default=None,
        help="target SDK root; defaults to the host platform MSGC output SDK",
    )
    parser.add_argument(
        "--build-type",
        default="release",
        choices=["release", "debug", "relwithdebinfo"],
        help="runtime build type",
    )
    parser.add_argument(
        "--version",
        default="0.0.1",
        help="runtime SDK version string passed to build.py",
    )
    parser.add_argument(
        "--skip-clean",
        action="store_true",
        help="skip 'python3 build.py clean' before rebuilding runtime",
    )
    parser.add_argument(
        "--skip-verify-output",
        action="store_true",
        help="skip installed-file existence checks after copy",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print planned commands without executing them",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    script_path = pathlib.Path(__file__)
    repo = resolve_repo_layout(script_path)
    runtime_root = args.runtime_root.resolve() if args.runtime_root else repo.runtime_root
    host = detect_host_layout()
    sdk_root = (
        args.sdk_root.resolve()
        if args.sdk_root
        else default_msgc_sdk_root(host)
    )
    config = BuildConfig(
        runtime_root=runtime_root,
        sdk_root=sdk_root,
        build_type=args.build_type,
        version=args.version,
        skip_clean=args.skip_clean,
        verify_output=not args.skip_verify_output,
        dry_run=args.dry_run,
    )

    print(f"[info] runtime_root={config.runtime_root}", flush=True)
    print(f"[info] sdk_root={config.sdk_root}", flush=True)
    print(f"[info] host={host.os_name}/{host.arch}", flush=True)
    build_environment = build_subprocess_environment(
        host,
        validate_tools=not config.dry_run,
        install_missing=not config.dry_run,
    )

    for stage in build_stage_plan(config, host):
        run_command(
            stage.command,
            cwd=stage.cwd,
            dry_run=config.dry_run,
            environment=build_environment,
        )

    if host.os_name == "windows":
        print("\n== repair Windows boundscheck runtime ==", flush=True)
        repair_windows_msvcrt_boundscheck(
            config.runtime_root,
            config.build_type,
            build_environment,
            config.dry_run,
        )

    print("\n== repair runtime output layout ==", flush=True)
    repair_runtime_output_layout(config, host, dry_run=config.dry_run)

    print("\n== copy runtime into sdk ==", flush=True)
    for src, dst in build_copy_plan(config, host):
        copy_tree(src, dst, dry_run=config.dry_run)

    if host.os_name == "darwin":
        print("\n== codesign runtime dylibs ==", flush=True)
        for root in build_codesign_targets(config, host):
            codesign_tree(root, dry_run=config.dry_run)

    if config.verify_output and not config.dry_run:
        print("\n== verify installed runtime outputs ==", flush=True)
        verify_outputs(config, host)
        print("\n== verify installed runtime contents ==", flush=True)
        verify_installed_runtime_contents(config, host)

    print("\n[done] MSGC runtime rebuilt and installed into output SDK.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
