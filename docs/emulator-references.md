# Emulator reference assessment

This records a source review from 2026-10-20. Ryujinx provided useful CPU-state,
service and texture/shader references, with Eden used for comparison. The project
continues to use ARM64-to-C static recompilation rather than adopting a complete
emulator runtime.

The review itself was source-only. Actual imported/adapted files are independently
listed in [third-party notices](../THIRD_PARTY_NOTICES.md) and their source manifests.
Do not interpret a reference implementation as proof of this runtime's behavior.

## Reviewed revisions

| Reference | Revision reviewed | License boundary |
| --- | --- | --- |
| Ryujinx/Ryubing reference checkout | `475615f0431b4995b03c930c5089d20f6f3570e0` | MIT root notice; inspect each dependency |
| Eden reference checkout | `989768aa46720e3fcab9e2111e3ddd70819c382b` | GPLv3-or-later root notice; files may have additional notices |

These are historical review revisions, not claims about the latest upstream state.
External checkouts remain read-only and are not build dependencies.

## Loading and CPU semantics

Emulator loading code is useful for module order, segment placement, BSS and
permissions. It does not supply NSWRecomp's host relocation/symbol resolution or
guest-address-to-compiled-function dispatch. Native-execution patch relocation
is also not a substitute for ELF relocation. Keep [load-only](loading.md) checks
separate from initialization and reject unsupported relocation mechanisms.

FPSR/QC implementations and tests offer useful cases for cumulative status,
clearing, NaNs, denormals and MRS/MSR. Approximate fast paths, unimplemented traps
or explicitly disabled tests must not become the sole correctness oracle.
Do not equate host floating-point state with guest state.

Value-based exclusive-monitor paths do not by themselves solve ABA behavior
from ordinary intervening writes. Either prove transaction-conversion conditions
or cover all relevant writes in reservation invalidation. Preserve the
[CPU validation limits](cpu-validation.md).

Instruction support also depends on the selected architecture level. A decode
entry in a newer backend does not establish availability under a Cortex-A57
preset or prove an unsupported SDK path unreachable. Keep explicit traps unless
semantics or unreachable-path claims have been independently established.

## Useful subsystem references

| Area | Source locations and review focus |
| --- | --- |
| Module loading | Ryujinx `src/Ryujinx.HLE/Loaders/`; Eden `src/core/loader/` and `src/core/file_sys/` |
| Threads and IPC | Ryujinx `src/Ryujinx.HLE/HOS/Kernel/` and `HOS/Ipc/`; Eden `src/core/hle/kernel/`; waiter ordering, cancellation and buffer bounds |
| CPU behavior | Ryujinx `src/ARMeilleure/Instructions/` and `src/Ryujinx.Tests/Cpu/`; Eden's Dynarmic monitor and memory callbacks |
| Texture layout | Ryujinx `src/Ryujinx.Graphics.Texture/`; synthetic dimensions, mip levels and block-compressed boundaries |
| Shader translation | Ryujinx `src/Ryujinx.Graphics.Shader/`; decode, IR, resource environment and host emission contracts |
| Pipeline/cache behavior | Ryujinx `src/Ryujinx.Graphics.Gpu/Shader/DiskCache/`; Eden `src/video_core/`; guest inputs, host outputs and version invalidation |
| Container preparation | Content selection, integrity-checked base/update views and bounded streaming rather than unconditional expansion |

Static CPU recompilation does not translate GPU programs. A shader compiler also
needs constants, descriptors, resource state and backend capabilities supplied by
our runtime. Avoid importing an entire host/service stack for one useful compiler
or test component.

NSZ/NCZ preparation remains a separate bounded import layer. Containers may mix
NCA/NCZ, and compressed inputs may use continuous or block-based layouts; do not
assume cheap random access. Follow the [NSZ format description](https://github.com/nicoboss/nsz/blob/master/docs/formats.md)
and retain user data and keys only in ignored local directories.

Preserve Ryujinx MIT notices and inspect dependency licenses. Eden is a behavioral
reference; no Eden source is copied or linked. A historical permissive header is
not sufficient justification to ignore additional notices in a reviewed file.
