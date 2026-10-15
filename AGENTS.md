# Public information boundary and documentation authority

<!-- docs-scope: current-guide -->

Public content explains what HEAV3NS-MUSE does, how to drive it via the studio protocol or Python SDK, and what music producers and developers can expect. Keep operational topology, private local credentials, proprietary reasoning endpoints, and internal diagnostic paths private. Do not publish unverified model claims, staging tokens, or host-specific paths in public documentation.

Public documentation describes capability, contracts, and observable audio behavior, not transient internal state.

---

## One source for each kind of truth

| Question | Authority |
| :--- | :--- |
| **What is the project scope & product vision?** | [README.md](README.md), [ROADMAP.md](ROADMAP.md), [doc/agent-native-architecture.md](doc/agent-native-architecture.md) |
| **What does code do & how does it execute?** | Owning source under `src/core/` and tests under `tests/`, with ADR precedence under [doc/adr/](doc/adr/README.md) |
| **What was implemented and actually verified?** | [Engineering Record](doc/agent-native-architecture.md) and dated evidence under [doc/roadmap/ROLLING_LOG.md](doc/roadmap/ROLLING_LOG.md) |
| **How do we drive the studio protocol?** | [Native Studio Protocol](doc/native-studio-protocol.md) and [Capabilities Reference](README.md#-studio-capabilities-reference) |
| **How do we make changes with synchronized docs?** | [How-to: Change with documentation](doc/how-to/change-with-documentation.md) and [Documentation Policy](doc/DOCUMENTATION_POLICY.md) |
| **What is the current milestone and what remains open?** | [Auto-Rolling Roadmap](ROADMAP.md), [doc/roadmap/OPEN_ITEMS.md](doc/roadmap/OPEN_ITEMS.md), and [doc/roadmap/register.json](doc/roadmap/register.json) |
| **Where is every guide & document?** | [Complete docs catalog](doc/CATALOG.md) and [knowledge registry](doc/knowledge/registry.json) |

Source presence is not completion. A test file is not continuous runtime verification. Current state includes the observation date, target platform, and limitations; root documentation links to authoritative records rather than maintaining competing summaries.

---

## Scope categories

Every Markdown file under `doc/` must contain exactly one scope marker:

- `<!-- docs-scope: current-guide -->`: Active, authoritative instructions within its stated component.
- `<!-- docs-scope: domain-reference -->`: Technical reference describing a subsystem; requires source review before reuse outside that domain.
- `<!-- docs-scope: decision-record -->`: Architectural decision retaining rationale, context, and topic-specific supersession.
- `<!-- docs-scope: operational-record -->`: Dated empirical evidence, test results, benchmarks, and historical engineering logs.
- `<!-- docs-scope: research-reference -->`: Exploratory designs, experiments, and literature notes; does not establish runtime guarantees.

---

## Reconcile whole documents

When DAW operations, C++ models, or protocol behaviors change:

1. Update affected capability schemas, status, body instructions, examples, error/recovery guidance, and navigation together.
2. Remove obsolete actionable recipes. Adding a successor banner above contradictory commands is prohibited.
3. Preserve original measurements, test runs, and completion evidence as dated records. Do not rename historical observations as current or erase failed tests.
4. ADR updates must explain which previous decision they supersede (`supersedes` / `superseded_by`).

---

## Enforcement and review

- `python3 scripts/docs-kb.py sync` (or `npm run docs:sync`) regenerates catalogs, domain inventories, and decision tables without certifying a review.
- `python3 scripts/docs-kb.py check` and `python3 scripts/verify-docs.py` (or `npm run docs:check`) validate complete docs coverage, scopes, local links, ADR numbering, and source review freshness.
- `python3 scripts/roadmap-sync.py sync` (or `npm run roadmap:sync`) auto-rolls roadmap milestones, evaluates completion gates against physical evidence, and updates the public roadmap.
- After a semantic review of a monitored change, record an explicit dated attestation with:
  ```sh
  python3 scripts/docs-kb.py review --domain <domain-id> --reviewer "name-or-role" --note "What was reviewed"
  ```
  This refreshes cryptographic SHA-256 source fingerprints. The guard detects modified source files and additions matching monitored patterns; it prevents documentation drift across releases.
