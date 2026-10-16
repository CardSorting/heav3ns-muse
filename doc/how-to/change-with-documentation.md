# Maintain knowledge with a change

<!-- docs-scope: current-guide -->

Use this procedure for any studio feature, C++ capability, bug fix, policy change, or documentation update. Complete every change with one reviewable commit that explains the resulting behavior and practical limits.

---

## 7-Step Workflow

1. **Impact Analysis**: Run `python3 scripts/docs-kb.py impact` against the working tree (or add `--base <commit>` for branch comparison). Inspect affected domains and unmapped files. Add a domain mapping in [registry.json](../knowledge/registry.json) if a new C++ or Python boundary is introduced.
2. **Review Authority & Precedence**: Read the owning guide, related Architectural Decision Records ([ADRs](../adr/README.md)), and dated evidence. If the change alters product access, capability signatures, persistence, hardware backends, or security boundaries, write or amend an ADR.
3. **Reconcile Documents in Full**: Update affected capability schemas, status, instructions, examples, error guidance, and rollback procedures together. Keep one canonical procedure and link to it from discovery pages.
4. **Link Roadmap & Follow-up Actions**: Update planned actions in [register.json](../roadmap/register.json) and run `python3 scripts/roadmap-sync.py sync`. For status transitions, use `roll`; completions require the [retained-evidence procedure](#close-a-roadmap-gate) below.
5. **Verify Against Fixtures**: Run tests appropriate to the changed boundary (e.g. `python3 tests/studio_process_test.py` or `ctest`). Record command, result, revision, environment, and limitations.
6. **Record Explicit Review Attestation**: After reviewing the source and canonical guides, record a maintainer review attestation:
   ```sh
   python3 scripts/docs-kb.py review --domain <domain-id> --reviewer "your-role" --note "Explanation of review"
   ```
7. **Run Validation Guards**: Run `python3 scripts/docs-kb.py check`, `python3 scripts/verify-docs.py`, and `python3 scripts/roadmap-sync.py check`. Ensure all link, scope, and hash checks pass cleanly before committing.

## Close a roadmap gate

Run the gate's verification command and retain its output before requesting a
completion transition. Review the full criterion against the observed platform,
revision, scope and limitations. A passing deterministic fixture does not prove
real-provider musical acceptance or physical hardware coverage.

Create a JSON execution receipt containing `gate`, `command`, `criterion` (the
exact register values), integer `exit_code: 0`, timezone-qualified `executed_at`,
and nonempty `platform`, `revision`, `scope`, `limitations`, and
`verified_invariants` (a list of observed assertions). Its `output` object must
contain a repository-relative `path` to separate, nonempty retained execution
output and that file's `sha256`. Retained artifacts must respect the public
information boundary; review output for credentials and host-specific details
before adding it. A receipt records observed execution; writing JSON does not
perform verification.

Register the receipt's hash with `docs-kb.py record-evidence`, then use the
following command with the actual receipt path and review note:

```sh
python3 scripts/roadmap-sync.py roll --gate GATE-014 --status completed \
  --receipt doc/evidence/test-runs/YYYYMMDD-gate014.json \
  --note "Describe how the retained observations cover the complete criterion"
```

The transition refuses absent, mismatched, failed or modified receipts and
output. It appends a gate event and, if necessary, a horizon transition event,
then regenerates the public roadmap and open actions. Reopening uses the same
`roll` command with `--status open` and a review note, without `--receipt`;
current completion fields are cleared while the original event is retained.
Synchronization alone never closes a gate or executes its verification command.
