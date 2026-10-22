# Project rules

- Read README.md and docs/status.md before changing the pipeline. Keep proof of
  analysis, C generation, compilation, isolated execution and game play distinct.
- Keep user packages, keys, extracted files, generated C, shaders, saves and
  binaries under ignored local/ or work/. Never commit game-derived fixtures.
  Synthetic instruction fixtures are allowed. Never print key contents.
- Put temporary files and caches on the external volume under work/tmp/. Bound
  long jobs; default to at most eight compiler processes with a 4 GiB aggregate
  RSS guard (user authorized multicore builds on 2026-10-21). Remove obsolete task-owned output
  after checking it is not in use. Never clean the reference projects or saves.
- LASRecomp is the source of the recompiler subset in third_party/lasrecomp.
  Fix generic defects upstream on its dedicated branch, test with synthetic
  inputs, then refresh the documented snapshot. Preserve license notices.
- Zelda-Mobile is a reference and optional read-only input source, not a build
  dependency. Preserve its worktree and user progress.
- Stage an explicit file list. Do not commit or publish unrelated work. Public
  source must build its synthetic checks without game data or keys.
