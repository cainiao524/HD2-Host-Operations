#!/usr/bin/env python3
"""Build and package the Host Operations HellForge mod."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
import zipfile


ROOT = Path(__file__).resolve().parent
BUILD_DIR = ROOT / "build"
DIST_DIR = ROOT / "dist"
SOURCE_DIR = ROOT / "src"
MANIFEST_PATH = ROOT / "hellforge-mod.json"

DEFAULT_HF_ROOT = Path(r"F:\Game\A\HD2\HellForge")
DEFAULT_IMGUI_DIR = ROOT / "third_party" / "imgui-1.91.8"
TARGET = "x86_64-windows-gnu"

PROJECT_C_SOURCES = (
    "operations.c",
    "menu.c",
    "i18n.c",
    "catalog_data.c",
    "mission_catalog.c",
    "game_text.c",
)
PROJECT_CPP_SOURCES = (
    "overlay.cpp",
    "host_ui.cpp",
)
IMGUI_SOURCES = (
    "imgui.cpp",
    "imgui_draw.cpp",
    "imgui_tables.cpp",
    "imgui_widgets.cpp",
    "backends/imgui_impl_win32.cpp",
    "backends/imgui_impl_dx12.cpp",
)
WINDOWS_LIBRARIES = (
    "user32",
    "gdi32",
    "d3d12",
    "dxgi",
    "dxguid",
    "ole32",
    "uuid",
    "shell32",
    "advapi32",
    "dwmapi",
    "imm32",
)

INCLUDE_PATTERN = re.compile(
    rb'^\s*#\s*include\s*[<"]([^">]+)[">]',
    re.MULTILINE,
)
SAFE_MANIFEST_VALUE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._-]*$")


class BuildError(RuntimeError):
    pass


class CompileTask:
    def __init__(
        self,
        name: str,
        source: Path,
        language: str,
        include_roots: tuple[Path, ...],
        definitions: tuple[str, ...] = (),
        warnings_as_errors: bool = False,
    ) -> None:
        self.name = name
        self.source = source
        self.language = language
        self.include_roots = include_roots
        self.definitions = definitions
        self.warnings_as_errors = warnings_as_errors


_print_lock = threading.Lock()


def announce(message: str) -> None:
    with _print_lock:
        print(message, flush=True)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def safe_remove(path: Path, allowed_root: Path) -> None:
    resolved = path.resolve()
    allowed = allowed_root.resolve()
    try:
        resolved.relative_to(allowed)
    except ValueError as error:
        raise BuildError(f"refusing to remove a path outside {allowed}: {resolved}") from error
    if resolved == allowed:
        raise BuildError(f"refusing to remove the build parent itself: {resolved}")
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


def resolve_zig(explicit_path: Path | None, hf_root: Path) -> Path:
    candidates: list[Path] = []
    environment_path = os.environ.get("HF_ZIG")
    if environment_path:
        candidates.append(Path(environment_path))
    if explicit_path is not None:
        candidates.append(explicit_path)
    candidates.extend(
        (
            hf_root / "tools" / "zig" / "zig.exe",
            hf_root / "app" / "tools" / "zig" / "zig.exe",
            hf_root / "zig" / "zig.exe",
        )
    )
    for candidate in candidates:
        candidate = candidate.expanduser()
        if candidate.is_file():
            return candidate.resolve()
    searched = "\n".join(f"  {candidate}" for candidate in candidates)
    raise BuildError(f"could not find the HellForge Zig compiler; searched:\n{searched}")


def zig_version(zig: Path) -> str:
    result = subprocess.run(
        [str(zig), "version"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        raise BuildError(f"cannot run {zig}:\n{result.stdout}{result.stderr}")
    return result.stdout.strip()


def compiler_flags(task: CompileTask) -> tuple[str, ...]:
    standard = "-std=c17" if task.language == "c" else "-std=c++20"
    flags = [
        "-target",
        TARGET,
        standard,
        "-O2",
        "-g0",
        "-DNDEBUG",
        "-DUNICODE",
        "-D_UNICODE",
    ]
    for definition in task.definitions:
        flags.append(f"-D{definition}")
    if task.language == "c":
        flags.extend(("-Wall", "-Wextra"))
    if task.warnings_as_errors:
        flags.append("-Werror")
    for include_root in task.include_roots:
        flags.extend(("-I", str(include_root)))
    return tuple(flags)


def resolve_include(
    include_name: str,
    source_path: Path,
    include_roots: tuple[Path, ...],
) -> Path | None:
    relative_name = Path(include_name)
    candidates = [source_path.parent / relative_name]
    candidates.extend(include_root / relative_name for include_root in include_roots)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def dependency_files(task: CompileTask) -> tuple[Path, ...]:
    pending = [task.source.resolve()]
    found: set[Path] = set()
    while pending:
        current = pending.pop()
        if current in found or not current.is_file():
            continue
        found.add(current)
        try:
            contents = current.read_bytes()
        except OSError:
            continue
        for match in INCLUDE_PATTERN.finditer(contents):
            include_name = match.group(1).decode("utf-8", "replace")
            dependency = resolve_include(include_name, current, task.include_roots)
            if dependency is not None and dependency not in found:
                pending.append(dependency)
    return tuple(sorted(found, key=lambda path: str(path).lower()))


def cache_key(
    task: CompileTask,
    compiler_identity: str,
) -> tuple[str, int]:
    digest = hashlib.sha256()
    digest.update(compiler_identity.encode("utf-8"))
    digest.update(task.name.encode("utf-8"))
    digest.update(task.language.encode("ascii"))
    for flag in compiler_flags(task):
        digest.update(b"\0")
        digest.update(flag.encode("utf-8"))
    dependencies = dependency_files(task)
    for dependency in dependencies:
        digest.update(b"\0")
        digest.update(str(dependency).encode("utf-8"))
        digest.update(b"\0")
        digest.update(dependency.read_bytes())
    return digest.hexdigest(), len(dependencies)


def object_path(task: CompileTask, key: str, use_cache: bool) -> Path:
    if use_cache:
        return BUILD_DIR / "cache" / f"{task.name}-{key[:20]}.o"
    return BUILD_DIR / "objects" / f"{task.name}.o"


def compile_task(
    task: CompileTask,
    zig: Path,
    compiler_identity: str,
    use_cache: bool,
) -> tuple[CompileTask, Path, bool]:
    key, dependency_count = cache_key(task, compiler_identity)
    output = object_path(task, key, use_cache)
    output.parent.mkdir(parents=True, exist_ok=True)
    if use_cache and output.is_file() and output.stat().st_size > 0:
        announce(f"[cache] {task.name} ({dependency_count} inputs)")
        return task, output, True

    compiler_mode = "cc" if task.language == "c" else "c++"
    command = [
        str(zig),
        compiler_mode,
        *compiler_flags(task),
        "-c",
        str(task.source),
        "-o",
        str(output),
    ]
    announce(f"[compile] {task.name}")
    result = subprocess.run(
        command,
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        details = result.stdout + result.stderr
        raise BuildError(f"{task.name} failed with exit code {result.returncode}:\n{details}")
    if not output.is_file() or output.stat().st_size == 0:
        raise BuildError(f"{task.name} produced no object file: {output}")
    return task, output, False


def compile_many(
    tasks: list[CompileTask],
    zig: Path,
    compiler_identity: str,
    jobs: int,
    use_cache: bool,
) -> list[Path]:
    results: dict[str, tuple[CompileTask, Path, bool]] = {}
    failures: list[BuildError] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = {
            executor.submit(compile_task, task, zig, compiler_identity, use_cache): task
            for task in tasks
        }
        for future in concurrent.futures.as_completed(futures):
            try:
                task, output, cached = future.result()
                results[task.name] = (task, output, cached)
            except BuildError as error:
                failures.append(error)
            except Exception as error:
                failures.append(BuildError(f"unexpected compile error: {error}"))
    if failures:
        raise BuildError("\n\n".join(str(error) for error in failures))
    ordered = [results[task.name] for task in tasks]
    hits = sum(1 for _, _, cached in ordered if cached)
    announce(f"[ok] {len(ordered)} object(s), {hits} cache hit(s)")
    return [output for _, output, _ in ordered]


def project_compile_tasks(
    sdk_include: Path,
    imgui_dir: Path,
    warnings_as_errors: bool,
) -> list[CompileTask]:
    tasks: list[CompileTask] = []
    include_roots = (sdk_include, SOURCE_DIR)
    definitions = ("NOMINMAX",)
    for source_name in PROJECT_C_SOURCES:
        tasks.append(
            CompileTask(
                name=source_name,
                source=SOURCE_DIR / source_name,
                language="c",
                include_roots=include_roots,
                definitions=definitions,
                warnings_as_errors=warnings_as_errors,
            )
        )
    for source_name in PROJECT_CPP_SOURCES:
        tasks.append(
            CompileTask(
                name=source_name,
                source=SOURCE_DIR / source_name,
                language="c++",
                include_roots=(*include_roots, imgui_dir, imgui_dir / "backends"),
                definitions=(*definitions, "QWORD=unsigned long long"),
                warnings_as_errors=warnings_as_errors,
            )
        )
    return tasks


def imgui_compile_tasks(imgui_dir: Path) -> list[CompileTask]:
    include_roots = (imgui_dir, imgui_dir / "backends")
    return [
        CompileTask(
            name=source_name.replace("/", "-"),
            source=imgui_dir / source_name,
            language="c++",
            include_roots=include_roots,
        )
        for source_name in IMGUI_SOURCES
    ]


def require_files(paths: list[Path]) -> None:
    missing = [path for path in paths if not path.is_file()]
    if missing:
        formatted = "\n".join(f"  {path}" for path in missing)
        raise BuildError(f"required source file(s) are missing:\n{formatted}")


def ensure_d3dcompiler_import_library(zig: Path) -> Path:
    system_root = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    compiler_dll = system_root / "System32" / "d3dcompiler_47.dll"
    if not compiler_dll.is_file():
        raise BuildError(f"cannot find {compiler_dll}; it is required by the ImGui DX12 backend")

    compiler_key = sha256_file(compiler_dll)[:20]
    import_dir = BUILD_DIR / "import-libs"
    import_dir.mkdir(parents=True, exist_ok=True)
    definition_path = import_dir / f"d3dcompiler_47-{compiler_key}.def"
    library_path = import_dir / f"libd3dcompiler_47-{compiler_key}.a"
    if library_path.is_file() and library_path.stat().st_size > 0:
        return library_path

    definition_path.write_text(
        "LIBRARY d3dcompiler_47.dll\nEXPORTS\nD3DCompile\n",
        encoding="ascii",
    )
    result = subprocess.run(
        [
            str(zig),
            "dlltool",
            "-m",
            "i386:x86-64",
            "-D",
            "d3dcompiler_47.dll",
            "-l",
            str(library_path),
            "-d",
            str(definition_path),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        raise BuildError(f"cannot generate d3dcompiler import library:\n{result.stdout}{result.stderr}")
    if not library_path.is_file() or library_path.stat().st_size == 0:
        raise BuildError(f"dlltool produced no import library: {library_path}")
    return library_path


def link_dll(
    zig: Path,
    objects: list[Path],
    output: Path,
    extra_link_inputs: tuple[Path, ...] = (),
) -> None:
    command = [
        str(zig),
        "c++",
        "-target",
        TARGET,
        "-shared",
        "-o",
        str(output),
        *(str(object) for object in objects),
        *(str(input_path) for input_path in extra_link_inputs),
        *(f"-l{library}" for library in WINDOWS_LIBRARIES),
    ]
    output.parent.mkdir(parents=True, exist_ok=True)
    announce(f"[link] {output.name}")
    result = subprocess.run(
        command,
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        details = result.stdout + result.stderr
        raise BuildError(f"link failed with exit code {result.returncode}:\n{details}")
    if not output.is_file() or output.stat().st_size == 0:
        raise BuildError(f"link produced no DLL: {output}")


def load_manifest() -> dict:
    try:
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        raise BuildError(f"cannot read {MANIFEST_PATH}: {error}") from error
    code = manifest.get("code")
    if not isinstance(code, dict):
        raise BuildError("hellforge-mod.json has no code object")
    if code.get("prebuilt") is not True:
        raise BuildError("hellforge-mod.json must set code.prebuilt to true")
    if not isinstance(code.get("dll"), str) or not code["dll"]:
        raise BuildError("hellforge-mod.json has no code.dll")
    if not isinstance(manifest.get("version"), str) or not manifest["version"]:
        raise BuildError("hellforge-mod.json has no version")
    if not SAFE_MANIFEST_VALUE.fullmatch(manifest["version"]):
        raise BuildError("hellforge-mod.json version contains unsafe path characters")
    if Path(code["dll"]).name != code["dll"]:
        raise BuildError("hellforge-mod.json code.dll must be a file name without a directory")
    return manifest


def validate_dll_exports(zig: Path, dll: Path) -> None:
    result = subprocess.run(
        [str(zig), "objdump", "--exports", str(dll)],
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        raise BuildError(f"cannot inspect DLL exports:\n{result.stdout}{result.stderr}")
    if "hellforge_mod_init" not in result.stdout:
        raise BuildError("linked DLL does not export hellforge_mod_init")


def add_file_to_zip(
    archive: zipfile.ZipFile,
    source: Path,
    archive_name: str,
) -> None:
    info = zipfile.ZipInfo(archive_name)
    info.date_time = (1980, 1, 1, 0, 0, 0)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    with source.open("rb") as stream:
        archive.writestr(info, stream.read(), compresslevel=9)


def package_mod(
    manifest: dict,
    dll: Path,
    imgui_dir: Path,
) -> tuple[Path, Path]:
    version = manifest["version"]
    package_name = f"host-ops-test-{version}"
    staging = DIST_DIR / package_name
    zip_path = DIST_DIR / f"{package_name}.zip"
    if staging.exists():
        safe_remove(staging, DIST_DIR)
    if zip_path.exists():
        safe_remove(zip_path, DIST_DIR)
    staging.mkdir(parents=True)

    shutil.copy2(dll, staging / Path(manifest["code"]["dll"]).name)
    shutil.copy2(MANIFEST_PATH, staging / "hellforge-mod.json")
    licenses = staging / "LICENSES"
    licenses.mkdir()
    shutil.copy2(imgui_dir / "LICENSE.txt", licenses / "Dear-ImGui.txt")

    files = (
        Path(manifest["code"]["dll"]).name,
        "hellforge-mod.json",
        "LICENSES/Dear-ImGui.txt",
    )
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for relative_name in files:
            add_file_to_zip(
                archive,
                staging / relative_name,
                f"{package_name}/{relative_name}",
            )
    return staging, zip_path


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hf-root", type=Path, default=DEFAULT_HF_ROOT)
    parser.add_argument("--zig", type=Path)
    parser.add_argument("--imgui", type=Path, default=DEFAULT_IMGUI_DIR)
    parser.add_argument(
        "--jobs",
        type=int,
        default=max(1, min(16, os.cpu_count() or 1)),
        help="maximum parallel compile jobs",
    )
    parser.add_argument("--imgui-only", action="store_true", help="compile ImGui and its Win32/DX12 backends only")
    parser.add_argument("--no-cache", action="store_true", help="compile into build/objects without reusing cache entries")
    parser.add_argument("--warnings-as-errors", action="store_true", help="add -Werror to project C/C++ sources")
    parser.add_argument("--clean", action="store_true", help="remove build/ and dist/ before starting")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    arguments = parse_arguments(argv)
    if arguments.jobs < 1:
        raise BuildError("--jobs must be at least 1")

    hf_root = arguments.hf_root.expanduser().resolve()
    zig = resolve_zig(arguments.zig, hf_root)
    version = zig_version(zig)
    compiler_identity = f"{zig}|{zig.stat().st_size}|{zig.stat().st_mtime_ns}|{version}"
    sdk_include = hf_root / "app" / "sdk"
    imgui_dir = arguments.imgui.expanduser().resolve()

    if arguments.clean:
        if BUILD_DIR.exists():
            safe_remove(BUILD_DIR, ROOT)
        if DIST_DIR.exists():
            safe_remove(DIST_DIR, ROOT)
    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Zig: {zig} ({version})")
    print(f"ImGui: {imgui_dir}")
    print(f"Jobs: {arguments.jobs}")

    require_files(
        [
            sdk_include / "hellforge_mod.h",
            *(imgui_dir / source_name for source_name in IMGUI_SOURCES),
            imgui_dir / "LICENSE.txt",
        ]
    )
    imgui_objects = compile_many(
        imgui_compile_tasks(imgui_dir),
        zig,
        compiler_identity,
        arguments.jobs,
        not arguments.no_cache,
    )
    if arguments.imgui_only:
        print("[done] ImGui dependencies are compiled")
        return 0

    project_tasks = project_compile_tasks(
        sdk_include,
        imgui_dir,
        arguments.warnings_as_errors,
    )
    require_files(
        [
            MANIFEST_PATH,
            *(task.source for task in project_tasks),
        ]
    )
    project_objects = compile_many(
        project_tasks,
        zig,
        compiler_identity,
        arguments.jobs,
        not arguments.no_cache,
    )

    manifest = load_manifest()
    dll = BUILD_DIR / "bin" / Path(manifest["code"]["dll"]).name
    d3dcompiler_import = ensure_d3dcompiler_import_library(zig)
    link_dll(
        zig,
        [*project_objects, *imgui_objects],
        dll,
        (d3dcompiler_import,),
    )
    validate_dll_exports(zig, dll)
    staging, zip_path = package_mod(manifest, dll, imgui_dir)

    print(f"[dist] {staging}")
    print(f"[zip]  {zip_path}")
    print(f"[sha256] {sha256_file(zip_path)}")
    print(f"[bytes] {zip_path.stat().st_size}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except BuildError as error:
        print(f"build error: {error}", file=sys.stderr)
        raise SystemExit(1)
