<p align="center">
  <img src="assets/branding/header.svg" alt="ps5-vulkan — Graphics and compute for the PlayStation 5 GPU" width="100%">
</p>

<p align="center">
  <a href="https://github.com/mpereiraesaa/ps5-vulkan/actions/workflows/host-contracts.yml"><img src="https://github.com/mpereiraesaa/ps5-vulkan/actions/workflows/host-contracts.yml/badge.svg?branch=main" alt="Host and compiler checks"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-8875ff" alt="License: GPL-3.0-or-later"></a>
  <a href="API.md"><img src="https://img.shields.io/badge/status-experimental-f25569" alt="Experimental"></a>
  <a href="https://github.com/mpereiraesaa/ps5-vulkan/stargazers"><img src="https://img.shields.io/github/stars/mpereiraesaa/ps5-vulkan?style=flat" alt="GitHub stars"></a>
  <a href="https://github.com/mpereiraesaa/ps5-vulkan/pulls"><img src="https://img.shields.io/github/issues-pr/mpereiraesaa/ps5-vulkan?color=8875ff" alt="Open pull requests"></a>
  <a href="https://github.com/mpereiraesaa/ps5-vulkan/issues"><img src="https://img.shields.io/github/issues/mpereiraesaa/ps5-vulkan?color=f25569" alt="Open issues"></a>
</p>

<p align="center">
  <a href="BUILDING.md">Build the SDK</a> ·
  <a href="API.md">Supported API</a> ·
  <a href="VALIDATION.md">Hardware validation</a> ·
  <a href="https://github.com/mpereiraesaa/ps5-vulkan/issues">Report an issue</a>
</p>

> **Fork note.** This fork is maintained by BlackBearReloaded for the [PS5 FSR4 port](https://github.com/blackbearreloaded/ps5-fsr4), which uses it as a submodule. It adds compute subgroup size control (wave32/wave64), chained compute submission with resident code and shared scratch, compute shader code prefetch (GL2 and instruction prefetch), fused compute FMAs in the PSBC compiler, larger command buffers and the extended compute profile. See [LICENSING.md](LICENSING.md).

An experimental, hardware-accelerated Vulkan 1.3 implementation
for native PlayStation 5 homebrew, targeting the console's **gfx1013 GPU**.
It provides a static SDK, runtime SPIR-V compilation and native 1080p
presentation. The instance and device report **Vulkan 1.3**. This is a
**non-conformant experimental implementation**; the supported features,
formats and resource limits are documented in [API.md](API.md).

## What works

- **Graphics:** indexed and indirect draws, multiview, geometry and tessellation,
  clip/cull distances, depth testing, multiple viewports, dynamic topology and
  vertex stride, plus geometry-stage transform feedback and stream queries.
- **Pixels and textures:** independent and dual-source blending, fragment
  storage writes/atomics, 2x/4x colour multisampling with per-sample shading and
  resolve, cube arrays, bounded BC textures, depth sampling and extended gather.
  The ledger records 61 sampled texture formats: 40 filterable rows and
  20 integer rows restricted to nearest filtering.
- **Compute and shaders:** runtime SPIR-V compilation through pinned PSBC/ACO,
  shader/pipeline caches, up to four compute descriptor sets, push and
  specialization constants, 8/16-bit storage access and wave32 compute BASIC.
- **Memory and resources:** a 1.25 GiB graphics heap, a 1 GiB single-buffer
  allocation limit, layout-sized sets of up to 1024 descriptors, and a
  driver-maintained host-coherent buffer memory type. Other resource limits
  remain narrower.
- **Execution:** synchronization2, timeline semaphores, dynamic rendering with
  depth/stencil resolve, imageless framebuffers, descriptor update templates,
  bounded robustness2, and precise occlusion queries.
- **Presentation:** native surface/swapchain acquisition and two-buffer 1080p
  VideoOut presentation, with measured Close Game and relaunch behavior.

Supported formats, stages and resource shapes are deliberately bounded.
See [API.md](API.md) for the exact contracts and [VALIDATION.md](VALIDATION.md)
for artifact-identified native witnesses and deterministic GPU readback.
Host CI alone does not establish hardware correctness.

## Current runtime milestone

The Vulkan 1.3 route exposes promoted feature queries and commands directly,
including synchronization2, dynamic rendering and maintenance4. The pinned
DXVK 2.6.2 D3D11/DXGI workload no longer needs the old version-filter bypass,
feature-level relaxation, or payload-side core/extension translation.
Platform build and WSI adaptations remain separate from DXVK's rendering logic.

The acceptance workload creates a feature-level 11_0 device, renders and reads
back 4096 pixels, and checks clean teardown. Its artifact-bound status is in the
[native result](VALIDATION.md#experimental-vulkan-13-native-dxvk).
An offscreen result is not a presented DXVK frame or general game compatibility.

Read the [runtime backlog](docs/DXVK_V262_BACKLOG.md)
for the remaining work.
There is no Vulkan loader/ICD. Focused [CTS results](UPSTREAM_CTS.md) are
diagnostic evidence, not a full conformance claim or a blanket delivery gate.

## Build and develop

Host checks require Python 3, Make, a C11 compiler and Git:

```sh
make vulkan-headers
make compiler-deps
make check
make check-sanitize
```

Native builds also need the PS5 payload SDK and the companion graphics support
library. Follow [BUILDING.md](BUILDING.md) for dependencies and SDK staging.
Development uses targeted host checks and artifact-identified native witnesses;
the historical requirement matrix is available through `make check-dxvk-ledger`,
not used as a build or promotion gate.

<!-- bbr-footer:start -->
<!-- Generated by ps5-homebrew-dev-protocol/scripts/readme-footer. Edit the template there, not here. -->

## Credits

Built with the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) by John Törnblom (ps5-payload-dev).
Third-party components, authors and licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

Copyright © 2026 BlackBearReloaded. Licensed under GPL-3.0-or-later; see [LICENSE](LICENSE). Third-party components keep their own licenses. Applications distributed with the static `libps5vk.a` must provide the corresponding source under GPL-compatible terms. Upstream licensing and source provenance are documented in [LICENSING.md](LICENSING.md).

## Disclaimer

- **No affiliation.** This is an independent homebrew project. It is not
  affiliated with, endorsed by, or sponsored by Sony Interactive Entertainment.
  "PlayStation", "PS5" and related marks are trademarks of Sony Interactive
  Entertainment Inc. Vulkan is a registered trademark of the Khronos Group Inc.
- **No proprietary material.** No Sony SDK, firmware, encryption keys or
  decrypted system modules are included.
- **No warranty.** This project is provided "as is", without warranty of any
  kind, to the extent permitted by law. See sections 15 and 16 of the GPL.
- **Use at your own risk.** Running homebrew requires a modified console, which
  may void its warranty, breach the platform's terms of service, or cause data
  loss.
- **Legal use only.** Use it only with hardware, accounts and content you own.
  This project does not support or enable piracy.

## AI assistance

This project was developed with AI assistance from OpenAI and/or Anthropic tools.
<!-- bbr-footer:end -->
