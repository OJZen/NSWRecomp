# Graphics bridge

The current backend connects compiled CPU calls to NVN/Metal and AppKit.
Synthetic ABI and resource checks, successful command submission, observed
rendering and gameplay are distinct validation layers; see [status](status.md).
Detailed title-specific visual reports are kept locally.

## Call and callback contracts

`runtime/hle_symbols.inc` lists host-provided SDK interfaces. LASRecomp's `--hle`
path emits bridges for both direct calls and indirect dispatch. A missing real
backend stops with status 78; weak link-time fallbacks do not claim rendering success.

NVN lookup returns guest address identifiers rather than executable host pointers.
Cpu bridges read integer/floating-point registers and stack arguments according
to AAPCS64. Callbacks re-enter compiled functions, preserve caller state, check
stack balance and propagate FPSR. Original ARM64 text remains non-executable;
unsupported interfaces or ABI forms stop explicitly.

The video bridge replaces the decoder while retaining compiled extraction and
buffer-management paths. Separately installed libvpx decodes VP8/VP9 to NV12.
Virtual calls and decoder-thread entries use the same dispatch mechanism; buffers,
thread objects and stacks use registered guest allocators. Callback resolution
shares the loader's symbol scope, including allocator overrides at destruction.
Queueing, format handling, conversion and cleanup have synthetic coverage.

Display and input use AppKit and the native window's own events. See the
[control panel guide](control-panel.md) for mappings and focus behavior.

## Shader and build paths

The pinned backend sources and adaptations are included here; external worktrees
and native ARM64 entry trampolines are not required. The current shader path uses
a Ryujinx subset to emit SPIR-V, pinned SPIRV-Cross to emit MSL, and Metal tools
to compile it. No captured game shaders or images are distributed.

```sh
TMPDIR="$PWD/work/tmp" work/venv/bin/python tools/bounded.py \
  --log work/logs/shader-tools-prepare.log -- tools/prepare_shader_tools.sh

make build EXEFS=/path/to/exefs
work/venv/bin/python tools/startup.py --exefs /path/to/exefs \
  --romfs /path/to/romfs.bin --graphics --seconds 60
```

Captures, indices, translations and Metal caches stay in `work/shaders/`.
Tool-source and binary hashes identify the translation chain. Requests freeze a
bounded set of needed programs/variants, so unrelated variants captured by other
threads do not invalidate an in-flight batch. Translator changes invalidate old
SPIR-V; cache cleanup rejects symlinks.

Offline preparation, identity checks and fallback rules are described in
[shader preparation](shader-preparation.md). Cache hits and shader compilation
alone are not rendering or cold-start performance evidence.

Graphics linking permits 2 GiB RSS and startup permits 3 GiB. A separate AppKit
polling guard limits the native process to 4 GiB physical footprint. Retain the
20 GiB free-space reserve, bounded jobs and normal trace limits. The shared
launcher is `script/build_and_run.sh`; it remains a development entry point.
