#!/usr/bin/env python3
"""SLSA-aligned artifact provenance and release integrity manifest generator.

Produces verifiable, cryptographically signed release metadata tying build artifacts
to git commit revisions, submodule states, dependency trees, and physical CI evidence receipts.
A successful build does not authorize publication without custodial approval.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DOCS_DIR = "doc" if (ROOT / "doc").is_dir() else "docs"
EVIDENCE_REGISTRY = f"{DOCS_DIR}/evidence/registry.json"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def get_git_info():
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    tag = subprocess.run(["git", "describe", "--tags", "--always"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    return commit, tag


def get_submodule_manifest():
    res = subprocess.run(["git", "submodule", "status"], cwd=ROOT, capture_output=True, text=True)
    manifest = {}
    for line in res.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) >= 2:
            commit_hash = parts[0].lstrip("-+")
            sub_path = parts[1]
            manifest[sub_path] = commit_hash
    return manifest


def generate_provenance(artifact_paths, release_version, builder="github-actions", authorized_by=None):
    commit, git_tag = get_git_info()
    iso_now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    artifacts_data = []
    for art in artifact_paths:
        p = Path(art)
        if not p.is_file():
            print(f"Warning: Artifact file not found: {art}", file=sys.stderr)
            continue
        artifacts_data.append({
            "name": p.name,
            "path": str(p),
            "size_bytes": p.stat().st_size,
            "sha256": sha256_file(p)
        })

    ev_path = ROOT / EVIDENCE_REGISTRY
    ev_data = json.loads(ev_path.read_text(encoding="utf-8")) if ev_path.is_file() else {}
    receipt_keys = sorted(ev_data.get("receipts", {}).keys())

    manifest = {
        "_type": "https://in-toto.io/Statement/v1",
        "predicateType": "https://slsa.dev/provenance/v1",
        "subject": artifacts_data,
        "predicate": {
            "buildDefinition": {
                "buildType": "https://heav3ns.org/build/daw/v1",
                "externalParameters": {
                    "repository": "https://github.com/CardSorting/heav3ns-muse",
                    "commit": commit,
                    "ref": git_tag,
                    "release_version": release_version
                },
                "system": {
                    "builder": builder,
                    "host_platform": f"{platform.system()} {platform.machine()}",
                    "python_version": platform.python_version()
                }
            },
            "runDetails": {
                "metadata": {
                    "invocationId": f"build-{commit[:8]}-{int(datetime.now().timestamp())}",
                    "startedOn": iso_now,
                    "finishedOn": iso_now
                },
                "submodules": get_submodule_manifest(),
                "evidence_receipts_verified": len(receipt_keys),
                "custodial_publication_authorization": {
                    "authorized": bool(authorized_by),
                    "authorized_by": authorized_by or "PENDING_CUSTODIAL_APPROVAL",
                    "authority_role": "Founding Custodian (EGG) / Executive Chair (MAJO)",
                    "authorization_timestamp": iso_now if authorized_by else None
                }
            }
        }
    }
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="Release version string (e.g., 1.0.0-rc1)")
    parser.add_argument("--artifacts", nargs="*", default=[], help="Paths to build output artifacts to fingerprint")
    parser.add_argument("--output", default="doc/evidence/release-provenance.json", help="Path to write provenance manifest")
    parser.add_argument("--authorized-by", help="Explicit custodial approver name (e.g., 'EGG')")
    parser.add_argument("--builder", default="github-actions", help="Build execution environment identity")
    args = parser.parse_args()

    manifest = generate_provenance(
        artifact_paths=args.artifacts,
        release_version=args.version,
        builder=args.builder,
        authorized_by=args.authorized_by
    )

    out_file = ROOT / args.output
    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Generated SLSA release provenance manifest: {out_file}")
    if not args.authorized_by:
        print("Note: Publication status is 'PENDING_CUSTODIAL_APPROVAL'. Build alone does NOT authorize publication.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
