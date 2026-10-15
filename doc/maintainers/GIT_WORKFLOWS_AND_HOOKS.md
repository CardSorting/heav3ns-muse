# Git Workflows, Hooks & PR Attestation

<!-- docs-scope: current-guide -->

Standards for version control, commit conventions, automated pre-commit hooks, and maintainer pull request attestations in HEAV3NS-MUSE.

---

## 1. Commit Message Conventions

We enforce [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <short description>

[optional body explaining rationale and architectural context]

[optional footer referencing issue or gate ID]
```

### Approved Types
- **`feat`**: A new studio protocol capability, synth feature, or SDK method.
- **`fix`**: A bug fix in audio DSP, socket communication, or state management.
- **`docs`**: Documentation updates, ADR authoring, or knowledgebase synchronization.
- **`perf`**: A code change improving audio latency or rendering throughput.
- **`test`**: Adding missing tests or refactoring test harnesses.
- **`refactor`**: Code changes neither fixing a bug nor adding a feature.

### Example
```
feat(studio): add atomic multi-track clip insertion

Enable agents to insert coordinated multi-stem clips across
drums, bass, and lead tracks within a single synchronized cycle.

Closes GATE-015
```

---

## 2. Pre-Commit Hooks

Maintainers can configure local git hooks to run automated verification prior to commit:

Create `.git/hooks/pre-commit`:
```sh
#!/bin/sh
set -e

echo "Running pre-commit documentation and code validation..."
python3 scripts/verify-docs.py
python3 scripts/docs-kb.py check
python3 scripts/roadmap-sync.py check

echo "Pre-commit checks passed."
```

Make executable:
```sh
chmod +x .git/hooks/pre-commit
```

---

## 3. Pull Request Attestation Checklist

Before a maintainer merges any pull request into `main`, verify:

1. [ ] **Physical Test Evidence**: Tests have been executed and command outputs are recorded in the PR description.
2. [ ] **Documentation Scope**: All new/modified Markdown files under `doc/` contain exactly one valid `<!-- docs-scope: ... -->` marker.
3. [ ] **Catalog Indexing**: All documentation files are linked in `doc/CATALOG.md`.
4. [ ] **Cryptographic Attestation**: Affected knowledgebase domains have been reviewed and updated via `npm run docs:review`.
5. [ ] **Roadmap Alignment**: If closing a roadmap gate, `npm run roadmap:roll` has updated `register.json`, `events.jsonl`, and `ROADMAP.md`.
