# CPU validation boundaries

The derived compiler and CPU helpers are maintained in
`third_party/lasrecomp/`. Its `SOURCE.json` records upstream origin and local
changes. Synthetic tests do not require game files or keys; `make test` builds
temporary test libraries under `work/tmp/` and removes them after use.

## Exclusive accesses

Adjacent exclusive load/store pairs are converted to a 64- or 128-bit CAS
transaction only when the address register is unchanged, widths match and the
second instruction has no independent entry, patch or hook. Register results
are committed after a successful CAS. Unsafe or unproven cases are rejected
rather than silently lowered to ordinary loads and stores.

Synthetic coverage includes register aliasing, zero registers, SP addressing,
width and ordering combinations, swapped halves, status-register overlap and
native threaded publication. Scalar reservation tests cover absent/consumed
reservations, `CLREX`, address/width mismatch and intervening value changes.
Each store-exclusive attempt consumes its reservation.

**Scalar LL/SC still has an ABA limitation.** Comparing a saved value cannot
observe an intervening A → B → A write. Address/width validation does not solve
this. A complete solution needs proven transaction conversion or reservation
invalidation covering every relevant CPU and HLE write. Passing selected pair
cases is not a proof of general concurrent correctness.

## Floating-point state and saturation

Synthetic instruction sequences compare results, registers, NZCV and FPSR with
Unicorn and, where supported, native ARM64 execution. The suites cover boundary
and random bit patterns, NaNs, infinities, denormals, signed zero, FPCR settings,
register aliasing and active vector lanes.

The implemented state behavior includes:

- Cumulative QC for supported saturating integer operations, including intermediate
  saturation and operations whose inactive vector halves must not affect state.
- Quiet/signalling NaN handling for comparison IOC, conditional comparisons,
  shared FZ/NaN helpers and selected min/max cases.
- FRINTX inexact status and supported float-to-integer IOC/IXC behavior, including
  preserving previously set status and avoiding spurious integer QC.
- MRS/MSR readback, clearing and preservation of unrelated status bits.

Portable-C comparison helpers carry signalling information; old generated C
must be regenerated when the helper contract changes. The tests and state model
refer to the [Cortex-A57 TRM](https://documentation-service.arm.com/static/5e906b9fc8052b1608760b6b).

**Floating-point state is not complete.** General arithmetic and precision
conversion still lack full exception coverage, and exception-trap delivery is
not implemented. Inactive lanes, special multiplication/reciprocal behavior,
tininess and host floating-point environment isolation require further work.
Do not merge host FPSR into guest state unconditionally or treat selected
instruction-family results as complete fenv support.

## Unsupported instructions and host differences

The current adapter still contains an unsupported SDK memory-tagging `STG`.
It remains an explicit trap; its conditional nature is not evidence that it is
unreachable, and an unproven NOP patch is not acceptable. A full generation/build
report with status 2 retains this limitation rather than claiming complete support.

Validate 128-bit atomics, alignment and runtime-library requirements separately
on each host. Native Apple Silicon results do not establish Linux ARM64 or
Windows x86-64 behavior. See the [compiler atomic builtins documentation](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html).
