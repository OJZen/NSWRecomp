# Third-party sources

NSWRecomp preserves the source history and notices imported from totk-recomp.
The LASRecomp-derived subset is now maintained in this repository; its manifest
separates upstream origin revisions from recorded local changes.

`third_party/lasrecomp/` is a source-only subset of
[LASRecomp](https://github.com/peiikai/LASRecomp), copyright 2026 Peiikai, MIT.
The original notice is retained in `third_party/lasrecomp/LICENSE`.
Its exact revision and file digests are recorded in `third_party/lasrecomp/SOURCE.json`.
The subset includes NSO analysis/generation, synthetic tests and portable CPU helpers;
the Link's Awakening patch tables, randomizer, UI and game content are not included.
Use this subset through `tools/recompile.py`, which always disables game-specific patches.
The static function cache in `runtime/probe.c` adapts LASRecomp's MIT
`runtime/dispatch.c`; its revision and source digest are also recorded in that manifest.

The C headers under `third_party/vulkan-headers/` are the core declarations and
their required video declarations from [Khronos Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers),
tag `vulkan-sdk-1.4.363.0`, revision `6802bb4733b63ed5efd3adb308a6c885ef180ea1`.
They retain their Apache-2.0 OR MIT notices and license texts. `SOURCE.json`
records the unmodified file hashes. No Vulkan loader, driver, shader compiler or
SDK binaries are vendored; the optional Windows backend loads the system Vulkan
loader. The host implementation and synthetic driver tests are local code.

Development dependencies are installed separately, not vendored or linked into a game:

| Dependency | Version | License / use |
| --- | --- | --- |
| [Capstone](https://github.com/capstone-engine/capstone) | 5.0.7 | BSD 3-clause; instruction analysis |
| [python-lz4](https://github.com/python-lz4/python-lz4) | 4.4.5 | BSD 3-clause; bundled LZ4 BSD 2-clause; NSO decompression |
| [Unicorn](https://github.com/unicorn-engine/unicorn) | 2.1.4 | GPL 2.0 engine / BSD Python bindings; generator analysis and test oracle |

[Zelda-Mobile](https://github.com/OJZen/totk-mobile) is a reference for the v1.4.3
input fingerprints and runtime boundaries. No game data from its checkout is
included here. The memory, IPC, filesystem and audio sources in `runtime/`, and
`tests/native_services.c`, adapt its committed implementations under Apache-2.0; the original
license and pinned source digest are in `third_party/zelda-mobile/`. The local
adaptation uses this project's loaded module metadata and guest stack. Its
address-space layout follows d2r-mobile; that MIT notice is retained under
`third_party/zelda-mobile/d2r/LICENSE`. `runtime/threads.c` adapts the MIT thread
and synchronization algorithms to C CPU dispatch, independent guest stacks,
retained wait-object references and cooperative shutdown. The audio output
design also follows d2r-mobile's CoreAudio queue. Building
does not read the neighboring reference repository. Nintendo properties and user-supplied game files
are not covered by this project's or LASRecomp's license.

`runtime/game_timing.c` adapts Zelda-Mobile's elapsed-time stepping from
`tools/game_timing.py` at `b96b5fe51a634357c91b8ab997fa0ae4298bf80c` under Apache-2.0.
The separate timing source digest is recorded in `third_party/zelda-mobile/SOURCE.json`.
Version-specific timing fields and sites were identified by masagrator's
[FPSLocker configuration](https://github.com/masagrator/FPSLocker-Warehouse/blob/v4/SaltySD/plugins/FPSLocker/patches/0100F2C0115B6000/277178B7DBA1B6D4.yaml).

The startup probe links the separately installed [Opus](https://opus-codec.org/license/)
library (BSD 3-clause, installed version 1.6.1 in the validation environment).
Its source, headers and library are not vendored. Apple AudioToolbox/CoreAudio
are system frameworks. Synthetic audio tests run with speaker output disabled;
they do not establish audible game playback.

`runtime/graphics/movie.c`, `movie_video.c` and `movie_video.h`, together with
the synthetic movie tests, adapt the pinned Zelda-Mobile Apache-2.0 decoder.
The local bridge uses compiled `Cpu` callbacks and guest allocations for SDK
thread objects and input buffers. The graphics host links separately installed
[libvpx](https://chromium.googlesource.com/webm/libvpx/+/refs/heads/main/LICENSE)
(BSD 3-clause; macOS validation environment 1.17.0; optional Windows build
pinned to 1.16.0) for VP8/VP9 decoding to NV12 or RGBA8.
`runtime/graphics/movie_audio.*` is original MIT code using separately installed
[Xiph Vorbis and Ogg](https://xiph.org/downloads/) (BSD 3-clause; optional Windows
builds pinned to libvorbis 1.3.7 and libogg 1.3.6). The bootstrap checks official
archive digests and retains upstream COPYING files under ignored `work/`.
These sources and binaries are not vendored; tests synthesize their own video
and six-channel audio. The Metal build resolves vpx and vorbis with pkg-config.

The frame statistics and GameController adapter in `runtime/graphics/performance.c`
and `gamepad.m` adapt the same pinned Zelda-Mobile host sources under Apache-2.0.
Their source digests are recorded in `third_party/zelda-mobile/SOURCE.json`.
The native control panel, GPU interval union, input dead-zone handling and synthetic
tests are local additions. GameController is an Apple system framework.

The NVN API headers under `third_party/zelda-mobile/core/gfx/nvn/` are retained
byte-for-byte from the same pinned source. `include/nvn.h` keeps its MIT notice,
copyright 2020 h1k421; the generated prototype header is part of the d2r-mobile
MIT subset. They describe the host interface and contain no game resources.

`runtime/graphics/` adapts the pinned Zelda-Mobile NVN/Metal backend and its
committed fixes. Core graphics files inherit the d2r-mobile MIT license; host
additions and shader preparation tools inherit Apache-2.0. The original ARM64
entry trampolines and game-derived builtin shaders are excluded. CPU calls use
typed bridges into compiled C, including callbacks. Per-file origins are in
`third_party/zelda-mobile/SOURCE.json`.

The Windows NVN resource/command subset and state layouts in `runtime/vulkan_*.inc`
also adapt this MIT graphics subset. `runtime/audio_headless.c` reuses the pinned
Apache-2.0 voice cursor for silent timing and ownership tracking. Local adaptation
hashes and notes are recorded alongside the unchanged upstream fingerprints.

The shader tool now includes the same pinned MIT Ryujinx 1.3.2 subset used by
Zelda-Mobile: `Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator`.
Its notice is retained in `third_party/zelda-mobile/core/third_party/ryujinx/LICENSE.txt`.
The bcdec texture decoder retains its MIT/Unlicense notice in
`third_party/zelda-mobile/core/third_party/bcdec/LICENSE`. Its shaders are generic
format/driver helpers; user shader captures, translations and caches stay in `work/`.
SPIRV-Cross is fetched at a pinned commit with its Apache-2.0 notice under
`work/tools/spirv-cross.LICENSE`; the pinned .NET SDK is installed separately in `work/tools/`.

The container importer adapts Zelda-Mobile's ExeFS/NCZ and RomFS preparation tools
under Apache-2.0. Its hactool patch derives from SciresM/hactool, ISC, and includes
an assembly clobber correction to bundled mbedTLS, Apache-2.0. The hactool notice
is retained in `third_party/hactool/LICENSE`. The fixed hactool build retains its
ISC and bundled mbedTLS Apache-2.0 licenses under `work/tools/`. AES uses the
separately installed `cryptography` 50.0.2 package (Apache-2.0 OR BSD-3-Clause).
No NSZ implementation, encryption keys or game data is vendored.

The adjacent Eden (GPLv3-or-later) checkout remains a read-only reference. No
Eden source is copied or linked. Earlier source locations and review boundaries
are recorded in [the reference review](docs/emulator-references.md).
