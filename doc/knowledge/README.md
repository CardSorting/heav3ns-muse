# Knowledgebase Governance & Operations

<!-- docs-scope: current-guide -->

This directory contains the machine-readable registry and human-readable references for the HEAV3NS-MUSE knowledgebase system.

---

## Structure

- [registry.json](registry.json): Maps domain identifiers to source file globs, authoritative guides, and verification test commands.
- [reviews.json](reviews.json): Contains cryptographic SHA-256 source file fingerprints, reviewer attestations, and review deadlines.
- [DOMAINS.md](DOMAINS.md): Human-readable directory of domains, owners, and relevant test scripts.
- [DECISIONS.md](DECISIONS.md): Architectural Decision Record lifecycle and evidence status.
- [catalog.json](catalog.json): Generated document inventory.

---

## Operating Commands

```sh
# Synchronize generated indexes and catalog
python3 scripts/docs-kb.py sync

# Validate scopes, links, and review freshness
python3 scripts/docs-kb.py check

# Display domain review freshness status
python3 scripts/docs-kb.py status

# Inspect impacted domains against git diff
python3 scripts/docs-kb.py impact

# Record an explicit maintainer review
python3 scripts/docs-kb.py review --domain <domain-id> --reviewer "name-or-role" --note "Review description"
```
