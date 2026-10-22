# Current status — 2026-10-22

NSWRecomp has an independent development baseline with an initial title adapter
on **macOS Apple Silicon**. Shared multi-title interfaces and additional host
backends remain work in progress; see the [architecture roadmap](architecture-roadmap.md).

## Evidence boundaries

| Layer | Recorded evidence | Remaining boundary |
| --- | --- | --- |
| Synthetic checks | Compiler and project suites passed during migration | Not a game or cross-platform compatibility result |
| C compilation and linking | Existing objects were verified and relinked in the new checkout | The known unsupported SDK instruction still produces status 2 |
| Load-only verification | Module mappings, relocations and compiled entries were checked in the new checkout | No guest initialization or gameplay execution |
| Runtime integration | Earlier adapter-specific runtime checks are recorded privately | They do not validate a new checkout, binary, title or platform |
| Shader preparation | Limited compute and render pipeline preparation has been checked | Not complete coverage or a controlled cold-cache performance result |

Detailed game results, scenes, save information and per-run measurements are
kept in ignored local records. The public repository contains no game input,
generated game code, captured shaders or saves.

## Development entry points

Use the [documentation index](README.md) and [adapter setup guide](totk.md).
For already configured local inputs:

```sh
./script/build_and_run.sh
./script/build_and_run.sh --seconds 600
```

This is a bounded development launcher that depends on its source checkout.
Check source, binary and evidence versions before continuing any runtime task;
use independent save copies and preserve user progress.

## Open limits

- SDK `STG` remains an explicit trap. Scalar LL/SC ABA behavior and parts of
  floating-point exception handling remain incomplete; see [CPU validation](cpu-validation.md).
- Mixed guest/host page permissions and process-lifetime TLS reclamation have
  known limits. Long-running thread churn and complete save/reload flows need validation.
- Complete title compatibility, long-term stability, audible output, physical
  controller coverage and high-frame-rate timing are not generally established.
- Linux ARM64, Windows x86-64 and iOS still need backend and runtime validation.
- First-use shader work can still stall rendering. Broader coverage needs
  controlled cache conditions and comparable inputs.

## Resource and publication limits

The launcher defaults to 180 seconds and permits at most 600. Ordinary tasks and
C generation use a 1.5 GiB RSS limit. Compilation defaults to at most eight jobs
and 4 GiB aggregate RSS; graphics linking uses 2 GiB and startup uses 3 GiB.
The native graphics process has a separate 4 GiB physical-footprint guard.
Keep at least 20 GiB free on the external volume. These are polling safeguards.

Only `main` is published. Old local histories and experiments require the
[merge review procedure](../CONTRIBUTING.md); do not merge private ancestors
back into public history. Keep detailed validation records local and this page brief.
