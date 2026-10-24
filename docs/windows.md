# Windows x86-64 core pipeline

Windows supports the native compiler, link-table verification, load-only runtime,
isolated CPU checks and bounded initialization with selected IPC services. This
is a console development workflow with an optional Vulkan backend and preview window.
Read-only resources, temporary filesystems and bounded NVN compute/shader draws
are implemented. Direct GPU presentation, audible output, physical
controllers and gameplay remain separate work; the application frontend uses
macOS frameworks.

## Tools and setup

Use 64-bit Python 3.14 or later and the UCRT x86-64 distribution of
[LLVM MinGW](https://github.com/mstorsjo/llvm-mingw/releases). Clang must target
`x86_64-w64-windows-gnu`; MSVC and clang-cl are not supported. The CPU must support
CMPXCHG16B. Python 3.14.9 and LLVM MinGW 20261020 / Clang 23.1.4 were used for the
Windows core checks. Other versions need their own verification.

Extract the toolchain under ignored `work/tools/llvm-mingw/`, or supply its `bin`
directory with `-ClangBin`. Install Python from its
[Windows distributions](https://www.python.org/downloads/windows/), then run from
the repository root in PowerShell:

```powershell
New-Item -ItemType Directory -Force work/tmp, work/logs | Out-Null
$env:TEMP = $env:TMP = $env:TMPDIR = "$PWD/work/tmp"
py -3.14 -m venv work/venv
./work/venv/Scripts/python.exe -m pip install --no-cache-dir -r requirements.txt
./script/windows.ps1 test -ClangBin ./work/tools/llvm-mingw/bin
```

The script defaults to `work/venv/Scripts/python.exe`; `-Python` selects another
interpreter. It restores the process environment on exit. It does not install
system software, edit global PATH or require MSYS2/WSL. The equivalent portable
test entry point is `python tools/test_core.py` (`make test-core` on POSIX).

## Validated contracts

- The compiler suite compares lifted code with Unicorn, including FP/SIMD status,
  exclusive operations and indirect control flow. Windows test DLLs have an
  explicit lifetime and compiler dependency search directory.
- A synthetic five-module pipeline uses the real NSO parser, C generator,
  incremental compiler, linker and loader, then executes a compiled initialization
  entry, including a path containing spaces.
- `VirtualAlloc` reserves exact guest addresses without overwriting existing
  mappings. `VirtualProtect` keeps ARM64 text readable but non-executable and
  enforces data/gap permissions. Collision, rollback, unload/reload, malformed
  plans and actual write faults are checked. Plan paths support Unicode.
- Windows Job Objects contain commands and their descendants. A launch gate
  prevents the command from running before assignment; termination waits for
  descendants to exit. Aggregate working-set RSS, elapsed time and disk reserve
  remain polling safeguards. Compiler batches default to at most eight processes
  and a 4 GiB RSS limit; keep at least 20 GiB free on the workspace volume.
  Each translation unit has a 300-second timeout and each batch a 900-second
  budget. Successful objects are reusable after a bounded failure.
- Build locks are nonblocking. Windows serializes readers as well as writers.
  Response-file quoting preserves Windows paths; compiler target and flags are
  part of object receipts. Failed compilation preserves previous valid objects.
- Kernel reservations, main/child TLS and guest stacks stay below the 39-bit
  address limit. Heap shrink/regrowth, stack copy-back, checked guest copies and
  real guard-page faults are tested. Winpthreads supplies the existing thread and
  monotonic-clock contracts; thread tests retain ASan/UBSan. Unsupported IPC and
  native timeouts produce stopped-run receipts rather than successful checkpoints.
- Basic services reuse the CMIF/TIPC parser and session/domain model. Sanitized
  tests cover concurrent sessions, clones, event lifetime, exhausted handles,
  malformed packets and invalid guest buffers. Synthetic NPDM access lists are
  tested independently of real inputs. Missing host APIs retain a distinct stop
  reason, and a partial exit cannot report a completed checkpoint.

The portable suite is deliberately separate from `make test`, which also covers
macOS-specific runtime and graphics code. A green core suite does not certify
those components on Windows.

## Local executable experiments

Keep packages, keys, extracted inputs, fingerprints, generated code, binaries and
reports in ignored `local/` or `work/`. The existing ExeFS extraction tool can read
supported solid NCZ packages directly; its final-section layout restriction still
applies. `tools/prepare_hactool.py` builds the pinned, patched extraction tool
with Clang; `tools/extract_romfs.py` accepts an explicit program ID and ExeFS
profile. These commands keep their tools and bounded temporary work inside this
repository. The original shell importer remains specific to the initial adapter.

The resource verifier opens Windows inputs with read sharing only, rejecting
concurrent writers and replacement through the end of native execution. It streams the full
SHA-256 on every verification: rapid same-size writes can retain identical file
timestamps, so a metadata-only cache hit is insufficient. This adds read time for
large images. Failed checks preserve the earlier receipt. The native child inherits
this exact verified file handle and duplicates it with read access; storage does
not reopen the path. The launcher retains its lock until the child has stopped.
See [CreateFile sharing](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew).

An optional resource input requires a JSON profile with `size`, `sha256` and
`exefs_sha256` (a mapping from every selected module name to its SHA-256).
The extractor writes this profile beside the merged image after checking all
IVFC levels, applicable BKTR metadata and every selected ExeFS module. Keep
these title-specific records in ignored storage. To prepare a supported solid
base/update pair using your local inputs:

```powershell
./work/venv/Scripts/python.exe tools/prepare_hactool.py
./work/venv/Scripts/python.exe tools/extract_romfs.py --base ./local/base.nsz --update ./local/update.nsz --keys ./local/prod.keys --profile ./local/profile.json --program-id 0xYOUR_PROGRAM_ID --output ./local/romfs.bin
```

Run these in the configured Python/LLVM environment described above. Preparation
uses one compiler process and a 4 GiB aggregate memory limit; extraction has a
900-second budget and keeps a 20 GiB free-space reserve. Raw extraction output
is withheld because it can contain key material. Only fixed diagnostic labels
and numeric checks are logged on failure.
The local hactool patch also declares the memory and condition-code effects of
its bundled x86-64 big-integer assembly. Preparation checks eight synthetic
2048-bit modular exponentiations against Python and rejects a corrupted result;
signature/hash verification stays enabled during extraction.

Add `-Romfs ./local/romfs.bin -RomfsProfile ./local/romfs.json` to a `startup`
command using `-Runtime BasicServices` or `-Runtime Vulkan`. Without a resource,
opening storage stops explicitly. `fsp-srv` command 200 returns read-only storage;
storage commands 0 and 4 perform bounded reads and report its size. The parser
validates the raw header and table bounds, copies at most 64 KiB at a time and
rejects invalid guest output buffers. Storage writes, save mounts and directory
services remain outside this mode. Synthetic checks cover paths with non-ASCII
characters, offsets above 4 GiB, malformed tables, inherited handle ownership,
domain/session clones, bounds and read-only guest pages.

The default adapter keeps its existing fingerprint checks. To analyze another
input, create an ignored JSON profile with a `version` label and ordered `modules`.
Each module needs its exact file `sha256` and a hexadecimal `base`; addresses must
be aligned to 64 KiB, nonoverlapping and increasing in load order. Supported names
are `rtld`, `main`, `sdk` and `subsdk0` through `subsdk9` (at most 13 modules).
The loader retains its 256 MiB aggregate image/plan limits. A local profile is an
experimental build/load description, not a complete title adapter.

```powershell
./script/windows.ps1 inspect -Exefs ./local/exefs -Profile ./local/profile.json -ClangBin ./work/tools/llvm-mingw/bin
./script/windows.ps1 generate -Module all -Exefs ./local/exefs -Profile ./local/profile.json -ClangBin ./work/tools/llvm-mingw/bin
./script/windows.ps1 build -Jobs 8 -Exefs ./local/exefs -Profile ./local/profile.json -ClangBin ./work/tools/llvm-mingw/bin
./script/windows.ps1 load-only -Exefs ./local/exefs -Profile ./local/profile.json -ClangBin ./work/tools/llvm-mingw/bin
```

`-Profile` sets `NSWRECOMP_PROFILE` for the invocation and its workers. Alternate
profiles never inherit default adapter hooks; a `hooks` path, relative to the
profile, is an explicit opt-in. Do not run generation, probing or builds against
the same output tree concurrently. Changing the profile invalidates generation
receipts, while identical source/object content can still be reused.

Optimization defaults to `-O2`. Complex lifted functions can exceed the per-file
compiler budget even with modest RSS. `-OptLevel 1` selects an explicit `-O1`
validation build; use the same option for `load-only`. The Python tools accept
`NSWRECOMP_OPT_LEVEL=1` (or `build.py --opt-level 1`). The effective flags are
recorded in receipts and changing them invalidates existing objects. There is no
silent optimization fallback; this does not establish runtime performance.

For isolated execution, a profile may provide `rtld_probe_functions`, mapping
one or more of `memset`, `strlen`, `memmove` and `strcmp` to verified text-relative
offsets. `probe` builds a DLL and compares those routines against Unicorn and
independent buffer results. Offsets must be established for the exact input;
they are not guessed from another version. Unsupported runtime calls stop the
probe. Successful isolated routines do not establish guest initialization.

## Bounded initialization

After building and loading the selected profile, request an explicit SDK stage:

```powershell
./script/windows.ps1 startup -Exefs ./local/exefs -Profile ./local/profile.json -Through _init_libc0 -Seconds 30 -ClangBin ./work/tools/llvm-mingw/bin
```

Use the same `-OptLevel` as the existing build. `startup` defaults to
`-Runtime Kernel` (`--kernel-only`). It reuses compiled objects, links `work/startup/probe.exe`
and records the run in `work/startup/result.json` and `work/logs/startup.log`.
The default requested stage is `nninitInitializeSdkModule`; `-Through` may select
any prefix of the SDK order documented in [initialization](startup.md). Missing
exports, compiled entries, instructions or system calls stop the run. Host
relocation is complete before execution, so startup never re-enters rtld.

Kernel-only mode includes memory, thread, synchronization and query operations.
It provides no service manager, filesystem or audio backend. SVC `0x1f`/`0x21`
(service connection/IPC) therefore stop explicitly. An alternate input still uses
the existing kernel resource policy and stack copy-back model; an early checkpoint
does not validate its full process metadata or establish title compatibility.

The native worker has a 1–600 second execution budget (default 30), inside the
Job Object's 1.5 GiB aggregate RSS guard. Windows faults include the stage, guest
call location, accessed address and host instruction address. Timeout receipts
have `timed_out: true` when stopped by the supervisor, or native
`stop_reason: time_budget` when stopped by the Windows watchdog. Neither is a
successful checkpoint. Fatal native stops emit a report and terminate the worker
without concurrently tearing down services still used by guest threads. Keep successful
and stopped-run receipts separately in an ignored experiment directory when
comparing stages, because the next run replaces `work/startup/result.json`.

For synchronization diagnosis, `NSW_TRACE_WAITS=1` includes guest wait frames in
the bounded thread snapshot. `NSW_TRACE_SYNC_RANGE=0xLOW:0xHIGH` records at most
512 mutex/condition transitions whose address or nonzero key lies in that
half-open guest address range. Recording uses an in-memory buffer without I/O
under the scheduler lock; published entries remain readable when that lock is
busy. It changes no wait, notification or timeout semantics. Keep addresses,
traces and title-specific conclusions in ignored local reports.

An exact-input adapter can optionally protect a verified completion callback with
its compiled guest mutex helpers. See [callback mutex guards](callback-guards.md)
for the explicit binding, fingerprint and call-boundary requirements.

Compiler control-flow/data discovery issues and
expensive `-O2` units remain separate compiler work; do not suppress their traps
to claim startup or gameplay compatibility.

## Basic IPC services

To enable the validated service subset, supply the expected SHA-256 of your
selected `ExeFS/main.npdm` in addition to the executable profile. Replace the
quoted hash with your recorded fingerprint:

```powershell
./script/windows.ps1 startup -Exefs ./local/exefs -Profile ./local/profile.json -Runtime BasicServices -NpdmSha256 'YOUR_RECORDED_SHA256' -Through _init_libc2 -Seconds 30 -ClangBin ./work/tools/llvm-mingw/bin
```

This selects `--basic-services`. The worker checks the NPDM fingerprint and ACI/ACID
client-service list bounds, including client/server distinction and trailing `*`
patterns. Requested clients must be covered by the descriptor. The receipt records
the fingerprint and decoded client policy; keep it private with the other input
records. Signature verification and other NPDM capabilities are separate work.

Supported initialization operations include SM registration and service lookup,
logging metadata, performance-profile state and CPU boost hints, application-proxy objects, initial
foreground/notification state, parental-control initialization, empty add-on
enumeration and filesystem-session initialization. CMIF/TIPC, domain conversion,
session clones and close operations share the same implementation as full mode.
The operation profile is fixed to docked mode, no add-on packages are mounted,
and application state is a local headless model. These policies do not establish
hardware clock changes, a Windows window, parental-policy coverage or rendering.
CPU boost requests retain a validated guest hint (modes 0–2); Windows continues
to control host clock policy.

The offline account model supplies one local profile, and settings supply a fixed
language list. HID exposes a neutral FullKey controller through both single-state
and bulk-state SDK calls, plus style-change events;
no host controller is read. The selected program identity comes from the checked
NPDM. These are bounded local initialization contracts, with no network login.

For font initialization, explicitly set `ZELDA_SHARED_FONT` to a local TTF/TTC
with TrueType outlines, at most 17 MiB. The same host substitute serves the six
font faces through read-only shared memory; it is not the original system font.
Keep any task-owned font copy under ignored storage and observe its license.
Missing or invalid input stops explicitly. Calendar/timezone conversion and
title-specific version responses are excluded. An application-proxy
object can exist while some or all of its methods remain unsupported. Permitted
services without an implementation stop instead of receiving placeholder success.
Kernel memory/resource policy still uses the initial adapter's limits; service
access selection does not make the whole runtime a complete title adapter.

Basic-services linking does not require Opus or macOS frameworks. It also supports
graphics and devtools allocator registration through compiled guest callbacks.
The shared callback helper preserves caller CPU state, propagates FPSR and checks
target/stack validity; it never calls guest addresses as native function pointers.
See the [graphics contracts](graphics.md#call-and-callback-contracts).

Synthetic checks exercise the SVC-to-IPC path, five-module pipeline, allocator
re-entry and NVN register/stack ABI on Windows x86-64. These checks use no game
data or graphics device. Basic-services mode stops at `nv::InitializeGraphics`
unless the explicit Vulkan mode below is selected. Unavailable host APIs remain
stopped runs, even when constructors completed and `nnMain` was entered.

`-ScratchFilesystem` opts into a fresh, nonpersistent Windows filesystem for each
bounded run. It supports checked create/read/write/resize/rename/commit operations
under a 128 MiB quota. The worker confines UTF-16 host paths beneath its locked
root, rejects traversal and reparse points, and removes the scratch directory
after exit. This does not mount or validate user saves.

Vulkan mode also enables a silent AudioRenderer timing backend. It validates
revisioned control packets, advances voice/buffer consumption every 5 ms and
signals completion events. Pause and close stop that progression. It does not
decode, mix, apply DSP effects or open a sound output device.

## Read-only time service

Basic services include `time:u` when allowed by the selected NPDM client policy.
The user, local and network clock interfaces read one process-local wall-clock
anchor advanced by the host monotonic clock. IPC current-time/context reads and
the SDK's direct shared-memory reads use the same offset and source identifier.
The network context is a local fallback: network accuracy and automatic
correction both report false. Changes to the host wall clock after initialization
do not rebase the running guest.

The shared page has identical double-buffered snapshots and an identity continuous
adjustment context. It uses the same monotonic origin as `rt_ticks()`, with
nanoseconds in the steady base and seconds in system-clock offsets. The existing
kernel maps copies read-only; no asynchronous clock-page updates are required.
Synthetic checks verify the wire layout, SDK read formula, source identity,
overflow handling, shared-memory permissions and IPC object lifecycle.

Supported reads are static-service object creation (0–4), shared memory (20),
automatic-correction/network-accuracy queries (100/200), system clock current
time/context (0/2), and steady time/internal offset (0/200). Setters, calendar
conversion, timezone rules and calendar-bearing clock snapshots remain explicit
stops in basic/Vulkan mode. Full macOS calendar behavior remains separate.

Interface references: [libnx clock readers](https://github.com/switchbrew/libnx/blob/master/nx/source/services/time.c)
and [time service IPC](https://switchbrew.org/wiki/Glue_services#time:a,_time:r,_time:u).

## Headless Vulkan device and transfer check

A separately installed Vulkan 1.3-capable graphics driver is required for hardware
execution. No Vulkan SDK, SDL window or game input is required for this command:

```powershell
./script/windows.ps1 vulkan-probe -ClangBin ./work/tools/llvm-mingw/bin
```

The probe builds from pinned public headers, creates a hardware graphics+compute
queue and verifies GPU fill/copy/CPU readback for four sizes up to 1 MiB.
`-VulkanAdapter 0` selects an enumeration index; the default `-1` prefers an
eligible discrete GPU. CPU/software devices are excluded. Results, adapter/driver
identity and source hashes are recorded in `work/vulkan/result.json`; logs are in
`work/logs/vulkan-*.log`. Fence waits have a five-second limit, inside a 30-second,
1.5 GiB process guard. Failure is not a transfer verification result.

After independent hardware validation, the bounded guest path can opt in:

```powershell
./script/windows.ps1 startup -Exefs ./local/exefs -Profile ./local/profile.json -Runtime Vulkan -NpdmSha256 'YOUR_RECORDED_SHA256' -Through nnMain -Seconds 30 -ClangBin ./work/tools/llvm-mingw/bin
```

Use the same `-OptLevel` as the verified guest build. This selects basic services
plus `--vulkan-headless`, implements `nv::InitializeGraphics`/`FinalizeGraphics`
and records `runtime_mode: vulkan_headless`, `vulkan_device_created`,
`vulkan_clear_submissions`, `vulkan_depth_clear_submissions`,
`vulkan_compute_submissions`, `vulkan_shader_draws` and `vulkan_readback_pixels`.
The pixel counter covers clear readbacks; draw and copy diagnostics are separate.
Device initialization errors use `graphics_backend_error`. The flag records that
a device was created during this run, including after normal cleanup.

The NVN bridge supports full single-level 2D color/depth operations, image copies
and selected format conversions, buffer-only compute and vertex/fragment draws.
GPU targets are allocated on demand; submissions wait for fences before complete
readback. Draws support float vertex attributes, triangle lists/strips/fans,
uniforms, combined 2D samplers, standard color/alpha blending and read-only
depth/stencil tests. Dual-source blending, depth writes, compressed/3D textures
and broader pipeline state still stop explicitly.
`QueuePresentTexture` completes and releases an offscreen frame. Guest transfer
memory is validated but not imported into the GPU.

Add `-Window` to the Vulkan startup command (Python: add `--vulkan-window` to
`--vulkan-headless`) to display these completed RGBA8 readbacks. This is an
explicit opt-in: an inherited `NSW_VULKAN_WINDOW` cannot enable a window through
the launcher. The receipt then uses `runtime_mode: vulkan_window` and records
`window_created`, `window_frames_submitted` and `window_frames_painted`.
Frame capture metadata records `window_queued`; only the painted counter confirms
that the UI completed a draw. Minimized/occluded windows can paint fewer frames
than the renderer submits, and a repaint of the same frame does not increment it.

The preview owns a Win32 UI thread and one latest BGRA copy (at most 64 MiB per
frame). GDI scales the completed GPU readback with letterboxing; it is not a
Vulkan surface/swapchain or a performance result. Resize and minimize/restore
do not change guest render targets. Closing requests cooperative guest shutdown,
emits `window_closed` and exits the bounded process; a shutdown failure retains
a nonzero status. Normal VI finalization closes the preview without reporting
a user closure. Window startup/join waits are bounded to five seconds.
The synthetic suite opens owned windows briefly to verify pixel channels,
orientation, aspect ratio, copied-frame lifetime and both closure paths.

Run the independent hardware checks with Clang on `PATH`:

```powershell
./work/venv/Scripts/python.exe tools/vulkan_probe.py --render-clear
./work/venv/Scripts/python.exe tools/vulkan_shader_probe.py compute
./work/venv/Scripts/python.exe tools/vulkan_shader_probe.py draw
```

They verify clears, nonuniform resize/conversion, compute buffer arithmetic,
indexed triangle coverage, alpha blending, depth sampling and stencil rejection. Source and
binary receipts stay under `work/vulkan-*`. Core tests use synthetic inputs
and a fake driver without a GPU or game data.

For local shader development, build the pinned translator once:

```powershell
./work/venv/Scripts/python.exe tools/prepare_vulkan_tools.py
New-Item -ItemType Directory -Force work/vulkan-shaders | Out-Null
$env:NSW_VULKAN_CAPTURE = '1'
# Run a bounded Vulkan startup with your checked local inputs.
# A missing shader packet captures the pending command and stops.
./work/venv/Scripts/python.exe tools/vulkan_shaders.py
```

The bootstrap keeps its pinned .NET SDK, NuGet packages and build products in
`work/`. Translation uses the vendored Ryujinx subset; the reference checkout is
not a dependency. Prepared packets retain the complete original source bytes
for equality checks; a short hash is only a lookup index. Receipts record source,
translator and packet hashes. SPIR-V checks enforce the supported stages and
capabilities but are not a full validator. Clear the private packet cache when
changing translator semantics; runtime packet versioning does not track the DLL.

Opt-in capture stores at most 32 initial Draw readbacks totaling 128 MiB, one
additional first spatially varying Draw and one first Draw with differing
8-bit RGB pixels (each at most 64 MiB), eight initial offscreen frame readbacks,
one first frame with differing RGB pixels and one rolling latest frame
(each at most 64 MiB). Alpha-only
changes and sub-byte HDR dither do not satisfy the visible-pixel capture. Remove the environment variable to disable capture. Captured
shaders, uniforms, images and run reports must remain in ignored `work/`.

The optional movie bridge reuses the existing VP8/VP9 decoder interface and
compiled SDK container/event helpers, plus a Vorbis packet decoder for PCM16
movie audio. Prepare its pinned libraries with
`python tools/prepare_vulkan_video.py`, then add `-VulkanVideo` to the Vulkan
startup command. Preparation needs LLVM MinGW (including its BusyBox and make)
and a POSIX shell/Perl, such as Git for Windows. It builds generic C libvpx 1.16.0,
libvorbis 1.3.7 and libogg 1.3.6 under `work/`, retaining their licenses, with at
most eight compiler processes and a 4 GiB RSS guard. Source,
headers and library identities are checked before use. Run
`python -m unittest discover -s tests -p test_movie.py` for synthetic decoding
and callback/lifecycle checks, including VP9-to-RGBA texture delivery and a
six-channel Vorbis stream generated by the test. Vorbis accepts checked `csd-0`
and `csd-1` headers and emits interleaved PCM16 in Vorbis channel order; audible
playback and speaker mapping remain unverified. VP8/VP9 supports NV12 buffers
and bounded RGBA8 NVN texture output. Independent SDK Opus calls still forward
to compiled software helpers. None of these paths opens a host sound device. The silent AudioOut service
supports one PCM16 stream at 48 kHz, with 1, 2 or 6 channels, at most 64 owned
buffers and 16 MiB per buffer. It advances only submitted samples while started;
Stop preserves the queue and Flush releases stopped buffers. IPC tests cover
completion events, ordered release, duplicate tags, ranges and cloned sessions.

The preview window requires `-Window`; Vulkan swapchain presentation remains
unimplemented. Missing services and NVN APIs continue to stop explicitly.
The native hardware probe, shader execution, frame completion
and game compatibility are independent claims. Initialization can still stall
in guest synchronization. See the
[Vulkan ownership and synchronization contract](graphics.md#windows-vulkan-foundation).

Metadata format reference: [NPDM service access control](https://switchbrew.org/wiki/NPDM#Service_Access_Control).
Performance hint reference: [SetCpuBoostMode](https://switchbrew.org/wiki/AM_services#SetCpuBoostMode).

Host API references: [VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc),
[VirtualProtect](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect),
[ReadProcessMemory](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-readprocessmemory).

Receipts and logs are under `work/`. Generation and linking retain exit status 2
when unsupported instructions remain. Loading verifies matching compiler,
source, executable and input receipts before mapping anything. Keep analysis,
generation, compilation, loading, isolated execution, initialization and gameplay evidence
separate when reporting results.
