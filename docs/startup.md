# Initialization, services and threads

`make startup EXEFS=/path/to/exefs THROUGH=nninitInitializeSdkModule` executes
compiled C in an isolated process. `THROUGH` defaults to `nnMain`. Original ARM64
text remains non-executable; unknown instructions, entries or services stop
execution rather than returning fabricated success.

Initialization follows the configured SDK order: `_init_libc0`, `nnosInitialize`,
`_init_libc1`, `nninitInitializeSdkModule`, `nninitStartup`, `_init_libc2`, module
constructors and `nnMain`. Host relocation is already complete, so startup must
not re-enter rtld. Reaching a checkpoint or creating a swapchain is distinct from
observing correct rendering or completing gameplay validation.

`tools/startup.py --kernel-only` links only the loader, kernel and threads. This
mode is available on Windows through `script/windows.ps1 startup`; it stops at
IPC without opening resources or providing audio services. Only exports through
the requested stage are required. Receipts identify the mode, completed stages,
stop reason and timeout; a stopped run is not a completed checkpoint. The Windows
entry point uses UTF-8 paths and reports host faults without an error dialog.

`--basic-services --npdm-sha256 <expected hash>` adds the shared CMIF/TIPC parser,
session/domain ownership and selected initialization services. It reads
`ExeFS/main.npdm`, verifies the supplied fingerprint and checks bounded ACI/ACID
client-service lists. This does not verify Nintendo signatures or implement all
process capabilities. An absent client permission returns SM NotAllowed; a
permitted but unimplemented service or method stops explicitly. Basic services
also include graphics/devtools allocator registration and compiled guest callback
support; graphics device initialization remains unsupported in this mode. See
the [callback contract](graphics.md#call-and-callback-contracts). Kernel-only mode
remains available to isolate service-independent checks. See the
[Windows service scope](windows.md#basic-ipc-services).

On Windows, `--basic-services --vulkan-headless` adds Vulkan device/queue
initialization, bounded NVN resources and command recording, image transfers,
compute and shader draws, offscreen VI events and silent audio timing. Receipts
record the distinct mode, device creation, completed clear/compute/draw counts
and verified clear-readback pixels. Unsupported operations remain stopped runs;
desktop presentation is unimplemented. See the
[independent hardware check](windows.md#headless-vulkan-device-and-transfer-check).

Provide resources explicitly with `ROMFS=/path/to/romfs.bin`. Initial validation
streams the full fingerprint; later receipts are bound to file identity, size
and timestamps. On Windows every verification streams the fingerprint under a
read-sharing-only handle; metadata alone cannot justify reuse after rapid writes.
Failure does not replace an earlier successful validation record.
The tools do not infer resources or saves from a neighboring reference checkout.

## Thread and memory contracts

Each guest thread has its own Cpu state, TLS/TLR and guest stack. Host C frames
use a separate pthread stack. Exit unwinds C frames before publishing completion,
so subsequent guest-stack unmapping cannot invalidate active host frames.
Main-thread TLS/TLR also belongs to the guest map below the 39-bit address limit.
Windows uses LLVM MinGW's winpthreads for the existing synchronization and
monotonic clock interfaces. Host memory operations reserve without replacement,
commit on demand, preserve NX and zero-fill discarded pages on recommit.

Event waits retain object references after public handles close. Condition waits
publish their wait key before releasing the mutex word, then hand off by priority
and arrival order within a priority. Synchronization waits consume cancellation
state. Checkpoint shutdown is cooperative; the enclosing process budget remains
the final bound.

Hosts with 16 KiB pages retain the mixed 4 KiB guest / 16 KiB host page
limitation. Stack mappings still use the documented copy-back ownership model,
and kernel resource limits retain the initial adapter's policy. TLS slots are
reclaimed at process exit; long-running thread churn
needs reusable slots. Unknown scheduling and service behavior fails explicitly.
A virtual-core mask is not evidence of physical host-core affinity.

## Services and checks

IPC, filesystem transactions and audio services adapt pinned sources listed in
`third_party/zelda-mobile/SOURCE.json`. They use this project's own working data
and are not dependent on an external checkout.

Synthetic checks exercise state continuation, handles, memory bounds, malformed
IPC, thread ordering, cancellation, retained waits and shutdown. IPC/thread checks
use address and undefined-behavior sanitizers. IPC checks disable ASan's fake
stack because their guest-output buffer registration uses real pthread stack
bounds; other address checks remain enabled. Synthetic audio keeps speaker output
disabled and does not establish audible playback.

The portable core suite includes kernel memory, sanitized thread execution,
startup state continuity, explicit IPC rejection, a real stack guard fault and
supervisor timeout. Basic IPC tests also cover CMIF/TIPC, malformed descriptors,
domain clones, retained events, concurrent sessions, handle exhaustion and guest
pointer failures under ASan/UBSan. Synthetic NPDM fixtures cover bounds,
fingerprints, client/server distinction and trailing wildcard permissions.
Read-only time services share an immutable wall/monotonic anchor with the SDK
clock page. Synthetic tests check source identity, both snapshot slots,
nanosecond/second units, overflow and mapping permissions; see the
[time service contract](windows.md#read-only-time-service).
Optional read-only RomFS storage uses the same IPC session/domain machinery.
Windows startup retains and inherits the verified file handle; synthetic checks
cover 64-bit offsets, read bounds, invalid headers and write exclusion. Resource
profiles bind the image fingerprint to every selected ExeFS module. Windows
checks also cover temporary scratch filesystems and silent audio ownership;
persistent saves and audible output need separate validation.

The native receipt identifies `kernel_only`, `basic_services`, `vulkan_headless`, `vulkan_window` or `full` runtime
mode. Missing host APIs report `unsupported_host_api`; an unreported exit remains
`incomplete`. Checkpoint success is assigned only after requested stages and
shutdown complete. Reaching `nnMain` is distinct from its successful return or
the creation of a working graphics backend.

Normal AppKit or Windows preview closure reports `window_closed`, not a successful return
from `nnMain` or completion of every initialization stage. Early closure still
emits a structured report, deduplicated with `atexit`. Automated fields such as
`title_verified` do not replace separately observed visual evidence.
Windows preview receipts distinguish window creation, accepted frame copies and
completed UI paints. If fatal reporting finds the presenter lock busy, these
counters retain their last snapshot as a lower bound rather than blocking the
watchdog. See the [Windows preview contract](windows.md).

Interface references: [TLR](https://switchbrew.org/wiki/Thread_Local_Region),
[SVC](https://switchbrew.org/wiki/SVC), [emulator reference assessment](emulator-references.md).
