#!/usr/bin/env python3
"""Synchronize the roadmap and validate retained gate evidence without running tests."""

import argparse
import copy
from datetime import date, datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
STATUSES = {"completed", "in-progress", "open", "deferred"}
# Preserve pre-policy closure claims as history, not freshly verified outcomes.
RECEIPT_POLICY_DATE = date(2026, 10, 16)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def dump_json(value):
    return json.dumps(value, indent=2, ensure_ascii=False) + "\n"


def safe_file(root, name):
    if not isinstance(name, str) or not name or Path(name).is_absolute():
        raise ValueError(f"Expected repository-relative file: {name!r}")
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError(f"Missing or out-of-repository file: {name}")
    return path


def timestamp():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_timestamp(value):
    if not isinstance(value, str):
        raise ValueError("Expected an ISO execution timestamp")
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise ValueError("Execution timestamps must include a timezone")
    return parsed


def normalize(register):
    result = copy.deepcopy(register)
    for h_id, horizon in result["horizons"].items():
        gates = [g for g in result["gates"] if g["horizon"] == h_id]
        completed = sum(g["status"] == "completed" for g in gates)
        horizon["completion_percentage"] = round(100 * completed / len(gates)) if gates else 0
        if gates and completed == len(gates):
            horizon["status"] = "completed"
        elif any(g["status"] in {"completed", "in-progress"} for g in gates):
            horizon["status"] = "in-progress"
        elif gates and all(g["status"] == "deferred" for g in gates):
            horizon["status"] = "deferred"
        else:
            horizon["status"] = "open"
    ordered = sorted(result["horizons"], key=lambda h: int(h[1:]))
    result["active_horizon"] = next(
        (h for h in ordered if result["horizons"][h]["status"] != "completed"), None)
    return result


def legacy_closure(gate, events):
    """Only an unchanged, dated, pre-policy completion event is grandfathered."""
    for event in reversed(events):
        if event.get("gate_id") != gate["id"]:
            continue
        observed = parse_timestamp(event["timestamp"]).date()
        return (event.get("status") == "completed" and observed < RECEIPT_POLICY_DATE
                and str(observed) == gate.get("completed_at"))
    return False


def validate_receipt(root, gate, name):
    receipt = read_json(safe_file(root, name))
    registry = read_json(root / "doc/evidence/registry.json").get("receipts", {})
    entry = registry.get(name, {})
    if not isinstance(receipt, dict) or not isinstance(entry, dict):
        raise ValueError("Completion receipt and registry entry must be JSON objects")
    digest = hashlib.sha256(safe_file(root, name).read_bytes()).hexdigest()
    if entry.get("sha256") != digest or entry.get("status") != "verified":
        raise ValueError("Completion receipt must have a matching verified SHA-256 registry entry")
    for source in (receipt, entry):
        if source.get("gate") != gate["id"] or source.get("command") != gate.get("verification"):
            raise ValueError("Receipt gate and command must match the roadmap gate")
    if receipt.get("criterion") != gate.get("criterion"):
        raise ValueError("Completion receipt must identify the exact reviewed gate criterion")
    if type(receipt.get("exit_code")) is not int or receipt["exit_code"] != 0:
        raise ValueError("Completion receipt must retain an integer exit_code of zero")
    executed = parse_timestamp(receipt["executed_at"])
    if executed > datetime.now(timezone.utc):
        raise ValueError("Completion receipt execution date is in the future")
    for field in ("platform", "revision", "scope", "limitations"):
        if not isinstance(receipt.get(field), str) or not receipt[field].strip():
            raise ValueError(f"Completion receipt must describe {field}")
    invariants = receipt.get("verified_invariants")
    if not isinstance(invariants, list) or not invariants or not all(
            isinstance(item, str) and item.strip() for item in invariants):
        raise ValueError("Completion receipt must list verified_invariants for criterion review")
    output = receipt["output"]
    if not isinstance(output, dict):
        raise ValueError("Receipt output must identify a retained file and SHA-256")
    output_file = safe_file(root, output["path"])
    if not output_file.stat().st_size or hashlib.sha256(output_file.read_bytes()).hexdigest() != output["sha256"]:
        raise ValueError("Retained output must be nonempty and match its SHA-256 fingerprint")
    if output_file == safe_file(root, name):
        raise ValueError("Receipt must reference a separate execution output artifact")


