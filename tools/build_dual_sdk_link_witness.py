#!/usr/bin/env python3
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cross-link both PS5 SDKs with separate PSBC symbol namespaces; no GPU run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def real_archives(paths):
    archives = []
    for path in paths:
        with path.open("rb") as file:
            if file.read(8) == b"!<arch>\n":
                archives.append(path)
    return archives


def definitions(paths):
    archives = real_archives(paths)
    if not archives:
        raise ValueError("No ELF archives")
    output = run(["nm", "-g", "--defined-only", "-P", *archives],
                 capture_output=True, text=True).stdout
    names = set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 2 and re.fullmatch("[A-Z]", fields[1]):
            names.add(fields[0])
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--opengl-prefix", type=Path, required=True)
    parser.add_argument("--payload-sdk", type=Path, required=True)
    parser.add_argument("--vulkan-sdk", type=Path, default=ROOT / "dist-sdk")
    parser.add_argument("--output", type=Path, default=ROOT / "build/opengl-coexistence")
    args = parser.parse_args()
    gl = args.opengl_prefix.resolve()
    sdk = args.payload_sdk.resolve()
    vk = args.vulkan_sdk.resolve()
    out = args.output.resolve()
    gl_lib = gl / "lib"
    vk_lib = vk / "lib"
    manifest = gl / "manifest.sha256"
    if not manifest.is_file():
        raise ValueError("OpenGL SDK needs its installed-file manifest")
    run(["sha256sum", "--check", manifest.name], cwd=gl,
        stdout=subprocess.DEVNULL)
    facade = next((gl_lib / name for name in
                   ("libPS5OpenGL.a", "libPS5OpenGLCore33.a")
                   if (gl_lib / name).is_file()), None)
    if facade is None:
        raise ValueError("OpenGL SDK has no canonical linker facade")
    vk_driver = vk_lib / "libps5vk.a"
    vk_compiler = vk_lib / "libpsbc.a"
    gl_compiler = gl_lib / "libpsbc.ps5.a"
    for path in (vk_driver, vk_compiler, gl_compiler,
                 gl_lib / "libSceAgc.so", gl_lib / "libSceAgcDriver.so",
                 vk_lib / "ps5-pie.ld", vk_lib / "app-symbols.map",
                 sdk / "target/lib/crt1.o"):
        if not path.is_file():
            raise FileNotFoundError(path)
    nm = shutil.which("nm")
    objcopy = shutil.which("llvm-objcopy-18")
    readelf = shutil.which("llvm-readelf-18")
    if not all((nm, objcopy, readelf)):
        raise RuntimeError("Requires nm, llvm-objcopy-18 and llvm-readelf-18")
    vk_names = definitions([vk_driver, vk_compiler])
    gl_names = definitions(sorted(gl_lib.glob("*.a")))
    shared = sorted(vk_names & gl_names)
    if not {"psbc_compile_shader", "_mesa_log_multiline", "open_memstream"} <= set(shared):
        raise ValueError("Expected compiler collision is absent; re-audit this SDK pair")
    if any("ps5vk_isolated_" + name in vk_names | gl_names for name in shared):
        raise ValueError("Isolated symbol name already exists")
    out.mkdir(parents=True, exist_ok=True)
    symbols = out / "symbols.txt"
    symbols.write_text("".join(f"{name} ps5vk_isolated_{name}\n" for name in shared))
    driver_isolated = out / "libps5vk.isolated.a"
    compiler_isolated = out / "libpsbc.isolated.a"
    run([objcopy, f"--redefine-syms={symbols}", vk_driver, driver_isolated])
    run([objcopy, f"--redefine-syms={symbols}", vk_compiler, compiler_isolated])
    if definitions([driver_isolated, compiler_isolated]) & gl_names:
        raise ValueError("Isolated Vulkan archives still define OpenGL symbols")
    for archive in (driver_isolated, compiler_isolated):
        output = run(["nm", "-g", "--undefined-only", "-P", archive],
                     capture_output=True, text=True).stdout
        unresolved = {line.split()[0] for line in output.splitlines()
                      if len(line.split()) >= 2 and line.split()[1] == "U"}
        if unresolved & set(shared):
            raise ValueError("Isolated Vulkan archive still imports OpenGL compiler symbols")
    compiler = sdk / "bin/prospero-clang"
    linker = sdk / "bin/prospero-lld"
    source = ROOT / "examples/dual_sdk_link_witness/main.c"
    obj = out / "main.o"
    env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
    run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
         f"-I{vk / 'include'}", f"-I{gl / 'include'}",
         "-c", source, "-o", obj], env=env)
    syslib = sdk / "target/lib"
    runtime = [syslib / name for name in
               ("libc++.a", "libc++abi.a", "libunwind.a", "libc.a")]
    if (syslib / "libpthread.a").is_file():
        runtime.insert(-1, syslib / "libpthread.a")
    elf = out / "combined.elf"
    mapfile = out / "combined.map"
    run([linker, f"-L{syslib}", "-T", vk_lib / "ps5-pie.ld",
         "--eh-frame-hdr", "--version-script", vk_lib / "app-symbols.map",
         "-e", "_start", "-u", "ps5_agc_gate2_run",
         "-u", "ps5vk_runtime_compile_compute_features",
         f"-Map={mapfile}", "-o", elf, syslib / "crt1.o", obj,
         "--start-group", driver_isolated, compiler_isolated, facade,
         "--end-group", *runtime, "--as-needed",
         *sorted(syslib.glob("*.so")),
         gl_lib / "libSceAgc.so", gl_lib / "libSceAgcDriver.so"])
    table = run([readelf, "-Ws", elf], capture_output=True, text=True).stdout
    symbols_present = {line.split()[-1] for line in table.splitlines()
                       if len(line.split()) >= 8}
    required = {"psbc_compile_shader", "ps5vk_isolated_psbc_compile_shader",
                "ps5vk_runtime_compile_compute_features"}
    if not required <= symbols_present:
        raise ValueError(f"ELF is missing a compiler: {required - symbols_present}")
    map_text = mapfile.read_text()
    if str(compiler_isolated) not in map_text or str(gl_compiler) not in map_text:
        raise ValueError("Link map does not include both PSBC archives")
    report = {
        "schema": "ps5-fsr4-dual-sdk-link-witness/1",
        "scope": "native ELF cross-link only; no PS5 launch, compiler invocation or GL/Vulkan sharing",
        "opengl_manifest_sha256": sha256(manifest),
        "opengl_facade": facade.name,
        "vulkan_driver_sha256": sha256(vk_driver),
        "vulkan_psbc_sha256": sha256(vk_compiler),
        "opengl_psbc_sha256": sha256(gl_compiler),
        "isolated_symbol_count": len(shared),
        "elf_sha256": sha256(elf),
    }
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Linked {elf} with both compiler archives and "
          f"{len(shared)} isolated Vulkan symbols")


if __name__ == "__main__":
    main()
