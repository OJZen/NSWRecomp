# Contribution, merge and publication requirements

These rules apply to people and agents. Read [AGENTS.md](AGENTS.md),
[current status](docs/status.md) and the relevant technical notes first.
Authorization to develop, commit, merge and publish is scoped to the user's task;
finishing an implementation does not independently authorize publication.

## Documentation and disclosure

- Use English as the primary language for public documentation, headings,
  tables, examples, contribution guidance and PR descriptions.
- Document architecture, interfaces, reproducible synthetic checks and essential
  adapter/input requirements. Keep the current status page short and link to
  the page responsible for each technical contract.
- Keep detailed gameplay scenes, story descriptions, save origins or contents,
  playthrough logs, screenshots and per-run game benchmarks in ignored local
  records. Publish detailed title-specific reports only with explicit owner approval.
- Preserve source attribution, licenses and known limitations. Reducing public
  detail must not turn an unverified result into a compatibility or performance claim.

## Branches and commits

- Start ordinary work on `codex/<topic>` from current `main`, using a separate
  worktree when appropriate. Direct work on `main` requires task-specific authorization.
- Record the branch, HEAD, `git status --short` and existing changes before work;
  check them again before finishing. Preserve other agents' and users' changes.
- Keep each commit focused on one problem, including affected callers, necessary
  regression checks and documentation. Stage explicit paths or reviewed hunks.
  Do not use `git add .`, `git add -A` or `git commit -a`.
- Review `git diff --cached --stat`, `git diff --cached` and
  `git diff --cached --check`. Describe the concrete behavior, reason, actual
  validation and remaining limits; do not paste conversations or raw logs.
- Use your GitHub handle and that account's noreply email for both author and
  committer. Exclude personal email, local account/host names, user/volume paths,
  private network addresses, credentials and authenticated URLs. Use relative
  paths or `/path/to/...` in examples.

## Files and validation evidence

- Keep packages, keys, extracted resources, generated C, captured shaders,
  caches, screenshots, saves, binaries and raw logs under ignored `local/` or
  `work/`. Ignore rules do not protect tracked files or remove previous commits.
- Synthetic tests must run without game data or keys. Add a focused regression
  for nontrivial logic. Check documentation links and source accuracy for prose
  changes. Run affected CPU/runtime/graphics checks, and `make test` when needed;
  do not rerun a game solely to validate documentation.
- Distinguish source analysis, C generation, compilation/linking, isolated
  execution, observed rendering and gameplay. Record the version, command,
  result, resource limits and unresolved failures. Evidence for an older binary
  is not evidence for a new commit.
- Preserve licenses and upstream origin revisions when modifying derived code.
  Update the corresponding local-change record and file hashes in `SOURCE.json`.
  Do not modify reference checkouts, overwrite saves or remove active outputs.

## Merge requirements

1. Confirm the target is current `main`. Review every incoming commit and the
   final tree difference, not only the last commit.
2. Check scope, validation against the actual candidate and the evidence claimed
   by its documentation. Recheck affected behavior after resolving conflicts.
3. Review experiments separately. A directory move, cleanup or passing test does
   not by itself justify merging an experiment.
4. **Local branches and tags from before a publication cleanup retain
   private history and must not be merged directly into public `main`.** Start
   from public `main` and transfer only reviewed changes, for example with
   `git cherry-pick --no-commit <reviewed-commit>`, then review and commit with
   the public identity. Do not reconnect old ancestors with
   `--allow-unrelated-histories`.
5. Run `python3 tools/check_publication.py --ref HEAD` on the final candidate.
   It inspects the complete reachable history, rejects shallow clones and ignores
   replace refs. Deleting sensitive text in a new commit does not remove it from
   history. Stop publication and revoke/rotate any real exposed credential before
   repairing its history.

## Local hooks and publication

Install hooks from the repository root in every new clone. An absolute hooks path
also applies these rules to this repository's older linked worktrees:

```sh
git config --local core.hooksPath "$PWD/.githooks"
git config --local user.name YOUR_GITHUB_HANDLE
git config --local user.email YOUR_GITHUB_NOREPLY_EMAIL
git config --local push.default nothing
git config --local push.followTags false
git config --local remote.origin.push refs/heads/main:refs/heads/main
```

`pre-commit` checks the full index, commit identities and whitespace. `pre-push`
allows only `main` to `origin` and scans the actual outgoing commit and its full
ancestry. Findings identify locations and categories without printing matched
values. The lightweight rules cover common privacy and credential patterns and
excluded artifact types; they do not replace manual review.

Retain public attribution and source hashes in third-party notices. Investigate
false positives narrowly rather than removing attribution or bypassing checks.
After explicit publication authorization and successful review, use:

```sh
python3 tools/check_publication.py --ref main
git -c push.followTags=false push origin refs/heads/main:refs/heads/main
git ls-remote --heads --tags origin
```

Do not use `--all`, `--mirror`, `--tags`, `--no-verify` or disabled
hooks to bypass these rules. Fetch and inspect a divergent remote history instead
of overwriting it. Verify the remote main SHA and the absence of unintended refs
following publication.

History rewrites require explicit owner authorization for the specific operation.
Keep the original main in a private local ref and a verified bundle, review the
complete replacement history, and use `--force-with-lease` bound to the exact
previous remote main SHA. Stop if the lease no longer matches. Publish only main;
never include backup refs, other branches or tags. A prior rewrite authorization
does not authorize a later force push.

These hooks are local safeguards, not GitHub branch protection. Server-enforced
review rules require separate configuration. A documentation edit also does not
remove material already available in published Git history.
