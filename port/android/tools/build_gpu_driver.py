#!/usr/bin/env python3
"""Build libadrenotools and its hooks (load the bundled or an installed GPU driver: vk/gpu_driver.c) from pinned open-source
inputs, into work/android/gpu-driver/, which build_apk.sh bundles.

    python3 port/android/tools/build_gpu_driver.py

From the D2R Android port's dm/android/tools/build_gpu_driver.py (by ImL1s, gitlab.com/aa22396584/d2r-mobile at 99cac6a;
AGPL-3.0, as this project; NOTICE), modified by this project since 2026-10-20, with the same pinned sources and compiler
flags; the NDK comes from tools/env.sh (ANDROID_SDK_ROOT in CI) and the sources are downloaded to
work/android/gpu-driver-src/. The licences of both sources are copied to work/android/gpu-driver/licenses/
(app/licenses.tsv).
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import io
import json
import os
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
SDK = Path(os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME") or "/opt/android-sdk")
NDK = Path(os.environ.get("ANDROID_NDK_HOME") or SDK / "ndk/29.0.14206865")   # tools/env.sh's NDK_VERSION
BIN = NDK / "toolchains/llvm/prebuilt/linux-x86_64/bin"
PINNED = (("eden-emulator/libadrenotools", "8ba23b42d7"),
          ("bylaws/liblinkernsbypass", "aa3975893d"))
SOURCES = ROOT / "work/android/gpu-driver-src"
OUTPUT = ROOT / "work/android/gpu-driver"


def source_directory(repo, commit):
    source = SOURCES / (repo.split("/")[-1] + "-" + commit)
    if source.is_dir():
        return source
    archive = urllib.request.urlopen(f"https://codeload.github.com/{repo}/tar.gz/{commit}", timeout=60).read()
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        members = []
        for item in tar.getmembers():
            parts = Path(item.name).parts[1:]
            if not parts:
                continue
            if ".." in parts or item.issym() or item.islnk():
                raise ValueError("Unsafe source archive entry")
            item.name = str(Path(*parts))
            members.append(item)
        tar.extractall(source, members=members, filter="data")
    return source


def build(output):
    source, linker = (source_directory(*p) for p in PINNED)
    output.mkdir(parents=True, exist_ok=True)
    include = output / "include/adrenotools"
    include.mkdir(parents=True, exist_ok=True)
    for header in (source / "include/adrenotools").glob("*.h"):
        shutil.copyfile(header, include / header.name)
    sources = [source / "src/driver.cpp", source / "src/bcenabler.cpp",
               linker / "android_linker_ns.cpp", linker / "elf_soname_patcher.cpp",
               source / "src/hook/hook_impl.cpp", source / "src/hook/main_hook.c",
               source / "src/hook/file_redirect_hook.c", source / "src/hook/gsl_alloc_hook.c"]

    def compile_file(path):
        obj = output / (path.stem + ".o")
        cpp = path.suffix == ".cpp"
        compiler = BIN / ("aarch64-linux-android29-clang++" if cpp else "aarch64-linux-android29-clang")
        command = [str(compiler), "-c", "-O2", "-fPIC", "-Wall", "-Wextra",
                   "-I", str(source), "-I", str(source / "include"), "-I", str(linker), str(path), "-o", str(obj)]
        if cpp:
            command.insert(2, "-std=c++17")
        if path.stem == "hook_impl":
            command.insert(2, "-fvisibility=hidden")
        result = subprocess.run(command, capture_output=True, text=True)
        (output / (path.stem + ".log")).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError("GPU driver compile failed: " + str(path) + "\n" + result.stderr)
        return obj

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        by_name = {obj.stem: obj for obj in pool.map(compile_file, sources)}
    archive = output / "liblinkernsbypass.a"
    archive.unlink(missing_ok=True)
    subprocess.run([str(BIN / "llvm-ar"), "rcs", str(archive), str(by_name["android_linker_ns"]), str(by_name["elf_soname_patcher"])], check=True)
    targets = {
        "adrenotools": [by_name["driver"], by_name["bcenabler"], archive],
        "hook_impl": [by_name["hook_impl"], archive],
        "main_hook": [by_name["main_hook"]],
        "file_redirect_hook": [by_name["file_redirect_hook"]],
        "gsl_alloc_hook": [by_name["gsl_alloc_hook"]],
    }
    libraries = []
    for name, inputs in targets.items():   # in order: the hooks link against libhook_impl.so
        library = output / ("lib" + name + ".so")
        command = [str(BIN / "aarch64-linux-android29-clang++"), "-shared", "-static-libstdc++",
                   "-Wl,-z,defs", "-Wl,-z,max-page-size=16384", "-Wl,-soname," + library.name, *map(str, inputs)]
        if name == "adrenotools":
            command.append("-Wl,--exclude-libs,liblinkernsbypass.a")
        if name not in ("adrenotools", "hook_impl"):
            command += ["-Wl,-z,global", "-L", str(output), "-lhook_impl"]
        command += ["-landroid", "-ldl", "-llog", "-lm", "-o", str(library)]
        result = subprocess.run(command, capture_output=True, text=True)
        (output / (name + "-link.log")).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError("GPU driver link failed: " + name + "\n" + result.stderr)
        libraries.append(library)
    licenses = output / "licenses"
    licenses.mkdir(exist_ok=True)
    for name, src in (("libadrenotools", source), ("liblinkernsbypass", linker)):
        shutil.copyfile(src / "LICENSE", licenses / (name + ".txt"))
    manifest = {"sources": [{"repo": repo, "commit": commit} for repo, commit in PINNED],
                "libraries": [p.name for p in libraries], "ndk": str(NDK), "abi": "arm64-v8a", "api": 29}
    (output / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("GPU driver libraries:", ", ".join(p.name for p in libraries), flush=True)


if __name__ == "__main__":
    build(OUTPUT)
