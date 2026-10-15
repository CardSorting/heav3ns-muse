#!/usr/bin/env python3
"""Offline knowledge catalog, source impact, and review tooling for HEAV3NS-MUSE."""

import argparse
from datetime import date, datetime, timezone
import fnmatch
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DOCS_DIR = "doc" if (ROOT / "doc").is_dir() else "docs"
CONFIG = f"{DOCS_DIR}/knowledge/registry.json"
REVIEWS = f"{DOCS_DIR}/knowledge/reviews.json"
CATALOG = f"{DOCS_DIR}/knowledge/catalog.json"
KINDS = {
    "tutorial", "how-to", "runbook", "reference",
    "explanation", "decision", "evidence", "research", "draft", "navigation"
}
STATUSES = {"proposed", "accepted", "rejected", "deprecated", "superseded"}
DELIVERY = {"not-started", "local", "deployed", "verified"}
RUNBOOK_SECTIONS = (
    "Purpose and trigger",
    "Authority and prerequisites",
    "Procedure",
    "Stop conditions",
    "Recovery and rollback",
    "Verification and evidence"
)
EVIDENCE_REGISTRY = f"{DOCS_DIR}/evidence/registry.json"


def read(root, path):
    return json.loads((root / path).read_text(encoding="utf-8"))


def dump(value):
    return json.dumps(value, indent=2, ensure_ascii=False) + "\n"


def safe_file(root, name):
    path = Path(name)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError(f"Unsafe repository reference: {name}")
    result = (root / path).resolve()
    if not result.is_relative_to(root.resolve()) or not result.is_file():
        raise ValueError(f"Missing or external repository reference: {name}")
    return result


def source_files(root, domain):
    paths = set(domain.get("guides", []))
    for pattern in domain.get("sources", []):
        paths.update(
            str(p.relative_to(root))
            for p in root.glob(pattern)
            if p.is_file() and not p.is_symlink() and "__pycache__" not in p.parts
        )
    return sorted(paths)


def fingerprints(root, domain):
    return {
        name: hashlib.sha256(safe_file(root, name).read_bytes()).hexdigest()
        for name in source_files(root, domain)
    }


def review_state(root, domain, review, today=None):
    if not review:
        return "unreviewed"
    if review.get("files") != fingerprints(root, domain):
        return "stale"
    today = today or datetime.now(timezone.utc).date()
    return "due" if date.fromisoformat(review["review_by"]) < today else "reviewed"


def documents(root, config):
    result = []
    paths = list((root / DOCS_DIR).rglob("*.md"))
    for path in sorted(paths):
        name = path.relative_to(root).as_posix()
        body = path.read_text(encoding="utf-8")
        title = next((s[2:].strip() for s in body.splitlines() if s.startswith("# ")), path.stem)
        scope = re.search(r"<!-- docs-scope: ([\w-]+) -->", body)
        scope = scope[1] if scope else "missing"
        override = config.get("documents", {}).get(name, {})
        kind = {
            "decision-record": "decision",
            "operational-record": "evidence",
            "research-reference": "research",
            "commercial-draft": "draft"
        }.get(scope, "reference")
        for folder, folder_kind in [
            ("tutorials", "tutorial"), ("how-to", "how-to"),
            ("runbooks", "runbook"), ("explanations", "explanation")
        ]:
            if f"/{folder}/" in name:
                kind = folder_kind
        if path.name in {"README.md", "CATALOG.md", "DOMAINS.md", "DECISIONS.md"}:
            kind = "navigation"
        domains = [key for key, domain in config.get("domains", {}).items() if name in domain.get("guides", [])]
        result.append({
            "path": name, "title": title, "scope": scope,
            "kind": override.get("kind", kind), "domains": domains,
            "curation": "assigned" if domains else "unreviewed",
            "keywords": override.get("keywords", []),
            "decision": config.get("decisions", {}).get(name) if kind == "decision" else None
        })
    return result


