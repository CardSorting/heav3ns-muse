#!/usr/bin/env python3
"""Check documentation scope, catalog consistency, ADR numbering, and local links."""

from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
DOCS_DIR = ROOT / ("doc" if (ROOT / "doc").is_dir() else "docs")
SCOPES = {
    "current-guide", "domain-reference", "decision-record",
    "operational-record", "research-reference", "commercial-draft",
}


def prose(text):
    return re.sub(r"(?ms)^\s*(`{3,}|~{3,})[^\n]*\n.*?^\s*\1\s*$", "", text)


def links(text):
    text = prose(text)
    for match in re.finditer(r"\[[^\]\n]*\]\((<[^>]+>|[^\s)]+)(?:\s+\"[^\"]*\")?\)", text):
        yield match.group(1).strip("<>")


def anchors(text):
    result = set()
    counts = {}
    for line in prose(text).splitlines():
        match = re.match(r"^#{1,6}\s+(.+?)\s*#*\s*$", line)
        if not match:
            continue
        label = re.sub(r"\[([^]]+)\]\([^)]+\)", r"\1", match.group(1))
        slug = re.sub(r"[^\w\- ]", "", label.lower()).replace(" ", "-")
        count = counts.get(slug, 0)
        counts[slug] = count + 1
        result.add(slug if not count else f"{slug}-{count}")
    result.update(re.findall(r'(?:id|name)=[\"\']([^\"\']+)[\"\']', text))
    return result


def main():
    errors = []
    files = sorted(DOCS_DIR.rglob("*.md"))
    catalog = DOCS_DIR / "CATALOG.md"
    indexed = set()
    if catalog.is_file():
        indexed = {
            (catalog.parent / unquote(urlsplit(link).path)).resolve()
            for link in links(catalog.read_text(encoding="utf-8"))
            if not urlsplit(link).scheme and not link.startswith("#")
        }

    for path in files:
        text = path.read_text(encoding="utf-8")
        markers = re.findall(r"<!-- docs-scope: ([\w-]+) -->", text)
        if len(markers) != 1 or markers[0] not in SCOPES:
            errors.append(f"{path.relative_to(ROOT)}: requires exactly one valid scope marker (found {markers})")
        if catalog.is_file() and path.resolve() not in indexed and path.name != "CATALOG.md":
            errors.append(f"{path.relative_to(ROOT)}: missing from complete catalog ({catalog.relative_to(ROOT)})")

    decisions = sorted((DOCS_DIR / "adr").glob("[0-9]*.md"))
    if decisions:
        ids = [int(path.name.split("-", 1)[0]) for path in decisions]
        if ids != list(range(1, max(ids, default=0) + 1)):
            errors.append("ADR IDs must be unique and contiguous starting from 1")
        adr_index_path = DOCS_DIR / "adr/README.md"
        if adr_index_path.is_file():
            adr_index = adr_index_path.read_text(encoding="utf-8")
            for path in decisions:
                if path.name not in adr_index:
                    errors.append(f"{path.name}: missing from ADR catalog index")

    roots = [ROOT / name for name in ("README.md", "AGENTS.md", "ROADMAP.md")]
    for path in files + [r for r in roots if r.is_file()]:
        text = path.read_text(encoding="utf-8")
        for link in links(text):
            parsed = urlsplit(link)
            if parsed.scheme or parsed.netloc or link.startswith("/") or link.startswith("#"):
                continue
            target = (path.parent / unquote(parsed.path)).resolve() if parsed.path else path
            if not target.exists():
                errors.append(f"{path.relative_to(ROOT)}: broken link to missing target {link}")
            elif parsed.fragment and target.suffix == ".md":
                target_text = target.read_text(encoding="utf-8")
                if "<!-- docs-scope: current-guide -->" in target_text and unquote(parsed.fragment) not in anchors(target_text):
                    errors.append(f"{path.relative_to(ROOT)}: missing anchor in current guide: {link}")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        print(f"\nDocumentation validation failed: {len(errors)} issue(s).", file=sys.stderr)
        return 1
    print(f"Documentation validation passed: {len(files)} guides, {len(decisions)} ADRs; scope, catalog, links, and anchors valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
