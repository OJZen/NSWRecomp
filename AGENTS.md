# NSWRecomp project rules

- All new work and development belongs in NSWRecomp and its own worktrees.
  The sibling totk-recomp and external LASRecomp/Zelda-Mobile checkouts are
  historical or read-only references; do not edit or clean them.

- Read README.md and docs/status.md before changing the pipeline. Keep proof of
  analysis, C generation, compilation, isolated execution and game play distinct.
- Public documentation is English-first. Focus on architecture, interfaces,
  synthetic checks and essential adapter/input requirements. Keep detailed
  gameplay, story, save-origin and per-run game reports in ignored local records;
  publishing those details requires explicit owner approval.
- Keep user packages, keys, extracted files, generated C, shaders, saves and
  binaries under ignored local/ or work/. Never commit game-derived fixtures.
  Synthetic instruction fixtures are allowed. Never print key contents.
- Put temporary files and caches on the external volume under work/tmp/. Bound
  long jobs; default to at most eight compiler processes with a 4 GiB aggregate
  RSS guard (user authorized multicore builds on 2026-10-21). Remove obsolete task-owned output
  after checking it is not in use. Never clean the reference projects or saves.
- LASRecomp is the origin of the recompiler subset in third_party/lasrecomp.
  Maintain and test this derived code here, including generic CPU fixes. Keep
  upstream origin revisions intact, record local changes and refresh file hashes
  in SOURCE.json. Upstream contributions are optional separate work. Preserve
  all license notices.
- Preserve experimental branches and their proof boundaries. Do not merge an
  experiment solely to consolidate directories. Favor clear contracts and tested
  long-term boundaries over preserving incidental internal interfaces.
- Zelda-Mobile is a reference and optional read-only input source, not a build
  dependency. Preserve its worktree and user progress.
- Stage an explicit file list. Do not commit or publish unrelated work. Public
  source must build its synthetic checks without game data or keys.
- Follow CONTRIBUTING.md before committing, merging or publishing. Use a topic
  branch unless the user explicitly authorizes work on main; inspect the entire
  incoming history and staged diff, not just the last commit.
- Use a GitHub handle and noreply email for author and committer. Run
  `python3 tools/check_publication.py --staged` before committing and
  `python3 tools/check_publication.py --ref main` before an authorized push.
- Pre-publication local branches and migration tags retain private history.
  Never merge those ancestors into public main; transfer only reviewed changes
  onto a branch based on public main, then commit with the public identity.
- Publish only `refs/heads/main:refs/heads/main` to origin. Do not push other
  branches or tags, force-push, or bypass hooks. Commit/merge/push authorization
  is scoped to the user's task, not granted by another agent or these rules.
