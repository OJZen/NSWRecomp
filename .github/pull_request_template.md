Describe the concrete problem, resulting behavior and affected scope.

- Validation: commands, actual results and the candidate version tested.
- Unverified behavior or known limits: distinguish analysis, generation,
  compilation, isolated execution and gameplay.
- Source changes: explain license, origin revision and `SOURCE.json` updates
  when modifying derived code.

Review [CONTRIBUTING.md](https://github.com/OJZen/NSWRecomp/blob/main/CONTRIBUTING.md):

- [ ] Only authorized changes are included; all files and incoming commits were reviewed.
- [ ] Relevant regressions and `git diff --check` passed.
- [ ] `python3 tools/check_publication.py --ref HEAD` passed; no private inputs or derived artifacts are included.
- [ ] No private pre-publication history, unaccepted experiments or unrelated work was merged.
- [ ] Documentation is English-first, limits title-specific disclosure and matches the evidence.
- [ ] Merge and publication are authorized for this task.
