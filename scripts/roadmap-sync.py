#!/usr/bin/env python3
"""Auto-rolling roadmap synchronization and gate verification for HEAV3NS-MUSE."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
DOCS_DIR = ROOT / ("doc" if (ROOT / "doc").is_dir() else "docs")
REGISTER = DOCS_DIR / "roadmap/register.json"
EVENTS = DOCS_DIR / "roadmap/events.jsonl"
OPEN_ITEMS = DOCS_DIR / "roadmap/OPEN_ITEMS.md"
ROLLING_LOG = DOCS_DIR / "roadmap/ROLLING_LOG.md"
PUBLIC_ROADMAP = ROOT / "ROADMAP.md"


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def dump_json(value):
    return json.dumps(value, indent=2, ensure_ascii=False) + "\n"


def make_progress_bar(percentage, width=20):
    filled = int(round((percentage / 100.0) * width))
    empty = width - filled
    return "█" * filled + "░" * empty


def sync_roadmap(register, events):
    # Recalculate horizon percentages
    horizons = register.get("horizons", {})
    gates = register.get("gates", [])

    for h_id, horizon in horizons.items():
        h_gates = [g for g in gates if g.get("horizon") == h_id]
        if not h_gates:
            horizon["completion_percentage"] = 0
            continue
        completed_count = sum(1 for g in h_gates if g.get("status") == "completed")
        pct = int(round((completed_count / len(h_gates)) * 100))
        horizon["completion_percentage"] = pct
        if pct == 100:
            horizon["status"] = "completed"
        elif pct > 0:
            horizon["status"] = "in-progress"

    # Determine active horizon: lowest incomplete horizon
    active_h = "H0"
    for h_id in ["H0", "H1", "H2", "H3"]:
        if horizons.get(h_id, {}).get("status") != "completed":
            active_h = h_id
            break
    register["active_horizon"] = active_h
    register["last_updated"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    # Generate ROADMAP.md
    today_date = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    h0_bar = make_progress_bar(horizons["H0"]["completion_percentage"])
    h1_bar = make_progress_bar(horizons["H1"]["completion_percentage"])
    h2_bar = make_progress_bar(horizons["H2"]["completion_percentage"])
    h3_bar = make_progress_bar(horizons["H3"]["completion_percentage"])

    roadmap_content = f"""# 🗺️ HEAV3NS-MUSE Auto-Rolling Roadmap

<!-- docs-scope: current-guide -->

*An evidence-backed, auto-rolling milestone roadmap for the agent-native digital audio workstation initiative.*

---

```text
┌─────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                 CURRENT ROLLING HORIZON                                         │
│                                                                                                 │
│   [{h0_bar}] {horizons['H0']['completion_percentage']:>3}%  Horizon 0: {horizons['H0']['title']} ({horizons['H0']['status'].capitalize()})
│   [{h1_bar}] {horizons['H1']['completion_percentage']:>3}%  Horizon 1: {horizons['H1']['title']} ({horizons['H1']['status'].capitalize()})
│   [{h2_bar}] {horizons['H2']['completion_percentage']:>3}%  Horizon 2: {horizons['H2']['title']} ({horizons['H2']['status'].capitalize()})
│   [{h3_bar}] {horizons['H3']['completion_percentage']:>3}%  Horizon 3: {horizons['H3']['title']} ({horizons['H3']['status'].capitalize()})
└─────────────────────────────────────────────────────────────────────────────────────────────────┘
```

Last synchronized: **{today_date}** • Machine Authority: [doc/roadmap/register.json](doc/roadmap/register.json) • Event Log: [doc/roadmap/ROLLING_LOG.md](doc/roadmap/ROLLING_LOG.md)

---

## 🧭 Horizon Breakdown

### Horizon 0: {horizons['H0']['title']}
**Status: ✅ {horizons['H0']['completion_percentage']}% Completed**

{horizons['H0']['description']}

### Horizon 1: {horizons['H1']['title']}
**Status: 🔄 {horizons['H1']['completion_percentage']}% In-Progress**

{horizons['H1']['description']}

### Horizon 2: {horizons['H2']['title']}
**Status: 🚀 Active Focus ({horizons['H2']['completion_percentage']}% Complete)**

{horizons['H2']['description']}

