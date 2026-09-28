#!/usr/bin/env python3
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the native GPU ceiling benchmark (PPSA88900) against the staged SDK (dist-sdk).

It measures peak FP32 FMA throughput (wave32 and wave64) for a looped kernel
and for straight-line kernels like FSR4's baked-weight network passes,
memory read bandwidth and per-dispatch submission cost through ps5vk, and
writes GPU_BENCH_* lines to the kernel log and gpu-bench-log.txt.
"""
import argparse
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_consumer import native_inputs  # noqa: E402

DIST_SDK = ROOT / "dist-sdk"
TOOLCHAIN_BIN = ROOT / "build/runtime-graphics/toolchain/usr/bin"

SHADERS = ("fma", "read", "empty", "unrolled", "unrolled_small")
UNROLLED_STEPS = 1024  # 16 chains each: 16,384 FMAs per invocation
# Code-size sweep: distinct-weight straight-line kernels of 128 bytes per step.
SWEEP_STEPS = (64, 128, 192, 224, 256, 320, 384, 512, 768)


def unrolled_source(distinct, steps=UNROLLED_STEPS):
    """Straight-line FMA chains: a literal weight per FMA (distinct, like the
    baked-weight network passes, about 128 KB of code) or 16 repeated weights."""
    rng = random.Random(1)
    lines = ["#version 450", "layout(local_size_x = 64) in;",
             "layout(set = 0, binding = 0, std430) writeonly buffer Out { float values[]; };",
             "void main() {", "    const float c = 1e-6;"]
    lines += [f"    float a{i} = float(gl_GlobalInvocationID.x + {i}u) * 1e-7;" for i in range(16)]
    for _ in range(steps):
        for i in range(16):
            weight = rng.uniform(-1.0, 1.0) if distinct else (i + 1) / 17.0
            lines.append(f"    a{i} = fma(a{i}, {weight:.8f}, c);")
    lines.append("    values[gl_GlobalInvocationID.x] = " + " + ".join(f"a{i}" for i in range(16)) + ";")
    lines.append("}")
    return "\n".join(lines) + "\n"


def build_native_app(out, source_file, title_name, extra_sources=(), include_dirs=(), libraries=()):
    """Link one native PPSA88900 program against the staged public SDK."""
    foundation, sdk, compiler, builder, gears = native_inputs()
    env = dict(os.environ, PS5_PAYLOAD_SDK=str(sdk))
    package = out / "PPSA88900"
    for folder in ("sce_sys", "sce_module"):
        (package / folder).mkdir(parents=True, exist_ok=True)
    objects, crt = [], out / "crt.o"
    # Explicit source directories come first so a staged SDK header never shadows them.
    includes = [*("-I" + str(x) for x in include_dirs), "-I" + str(DIST_SDK / "include"), "-I" + str(out)]
    for n, source in enumerate((source_file, *extra_sources)):
        obj, dep = out / ("main.o" if n == 0 else f"extra{n}.o"), out / ("main.d" if n == 0 else f"extra{n}.d")
        subprocess.run([str(compiler), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-MD", "-MF", str(dep), *includes, "-c", str(source), "-o", str(obj)],
                       env=env, check=True)
        # Only the public SDK and generated headers are used by this program.
        deps = dep.read_text()
        if str(ROOT / "src/ps5vk_") in deps or str(ROOT / "native") + "/" in deps:
            raise ValueError("Private implementation header in native program")
        objects.append(obj)
    subprocess.run([str(sdk / "bin/prospero-clang++"), "-std=c++20", "-O2",
                    "-fno-exceptions", "-fno-rtti", "-c",
                    str(foundation / "tooling/native/app_crt.cpp"), "-o", str(crt)], env=env, check=True)
    heap = out / "native_heap.o"
    subprocess.run([str(compiler), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-c", str(ROOT / "examples/gpu_bench/native_heap.c"), "-o", str(heap)],
                   env=env, check=True)
    pie, elf = out / "pie.elf", out / "eboot.elf"
    subprocess.run([str(sdk / "bin/prospero-lld"), "-L" + str(sdk / "target/lib"),
                    "-T", str(DIST_SDK / "lib/ps5-pie.ld"), "--eh-frame-hdr",
                    "--version-script", str(DIST_SDK / "lib/app-symbols.map"),
                    "-e", "_start", "-o", str(pie), str(crt), *map(str, objects), str(heap),
                    *["--wrap=" + name for name in ("malloc", "calloc", "realloc", "free",
                      "posix_memalign", "memalign", "aligned_alloc", "malloc_usable_size")],
                    *map(str, libraries), str(DIST_SDK / "lib/libps5vk.a"), str(DIST_SDK / "lib/libpsbc.a"),
                    *[str(sdk / "target/lib" / n) for n in
                      ("libc++.a", "libc++abi.a", "libunwind.a", "libc.a")],
                    "--as-needed", str(sdk / "target/lib/libkernel.so"),
                    *sorted(str(x) for x in (sdk / "target/lib").glob("*.so")),
                    str(DIST_SDK / "lib/libSceAgc.so"), str(DIST_SDK / "lib/libSceAgcDriver.so")],
                   check=True)
    subprocess.run([str(builder), "link", "--in", str(pie), "--out", str(elf),
                    "--stub-dir", str(sdk / "target/lib"), "--module-sdk", "0x02000009",
                    "--stub", str(DIST_SDK / "lib/libSceAgc.so"),
                    "--stub", str(DIST_SDK / "lib/libSceAgcDriver.so"),
                    "--companion-sdk", "0x08050001", "--file-name", "eboot.elf"], check=True)
    subprocess.run([str(builder), "self", "--sign", "--in", str(elf), "--out",
                    str(package / "eboot.bin"), "--magic", "0x1D3D154F"], check=True)
    param = json.loads((gears / "sce_sys/param.json").read_text())
    param.update(titleId="PPSA88900", conceptId="88900",
                 contentId="UP9000-PPSA88900_00-PS5VKGPUBENCH001")
    param["localizedParameters"]["en-US"]["titleName"] = title_name
    (package / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    shutil.copyfile(foundation / "runtime/libc.prx", package / "sce_module/libc.prx")
    # The shell refuses to launch PPSA88900 (0x80940033) without the launch assets.
    for name in ("icon0.png", "pic0.dds", "pic1.dds", "snd0.at9"):
        shutil.copyfile(foundation / "sce_sys" / name, package / "sce_sys" / name)


def glslang():
    local = TOOLCHAIN_BIN / "glslangValidator"
    return str(local) if local.exists() else os.environ.get("GLSLANG", "glslangValidator")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/gpu-bench")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    header = ["#include <stdint.h>", f"#define GPU_BENCH_UNROLLED_FMAS {16 * UNROLLED_STEPS}u"]
    for name in SHADERS:
        spv = out / f"gpu_bench_{name}.spv"
        source = ROOT / f"examples/gpu_bench/{name}.comp"
        if name.startswith("unrolled"):
            source = out / f"gpu_bench_{name}.comp"
            source.write_text(unrolled_source(name == "unrolled"))
        subprocess.run([glslang(), "-V", "--target-env", "vulkan1.1", str(source), "-o", str(spv)],
                       check=True, stdout=subprocess.DEVNULL)
        words = struct.unpack(f"<{spv.stat().st_size // 4}I", spv.read_bytes())
        header.append(f"static const uint32_t gpu_bench_{name}_spv[] = {{" + ",".join(map(hex, words)) + "};")
    table = []
    for steps in SWEEP_STEPS:
        source, spv = out / f"gpu_bench_sweep{steps}.comp", out / f"gpu_bench_sweep{steps}.spv"
        source.write_text(unrolled_source(True, steps))
        subprocess.run([glslang(), "-V", "--target-env", "vulkan1.1", str(source), "-o", str(spv)],
                       check=True, stdout=subprocess.DEVNULL)
        words = struct.unpack(f"<{spv.stat().st_size // 4}I", spv.read_bytes())
        header.append(f"static const uint32_t gpu_bench_sweep{steps}_spv[] = {{" + ",".join(map(hex, words)) + "};")
        table.append(f"    {{{steps}u, gpu_bench_sweep{steps}_spv, sizeof(gpu_bench_sweep{steps}_spv)}},")
    header += ["static const struct { uint32_t steps; const uint32_t *spv; size_t bytes; } gpu_bench_sweep[] = {",
               *table, "};"]
    (out / "gpu_bench_shaders.h").write_text("\n".join(header) + "\n")
    build_native_app(out, ROOT / "examples/gpu_bench/main.c", "GPU Bench")
    print(out / "PPSA88900")


if __name__ == "__main__":
    main()
