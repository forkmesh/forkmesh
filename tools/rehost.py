#!/usr/bin/env python3
"""Rewrite production ForkMesh hostnames to the ForkMesh Next hostnames.

ForkMesh Next (docs/PLAN.md, Phase 0 step 2) must never send traffic to the
production deployment. This tool finds every production hostname used as a
host (URLs, bare host strings, ``git@ssh...`` remotes, URL userinfo) and maps
it onto the Next hosts:

    forkmesh.com, www.forkmesh.com  -> next.forkmesh.com
    app.forkmesh.com, api.forkmesh.com -> app.next.forkmesh.com
    world.forkmesh.com              -> world.next.forkmesh.com
    ssh.forkmesh.com                -> ssh.next.forkmesh.com

Anything already under ``next.forkmesh.com`` is left alone, so the rewrite is
idempotent. Email addresses (``founders@forkmesh.com``), ActivityPub handles,
the ``users.noreply.forkmesh.com`` commit-email domain are not traffic hosts
and are never rewritten. A bare ``.forkmesh.com`` suffix (cookie domains,
``endswith`` trust checks) becomes ``.next.forkmesh.com`` so a Next cookie or
trust rule never covers the production hosts. Production
mirror hosts (``mirrorNN.forkmesh.com``) and other unknown subdomains are left
as-is and reported as flags: Next has its own n1/n2/n3 nodes, and a mirror
name in a fixture or doc is not something a blind rewrite can map.

Usage:
    python3 tools/rehost.py --check   # exit 1 if any mappable host remains
    python3 tools/rehost.py --apply   # rewrite in place
    python3 tools/rehost.py --flags   # also list unmapped production hosts
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Directories rehosted by default (docs/PLAN.md). desktop/ is reference only.
DEFAULT_ROOTS = ("app", "world", "www", "server", "tools", "marketplace", "conformance")

SKIP_DIRS = frozenset({
    ".git", "node_modules", "python_modules", "dist", "src_build", "build",
    "__pycache__", ".venv", ".venv-workers", ".wrangler", ".pytest_cache",
})

# Files this tool never rewrites: they must name the production hosts on
# purpose (this mapping table, and the guard test that scans for them).
SELF_FILES = frozenset({
    "tools/rehost.py",
    "app/tests/test_next_no_production_hosts.py",
})

# Signed or hashed protocol vectors. Their bytes are covered by signatures or
# digests, so rewriting a URL inside them breaks verification; they are test
# data describing upstream, not addresses Next sends traffic to.
FROZEN_FILES = frozenset({
    # Social-signal protocol vectors: signed records and their generator.
    "conformance/social-signals/v1/vectors.json",
    "tools/social_signals.py",
    # Disaster-recovery runbook: its digest is attested by committed exercise
    # records, and runbook.md is rendered from it by the recovery tool.
    "conformance/disaster-recovery/v1/runbook.json",
    "tools/forkmesh_disaster_recovery.py",
    "www/docs/disaster-recovery/runbook.md",
})

APEX = "forkmesh.com"
NEXT_APEX = "next.forkmesh.com"

# Production host -> Next host. Keys are full hostnames (lowercase).
HOST_MAP = {
    "forkmesh.com": "next.forkmesh.com",
    "www.forkmesh.com": "next.forkmesh.com",
    "app.forkmesh.com": "app.next.forkmesh.com",
    "api.forkmesh.com": "app.next.forkmesh.com",
    "world.forkmesh.com": "world.next.forkmesh.com",
    "ssh.forkmesh.com": "ssh.next.forkmesh.com",
}

# Subdomains that name mail or identity domains, not traffic hosts.
NON_TRAFFIC_HOSTS = frozenset({
    "users.noreply.forkmesh.com",
})

# A hostname ending in forkmesh.com. The lookbehind refuses a preceding
# hostname character, so "notforkmesh.com" and ".forkmesh.com" suffix checks
# never match; "%2F" (an encoded "/") and a shell default ("${X:-host}") are
# allowed through separately.
HOST_RE = re.compile(
    r"(?:(?<=%2F)|(?<=%2f)|(?<=:-)|(?<![A-Za-z0-9._%-]))"
    r"((?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)*)forkmesh\.com"
    r"(?![A-Za-z0-9-]|\.[A-Za-z0-9])"
)

# A domain suffix on its own: ".forkmesh.com" not preceded by a label.
SUFFIX_RE = re.compile(r"(?<![A-Za-z0-9_-])\.forkmesh\.com(?![A-Za-z0-9-]|\.[A-Za-z0-9])")

TEXT_SUFFIX_DENY = (
    ".png", ".jpg", ".jpeg", ".gif", ".webp", ".ico", ".woff", ".woff2",
    ".ttf", ".otf", ".pdf", ".zip", ".gz", ".br", ".wasm", ".glb", ".mp3",
    ".mp4", ".ogg", ".wav", ".sqlite", ".db", ".lock",
)


def _is_email_context(text: str, start: int, host: str) -> bool:
    """True when the host at ``start`` is the domain of an email/handle.

    ``git@ssh.forkmesh.com`` (an SSH remote) and ``user:pass@app.forkmesh.com``
    (URL userinfo) are traffic, so a subdomain Next maps is still rewritten
    after an ``@``. A bare ``someone@forkmesh.com`` is a mailbox or handle.
    """
    if start == 0 or text[start - 1] != "@":
        return False
    return host == APEX or host == "www." + APEX


def classify(host: str) -> str:
    """Return "next", "map", "skip" or "flag" for a forkmesh.com hostname."""
    if host == NEXT_APEX or host.endswith("." + NEXT_APEX):
        return "next"
    if host in HOST_MAP:
        return "map"
    if host in NON_TRAFFIC_HOSTS:
        return "skip"
    return "flag"


def rewrite_text(text: str) -> tuple[str, int, list[str]]:
    """Rewrite production hosts in ``text``.

    Returns (new_text, rewrite_count, flagged_hosts).
    """
    count = 0
    flags: list[str] = []
    out: list[str] = []
    last = 0
    for match in HOST_RE.finditer(text):
        host = match.group(0)
        kind = classify(host)
        if kind == "flag":
            flags.append(host)
            continue
        if kind != "map" or _is_email_context(text, match.start(), host):
            continue
        out.append(text[last:match.start()])
        out.append(HOST_MAP[host])
        last = match.end()
        count += 1
    out.append(text[last:])
    text, suffixes = SUFFIX_RE.subn("." + NEXT_APEX, "".join(out))
    return text, count + suffixes, flags


def iter_files(roots):
    for root in roots:
        base = REPO_ROOT / root
        if base.is_file():
            yield base
            continue
        if not base.is_dir():
            continue
        stack = [base]
        while stack:
            current = stack.pop()
            for entry in sorted(current.iterdir()):
                if entry.is_symlink():
                    continue
                if entry.is_dir():
                    if entry.name not in SKIP_DIRS:
                        stack.append(entry)
                    continue
                if entry.suffix.lower() in TEXT_SUFFIX_DENY:
                    continue
                yield entry


def _read_text(path: Path) -> str | None:
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if b"\0" in data[:8192]:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def scan(roots, apply: bool):
    """Scan (and optionally rewrite). Returns (per_file_counts, flags)."""
    counts: dict[str, int] = {}
    flags: dict[str, list[str]] = {}
    for path in iter_files(roots):
        rel = path.relative_to(REPO_ROOT).as_posix()
        if rel in SELF_FILES or rel in FROZEN_FILES:
            continue
        text = _read_text(path)
        if text is None or "forkmesh.com" not in text:
            continue
        new_text, count, file_flags = rewrite_text(text)
        if count:
            counts[rel] = count
            if apply:
                path.write_text(new_text, encoding="utf-8")
        if file_flags:
            flags[rel] = file_flags
    return counts, flags


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="exit 1 if a mappable production host remains")
    mode.add_argument("--apply", action="store_true", help="rewrite files in place")
    parser.add_argument("--flags", action="store_true", help="list unmapped production hosts (mirrorNN etc.)")
    parser.add_argument("roots", nargs="*", default=list(DEFAULT_ROOTS))
    args = parser.parse_args(argv)

    counts, flags = scan(args.roots, apply=args.apply)
    per_dir: dict[str, int] = {}
    for rel, count in counts.items():
        top = rel.split("/", 1)[0]
        per_dir[top] = per_dir.get(top, 0) + count
    verb = "rewrote" if args.apply else "would rewrite"
    for rel, count in sorted(counts.items()):
        if args.check:
            print(f"{rel}: {count}")
    for top, count in sorted(per_dir.items()):
        print(f"{verb} {count} host(s) in {top}/")
    if args.flags:
        for rel, hosts in sorted(flags.items()):
            print(f"flag {rel}: {', '.join(sorted(set(hosts)))}")
    elif flags:
        total = sum(len(v) for v in flags.values())
        print(f"{total} unmapped production host(s) left as-is in {len(flags)} file(s); see --flags")
    if args.check and counts:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
