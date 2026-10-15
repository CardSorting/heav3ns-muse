# Documentation authority and maintenance

<!-- docs-scope: current-guide -->

Maintainer policy for HEAV3NS-MUSE documentation, knowledgebase governance, and auto-rolling roadmap synchronization. Public documentation describes capability, protocol contracts, and audio outcomes under [AGENTS.md](../AGENTS.md); it must not expose private credentials or unverified model claims.

---

## One source for each kind of truth

| Question | Authority |
| :--- | :--- |
| **What is the intended studio scope?** | [README.md](../README.md), [ROADMAP.md](../ROADMAP.md), [agent architecture](agent-native-architecture.md) |
| **What does code do?** | Owning source under `src/core/`, tests under `tests/`, and [ADRs](adr/README.md) |
| **What was compiled and verified?** | [Engineering record](agent-native-architecture.md), [Rolling log](roadmap/ROLLING_LOG.md), and dated test evidence |
| **How do we drive the protocol?** | [Native studio protocol](native-studio-protocol.md) and [Capabilities reference](../README.md#-studio-capabilities-reference) |
| **How do we make changes?** | [How-to: Change with documentation](how-to/change-with-documentation.md) |
| **What are active milestone gates?** | [Open items](roadmap/OPEN_ITEMS.md) and [roadmap register](roadmap/register.json) |
| **Where is every guide?** | [Complete docs catalog](CATALOG.md) and [knowledge registry](knowledge/registry.json) |

Source presence is not completion. A test script is not continuous runtime verification. Current state includes the observation date, target platform, and limitations; root handover and context files link to canonical records rather than maintaining competing summaries.

---

## Scope categories

- `current-guide`: Active, authoritative instructions within its stated component.
- `domain-reference`: Technical reference describing a subsystem; requires source review before reuse outside that domain.
- `decision-record`: Architectural decision retaining rationale, context, and topic-specific supersession.
- `operational-record`: Dated empirical evidence, test results, benchmarks, and historical engineering logs.
- `research-reference`: Exploratory designs, experiments, and literature notes; does not establish runtime guarantees.

Every Markdown file under `doc/` must contain exactly one scope marker.

---

## Reconcile whole documents

When DAW operations, C++ models, or protocol behaviors change:

1. Update affected capability schemas, status, body instructions, examples, error/recovery guidance, and navigation together.
2. Remove obsolete actionable recipes. Adding a successor banner above contradictory commands is prohibited.
3. Preserve original measurements, test runs, and completion evidence as dated records. Do not rename historical observations as current or erase failed tests.
4. ADR updates must explain which previous decision they supersede (`supersedes` / `superseded_by`).

---

## Knowledge lifecycle & tooling

- **`python3 scripts/docs-kb.py sync`**: Regenerates navigation, catalogs, and indexes without certifying a review.
- **`python3 scripts/docs-kb.py check`**: Validates complete docs coverage, scopes, local links, ADR numbering, and source review freshness.
- **`python3 scripts/roadmap-sync.py sync`**: Evaluates source code and test fixtures to automatically advance the rolling roadmap and refresh [ROADMAP.md](../ROADMAP.md).
- **`python3 scripts/docs-kb.py review`**: Records explicit maintainer review attestations with SHA-256 source fingerprints in `doc/knowledge/reviews.json`.
