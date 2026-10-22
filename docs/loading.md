# Module loading and relocation

`make load-only EXEFS=/path/to/exefs` checks actual mappings, relocated memory and
compiled entries for the configured adapter. It **does not execute guest
initialization, the rtld entry point or `nnMain`**. All mappings are released on exit.

## Responsibilities

1. `tools/load.py` validates input fingerprints and link/runtime receipts while
   holding the build's shared lock. It reuses the NSO/RELR parser, validates BSS,
   symbols, strings and initialization metadata, and prepares a temporary plan.
2. `runtime/load.c` reserves the requested guest addresses without overwriting
   existing mappings. It copies initial data, preserves zeroed BSS and applies
   ordered, non-overlapping 64-bit relocation writes after range, size, alignment
   and budget checks. Partial failures undo mappings created by that attempt.
3. C computes CRC32 over relocated memory and compares it with an independently
   prepared Python image. CRC32 checks loading consistency; SHA-256 identifies
   inputs. Initialization entries must resolve into the compiled function table.
4. `runtime/probe.c --load-only` reports results and unmaps the image. The
   supervisor owns temporary files and cleans them even when a worker is stopped
   by a resource guard. Only success replaces a successful result receipt.

On macOS, fixed allocation uses `mach_vm_allocate` with `VM_FLAGS_FIXED`, without
OVERWRITE. An occupied address produces status 75. The load-only supervisor may
retry that status in at most eight fresh processes, within its overall budget;
other errors fail immediately. It never releases someone else's reservation.
This probe policy does not establish the final process layout on another host.

The versioned plan is a development format containing module/segment ranges,
BSS, initialization metadata, checksums, private initial images and resolved
writes. It belongs under ignored `work/tmp/`; it is not a distribution format or
proof of an independent on-device importer.

## Symbol and permission contracts

Supported relocation types are ABS64, GLOB_DAT, JUMP_SLOT and RELATIVE, including
expanded RELR; NONE performs no write. Undefined weak symbols use S = 0 while
still applying S + A. Absolute symbols are not rebased. Local, hidden, internal
and protected definitions bind within their module; other exports use the first
matching definition in fixed module order, including weak definitions.

Synthetic tests cover this limited model. Unsupported TLS relocations, symbol
versioning, IFUNC and SYMBOLIC behavior are rejected. Do not equate these checks
with a complete differential validation of a guest dynamic linker.

Original guest `.text` remains read-only and non-executable. Host instructions
come from compiled C. Data/BSS is writable; uncovered whole pages are inaccessible.
A 16 KiB host page can combine guest read-only and writable data, so exact 4 KiB
guest permissions are not always representable. Tests check read-only write
rejection and mixed-page accounting.

The host has already applied relocations; later initialization must not rerun
rtld relocation. See [startup](startup.md) for guest execution and
[CPU validation](cpu-validation.md) for semantic limits. Load-only success does
not prove graphics, audio, gameplay or another platform.

References: [ELF symbols](https://gabi.xinuos.com/elf/05-symtab.html),
[RELR](https://gabi.xinuos.com/elf/06-reloc.html#relative-relocation-table),
[AArch64 ELF ABI](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst).