def validate(root, config, reviews, require_complete=True):
    errors = []
    if config.get("version") != 1:
        errors.append("Unsupported knowledge registry version")
    domains = config.get("domains", {})
    if require_complete and config.get("require_domain_reviews"):
        for key in domains.keys() - reviews.keys():
            errors.append(f"{key}: missing domain review; inspect source and canonical guides")
    for key, domain in domains.items():
        if not re.fullmatch(r"[a-z][a-z0-9-]*", key) or not domain.get("owner"):
            errors.append(f"{key}: domain requires an ID and responsible role")
        if not domain.get("sources") or not domain.get("guides"):
            errors.append(f"{key}: source and guide coverage required")
        for pattern in domain.get("sources", []):
            if Path(pattern).is_absolute() or ".." in Path(pattern).parts:
                errors.append(f"{key}: unsafe source pattern: {pattern}")
            elif not any(p.is_file() for p in root.glob(pattern)):
                errors.append(f"{key}: source pattern matches no files: {pattern}")
        for guide in domain.get("guides", []):
            safe_file(root, guide)
    for name, metadata in config.get("documents", {}).items():
        safe_file(root, name)
        if metadata.get("kind") not in KINDS:
            errors.append(f"{name}: invalid content kind: {metadata.get('kind')}")
    for name in config.get("runbooks", []):
        body = safe_file(root, name).read_text(encoding="utf-8")
        for heading in RUNBOOK_SECTIONS:
            if f"## {heading}\n" not in body and f"## {heading}\r\n" not in body:
                errors.append(f"{name}: missing runbook section: {heading}")
    for path in (root / f"{DOCS_DIR}/runbooks").glob("*.md"):
        name = path.relative_to(root).as_posix()
        if path.name != "README.md" and name not in config.get("runbooks", []):
            errors.append(f"{name}: new runbook requires registration in registry.json")

    ev_reg_path = root / EVIDENCE_REGISTRY
    if ev_reg_path.is_file():
        try:
            ev_data = json.loads(ev_reg_path.read_text(encoding="utf-8"))
            for receipt_path in ev_data.get("receipts", {}).keys():
                safe_file(root, receipt_path)
        except Exception as e:
            errors.append(f"Evidence registry invalid: {e}")

    decisions = config.get("decisions", {})
    for name, record in decisions.items():
        safe_file(root, name)
        if not re.fullmatch(rf"{DOCS_DIR}/adr/\d{{4}}-.+\.md", name):
            errors.append(f"{name}: invalid ADR path")
        if record["status"] not in STATUSES or record["delivery"] not in DELIVERY:
            errors.append(f"{name}: invalid decision/delivery state")
        if record.get("domain") not in domains or not record.get("owner"):
            errors.append(f"{name}: missing decision domain/owner")
        for item in record.get("evidence", []):
            safe_file(root, item)
        successor = record.get("superseded_by")
        if record["status"] == "superseded" and not successor:
            errors.append(f"{name}: superseded requires a successor")
        if successor and (successor not in decisions or name not in decisions[successor].get("supersedes", [])):
            errors.append(f"{name}: successor must reciprocate supersession")
        for previous in record.get("supersedes", []):
            if previous not in decisions or decisions[previous].get("superseded_by") != name:
                errors.append(f"{name}: predecessor must reciprocate supersession")
        seen = {name}
        while successor in decisions:
            if successor in seen:
                errors.append(f"{name}: supersession cycle")
                break
            seen.add(successor)
            successor = decisions[successor].get("superseded_by")
    for key, review in reviews.items():
        if key not in domains:
            errors.append(f"{key}: review for unknown domain")
            continue
        if not all(review.get(k) for k in ("reviewer", "note", "reviewed_at", "review_by", "files")):
            errors.append(f"{key}: incomplete explicit review")
            continue
        evidence = review.get("evidence", {})
        if config.get("require_review_evidence") and not evidence:
            errors.append(f"{key}: explicit review requires retained evidence")
        for name, expected in evidence.items():
            path = safe_file(root, name)
            if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                errors.append(f"{key}: review evidence changed: {name}")
        reviewed = date.fromisoformat(review["reviewed_at"])
        due = date.fromisoformat(review["review_by"])
        if due < reviewed or reviewed > datetime.now(timezone.utc).date():
            errors.append(f"{key}: invalid review dates")
        if review_state(root, domains[key], review) == "stale":
            errors.append(f"{key}: source/guide review stale; review affected bodies before docs:review")
    return errors


