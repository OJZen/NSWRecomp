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
pre-publication branches or reconnect their private ancestors. Follow the
[contribution requirements](../CONTRIBUTING.md) for commit identity, complete-history
checks and publication scope.

Preserve independent experiments, reference checkouts, active caches and user
saves. Keep local evidence tied to the source and binary it actually tested.
Publishing a revised document does not remove older versions from Git history.
