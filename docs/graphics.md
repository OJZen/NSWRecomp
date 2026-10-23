# Graphics bridge

The application rendering backend connects compiled CPU calls to NVN/Metal and
AppKit. Windows x86-64 has a Vulkan bridge for resources, command
recording, image transfers and bounded compute/shader draws, with an optional
Win32 preview of completed readbacks. Its callback and generated NVN ABI checks
use synthetic inputs without a graphics device.
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

`runtime/guest_callback.c` provides synchronous re-entry with up to eight integer
arguments, using a copy of the active thread's `Cpu`. It clears the callback's
exclusive monitor, preserves the caller's registers/TLS/stack, propagates FPSR
and returns x0. Guest memory writes remain visible. Targets must resolve to
compiled functions; host API tokens, unaligned stacks, missing targets and
unbalanced returns are rejected. Floating-point and stack-passed callback
arguments need a separate contract before they can use this helper.

`runtime/graphics_allocator.c` registers separate graphics and devtools callbacks:
`allocate(size, alignment, user)`, `free(pointer, user)` and
`reallocate(pointer, size, user)`. Registration checks non-null targets and copies
the complete set atomically. Invocation releases the registry lock before guest
re-entry, permitting nested callbacks. Null callbacks cannot be invoked; a null
allocation result is a valid guest result. Callers own the lifetime of registered
code and user data, and backends must validate returned memory before accessing it.

Basic-services startup includes allocator registration on Windows and macOS.
Its default mode does not initialize a graphics device: `nv::InitializeGraphics`
retains the explicit missing-backend stop. Windows `--vulkan-headless` adds actual
device/queue creation after checking guest transfer memory. It does not import
that memory directly. Its NVN device/resource API stores checked host metadata
and allocates Vulkan images lazily for supported GPU operations.
The Metal startup path uses the same registration
and NVN callback helper. The movie-specific callback bridge remains separate.
Run `tools/test_core.py` for allocator registration/state boundaries, NVN integer,
floating-point, stack and small-handle ABI checks; tests use synthetic callbacks
and native functions, with no game input or rendering claim.

## Windows Vulkan foundation

`runtime/vulkan_host.c` owns a Vulkan 1.3 instance, a hardware device and one
graphics+compute queue. Device selection checks the queue requirements for every
candidate, and supports an explicit adapter index. Depth clamp, extended image
gather and shader draw parameters are enabled only when supported. No surface
extensions or validation layers are enabled yet. The Windows loader
boundary loads `vulkan-1.dll` from the system directory, with no SDK installation
or import-library dependency. Pinned public C headers are included in the tree.

The separate hardware probe fills a device buffer, orders its writes before a
copy to host-visible memory, inserts a host-read barrier and waits on a fence.
Non-coherent memory is invalidated before CPU verification. All words must match;
the probe exercises four sizes from 4 bytes to 1 MiB with distinct patterns.
This follows the [Khronos host-read synchronization contract](https://docs.vulkan.org/guide/latest/synchronization_examples.html#_cpu_read_back_of_data_written_by_a_compute_shader).

Fence waits are limited to five seconds and the process runs under the normal
time/RSS guard. A failed submit or incomplete wait retains referenced objects;
close only destroys them after a zero-time fence check confirms completion.
An unrecoverable worker exits instead of freeing in-flight resources or waiting
indefinitely. The low-level API has one owner. The runtime serializes Vulkan
operations and queue replay; recorders retain their own guest command arenas.

Synthetic tests cover coherent/non-coherent reads, unsuitable adapters, missing
functions, partial allocations, submit/wait failures, corrupt readback and runtime
initialization/cleanup. Hardware checks remain a separate opt-in command; see
[Windows Vulkan usage](windows.md#headless-vulkan-device-and-transfer-check).
`runtime/vulkan_nvn.inc` and its companion includes implement checked memory
pools, buffers, texture layouts/views, descriptor registration, deferred shader
binary snapshots, synchronization objects and guest command arenas. Recording
does not execute GPU work. Commands snapshot mutable metadata and values;
queue submission validates and replays the recorded stream. Exhausted arenas
invoke the compiled guest memory callback and reserve space for a jump record.

`runtime/vulkan_render.c` executes full color/depth clears and image transfers,
and verifies readback after the fence. Supported formats are R8, RGBA8 UNORM,
sRGB, packed unsigned R11G11B10 floating point, RGBA16F and D24S8. Targets are
single-sample 2D images with one layer/mip, at most 4096 pixels per dimension.
Depth resize uses a checked readback/nearest-resample/upload fallback when the
driver cannot blit that format. Host-pool writes use the shared Tegra block layout
and invalidate cached GPU targets; general CPU/GPU texture coherence is incomplete.

`runtime/vulkan_compute.c` snapshots uniform/storage buffers and commits writable
results only after every readback succeeds. Aliased writable bindings are rejected.
`runtime/vulkan_draw.inc` owns the descriptor sets, pipeline, buffers and images
through fence completion. It supports float vertices, triangle topology, uniform
buffers, combined 2D samplers, standard color/alpha blending and read-only
depth/stencil testing. Vertex fetch checks use the translated shader's actual
input locations, allowing unused attributes from earlier draws to remain bound.
Draws load the
existing target rather than replacing it with a clear. Depth can simultaneously
be sampled and attached with a read-only layout. Dual-source blending, depth writes and
unsupported formats/state stop explicitly. GPU writes remain available to later
sampling, copies and offscreen frame readback.

Timestamp report commands write a host completion timeline in the NVN tick unit
at replay, after earlier synchronous operations finish. They are not GPU profiling
queries. VI objects are offscreen tokens with a 60 Hz event; the SDK attachment
helper is resolved from the selected modules. Offscreen frame completion reads
the image and releases the acquired buffer ticket. With `--vulkan-window`,
`runtime/window_windows.c` copies the RGBA8 readback into an owned BGRA buffer
and paints it on a separate Win32 UI thread using GDI. GPU/guest objects never
cross that boundary. The latest frame replaces older pending frames; accepted
copies and completed paints have separate counters. The default path remains
offscreen. This preview does not enable Vulkan WSI or establish presentation
performance; see the [Windows contract](windows.md).
The AudioRenderer support advances buffer ownership and events without sound.

The optional `tools/vulkan_probe.py --render-clear` checks linear, sRGB and packed
float attachments with twelve clear passes, depth/stencil preservation and image
copy/conversion/resize. `tools/vulkan_shader_probe.py compute` and `draw` verify
synthetic buffer arithmetic and indexed triangle coverage with alpha blending, sampled depth and
stencil rejection. Each command retains matching source/executable receipts.
NVN lower-left viewports use a negative Vulkan viewport height with matching
scissor conversion; upper-left viewports retain top-down Vulkan coordinates.
Synthetic coordinate checks cover off-center rectangles in both origin modes.
These synthetic results, guest clear submissions, shader execution and a visible
frame are separate evidence boundaries. Device creation alone proves none of them.

The video bridge replaces the decoder while retaining compiled extraction and
buffer-management paths. Separately installed libvpx decodes VP8/VP9 to NV12
or, on the optional Windows movie path, checked RGBA8 NVN textures. A portable
libvorbis adapter decodes configured Vorbis packets to interleaved PCM16.
Synthetic tests generate their own multichannel stream and check PCM, events,
flush/replay and buffer ownership. Windows AudioOut has a bounded silent queue
that consumes submitted PCM according to a monotonic clock and releases tags
through copied event handles. It has no output device; speaker mapping, mixing
and audible playback remain separate work.
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
