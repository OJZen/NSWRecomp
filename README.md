# NSWRecomp

An experimental Switch ARM64 static recompilation toolkit and runtime, designed
for multiple title adapters and host platforms. It translates user-supplied ARM64
executables into C and connects the compiled code to a host runtime.

Development moved from `totk-recomp` into this repository. The application platform
is **macOS on Apple Silicon**. **Windows x86-64** supports the native build,
load-only, isolated CPU checks and bounded initialization with basic IPC services
and optional read-only RomFS storage and temporary scratch filesystems.
An optional headless Vulkan path supports bounded NVN resource/command recording,
image transfers, depth/stencil clears, compute dispatch and shader draws with
offscreen readback; see the
[Windows guide](docs/windows.md).
Additional adapters and host backends remain under development.
See [status and validation boundaries](docs/status.md) before
interpreting build or runtime results as compatibility claims.

## Getting started

Use Python 3.14 or later. The current validation toolchain uses Apple Clang 21.
Keep temporary files and caches on the same volume as the repository.
Windows users should use the PowerShell setup and LLVM MinGW toolchain in the
[Windows guide](docs/windows.md); the commands below describe macOS.

```sh
mkdir -p work/tmp
python3.14 -m venv work/venv
TMPDIR="$PWD/work/tmp" work/venv/bin/python -m pip install --no-cache-dir -r requirements.txt
make test
```

Synthetic checks do not require game files or keys. Native tests need the host
dependencies listed in the [adapter setup guide](docs/totk.md). Once local inputs
and tools have been prepared, the development entry point is:

```sh
./script/build_and_run.sh
```

## Repository layout

| Path | Responsibility |
| --- | --- |
| `third_party/lasrecomp/` | Locally maintained ARM64 analysis, C generation and CPU semantics, with pinned provenance |
| `runtime/` | Module loading, guest memory, threads, services, audio/video and host integration |
| `runtime/graphics/` | NVN bridge, resource lifetime, synchronization and the current Metal backend |
| `config/`, `runtime/game_timing.*` | Adapter-specific versions, module layout, hooks and timing |
| `tools/`, `script/`, `tests/` | Import/build tools, resource guards, launchers and synthetic checks |
| `local/`, `work/` | Ignored inputs, saves, tools, caches and private validation evidence |

All new development belongs in NSWRecomp or its own worktrees. Reference
checkouts are not build dependencies. Experimental branches and original
migration tags stay local until separately reviewed; only `main` is published.
See the [migration notes](docs/migration.md) before transferring older work.

- [Documentation index](docs/README.md)
- [Contribution, merge and publication requirements](CONTRIBUTING.md)
- [Current architecture](docs/architecture.md) and [architecture roadmap](docs/architecture-roadmap.md)
- [Source attribution and licenses](THIRD_PARTY_NOTICES.md)

Public documentation is English-first and focuses on implementation contracts,
synthetic validation and essential setup. Detailed gameplay reports and local
user data stay outside the public documentation tree.

Original code is [MIT-licensed](LICENSE). Imported code retains its own notices
and licenses. Those licenses do not cover user-supplied game data or derived assets.