def validate(register, events, root):
    errors = []
    horizons = register.get("horizons", {})
    if not horizons or any(not h.startswith("H") or not h[1:].isdigit() for h in horizons):
        return ["Horizons must use H<number> identifiers"]
    seen = set()
    for gate in register.get("gates", []):
        gate_id = gate.get("id")
        if not gate_id or gate_id in seen:
            errors.append(f"Duplicate or missing gate ID: {gate_id}")
        seen.add(gate_id)
        if gate.get("horizon") not in horizons or gate.get("status") not in STATUSES:
            errors.append(f"{gate_id}: unknown horizon or status")
        if gate.get("status") == "completed":
            try:
                date.fromisoformat(gate.get("completed_at", ""))
                safe_file(root, gate.get("evidence"))
                if not gate.get("verification"):
                    raise ValueError("Missing verification command")
                if gate.get("completion_receipt"):
                    validate_receipt(root, gate, gate["completion_receipt"])
                elif not legacy_closure(gate, events):
                    raise ValueError("Completed gate requires a completion_receipt; source presence is insufficient")
            except (ValueError, TypeError, KeyError, OSError) as exc:
                errors.append(f"{gate_id}: {exc}")
        elif gate.get("completed_at") or gate.get("completion_receipt"):
            errors.append(f"{gate_id}: incomplete gate retains current completion fields")
    event_ids = set()
    for event in events:
        event_id = event.get("event_id")
        if not event_id or event_id in event_ids:
            errors.append(f"Duplicate or missing event ID: {event_id}")
        event_ids.add(event_id)
        try:
            parse_timestamp(event["timestamp"])
        except (ValueError, TypeError, KeyError) as exc:
            errors.append(f"{event_id}: invalid timestamp: {exc}")
    return errors


def cell(value):
    return str(value).replace("|", "\\|").replace("\n", " ")


def render(register):
    horizons, gates = register["horizons"], register["gates"]
    active = register["active_horizon"] or "All horizons complete"
    lines = ["# 🗺️ HEAV3NS-MUSE Auto-Rolling Roadmap", "", "<!-- docs-scope: current-guide -->", "",
             "*An evidence-backed, auto-rolling milestone roadmap for the agent-native digital audio workstation initiative.*", "",
             f"Active horizon: **{active}** • Last synchronized: **{register['last_updated'][:10]}**", "",
             "Machine authority: [register.json](doc/roadmap/register.json) • "
             "[Open actions](doc/roadmap/OPEN_ITEMS.md) • [Engineering evidence](doc/agent-native-architecture.md) • "
             "[Event log](doc/roadmap/events.jsonl)", "", "## Current rolling horizon", "", "```text"]
    for h_id in sorted(horizons, key=lambda h: int(h[1:])):
        h = horizons[h_id]
        pct = h["completion_percentage"]
        filled = round(pct / 5)
        lines.append(f"[{('█' * filled + '░' * (20 - filled))}] {pct:>3}%  {h_id}: {h['title']} ({h['status']})")
    lines.extend(["```", "", "## Horizon breakdown", ""])
    for h_id in sorted(horizons, key=lambda h: int(h[1:])):
        h = horizons[h_id]
        lines.extend([f"### {h_id}: {h['title']}", "", f"**Status: {h['status']} ({h['completion_percentage']}% complete)**", "", h["description"], ""])
    lines.extend(["## Milestone gate register", "", "| Gate | Horizon | Title | Status | Verification command | Implementation / retained receipt |",
                  "| :--- | :--- | :--- | :--- | :--- | :--- |"])
    for g in gates:
        ev = g.get("completion_receipt") or g.get("evidence")
        evidence = f"[{cell(ev)}]({ev})" if ev else "Future planning"
        # ROADMAP.md is at the repository root.
        lines.append(f"| `{g['id']}` | {g['horizon']} | {cell(g['title'])} | `{g['status']}` | {cell(g.get('verification') or 'N/A')} | {evidence} |")
    lines.extend(["", "## Auto-rolling governance", "",
                  "1. New completions require a registered, hash-checked execution receipt and retained output, plus a maintainer's criterion review. Source files alone do not close gates.",
                  "2. Pre-2026-10-16 completion claims remain dated history. Their original events and dates are preserved; synchronization does not reverify them.",
                  "3. Synchronization derives horizon status, percentages and the first incomplete horizon from gate state. Horizon changes append an event; repeated synchronization without changes writes nothing.",
                  "4. `python3 scripts/roadmap-sync.py check` validates register consistency, generated views and referenced completion receipts. It does not execute verification commands or establish continuous runtime coverage.",
                  "5. Use `python3 scripts/roadmap-sync.py sync` after register edits. [Completion procedure](doc/how-to/change-with-documentation.md) and [append-only rolling log](doc/roadmap/ROLLING_LOG.md).", ""])
    roadmap = "\n".join(lines)
    lines = ["# Active Roadmap Actions & Open Gates", "", "<!-- docs-scope: current-guide -->", "",
             "Generated from [register.json](register.json). Dates are review targets, not delivery promises.", "",
             "[Engineering record](../agent-native-architecture.md) describes observed results and current limitations.", "",
             "| Gate | Horizon | Status | Owner | Target date | Description and closure criterion |",
             "| :--- | :--- | :--- | :--- | :--- | :--- |"]
    for g in gates:
        if g["status"] != "completed":
            lines.append(f"| **{g['id']}** | {g['horizon']} | {g['status']} | {cell(g['owner'])} | {g.get('target_date') or 'Unscheduled'} | **{cell(g['title'])}**: {cell(g['criterion'])} |")
    if all(g["status"] == "completed" for g in gates):
        lines.extend(["", "All registered gates are complete."])
    lines.append("")
    return {"ROADMAP.md": roadmap, "doc/roadmap/OPEN_ITEMS.md": "\n".join(lines)}


