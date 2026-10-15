# Maintain knowledge with a change

<!-- docs-scope: current-guide -->

Use this procedure for any studio feature, C++ capability, bug fix, policy change, or documentation update. Complete every change with one reviewable commit that explains the resulting behavior and practical limits.

---

## 7-Step Workflow

1. **Impact Analysis**: Run `python3 scripts/docs-kb.py impact` against the working tree (or add `--base <commit>` for branch comparison). Inspect affected domains and unmapped files. Add a domain mapping in [registry.json](../knowledge/registry.json) if a new C++ or Python boundary is introduced.
2. **Review Authority & Precedence**: Read the owning guide, related Architectural Decision Records ([ADRs](../adr/README.md)), and dated evidence. If the change alters product access, capability signatures, persistence, hardware backends, or security boundaries, write or amend an ADR.
3. **Reconcile Documents in Full**: Update affected capability schemas, status, instructions, examples, error guidance, and rollback procedures together. Keep one canonical procedure and link to it from discovery pages.
4. **Link Roadmap & Follow-up Actions**: If this change advances a roadmap milestone or closes an open item, update [register.json](../roadmap/register.json) and run `python3 scripts/roadmap-sync.py sync`.
5. **Verify Against Fixtures**: Run tests appropriate to the changed boundary (e.g. `python3 tests/studio_process_test.py` or `ctest`). Record command, result, revision, environment, and limitations.
6. **Record Explicit Review Attestation**: After reviewing the source and canonical guides, record a maintainer review attestation:
   ```sh
   python3 scripts/docs-kb.py review --domain <domain-id> --reviewer "your-role" --note "Explanation of review"
   ```
7. **Run Validation Guards**: Run `python3 scripts/docs-kb.py check`, `python3 scripts/verify-docs.py`, and `python3 scripts/roadmap-sync.py check`. Ensure all link, scope, and hash checks pass cleanly before committing.
