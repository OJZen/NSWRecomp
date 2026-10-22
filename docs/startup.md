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

Provide resources explicitly with `ROMFS=/path/to/romfs.bin`. Initial validation
streams the full fingerprint; later receipts are bound to file identity, size
and timestamps. Failure does not replace an earlier successful validation record.
The tools do not infer resources or saves from a neighboring reference checkout.

## Thread and memory contracts

Each guest thread has its own Cpu state, TLS/TLR and guest stack. Host C frames
use a separate pthread stack. Exit unwinds C frames before publishing completion,
so subsequent guest-stack unmapping cannot invalidate active host frames.

Event waits retain object references after public handles close. Condition waits
publish their wait key before releasing the mutex word, then hand off by priority
and arrival order within a priority. Synchronization waits consume cancellation
state. Checkpoint shutdown is cooperative; the enclosing process budget remains
the final bound.

The current implementation retains the mixed 4 KiB guest / 16 KiB host page
limitation. TLS slots are reclaimed at process exit; long-running thread churn
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

Normal AppKit window closure reports `window_closed`, not a successful return
from `nnMain` or completion of every initialization stage. Early closure still
emits a structured report, deduplicated with `atexit`. Automated fields such as
`title_verified` do not replace separately observed visual evidence.

Interface references: [TLR](https://switchbrew.org/wiki/Thread_Local_Region),
[SVC](https://switchbrew.org/wiki/SVC), [emulator reference assessment](emulator-references.md).
