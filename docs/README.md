# Documentation

Start with [current status](status.md), then choose the relevant technical guide.
English is the primary language. Public pages describe implementation contracts,
validation limits and essential setup; detailed gameplay and local operational
records are kept outside this documentation tree.

| Task | Guide |
| --- | --- |
| Set up the current adapter | [Adapter setup](totk.md), [control panel and input](control-panel.md) |
| Build and validate the Windows core | [Windows x86-64 pipeline](windows.md) |
| Understand the implementation | [Architecture](architecture.md) |
| Plan shared compiler and host interfaces | [Architecture roadmap](architecture-roadmap.md) |
| Review CPU semantics and gaps | [CPU validation](cpu-validation.md) |
| Load modules and start guest services | [Loading](loading.md), [initialization and threads](startup.md) |
| Work on graphics and shader preparation | [Graphics bridge](graphics.md), [shader preparation](shader-preparation.md) |
| Transfer older work safely | [Migration notes](migration.md) |
| Review provenance and references | [Third-party notices](../THIRD_PARTY_NOTICES.md), [emulator reference assessment](emulator-references.md) |
| Commit, merge or publish | [Contribution requirements](../CONTRIBUTING.md), [agent rules](../AGENTS.md) |

Update the page that owns a fact instead of copying it into several summaries.
Keep validation tied to its source/binary version. A historical test or local
measurement does not automatically describe current `main` or another platform.
