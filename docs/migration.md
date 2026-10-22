# Migration and history boundaries

Development moved from `totk-recomp` into NSWRecomp on 2026-10-22. This repository
has its own Git object database and build inputs; neighboring historical
checkouts are read-only references, not build dependencies.

The migration retained development work, experimental branches, source
attribution and licenses. Independent checks covered synthetic suites, native
relinking, load-only behavior and local data integrity. They did not establish
new gameplay acceptance in the relocated checkout.

## Public and local history

Initial publication retained this project's development sequence while replacing
private author/committer identities with a GitHub handle and noreply email.
A documentation path containing a save-source name was generalized. Commit SHAs
changed; upstream source revisions, licenses and source manifests did not.

Original main is preserved locally at
`refs/private/pre-publication/main-20261022`. Migration tags, experimental
branches, commit mappings and detailed migration evidence remain local and are
not published with main.

The LASRecomp history stored in the ignored local reference repository is
separate from NSWRecomp's commit ancestry. Public provenance is recorded in
`third_party/lasrecomp/SOURCE.json` and [third-party notices](../THIRD_PARTY_NOTICES.md).
Generic fixes are maintained here; upstream contributions are optional separate work.

## Continuing older work

Start from public main and transfer only reviewed changes. Do not directly merge
branches from before a publication cleanup or reconnect their private ancestors. Follow the
[contribution requirements](../CONTRIBUTING.md) for commit identity, complete-history
checks and publication scope.

Preserve independent experiments, reference checkouts, active caches and user
saves. Keep local evidence tied to the source and binary it actually tested.
Publishing a revised document does not remove older versions from Git history.

## Historical disclosure cleanup — 2026-10-22

The owner authorized a one-time rewrite of main to remove detailed historical
game reports. Retired report pages are excluded from every published snapshot;
older narrative documents use brief archival notices, and title-specific commit
descriptions are generalized. Current English technical documentation is retained.
The source tree at each original development step and third-party provenance are
preserved; historical notices do not assert new runtime validation.

The previous published main is retained locally at
`refs/private/pre-disclosure/main-20261022`, with a verified bundle and commit map
under ignored `work/history-rewrites/20261022-disclosure/`. Original migration
branches and tags remain local. Continue from the rewritten main and transfer
only reviewed changes; do not merge a saved original history back into it.

This cleanup changes main's reachable history. Existing clones, forks and GitHub
cached commit views are outside the guarantee of a force push; see
[GitHub's history-removal guidance](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository).
