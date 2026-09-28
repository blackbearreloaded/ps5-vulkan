# Building and testing

## Fork status

This fork is maintained by BlackBearReloaded for the
[PS5 FSR4 port](https://github.com/blackbearreloaded/ps5-fsr4), which uses it
as a submodule. It adds compute subgroup size control (wave32/wave64), chained
compute submission with resident code and shared scratch, compute shader code
prefetch (GL2 and instruction prefetch), fused compute FMAs in the PSBC
compiler, larger command buffers and the extended compute profile.

The rest of this file is the recipe inherited from upstream. The reusable SDK builder
uses the public payload SDK compiler and the logger bundled in pinned public
`ps5-agc-gears`. It fails before staging if native dependencies are missing and
never substitutes the host test archive for the native library. The inherited
native sample and parts of the host suite still refer to sibling lab projects
(`LAB_SIBLINGS`).

The imported GitHub Actions workflow is manual-only until this is resolved.
Do not treat its presence, an upstream badge, or a host mock archive as proof
that this checkout builds or runs natively. The upstream hardware receipts
remain historical evidence for their exact upstream artifacts.

The dependency-free attribution check can run now:

```sh
python3 tools/run_python_tests.py test_license_policy
```

## Host checks

Install Python 3 with Mako and PyYAML, Make, a C11 compiler, Git and
`glslangValidator`, then prepare the pinned Vulkan headers, compiler dependencies
and run the contract suite:

```sh
make vulkan-headers
make compiler-deps
make check
make check-sanitize
```

The preparation targets fetch exact revisions. `make compiler-deps` uses the
public `mpereiraesaa/opengnm-psbc` GFX1013 fork plus pinned OpenGNM headers.
The regular host suite runs offline after these dependencies are prepared.
The pin is a single commit in `tools/prepare_compiler_deps.py`; a change that
needs newer compiler behaviour names that requirement in the pin comment and
moves the pin only once the corresponding fork PR is merged. The DXVK262-T06
sample-rate window did exactly that: the driver passes
`PsbcCompileOptions::sample_shading_enable`, which the previous pin `be4d043`
does not have, and the pin now names the merge commit of `opengnm-psbc` PR #23
(`0d4e80c`). The payload built from that revision is byte-identical to the one
the acceptance run measured, so the move changed no artifact.

`make check` runs the Python suite through `tools/run_python_tests.py`, one
process per `tests/test_*.py` module, as many at a time as there are CPUs
(`PYTHON_TEST_JOBS` overrides it). Modules that build into shared trees
(`dist-sdk`, the native consumer, `build/graphics`) share one serial lane.
Output appears per module; failing modules print in full. To run a subset:

```sh
python3 tools/run_python_tests.py test_upstream_selection test_vk_render_pass
python3 tools/run_python_tests.py -v -j 1        # serial, verbose
```

## Companion repositories

The reusable SDK cross-build fetches the public AGC support and its byte-identical
`ps5log/1` header plus PS5 network adapter at one pinned revision:

```sh
make vulkan-headers compiler-deps native-deps
export PS5_PAYLOAD_SDK=/absolute/path/to/ps5-payload-sdk
python3 tools/build_psbc.py --target ps5
python3 tools/build_sdk.py
```

This builds a native archive and checks an independent native consumer; it
does not run on a console. The inherited sample and full host suite still
expect sibling lab repositories:

```text
homebrew_ps5/
  projects/
    logging_server/
    ps5-agc-gears/
    ps5-vulkan/
```

The shared resolver also supports repository worktrees placed beside the
canonical `homebrew_ps5` directory. CI pins known companion revisions so public
pull requests do not silently change contracts underneath the build.

## Native build

Set `PS5_PAYLOAD_SDK` when the SDK is not available at the default sibling path.
Compile an owned graphics pipeline description, then pass the generated control
directory to the public native wrapper:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk/install
python3 tools/compile_graphics_control.py experiments/graphics/scene3d.pipe
make native-graphics GRAPHICS_CONTROL=build/graphics/control-REPLACE
```

The generated package remains under the ignored build/output trees. Deployment,
console credentials, telemetry configuration, captures, compiler worktrees and
proprietary artifacts are intentionally outside this repository.

## Reproducibility and evidence

For the owned runtime-compiled triangle diagnostic, reuse the generated
control directory as a negative offline-lookup reference:

```sh
PS5VK_USE_SDK=1 make native-runtime-graphics GRAPHICS_CONTROL=build/graphics/control-REPLACE
```

This builds the SDK and links its native archive into the diagnostic. Vertex
and fragment ISA are compiled on the console; only GLSL-to-SPIR-V runs on the
host. Set `GLSLANG=/path/to/glslangValidator` if needed. After the bounded test
retires resources, it waits for system Close Game; it is not the continuous
textured demo. Omit `PS5VK_USE_SDK=1` to compile backend objects directly.

`python3 tools/build_sdk.py` independently stages public headers, native and
host archives, the native compiler dependency and link-time import facades in
`dist-sdk/`. It requires the prepared native PSBC archive for native builds.
The generated SDK README documents linking and supported profiles. Native
consumer checks cross-link only; host execution is a mock-backend test.

Use `tools/verify_graphics_runtime.py LOG --artifact MANIFEST` to check the
runtime triangle's TCP receipt, cold/warm cache, GPU readbacks, presentation
and resource retirement. Verified deployment hashes and successful OS close
must be established separately; the log does not attest its executable.

For the bounded multi-set compute acceptance in the public consumer, use
`tools/verify_consumer_resource_abi.py LOG --artifact MANIFEST`. The artifact
manifest must identify the deployed SELF and profile; the verifier requires
three sets, two storage buffers, one uniform buffer, one uniform texel buffer,
two non-default scalar specialization values, one pushed 32-bit word, 64 exact
results, 128 intact guard words, complete queue witnesses and a clean TCP
`BYE`. The same verifier also requires the extension-negotiated 8/16-bit
storage witness: 64 byte results, 64 16-bit results, fixed checksums and 8,000
intact surrounding guard bytes. `tools/run_consumer.py` combines strict log
verification with a separately confirmed system Close Game.

Host checks validate API state machines, encoder contracts, resource ownership,
negative paths and generated-program invariants. A successful host build alone
does not prove GPU execution. Native evidence additionally requires exact
artifact identity, structured `ps5log/1` telemetry, GPU completion/readback and
VideoOut ownership; screenshots are only supporting visual evidence.

## Vulkan API contract tests (modeled after CTS)

A pinned set of 26 synthetic Vulkan API contract tests modeled after the Khronos
`VK-GL-CTS` mustpass selection is maintained in `cts/case_list.txt` and documented
in `cts/gap_matrix.md`. It remains separate from the focused genuine upstream
integration documented in [UPSTREAM_CTS.md](UPSTREAM_CTS.md).

```sh
# Host contract test execution (part of make check)
./build/tests/test_cts_host --json
./build/tests/test_cts_host --tap

# Contract runner and gap matrix unit tests
python3 -m unittest discover -s tests -p "test_cts*.py"

# Package native contract test title for console execution
python3 tools/build_cts_native.py
```

## Independent native SDK consumer

The standalone native consumer demonstrates SDK usage using strictly public headers
(`<ps5vk/ps5vk.h>`, `<ps5vk/ps5vk_present.h>`):

```sh
# Stage public SDK
python3 tools/build_sdk.py

# Build native consumer, verify zero private includes/symbols, and package
python3 tools/build_consumer.py

# For continuous rendering demonstration (60 FPS looped stream)
python3 tools/build_consumer.py --continuous

# Build the finite DXVK 2.6.2 FL11_0 capability probe. It queries only the
# staged public Vulkan ABI and does not submit graphics or compute work.
python3 tools/build_consumer.py --dxvk-v262-probe

# Verify one finalized ps5log/1 run against the exact profile and artifact.
python3 tools/verify_dxvk_probe.py <run-prefix-or-log> \
  --manifest dist-consumer/artifact.json \
  --artifact dist-consumer/PPSA99994/eboot.bin

# Consumer header and symbol isolation tests
python3 -m unittest tests/test_consumer_isolation.py

# Strict verifier regression tests for the multi-set resource witness
python3 -m unittest tests/test_consumer_resource_abi.py

# After an exact deployment, capture, verify and close one hardware run
python3 tools/run_consumer.py --host <console> \
  --runs-dir ../logging_server/runs --out <private-receipt.json>
```

## Public native consumer package

Prepare the public dependencies and native SDK above, then build the public
native-app template's host tool and runtime. Point the consumer at that prepared
template (containing `build/host/ps5-native-tool` and `runtime/libc.prx`):

```sh
export PS5_NATIVE_APP_TEMPLATE=/absolute/path/to/ps5-native-app-boilerplate
python3 tools/build_consumer.py
python3 tools/build_consumer.py --check-only
python3 tests/test_consumer_build_inputs.py
```

The result is a complete signed `dist-consumer/PPSA88900/` folder. The builder
uses the public payload SDK's C and C++ drivers, compiles the template startup,
and rejects missing native inputs before staging. There is no host fallback.
`--use-staged-sdk` is only for a previously verified current native SDK.

The consumer still requires a local `dev.conf` with a reachable ps5log TCP
receiver. The builder copies it when present; without it the current consumer
exits before GPU testing. A successful build is not a hardware receipt.

Deploy and test only on the console configured locally, via FTP port 2121 under
`/data/homebrew/PPSA88900/`. If unavailable, wait; no fallback console is
authorized. The native consumer has not yet been run in this checkout.

## GPU ceiling benchmark

`python3 tools/build_gpu_bench.py` builds a PPSA88900 program from the staged
SDK that reports peak FP32 FMA throughput (wave32 and wave64), straight-line code
throughput against code size, memory and cached re-read bandwidth and the
per-dispatch cost of a chained submission as `GPU_BENCH_*` lines in the kernel
log and `gpu-bench-log.txt`.

## FSR4 applications

The FSR4 runtime, its tests and native applications are built in
[ps5-fsr4](https://github.com/blackbearreloaded/ps5-fsr4) against this SDK
with `PS5VK_SHADER_INT8_DIAGNOSTIC=1 PS5VK_SHADER_INT16_DIAGNOSTIC=1
PS5VK_SUBGROUP_ALL_DIAGNOSTIC=1 PS5VK_EXTENDED_COMPUTE_DIAGNOSTIC=1`.
