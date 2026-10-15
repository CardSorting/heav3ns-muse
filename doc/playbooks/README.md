# Operational Playbooks

<!-- docs-scope: current-guide -->

Catalog of standard operating procedures, provisioning guides, and developer ergonomics doctrine for HEAV3NS-MUSE.

---

## 1. Playbook Directory

| Playbook | Purpose | Scope |
| :--- | :--- | :--- |
| [01: Provisioning & Preflight](01-provisioning-and-preflight.md) | Setting up development environments across macOS, Linux, and containerized CI | `current-guide` |
| [02: Zero-Hesitation Developer Ergonomics](02-zero-hesitation-developer-ergonomics.md) | API consistency, standard request shapes, low cognitive load, and actionable error UX | `current-guide` |
| [03: Incident Response & Audio Triage](03-incident-response-and-audio-triage.md) | Immediate containment of DSP crashes, audio feedback, hung sockets, or project corruption | `current-guide` |

---

## 2. Playbook Standards

All playbooks adhere to the following principles:
- **Reproducible Commands**: Every instruction must be runnable in a standard zsh/bash shell without hidden interactive prompts.
- **Explicit Stop Conditions**: Clearly state when an operator or autonomous agent must halt execution rather than continuing blindly.
- **Recovery & Verification**: Every operational action must include verification commands confirming intended system state.
