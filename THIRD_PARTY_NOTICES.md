# Third-party sources

`third_party/lasrecomp/` is a source-only subset of
[LASRecomp](https://github.com/peiikai/LASRecomp), copyright 2026 Peiikai, MIT.
The original notice is retained in `third_party/lasrecomp/LICENSE`.
Its exact revision and file digests are recorded in `third_party/lasrecomp/SOURCE.json`.
The subset includes NSO analysis/generation, synthetic tests and portable CPU helpers;
the Link's Awakening patch tables, randomizer, UI and game content are not included.
Use this subset through `tools/recompile.py`, which always disables game-specific patches.

Development dependencies are installed separately, not vendored or linked into a game:

| Dependency | Version | License / use |
| --- | --- | --- |
| [Capstone](https://github.com/capstone-engine/capstone) | 5.0.7 | BSD 3-clause; instruction analysis |
| [python-lz4](https://github.com/python-lz4/python-lz4) | 4.4.5 | BSD 3-clause; bundled LZ4 BSD 2-clause; NSO decompression |
| [Unicorn](https://github.com/unicorn-engine/unicorn) | 2.1.4 | GPL 2.0 engine / BSD Python bindings; generator analysis and test oracle |

[Zelda-Mobile](https://github.com/OJZen/totk-mobile) is a reference for the v1.4.3
input fingerprints and runtime boundaries. No game data from its checkout is
included here. The memory, IPC, filesystem and audio sources in `runtime/`, and
`tests/native_services.c`, adapt its committed implementations under Apache-2.0; the original
license and pinned source digest are in `third_party/zelda-mobile/`. The local
adaptation uses this project's loaded module metadata and guest stack. Its
address-space layout follows d2r-mobile; that MIT notice is retained under
`third_party/zelda-mobile/d2r/LICENSE`. `runtime/threads.c` adapts the MIT thread
and synchronization algorithms to C CPU dispatch, independent guest stacks,
retained wait-object references and cooperative shutdown. The audio output
design also follows d2r-mobile's CoreAudio queue. Building
does not read the neighboring reference repository. Nintendo properties and user-supplied game files
are not covered by this project's or LASRecomp's license.

The startup probe links the separately installed [Opus](https://opus-codec.org/license/)
library (BSD 3-clause, installed version 1.6.1 in the validation environment).
Its source, headers and library are not vendored. Apple AudioToolbox/CoreAudio
are system frameworks. Synthetic audio tests run with speaker output disabled;
they do not establish audible game playback.

The NVN API headers under `third_party/zelda-mobile/core/gfx/nvn/` are retained
byte-for-byte from the same pinned source. `include/nvn.h` keeps its MIT notice,
copyright 2020 h1k421; the generated prototype header is part of the d2r-mobile
MIT subset. They describe the host interface and contain no game resources.

The local Ryujinx/Ryubing (root MIT) and Eden (GPLv3-or-later) checkouts were
reviewed as references only. No code from either is included or linked here.
Pinned revisions, source locations, per-file license caveats and the intended
reuse boundaries are recorded in [the reference review](docs/emulator-references.md).
