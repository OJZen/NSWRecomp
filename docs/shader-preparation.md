# Shader and pipeline preparation

The objective is to reduce first-use compilation stalls without changing rendering
semantics. Keep extraction, source analysis, translation, Metal compilation,
pipeline creation and observed output as separate evidence stages. Detailed
real-input catalogs and per-run game measurements remain local.

## Preparation layers

| Layer | Required inputs and boundary |
| --- | --- |
| Static extraction | Validated local resources and module data; archive bounds and decompression limits apply |
| Program translation | Complete program/control data, constants, stage, thread group and translation options |
| Interface variants | Actual attribute/output types and paired stages; do not guess arbitrary type combinations |
| Metal IR | Verified source, options and library identity |
| Compute pipeline binary | Verified IR, entry function, reflection and device-specific configuration |
| Render pipeline binary | Complete captured descriptor, matching function/library identity and device configuration |

Static program availability does not prove that all programs are translated,
reachable or covered at runtime. A resource match is not a complete generated-C
call graph. Fixed host kernels can be prepared independently, while format,
attachment and sample-dependent pipeline states still require valid descriptors.

## Identity, loading and fallback

Caches bind source transformations, entry points, translation-chain and runtime
contracts, device/architecture, OS build and binary hashes. Stable keys select
candidates; Metal's `FailOnBinaryArchiveMiss` verifies the complete descriptor
and function binaries. Missing, damaged or rejected caches use the existing
compilation path.

Normal dispatch, prediction and warmup should share descriptor construction and
loading logic. Do not create a second descriptor builder that can drift from actual
drawing behavior. Do not modify a rejected descriptor merely to make offline
compilation succeed.

Compute/render preparation is bounded to small batches and archive sizes. Captured
render input is opt-in through `TOTK_RENDER_CAPTURE`, normally disabled. Derived
functions without verifiable IR and unsupported layouts retain the existing path.
Do not enumerate arbitrary attachment combinations or perform unbounded binary
archive capture.

See Apple's [device pipeline capture](https://developer.apple.com/documentation/metal/creating-binary-archives-from-device-built-pipeline-state-objects)
and [offline configuration compilation](https://developer.apple.com/documentation/metal/compiling-binary-archives-from-a-custom-configuration-script)
documentation for the underlying Metal workflow.

## Local workflow

Use fresh output directories; extraction and batch preparation reject replacement
of existing output. Select keys explicitly from the resulting catalog before
running a batch. The keys in this workflow identify shaders, not encryption keys.

```sh
work/venv/bin/python tools/bounded.py --log work/logs/shader-extraction.log -- \
  work/venv/bin/python tools/extract_shaders.py --romfs local/game/romfs.bin \
  --exefs local/game/exefs --output work/shader-preparation/extracted

# Create keys.json from the catalog; at most 256 selected shader keys per batch.
work/venv/bin/python tools/bounded.py --log work/logs/shader-batch.log -- \
  work/venv/bin/python tools/prepare_shader_batch.py \
  --catalog work/shader-preparation/extracted --keys work/shader-preparation/keys.json \
  --output work/shader-preparation/prepared
```

The following tools operate on the verified translation-chain directory selected
by preparation output:

| Tool | Inputs |
| --- | --- |
| `tools/prepare_metal_ir.py` | `--directory`, selected library-name `--keys`, bounded `--jobs`, fresh `--report` |
| `tools/prepare_compute_pipelines.py` | `--directory` with valid IR, compute-library `--keys`, fresh `--report` |
| `tools/prepare_render_pipelines.py` | Bounded `--capture`, matching `--directory`, fresh `--output` |

Use library names without extensions for Metal preparation. IR batches permit
at most 256 selected libraries; compute batches permit at most 32. Metal tools
reject existing reports and reuse/update IR only after identity checks.
`extraction.json` and `preparation.json` record their own stages; fields that need
Metal or GPU proof must remain false until that stage actually runs.

## Validation requirements

Compare synthetic output against an independent CPU reference and the existing
compilation path. Check exact descriptors, reflection, resource layouts and device
limits. Record translation, library compilation, pipeline creation and execution
separately under explicit cache conditions.

A warm-cache sample, hit count or successful extraction does not measure cold
compilation speed or whole-title coverage. Expand batches only after explaining
failures in the current set. Keep source/option/device identity, explicit rejection
and runtime fallback throughout the process.