def generated(root, config):
    rows = documents(root, config)
    titles = {row["path"]: row["title"].replace("|", "\\|") for row in rows}
    domain_page = [
        "# Knowledge domains", "", "<!-- docs-scope: current-guide -->", "",
        "Generated from [registry.json](registry.json). Run `python3 scripts/docs-kb.py status` for current review state.", ""
    ]
    for key, domain in config["domains"].items():
        domain_page += [
            f"## {domain['title']}", "", f"Owner role: **{domain['owner']}**. Domain: `{key}`.", "",
            *[f"- [{titles.get(p, Path(p).stem)}](../../{p})" for p in domain["guides"]], "",
            "Relevant checks: " + ", ".join(f"`{c}`" for c in domain.get("checks", [])) + ".", ""
        ]
    decisions = [
        "# Decision lifecycle register", "", "<!-- docs-scope: current-guide -->", "",
        f"Generated metadata, separate from [the full ADR catalog](../adr/README.md).", "",
        "| Decision | Decision status | Delivery evidence | Owner |",
        "| :--- | :---: | :---: | :--- |"
    ]
    for name, row in sorted(config.get("decisions", {}).items()):
        decisions.append(f"| [{titles.get(name, Path(name).stem)}](../../{name}) | `{row['status']}` | `{row['delivery']}` | {row['owner']} |")
    return {
        CATALOG: dump({"version": 1, "documents": rows}),
        f"{DOCS_DIR}/knowledge/DOMAINS.md": "\n".join(domain_page).rstrip() + "\n",
        f"{DOCS_DIR}/knowledge/DECISIONS.md": "\n".join(decisions).rstrip() + "\n"
    }


def changed_files(root, base=None):
    if base:
        oid = subprocess.run(
            ["git", "rev-parse", "--verify", "--end-of-options", base + "^{commit}"],
            cwd=root, capture_output=True, text=True, check=True
        ).stdout.strip()
        args = ["git", "diff", "--name-only", "-z", oid, "--"]
    else:
        args = ["git", "diff", "--name-only", "-z", "HEAD", "--"]
    tracked = subprocess.run(args, cwd=root, capture_output=True, check=True).stdout
    untracked = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=root, capture_output=True, check=True
    ).stdout
    return sorted(set(p.decode() for p in (tracked + untracked).split(b"\0") if p))


def impact(config, names):
    result = {}
    for name in names:
        matched = False
        for key, domain in config.get("domains", {}).items():
            if any(fnmatch.fnmatchcase(name, pat) or fnmatch.fnmatchcase(name, pat.replace("**/", ""))
                   for pat in domain.get("sources", []) + domain.get("guides", [])):
                result.setdefault(key, []).append(name)
                matched = True
        if not matched:
            result.setdefault("_unmapped", []).append(name)
    return result


def search(root, config, query, history=False, limit=10):
    terms = re.findall(r"[\w-]+", query.lower())
    if not terms:
        return []
    results = []
    for row in documents(root, config):
        if row["kind"] == "navigation":
            continue
        if not history and (row["scope"] != "current-guide" and
                            (not row["decision"] or row["decision"]["status"] != "accepted")):
            continue
        title = (row["title"] + " " + " ".join(row["keywords"])).lower()
        body = safe_file(root, row["path"]).read_text(encoding="utf-8").lower()
        if not all(t in body or t in title for t in terms):
            continue
        score = sum(10 * title.count(t) + min(body.count(t), 3) for t in terms)
        snippet = ""
        for t in terms:
            idx = body.find(t)
            if idx != -1:
                start = max(0, idx - 40)
                end = min(len(body), idx + 80)
                snippet = "..." + body[start:end].replace("\n", " ").strip() + "..."
                break
        results.append({**row, "score": score, "snippet": snippet})
    return sorted(results, key=lambda r: (-r["score"], r["path"]))[:limit]


