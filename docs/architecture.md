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
| `runtime/probe.c`, `runtime/load.c` | Dispatch, mapped-memory validation, relocations, permissions and cleanup |
| `tools/startup.py`, `runtime/startup.c` | Bounded guest initialization with stack, TLS and diagnostic checkpoints |
| `tools/import_game.py`, `tools/extract_*.py` | Local container/resource preparation and private temporary-file cleanup |
| `runtime/graphics/`, `tools/graphics.py` | Cpu-to-NVN bridge, Metal, AppKit, display and input |
| `tools/*shaders.py` | Local shader extraction, translation and cache identity |
| `runtime/kernel.c`, `runtime/threads.c` | Memory, handles, threads, events, mutexes and condition waits |
| `runtime/services.c`, `runtime/filesystem.c`, `runtime/audio*` | IPC, transactional file access and audio services |
| `script/build_and_run.sh` | Shared build/launch entry point with process-identity checks |
| `tests/` | Synthetic checks without game data or keys |

Link verification and loading are distinct. `link-probe` checks compiled function
entries. `--load-only` consumes a temporary plan, maps and relocates module data,
checks entries and releases the mappings. Neither executes guest initialization.
Original ARM64 `.text` is mapped as non-executable data; host execution uses the
compiled C functions.

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
