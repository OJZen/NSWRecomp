# Architecture and implementation boundaries

NSWRecomp follows the LASRecomp ARM64-to-C static recompilation model. Its current
runtime adapts pinned memory, IPC, filesystem, audio and NVN/Metal sources to
compiled CPU dispatch, guest threads and callbacks. Provenance and licenses are
recorded separately; reference checkouts are not read during builds.

The [roadmap](architecture-roadmap.md) describes intended multi-adapter and
cross-platform boundaries. It must not be read as a list of completed features.
Windows work targets 64-bit x86-64; the current guest address model does not fit
a 32-bit host process.

## Build and runtime stages

1. **Build:** validate user-supplied ExeFS against the selected adapter, generate
   C and function-address tables, then compile them for the host. The generated
   tree does not embed complete read-only/data segments, RomFS or media assets.
2. **Runtime preparation:** validate and import matching user inputs, map initial
   module data, apply relocations and dispatch guest function addresses to their
   compiled host implementations. Matching resources are still required locally.

NSO files contain both code and initial data. Neither complete input executables
nor derived C are published in this repository. The public deliverable is the
compiler/runtime source and its reproducible synthetic checks.

The intended iOS route uses code compiled and signed on a build machine; in-app
import prepares data. Desktop compilation of dynamic libraries at startup is
not an iOS deployment model. See [Apple's code-signing documentation](https://support.apple.com/guide/security/app-code-signing-process-sec7c917bf14/web).

## Components

| Path | Contract |
| --- | --- |
| `config/` | Adapter fingerprints, guest layout, hooks and probe entry points |
| `third_party/lasrecomp/` | NSO parsing, function discovery, ARM64 lifting and C CPU helpers |
| `tools/recompile.py` | Input validation, analysis and staged per-module generation |
| `tools/build.py` | Bounded parallel compilation, object reuse, linking and function-table checks |
| `tools/load.py` | Input/build receipt checks, symbol resolution and temporary relocation plans |
| `tools/bounded.py` | Process-group RSS, time and free-space polling guards |
| `tools/host.py`, `tools/windows_job.py`, `script/windows.ps1` | Windows compiler/lock contracts, Job Object supervision and core entry point |
| `runtime/probe.c`, `runtime/load.c` | Dispatch, mapped-memory validation, relocations, permissions and cleanup |
| `tools/startup.py`, `runtime/startup.c` | Bounded guest initialization with stack, TLS and diagnostic checkpoints |
| `tools/npdm.py` | Fingerprinted, bounded client-service access lists for basic IPC; no signature or full process-policy validation |
| `tools/import_game.py`, `tools/extract_*.py` | Local container/resource preparation and private temporary-file cleanup |
| `runtime/graphics/`, `tools/graphics.py` | Cpu-to-NVN bridge, Metal, AppKit, display and input |
| `runtime/guest_callback.c`, `runtime/graphics_allocator.c` | Portable compiled guest re-entry and graphics/devtools allocator registration |
| `runtime/time_clock.c`, `runtime/time_clock.h` | Immutable wall/monotonic clock anchor and shared-memory wire layout used by time IPC |
| `runtime/vulkan_host.*`, `runtime/vulkan_windows.*`, `runtime/vulkan_runtime.*`, `runtime/vulkan_render.*` | Offscreen Windows Vulkan ownership, bounded transfers and color-attachment execution/readback |
| `runtime/vulkan_*.inc`, `runtime/vulkan_bridge.c`, `runtime/vulkan_display.*` | Checked NVN resources, command snapshots/replay and offscreen VI events |
| `runtime/window.h`, `runtime/window_windows.c` | Optional Windows UI thread, owned RGBA readback copies, scaled preview and closure |
| `tools/callback_guards.py`, `runtime/callback_guard.*` | Explicit fingerprinted adapter bindings around completion callbacks using compiled guest mutex helpers |
| `runtime/audio_headless.c`, `runtime/filesystem_windows.c` | Silent AudioRenderer timing and per-run scratch filesystems for initialization |
| `tools/*shaders.py` | Local shader extraction, translation and cache identity |
| `runtime/kernel.c`, `runtime/threads.c` | Memory, handles, threads, events, mutexes and condition waits |
| `runtime/host.c`, `runtime/host.h` | Windows/Mach reservation, commit, protection, discard, checked copies and entropy |
| `runtime/services.c`, `runtime/filesystem.c`, `runtime/audio*` | IPC, transactional file access and audio services |
| `script/build_and_run.sh` | Shared build/launch entry point with process-identity checks |
| `tests/` | Synthetic checks without game data or keys |

Link verification and loading are distinct. `link-probe` checks compiled function
entries. `--load-only` consumes a temporary plan, maps and relocates module data,
checks entries and releases the mappings. Neither executes guest initialization.
Original ARM64 `.text` is mapped as non-executable data; host execution uses the
compiled C functions.

The load-only runtime uses Windows virtual memory APIs on x86-64, with UTF-8 path
conversion at its native entry point. Module table declarations are generated from
the selected profile rather than fixed to one adapter. This shared core supports
up to 13 NSO modules. Kernel-only initialization uses the same module tables,
guest stack/TLS map and thread algorithms on Windows. Basic-services mode adds
the existing CMIF/TIPC/session code and portable initialization handlers, with
optional verified read-only storage and explicit host-font substitution. Windows
can also opt into temporary scratch filesystems. Offscreen Vulkan adds bounded
resources, image transfers, compute and shader draws, plus silent audio timing.
An opt-in Win32 preview paints completed readbacks through GDI.
Unsupported operations stop explicitly; direct GPU presentation, persistent storage
and audible output require separate work.
See [Windows contracts](windows.md).

The C output depends on Clang facilities such as `musttail`, 128-bit integers and
atomic operations. Generating C is not proof of arbitrary compiler or platform
support. Each host needs independently validated address reservation, threads,
clocks, files, graphics, audio and input.

See [loading](loading.md), [initialization](startup.md), [CPU limitations](cpu-validation.md)
and [graphics](graphics.md) for implementation-specific contracts. Current
validation limits are maintained in [status](status.md).

Format references: [NSO](https://switchbrew.org/wiki/NSO0),
[ELF RELR](https://gabi.xinuos.com/elf/06-reloc.html#relative-relocation-table),
[NSZ/NCZ](https://github.com/nicoboss/nsz/blob/master/docs/formats.md).
