# Architecture roadmap

This design direction was established on 2026-10-21. It prioritizes long-term
maintenance, explicit correctness contracts and independently extensible backends.
Current implementation and evidence remain documented in
[architecture](architecture.md) and [status](status.md).

## Decision criteria

A substantial change is justified when it removes identifiable coupling and has
a practical validation path. A new abstraction or rewrite is not a benefit on
its own; preserving an upstream internal interface is also not a reason to
accumulate permanent workarounds.

Keep the current execution path as a reproducible baseline. Internal interfaces,
cache formats and module ABIs may change through explicit migration points.
Separate builds and deliberate switches protect that baseline without requiring
indefinite compatibility between internal representations. User data, active
runs and resource guards remain protected.

## CPU and runtime contracts

- The generic ARM64 core owns instruction state, control flow, memory semantics
  and target emission. Adapters supply addresses, patches, module layouts and
  explicitly permitted compatibility behavior.
- Represent state access, widths, exceptions, atomics and memory side effects
  structurally. `Insn.code` strings can remain a transitional implementation,
  but future analysis should consume semantic information.
- Separate guest services and guest-address access from host threads, memory,
  files and devices. Direct mapping can remain an optimized implementation;
  HLE and asynchronous access must obey any new address-model contract.
- Keep the C backend as an executable reference. Add LLVM or another target only
  when concrete interface needs justify it, using the same semantic tests.

CPU and GPU IRs should fit their different execution models. Share diagnostic,
provenance and validation principles rather than forcing one operation model
onto both. Maintain generic CPU fixes in NSWRecomp with upstream origins,
licenses, local-change records and hashes intact.

Validate shared interfaces with more than one adapter and a second host platform.
Independent execution in older projects does not prove a shared implementation.

## GPU compiler contracts

The target flow is:

```text
Maxwell bytecode + guest shader environment
                    |
           Decode and semantic lifting
                    |
             Explicit GPU semantic IR
                    |
       Shared validation and semantic optimization
              /                         \
    Metal capability lowering     Vulkan capability lowering
              |                         |
          MSL emission             SPIR-V emission
              |                         |
    Metal compilation/execution   Vulkan compilation/execution
```

MSL and SPIR-V are peer targets. Keep the existing SPIR-V → SPIRV-Cross → MSL
route as a migration reference. Changing the backend should not require changes
to Maxwell bitfield decoding; capability differences belong in explicit lowering.
The guest environment provides stages, constants and logical resources, while
target layout assigns Metal slots or Vulkan sets/bindings.

Define support or rejection rules for precision, control flow, subgroups,
barriers and memory effects. Derive reflection, stage interfaces and bindings
from structured data. Move existing generated-MSL text rewrites into semantic
optimization, target lowering or adapter policy as appropriate, preserving tests.

NVN state, resource lifetime, upload, synchronization and presentation remain
runtime responsibilities. Supply compiler inputs explicitly and include all
specialization state and semantic options in cache identity.

Use Ryujinx's implementation and validation work where its contracts fit. Refine
or replace IR only for demonstrated gaps, not merely to rename it. Keep upstream
runtime dependencies out of the generic core and preserve provenance when
absorbing changes.

## Validation and migration

Begin with bounded, representative synthetic compute cases and independently
checked outputs. Private real-input cases can test applicability without becoming
public fixtures. Increase coverage for semantic reasons, not to meet a success count.

A candidate must demonstrate:

1. Shared decoding and semantics serving both targets, with backend-specific work
   confined to its target layer.
2. A frontend fix reaching both paths through shared implementation and tests.
3. An IR that expresses the required numeric, control-flow and resource behavior,
   with rejection diagnostics identifying the source operation and reason.
4. Comparable inputs, precision contracts, reflection and bindings. Measure
   frontend, optimization and backend changes separately.
5. Separate translation, library/pipeline compilation, GPU execution and memory
   measurements. Maintenance benefits and speedups are distinct claims.
6. Public synthetic checks without games or keys; real inputs and outputs stay local.

Reassess designs whose semantics cannot be stated or tested, whose target details
leak back into decoding, or whose added layers duplicate fixes. A maintainability
improvement can be useful without a GPU speedup; switching the default path still
requires compatibility, performance and resource validation.

Establish contracts and samples before compute support, then extend to vertex,
fragment and drawing paths. Give interface changes explicit migration points and
retire redundant reference paths only after validation. Keep experimental builds
under ignored `work/experiments/`, separate from normal launch outputs, with
bounded compiler/GPU jobs and the existing memory/free-space guards.

A cache index or pipeline record is not a complete replay input. This roadmap
itself does not establish a new IR implementation, backend or GPU comparison.
