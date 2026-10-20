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
input fingerprints and runtime boundaries. No source or generated game data from
its checkout is vendored here. Nintendo properties and user-supplied game files
are not covered by this project's or LASRecomp's license.

The local Ryujinx/Ryubing (root MIT) and Eden (GPLv3-or-later) checkouts were
reviewed as references only. No code from either is included or linked here.
Pinned revisions, source locations, per-file license caveats and the intended
reuse boundaries are recorded in [the reference review](docs/emulator-references.md).