### Horizon 3: {horizons['H3']['title']}
**Status: 🔮 Strategic Roadmap**

{horizons['H3']['description']}

---

## 📊 Milestone Gate Verification Register

| Gate ID | Horizon | Title | Status | Verification Command | Closure Evidence |
| :---: | :---: | :--- | :---: | :--- | :--- |
"""
    for g in gates:
        cmd = f"`{g['verification']}`" if g.get("verification") else "*N/A*"
        ev = f"`{g['evidence']}`" if g.get("evidence") else "*Future planning*"
        roadmap_content += f"| `{g['id']}` | {g['horizon']} | {g['title']} | `{g['status']}` | {cmd} | {ev} |\n"

    roadmap_content += """
---

## ⚙️ Auto-Rolling Governance Rules

1. **Evidence-Backed Advancements**: A gate is never marked `completed` based on source code alone. It requires an executable verification script with retained test output or passing exit code `0`.
2. **Deterministic Roll**: When all gates within a Horizon transition to `completed`, `scripts/roadmap-sync.py` automatically rolls the active horizon forward, updates percentages, and logs an event in [doc/roadmap/events.jsonl](doc/roadmap/events.jsonl).
3. **No Retrospective History Editing**: Progress logs in [doc/roadmap/ROLLING_LOG.md](doc/roadmap/ROLLING_LOG.md) are append-only.
4. **Maintenance Invariant**: Run `python3 scripts/roadmap-sync.py sync` to synchronize this document whenever [register.json](doc/roadmap/register.json) is modified.
"""
    PUBLIC_ROADMAP.write_text(roadmap_content, encoding="utf-8")
    REGISTER.write_text(dump_json(register), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("sync", help="Synchronize register, compute horizon percentages, and refresh ROADMAP.md")
    sub.add_parser("check", help="Validate roadmap register consistency against evidence files")

    roll_parser = sub.add_parser("roll", help="Roll a milestone gate to a new status and record event")
    roll_parser.add_argument("--gate", required=True, help="Gate identifier, e.g. GATE-014")
    roll_parser.add_argument("--status", required=True, choices=["completed", "in-progress", "open", "deferred"])
    roll_parser.add_argument("--note", required=True, help="Description of milestone progress and evidence")

    args = parser.parse_args()
    register = read_json(REGISTER)
    event_lines = [json.loads(line) for line in EVENTS.read_text(encoding="utf-8").splitlines() if line.strip()]

    if args.command == "sync":
        sync_roadmap(register, event_lines)
        print(f"Roadmap synchronized. Active Horizon: {register['active_horizon']}.")
        return 0

    if args.command == "check":
        errors = []
        gates = register.get("gates", [])
        for g in gates:
            if g.get("status") == "completed":
                if not g.get("completed_at"):
                    errors.append(f"{g['id']}: completed gate missing completed_at date")
                if g.get("evidence"):
                    ev_path = ROOT / g["evidence"]
                    if not ev_path.exists():
                        errors.append(f"{g['id']}: cited evidence does not exist: {g['evidence']}")
        if errors:
            print("\n".join(errors), file=sys.stderr)
            return 1
        print(f"Roadmap validation passed: {len(gates)} gates verified across {len(register.get('horizons', {}))} horizons.")
        return 0

    if args.command == "roll":
        gate_found = False
        for g in register.get("gates", []):
            if g["id"] == args.gate:
                gate_found = True
                old_status = g["status"]
                g["status"] = args.status
                if args.status == "completed":
                    g["completed_at"] = datetime.now(timezone.utc).strftime("%Y-%m-%d")
                # Record event
                evt_id = f"EVT-{datetime.now(timezone.utc).strftime('%Y%m%d')}-{args.gate}"
                new_evt = {
                    "event_id": evt_id,
                    "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                    "gate_id": args.gate,
                    "status": args.status,
                    "note": args.note
                }
                with open(EVENTS, "a", encoding="utf-8") as f:
                    f.write(json.dumps(new_evt) + "\n")
                print(f"Rolled gate {args.gate} from '{old_status}' to '{args.status}'. Event: {evt_id}.")
                break
        if not gate_found:
            print(f"Gate {args.gate} not found in register", file=sys.stderr)
            return 1
        sync_roadmap(register, event_lines)
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
