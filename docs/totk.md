# Initial adapter setup

The current adapter expects **TOTK v1.4.3** inputs and targets **macOS Apple
Silicon**. This version requirement is an input contract, not a claim of complete
compatibility. Keep detailed game results and user data in local records; see
[current status](status.md) for the public validation boundary.

## Dependencies and synthetic checks

Use Python 3.14+, Apple Clang 21, `pkg-config` and `opus`. Graphics/video checks
also require `libvpx` and `libvorbis`; graphics tool preparation needs `cmake`, `make` and network
access for pinned dependencies. On macOS, host libraries can be installed with
`brew install pkg-config opus libvpx libvorbis`.

```sh
mkdir -p work/tmp
python3.14 -m venv work/venv
TMPDIR="$PWD/work/tmp" work/venv/bin/python -m pip install --no-cache-dir -r requirements.txt
make test
```

Synthetic checks do not require game files, keys or saves. The importer uses
Python 3.14's Zstandard support and the pinned `cryptography` dependency.
All preparation and build outputs stay on the repository's volume.

## User-supplied inputs

For the currently supported solid NSZ format:

```sh
make import-game BASE=/path/to/base.nsz UPDATE=/path/to/update-v1.4.3.nsz KEYS=/path/to/prod.keys
```

The importer verifies NCA, BKTR/IVFC, ExeFS modules and the complete RomFS
fingerprint. Imported resources stay in `local/game/`; temporary archives and
key copies are removed when preparation ends. Keys and packages are never part
of the public source tree. Successful import writes ignored `local/inputs.json`.

Alternatively, point individual tools at already extracted, unmodified ExeFS
and a matching merged RomFS. The pipeline validates the configured module
fingerprints before generation. It does not use neighboring reference inputs
implicitly.

## Generation and validation stages

Run stages separately and inspect their results:

```sh
make inspect EXEFS=/path/to/exefs
make generate EXEFS=/path/to/exefs MODULE=rtld
make probe EXEFS=/path/to/exefs
make generate EXEFS=/path/to/exefs MODULE=all
make check-c EXEFS=/path/to/exefs MODULE=all
make build EXEFS=/path/to/exefs
make load-only EXEFS=/path/to/exefs
make startup EXEFS=/path/to/exefs THROUGH=nnosInitialize
make startup EXEFS=/path/to/exefs ROMFS=/path/to/romfs.bin
```

- `probe` compares a bounded set of compiled functions against Unicorn and
  independent expected memory results. Unicorn is an analysis/test dependency.
- Generation uses portable C and disables upstream title-specific patches,
  hooks and randomizer variants. Receipts remain under `work/`.
- `check-c` runs the Clang frontend over generated units. `build` produces native
  objects and a function-table verification executable; it does not run gameplay.
- Build caching validates source, headers, compiler and object hashes. Interrupted
  or incomplete outputs are not accepted as valid cache entries. Renumbered
  chunks can reuse equivalent objects; temporary hard links are removed afterward.
- `load-only` maps, relocates and checks module data without initialization.
  `startup` separately establishes guest stack, TLS, memory and services, then
  executes to a requested checkpoint. See [loading](loading.md) and [startup](startup.md).

The known unsupported SDK `STG` remains visible: full generation, checking and
build validation can return **status 2**, retaining diagnostic outputs. Do not
report that as complete instruction support. Successful native probes return 0.
Other semantic limits are described in [CPU validation](cpu-validation.md).

## Bounded development launcher

Prepare the graphics tools before launching configured local inputs:

```sh
work/venv/bin/python tools/bounded.py --log work/logs/shader-tools-prepare.log -- tools/prepare_shader_tools.sh
./script/build_and_run.sh
./script/build_and_run.sh --seconds 600
```

The launcher accepts explicit `--exefs` and `--romfs` paths as well as the local
input configuration. It defaults to 180 seconds and allows 1–600 seconds.
`--fps` selects a supported target rate for the current run, and `--jobs` selects
compiler concurrency; see the [control panel guide](control-panel.md).

The generated `work/app/TOTK Recompiled.app` uses the same entry point and allows
at most ten minutes per launch. It depends on the source checkout and cannot be
moved away as an independent distribution package. Preserve `work/save/`; use
an independent copy for tests that may change progress.

C compilation defaults to at most eight jobs with a 4 GiB aggregate RSS guard;
single-job/ordinary probes use 1.5 GiB. Graphics linking uses 2 GiB and startup
uses 3 GiB, with a separate 4 GiB native physical-footprint guard. Keep at least
20 GiB free. Polling guards fail closed when monitoring is unavailable; they are
not operating-system hard quotas.

See [architecture](architecture.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
Original code uses the [MIT license](../LICENSE). The project is not affiliated
with Nintendo; game data and derived content are not covered by that license.