def record_evidence(root, gate, command, artifact, note="", platform="Darwin/Linux"):
    ev_path = root / EVIDENCE_REGISTRY
    ev_data = json.loads(ev_path.read_text(encoding="utf-8")) if ev_path.is_file() else {"version": 1, "receipts": {}}
    art_file = safe_file(root, artifact)
    sha = hashlib.sha256(art_file.read_bytes()).hexdigest()
    iso_now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    ev_data.setdefault("receipts", {})[artifact] = {
        "gate": gate,
        "recorded_at": iso_now,
        "command": command,
        "platform": platform,
        "sha256": sha,
        "status": "verified",
        "note": note
    }
    ev_path.write_text(dump(ev_data), encoding="utf-8")
    print(f"Recorded evidence receipt for {gate} ({artifact}, sha256={sha[:12]}).")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("sync", help="Synchronize generated catalogs and markdown views")
    sub.add_parser("check", help="Validate knowledge registry, reviews, and decision records")
    sub.add_parser("audit", help="Run comprehensive documentation and governance audit scorecard")

    search_parser = sub.add_parser("search", help="Search authoritative knowledgebase documents")
    search_parser.add_argument("query", help="Keywords to query")
    search_parser.add_argument("--history", action="store_true", help="Include non-current guides and superseded decisions")
    search_parser.add_argument("--limit", type=int, default=10, help="Maximum results to return")

    ev_parser = sub.add_parser("record-evidence", help="Record physical evidence receipt into registry")
    ev_parser.add_argument("--gate", required=True, help="Roadmap gate ID")
    ev_parser.add_argument("--command", dest="exec_command", required=True, help="Verification command executed")
    ev_parser.add_argument("--artifact", required=True, help="Path to evidence receipt file")
    ev_parser.add_argument("--note", default="", help="Contextual note")
    ev_parser.add_argument("--platform", default="Darwin/Linux", help="Execution platform")

    status_parser = sub.add_parser("status", help="Show domain review freshness status")
    status_parser.add_argument("--fail-on-due", action="store_true", help="Exit nonzero if any domain review is due")
    status_parser.add_argument("--fail-on-unreviewed", action="store_true", help="Exit nonzero if any domain is unreviewed")

    impact_parser = sub.add_parser("impact", help="Analyze working-tree changes against domain boundaries")
    impact_parser.add_argument("--base", help="Git commit or branch to compare against")

    review_parser = sub.add_parser("review", help="Record an explicit maintainer domain review")
    review_parser.add_argument("--domain", required=True, help="Domain identifier")
    review_parser.add_argument("--reviewer", required=True, help="Reviewer name or role")
    review_parser.add_argument("--note", required=True, help="Review notes and context")
    review_parser.add_argument("--evidence", help="Path to evidence file to hash")

    args = parser.parse_args()
    config = read(ROOT, CONFIG)
    reviews = read(ROOT, REVIEWS)

    if args.command == "sync":
        for path, content in generated(ROOT, config).items():
            (ROOT / path).write_text(content, encoding="utf-8")
        print(f"Synchronized {CATALOG}, DOMAINS.md, and DECISIONS.md.")
        return 0

    if args.command == "check":
        errors = validate(ROOT, config, reviews, require_complete=True)
        if errors:
            print("\n".join(errors), file=sys.stderr)
            print(f"\nKnowledgebase validation failed: {len(errors)} error(s).", file=sys.stderr)
            return 1
        print(f"Knowledgebase validation passed ({len(config['domains'])} domains, {len(reviews)} reviews).")
        return 0

    if args.command == "audit":
        kb_errors = validate(ROOT, config, reviews, require_complete=True)
        docs_proc = subprocess.run(["python3", "scripts/verify-docs.py"], cwd=ROOT, capture_output=True, text=True)
        docs_ok = (docs_proc.returncode == 0)
        roadmap_proc = subprocess.run(["python3", "scripts/roadmap-sync.py", "check"], cwd=ROOT, capture_output=True, text=True)
        roadmap_ok = (roadmap_proc.returncode == 0)

        domain_counts = {"reviewed": 0, "due": 0, "stale": 0, "unreviewed": 0}
        for key, domain in config.get("domains", {}).items():
            rev = reviews.get(key)
            st = review_state(ROOT, domain, rev)
            domain_counts[st] = domain_counts.get(st, 0) + 1

        docs_list = documents(ROOT, config)
        scope_counts = {}
        for d in docs_list:
            sc = d.get("scope", "unknown")
            scope_counts[sc] = scope_counts.get(sc, 0) + 1

        register_path = ROOT / "doc/roadmap/register.json"
        roadmap_data = json.loads(register_path.read_text(encoding="utf-8")) if register_path.is_file() else {}
        horizons = roadmap_data.get("horizons", {})
        gates = roadmap_data.get("gates", [])
        completed_gates = sum(1 for g in gates if g.get("status") == "completed")
        total_gates = len(gates)

        print("========================================================================")
        print("            HEAV3NS-MUSE GOVERNANCE & KNOWLEDGEBASE AUDIT SCORECARD     ")
        print("========================================================================")
        print(f"  Documentation Integrity:   {'[PASS] Clean' if docs_ok else '[FAIL] Issues detected'}")
        print(f"    - Total Guides:          {len(docs_list)}")
        print(f"    - Current Guides:        {scope_counts.get('current-guide', 0)}")
        print(f"    - Domain References:     {scope_counts.get('domain-reference', 0)}")
        print(f"    - Decision Records:      {scope_counts.get('decision-record', 0)}")
        print(f"    - Operational Records:   {scope_counts.get('operational-record', 0)}")
        print("------------------------------------------------------------------------")
        print(f"  Knowledgebase Domains:     {'[PASS] All Fresh' if not kb_errors else '[FAIL] Stale/Errors'}")
        print(f"    - Total Domains:         {len(config.get('domains', {}))}")
        print(f"    - Reviewed & Sealed:     {domain_counts['reviewed']}")
        print(f"    - Review Due:            {domain_counts['due']}")
        print(f"    - Stale / Modified:      {domain_counts['stale']}")
        print("------------------------------------------------------------------------")
        print(f"  Auto-Rolling Roadmap:      {'[PASS] Valid' if roadmap_ok else '[FAIL] Schema error'}")
        print(f"    - Gates Completed:       {completed_gates} / {total_gates} ({int(completed_gates/total_gates*100 if total_gates else 0)}%)")
        for hid, hdata in sorted(horizons.items()):
            h_gates = [g for g in gates if g.get("horizon") == hid]
            h_comp = sum(1 for g in h_gates if g.get("status") == "completed")
            pct = int(h_comp / len(h_gates) * 100) if h_gates else 0
            bar = ("#" * (pct // 5)).ljust(20, ".")
            print(f"    - [{hid}] {hdata.get('title', ''):<32} [{bar}] {pct:>3}%")
        print("========================================================================")

        if not docs_ok:
            print("\nDocumentation check errors:\n" + docs_proc.stderr + docs_proc.stdout, file=sys.stderr)
        if kb_errors:
            print("\nKnowledgebase errors:\n" + "\n".join(kb_errors), file=sys.stderr)
        if not roadmap_ok:
            print("\nRoadmap errors:\n" + roadmap_proc.stderr + roadmap_proc.stdout, file=sys.stderr)

        return 0 if (docs_ok and not kb_errors and roadmap_ok) else 1

    if args.command == "status":
        exit_code = 0
        print(f"{'Domain':<24} {'Status':<12} {'Reviewer':<28} {'Review By'}")
        print("-" * 76)
        for key, domain in config.get("domains", {}).items():
            rev = reviews.get(key)
            st = review_state(ROOT, domain, rev)
            reviewer = rev.get("reviewer", "-") if rev else "-"
            review_by = rev.get("review_by", "-") if rev else "-"
            print(f"{key:<24} {st:<12} {reviewer:<28} {review_by}")
            if st == "due" and args.fail_on_due:
                exit_code = 1
            if st in {"unreviewed", "stale"} and args.fail_on_unreviewed:
                exit_code = 1
        return exit_code

    if args.command == "impact":
        changes = changed_files(ROOT, base=args.base)
        imp = impact(config, changes)
        print(f"Impacted domains ({len(changes)} changed files):")
        for domain, files in sorted(imp.items()):
            print(f"\n  [{domain}] ({len(files)} files)")
            for f in files[:8]:
                print(f"    - {f}")
            if len(files) > 8:
                print(f"    ... and {len(files) - 8} more")
        return 0

    if args.command == "review":
        domain_key = args.domain
        if domain_key not in config.get("domains", {}):
            print(f"Unknown domain: {domain_key}", file=sys.stderr)
            return 1
        dom = config["domains"][domain_key]
        fps = fingerprints(ROOT, dom)
        evidence_dict = {}
        if args.evidence:
            ev_path = safe_file(ROOT, args.evidence)
            evidence_dict[args.evidence] = hashlib.sha256(ev_path.read_bytes()).hexdigest()
        today_iso = datetime.now(timezone.utc).date().isoformat()
        next_month = date.fromordinal(datetime.now(timezone.utc).date().toordinal() + 30).isoformat()
        reviews[domain_key] = {
            "reviewer": args.reviewer,
            "note": args.note,
            "reviewed_at": today_iso,
            "review_by": next_month,
            "files": fps,
            "evidence": evidence_dict or reviews.get(domain_key, {}).get("evidence", {})
        }
        (ROOT / REVIEWS).write_text(dump(reviews), encoding="utf-8")
        print(f"Recorded review attestation for domain '{domain_key}' by {args.reviewer} valid through {next_month}.")
        return 0

    if args.command == "search":
        results = search(ROOT, config, args.query, history=args.history, limit=args.limit)
        if not results:
            print(f"No documents matched '{args.query}'.")
            return 0
        print(f"Found {len(results)} matching document(s) for '{args.query}':\n")
        for r in results:
            domains_str = ", ".join(r["domains"]) if r["domains"] else "unassigned"
            print(f"  [{r['score']:>2}] {r['title']}")
            print(f"       Path:   {r['path']}")
            print(f"       Scope:  {r['scope']} | Domain: {domains_str}")
            if r.get("snippet"):
                print(f"       Match:  {r['snippet']}")
            print()
        return 0

    if args.command == "record-evidence":
        record_evidence(ROOT, args.gate, args.exec_command,
                        args.artifact, note=args.note, platform=args.platform)
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