def sync_roadmap(root, register, pending_events=()):
    normalized = normalize(register)
    views = render(normalized)
    changed = normalized != register or any(
        not (root / name).is_file() or (root / name).read_text(encoding="utf-8") != content
        for name, content in views.items())
    new_events = list(pending_events)
    if register.get("active_horizon") != normalized["active_horizon"]:
        new_events.append({"event_id": f"EVT-{uuid.uuid4().hex}", "timestamp": timestamp(),
                           "type": "horizon-changed", "from_horizon": register.get("active_horizon"),
                           "to_horizon": normalized["active_horizon"]})
    if changed or new_events:
        normalized["last_updated"] = timestamp()
        views = render(normalized)
        (root / "doc/roadmap/register.json").write_text(dump_json(normalized), encoding="utf-8")
        for name, content in views.items():
            (root / name).write_text(content, encoding="utf-8")
        if new_events:
            with (root / "doc/roadmap/events.jsonl").open("a", encoding="utf-8") as stream:
                for event in new_events:
                    stream.write(json.dumps(event) + "\n")
    return normalized


def main(argv=None, root=ROOT):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("sync", help="Derive horizon state and generate roadmap/open actions")
    sub.add_parser("check", help="Validate register, generated views and retained receipts; never run tests")
    roll = sub.add_parser("roll", help="Change a gate status with retained evidence for completions")
    roll.add_argument("--gate", required=True)
    roll.add_argument("--status", required=True, choices=sorted(STATUSES))
    roll.add_argument("--note", required=True, help="Criterion review and limitations")
    roll.add_argument("--receipt", help="Registered execution receipt required for completion")
    args = parser.parse_args(argv)
    try:
        register = read_json(root / "doc/roadmap/register.json")
        events = [json.loads(line) for line in (root / "doc/roadmap/events.jsonl").read_text(encoding="utf-8").splitlines() if line.strip()]
        if args.command == "check":
            errors = validate(register, events, root)
            if errors:
                raise ValueError("\n".join(errors))
            normalized = normalize(register)
            if normalized != register:
                errors.append("Derived horizon state is stale; run roadmap-sync.py sync")
            for name, content in render(normalized).items():
                if not (root / name).is_file() or (root / name).read_text(encoding="utf-8") != content:
                    errors.append(f"{name} is stale; run roadmap-sync.py sync")
            if errors:
                raise ValueError("\n".join(errors))
            legacy = sum(g["status"] == "completed" and not g.get("completion_receipt") for g in register["gates"])
            print(f"Roadmap consistent: {len(register['gates'])} gates; {legacy} historical closures without current receipt validation. No verification commands executed.")
        else:
            pending = []
            if args.command == "roll":
                gate = next((g for g in register["gates"] if g["id"] == args.gate), None)
                if gate is None:
                    raise ValueError(f"Unknown gate: {args.gate}")
                if not args.note.strip():
                    raise ValueError("A nonempty criterion review note is required")
                if args.status == "completed":
                    if not args.receipt:
                        raise ValueError("Completion requires --receipt; run and retain verification first")
                    validate_receipt(root, gate, args.receipt)
                elif args.receipt:
                    raise ValueError("--receipt applies only to completed gates")
                if gate["status"] == args.status and gate.get("completion_receipt") == args.receipt:
                    errors = validate(register, events, root)
                    if errors:
                        raise ValueError("\n".join(errors))
                    print(f"{gate['id']} already has the requested status and receipt; no event added.")
                    return 0
                old_status = gate["status"]
                gate["status"] = args.status
                gate["completed_at"] = timestamp()[:10] if args.status == "completed" else None
                gate.pop("completion_receipt", None)
                if args.receipt:
                    gate["completion_receipt"] = args.receipt
                pending.append({"event_id": f"EVT-{uuid.uuid4().hex}", "timestamp": timestamp(),
                                "gate_id": gate["id"], "from_status": old_status, "status": args.status,
                                "note": args.note, "completion_receipt": args.receipt})
            errors = validate(register, events, root)
            if errors:
                raise ValueError("\n".join(errors))
            result = sync_roadmap(root, register, pending)
            print(f"Roadmap synchronized. Active horizon: {result['active_horizon'] or 'all complete'}.")
    except (ValueError, TypeError, KeyError, OSError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
