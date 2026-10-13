#!/usr/bin/env python3
"""Plan, execute, validate, or restore ForkMesh's local history rewrite.

The rewrite is deliberately local and dependency-free. It preserves commit
trees, parent order, author/committer identity lines and timestamps while
replacing each message with one deterministic, name-redacted sentence.
Original messages and recovery metadata live under the repository's private
Git directory, never in the worktree.
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys


REWRITE_CONFIRMATION = "REWRITE-COMMIT-HISTORY"
RESTORE_CONFIRMATION = "RESTORE-COMMIT-HISTORY"
MAX_SUMMARY_CHARS = 120
REDACTION_TOKEN = "[contributor]"
TRAILER_RE = re.compile(
    r"(?im)^(?:co-authored-by|signed-off-by|reviewed-by|tested-by|"
    r"reported-by|suggested-by|acked-by|helped-by):.*$"
)
EMAIL_RE = re.compile(r"(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b")
HANDLE_RE = re.compile(r"(?<![\w@])@[A-Za-z0-9](?:[A-Za-z0-9_.-]{0,38})")
ATTRIBUTION_RE = re.compile(
    r"(?i)\b(?:thanks?|thank you|authored|co-authored|reviewed|tested|"
    r"reported|suggested|requested|helped|contributed)\s+"
    r"(?:(?:by|to)\s+)?(?:@?[A-Z][A-Za-z.'-]*(?:\s+[A-Z][A-Za-z.'-]*){0,3})"
)


class RewriteError(RuntimeError):
    pass


def run_git(root: Path, *args: str, input_bytes: bytes | None = None) -> bytes:
    result = subprocess.run(
        ["git", *args],
        cwd=root,
        input=input_bytes,
        check=False,
        capture_output=True,
    )
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise RewriteError(f"git {' '.join(args)} failed: {detail}")
    return result.stdout


def git_dir(root: Path) -> Path:
    value = run_git(root, "rev-parse", "--absolute-git-dir").decode().strip()
    path = Path(value).resolve()
    if not path.is_dir():
        raise RewriteError("resolved Git directory does not exist")
    return path


def default_refs(root: Path) -> list[str]:
    raw = run_git(
        root,
        "for-each-ref",
        "--format=%(refname)",
        "refs/heads",
        "refs/tags",
    ).decode()
    refs = sorted(line.strip() for line in raw.splitlines() if line.strip())
    if not refs:
        raise RewriteError("no local branches or tags are available")
    return refs


def resolve_refs(root: Path, requested: list[str]) -> dict[str, str]:
    refs = requested or default_refs(root)
    resolved: dict[str, str] = {}
    for ref in refs:
        if not (
            ref.startswith("refs/heads/") or ref.startswith("refs/tags/")
        ):
            raise RewriteError(
                f"out-of-scope ref {ref!r}; use refs/heads/* or refs/tags/*"
            )
        oid = run_git(root, "rev-parse", "--verify", ref).decode().strip()
        if not re.fullmatch(r"[0-9a-f]{40,64}", oid):
            raise RewriteError(f"could not resolve {ref}")
        resolved[ref] = oid
    return resolved


def commit_order(root: Path, refs: dict[str, str]) -> list[str]:
    raw = run_git(
        root,
        "rev-list",
        "--topo-order",
        "--reverse",
        *refs,
    ).decode()
    commits = [line.strip() for line in raw.splitlines() if line.strip()]
    if not commits:
        raise RewriteError("selected refs contain no commits")
    return commits


def object_type(root: Path, oid: str) -> str:
    return run_git(root, "cat-file", "-t", oid).decode().strip()


def object_bytes(root: Path, oid: str) -> bytes:
    return run_git(root, "cat-file", "-p", oid)


def parse_object(raw: bytes) -> tuple[list[tuple[bytes, bytes]], bytes]:
    headers_raw, separator, message = raw.partition(b"\n\n")
    if not separator:
        raise RewriteError("malformed Git object")
    headers: list[tuple[bytes, bytes]] = []
    for line in headers_raw.splitlines():
        if line.startswith(b" "):
            if not headers:
                raise RewriteError("orphaned continuation header")
            key, value = headers[-1]
            headers[-1] = (key, value + b"\n" + line)
            continue
        key, space, value = line.partition(b" ")
        if not space:
            raise RewriteError("malformed Git object header")
        headers.append((key, value))
    return headers, message


def header_values(headers: list[tuple[bytes, bytes]], key: bytes) -> list[bytes]:
    return [value for candidate, value in headers if candidate == key]


def identity_redactions(root: Path, commits: list[str]) -> list[re.Pattern]:
    raw = run_git(
        root,
        "show",
        "-s",
        "--format=%an%x00%cn%x00%ae%x00%ce",
        *commits,
    ).decode("utf-8", "replace")
    candidates: set[str] = set()
    for value in raw.replace("\n", "\0").split("\0"):
        clean = " ".join(value.strip().split())
        if not clean:
            continue
        if "@" in clean:
            local = clean.split("@", 1)[0]
            if len(local) >= 3:
                candidates.add(local)
        elif len(clean) >= 3:
            candidates.add(clean)
            for token in clean.split():
                if len(token) >= 4:
                    candidates.add(token)
    # Longest first prevents a first name from leaving the surname behind.
    return [
        re.compile(r"(?i)(?<![A-Za-z0-9])" + re.escape(value) +
                   r"(?![A-Za-z0-9])")
        for value in sorted(candidates, key=lambda item: (-len(item), item))
    ]


def summarize(message: bytes, redactions: list[re.Pattern]) -> str:
    text = message.decode("utf-8", "replace").replace("\r\n", "\n")
    text = TRAILER_RE.sub("", text)
    paragraphs = [
        " ".join(part.split())
        for part in re.split(r"\n\s*\n", text)
        if " ".join(part.split())
    ]
    candidate = paragraphs[0] if paragraphs else "Update repository state"
    sentinel = ""
    for codepoint in range(0xE000, 0xF900):
        proposed = chr(codepoint)
        if (
            proposed not in candidate
            and not any(pattern.search(proposed) for pattern in redactions)
        ):
            sentinel = proposed
            break
    if not sentinel:
        raise RewriteError("could not reserve a redaction sentinel")
    candidate = EMAIL_RE.sub(sentinel, candidate)
    candidate = HANDLE_RE.sub(sentinel, candidate)
    candidate = ATTRIBUTION_RE.sub(sentinel, candidate)
    for pattern in redactions:
        candidate = pattern.sub(sentinel, candidate)
    candidate = candidate.replace(sentinel, REDACTION_TOKEN)
    candidate = re.sub(r"\s+", " ", candidate).strip(" \t\r\n-:;,.")
    # Keep one sentence. The lookbehind lets common abbreviations and versions
    # survive while still dropping explanatory paragraphs from old messages.
    sentence = re.split(
        r"(?<=[!?])\s+|(?<=[a-z0-9\]\)])\.\s+",
        candidate,
        maxsplit=1,
    )[0]
    sentence = sentence.strip()
    if not sentence:
        sentence = "Update repository state"
    if len(sentence) > MAX_SUMMARY_CHARS - 1:
        clipped = sentence[: MAX_SUMMARY_CHARS - 2].rstrip()
        if " " in clipped:
            clipped = clipped.rsplit(" ", 1)[0]
        sentence = clipped.rstrip(" \t\r\n-:;,.")
    sentence = sentence[0].upper() + sentence[1:] if sentence else sentence
    if not sentence.endswith((".", "!", "?")):
        sentence += "."
    return sentence


def _write_private(path: Path, value: bytes) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL,
        0o600,
    )
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(value)
        stream.flush()
        os.fsync(stream.fileno())


def create_artifact_dir(root: Path) -> Path:
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    base = git_dir(root) / "forkmesh-history-rewrite"
    base.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(base, 0o700)
    candidate = base / stamp
    suffix = 0
    while candidate.exists():
        suffix += 1
        candidate = base / f"{stamp}-{suffix}"
    candidate.mkdir(mode=0o700)
    return candidate


def export_original_messages(
    root: Path,
    commits: list[str],
    artifact_dir: Path,
) -> None:
    records = []
    for oid in commits:
        headers, message = parse_object(object_bytes(root, oid))
        decoded = message.decode("utf-8", "replace")
        title, _, description = decoded.partition("\n")
        records.append(json.dumps({
            "commit": oid,
            "title": title,
            "description": description,
            "messageBase64": base64.b64encode(message).decode("ascii"),
            "authorHeaderBase64": base64.b64encode(
                header_values(headers, b"author")[0]
            ).decode("ascii"),
            "committerHeaderBase64": base64.b64encode(
                header_values(headers, b"committer")[0]
            ).decode("ascii"),
        }, ensure_ascii=False, sort_keys=True))
    _write_private(
        artifact_dir / "original-commit-messages.jsonl",
        ("\n".join(records) + "\n").encode("utf-8"),
    )


def create_backup(
    root: Path,
    refs: dict[str, str],
    artifact_dir: Path,
) -> dict[str, str]:
    bundle = artifact_dir / "pre-rewrite.bundle"
    run_git(root, "bundle", "create", str(bundle), "--all")
    run_git(root, "bundle", "verify", str(bundle))
    os.chmod(bundle, 0o600)
    run_name = artifact_dir.name
    backups: dict[str, str] = {}
    for ref, oid in refs.items():
        relative = ref.removeprefix("refs/")
        backup = f"refs/forkmesh-history-backup/{run_name}/{relative}"
        run_git(root, "update-ref", backup, oid, "0" * len(oid))
        backups[ref] = backup
    return backups


def rewrite_commits(
    root: Path,
    commits: list[str],
    redactions: list[re.Pattern],
) -> tuple[dict[str, str], dict[str, dict]]:
    mapping: dict[str, str] = {}
    evidence: dict[str, dict] = {}
    for old in commits:
        headers, message = parse_object(object_bytes(root, old))
        tree = header_values(headers, b"tree")
        parents = header_values(headers, b"parent")
        author = header_values(headers, b"author")
        committer = header_values(headers, b"committer")
        if len(tree) != 1 or len(author) != 1 or len(committer) != 1:
            raise RewriteError(f"{old}: required commit headers are malformed")
        mapped_parents = []
        for parent in parents:
            parent_text = parent.decode("ascii")
            if parent_text not in mapping:
                raise RewriteError(f"{old}: parent {parent_text} not rewritten")
            mapped_parents.append(mapping[parent_text].encode("ascii"))
        kept = [
            (key, value)
            for key, value in headers
            if key not in {
                b"tree", b"parent", b"author", b"committer",
                b"encoding", b"gpgsig", b"mergetag",
            }
        ]
        new_headers = (
            [(b"tree", tree[0])]
            + [(b"parent", value) for value in mapped_parents]
            + [(b"author", author[0]), (b"committer", committer[0])]
            + kept
        )
        summary = summarize(message, redactions)
        raw = (
            b"\n".join(key + b" " + value for key, value in new_headers)
            + b"\n\n"
            + summary.encode("utf-8")
            + b"\n"
        )
        new = run_git(
            root, "hash-object", "-t", "commit", "-w", "--stdin",
            input_bytes=raw,
        ).decode().strip()
        mapping[old] = new
        evidence[old] = {
            "new": new,
            "tree": tree[0].decode("ascii"),
            "parents": [value.decode("ascii") for value in parents],
            "mappedParents": [value.decode("ascii") for value in mapped_parents],
            "author": base64.b64encode(author[0]).decode("ascii"),
            "committer": base64.b64encode(committer[0]).decode("ascii"),
            "summary": summary,
        }
    return mapping, evidence


def strip_tag_signature(message: bytes) -> bytes:
    marker = b"-----BEGIN PGP SIGNATURE-----"
    return message.split(marker, 1)[0].rstrip() + b"\n"


def rewrite_tag_object(
    root: Path,
    oid: str,
    commit_mapping: dict[str, str],
    tag_mapping: dict[str, str],
) -> str:
    if oid in tag_mapping:
        return tag_mapping[oid]
    headers, message = parse_object(object_bytes(root, oid))
    targets = header_values(headers, b"object")
    types = header_values(headers, b"type")
    if len(targets) != 1 or len(types) != 1:
        raise RewriteError(f"{oid}: malformed annotated tag")
    target = targets[0].decode("ascii")
    target_type = types[0].decode("ascii")
    if target_type == "commit":
        replacement = commit_mapping.get(target)
    elif target_type == "tag":
        replacement = rewrite_tag_object(
            root, target, commit_mapping, tag_mapping)
    else:
        replacement = target
    if not replacement:
        raise RewriteError(f"{oid}: annotated tag target is outside scope")
    new_headers = [
        (key, replacement.encode("ascii") if key == b"object" else value)
        for key, value in headers
        if key != b"gpgsig"
    ]
    raw = (
        b"\n".join(key + b" " + value for key, value in new_headers)
        + b"\n\n"
        + strip_tag_signature(message)
    )
    new = run_git(
        root, "hash-object", "-t", "tag", "-w", "--stdin",
        input_bytes=raw,
    ).decode().strip()
    tag_mapping[oid] = new
    return new


def mapped_ref_targets(
    root: Path,
    refs: dict[str, str],
    commit_mapping: dict[str, str],
) -> dict[str, str]:
    result: dict[str, str] = {}
    tag_mapping: dict[str, str] = {}
    for ref, old in refs.items():
        kind = object_type(root, old)
        if kind == "commit":
            target = commit_mapping.get(old)
        elif kind == "tag":
            target = rewrite_tag_object(
                root, old, commit_mapping, tag_mapping)
        else:
            target = old
        if not target:
            raise RewriteError(f"{ref}: target is outside rewritten history")
        result[ref] = target
    return result


def update_refs(
    root: Path,
    old_refs: dict[str, str],
    new_refs: dict[str, str],
) -> None:
    commands = ["start"]
    for ref in sorted(old_refs):
        commands.append(f"update {ref} {new_refs[ref]} {old_refs[ref]}")
    commands.extend(["prepare", "commit", ""])
    run_git(
        root,
        "update-ref",
        "--stdin",
        input_bytes="\n".join(commands).encode("ascii"),
    )


def validate_mapping(
    root: Path,
    evidence: dict[str, dict],
    redactions: list[re.Pattern],
) -> None:
    for old, expected in evidence.items():
        new = expected["new"]
        headers, message = parse_object(object_bytes(root, new))
        summary = message.decode("utf-8", "strict").strip()
        redaction_check = summary.replace(REDACTION_TOKEN, "")
        if header_values(headers, b"tree") != [expected["tree"].encode()]:
            raise RewriteError(f"{old}: tree changed")
        parents = [
            value.decode("ascii") for value in header_values(headers, b"parent")
        ]
        if parents != expected["mappedParents"]:
            raise RewriteError(f"{old}: parent order/topology changed")
        if base64.b64encode(
            header_values(headers, b"author")[0]
        ).decode("ascii") != expected["author"]:
            raise RewriteError(f"{old}: author identity/timestamp changed")
        if base64.b64encode(
            header_values(headers, b"committer")[0]
        ).decode("ascii") != expected["committer"]:
            raise RewriteError(f"{old}: committer identity/timestamp changed")
        if (
            "\n" in summary
            or len(summary) > MAX_SUMMARY_CHARS
            or not summary.endswith((".", "!", "?"))
            or EMAIL_RE.search(summary)
            or HANDLE_RE.search(summary)
            or any(pattern.search(redaction_check) for pattern in redactions)
        ):
            raise RewriteError(f"{old}: summary policy failed: {summary!r}")


def scope_payload(
    root: Path,
    refs: dict[str, str],
    commits: list[str],
    backups: dict[str, str] | None = None,
    new_refs: dict[str, str] | None = None,
) -> dict:
    return {
        "schema": "forkmesh.commit-history-rewrite.v1",
        "repository": str(root.resolve()),
        "createdAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "scope": "local branches and tags explicitly listed below",
        "commitCount": len(commits),
        "refs": [
            {
                "ref": ref,
                "old": oid,
                "new": (new_refs or {}).get(ref, ""),
                "backupRef": (backups or {}).get(ref, ""),
            }
            for ref, oid in sorted(refs.items())
        ],
        "preserved": [
            "tree",
            "parent order and topology",
            "author identity and timestamp",
            "committer identity and timestamp",
        ],
        "invalidated": [
            "signed commit objects",
            "signed annotated tags",
            "old commit URLs and external references",
            "scan records pinned to old commit IDs",
        ],
        "pushPerformed": False,
    }


def write_json_private(path: Path, value: object) -> None:
    _write_private(
        path,
        (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8"),
    )


def plan(root: Path, requested: list[str]) -> int:
    refs = resolve_refs(root, requested)
    commits = commit_order(root, refs)
    payload = scope_payload(root, refs, commits)
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def rewrite(root: Path, requested: list[str], confirmation: str) -> int:
    if confirmation != REWRITE_CONFIRMATION:
        raise RewriteError(
            f"rewrite requires --confirm {REWRITE_CONFIRMATION}")
    for lock_name in ("index.lock", "packed-refs.lock", "HEAD.lock"):
        if (git_dir(root) / lock_name).exists():
            raise RewriteError(f"Git lock exists: {lock_name}")
    refs = resolve_refs(root, requested)
    commits = commit_order(root, refs)
    artifact_dir = create_artifact_dir(root)
    write_json_private(
        artifact_dir / "scope.pre-rewrite.json",
        scope_payload(root, refs, commits),
    )
    export_original_messages(root, commits, artifact_dir)
    backups = create_backup(root, refs, artifact_dir)
    redactions = identity_redactions(root, commits)
    mapping, evidence = rewrite_commits(root, commits, redactions)
    validate_mapping(root, evidence, redactions)
    new_refs = mapped_ref_targets(root, refs, mapping)
    write_json_private(
        artifact_dir / "old-to-new.json",
        {
            "schema": "forkmesh.commit-map.v1",
            "commits": mapping,
            "refs": new_refs,
        },
    )
    write_json_private(
        artifact_dir / "scope.ready.json",
        scope_payload(root, refs, commits, backups, new_refs),
    )
    update_refs(root, refs, new_refs)
    # Re-resolve every target after the atomic transaction.
    for ref, expected in new_refs.items():
        actual = run_git(root, "rev-parse", "--verify", ref).decode().strip()
        if actual != expected:
            raise RewriteError(f"{ref}: ref validation failed")
    write_json_private(
        artifact_dir / "scope.completed.json",
        {
            **scope_payload(root, refs, commits, backups, new_refs),
            "completedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
            "validation": "passed",
        },
    )
    print(str(artifact_dir))
    return 0


def restore(root: Path, artifact_dir: Path, confirmation: str) -> int:
    if confirmation != RESTORE_CONFIRMATION:
        raise RewriteError(
            f"restore requires --confirm {RESTORE_CONFIRMATION}")
    artifact_dir = artifact_dir.resolve()
    private_root = (git_dir(root) / "forkmesh-history-rewrite").resolve()
    if artifact_dir.parent != private_root:
        raise RewriteError("artifact directory is outside the private Git area")
    manifest_path = artifact_dir / "scope.completed.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RewriteError("completed rewrite manifest is unavailable") from exc
    old_refs = {
        item["ref"]: item["new"]
        for item in manifest.get("refs", [])
    }
    restored = {
        item["ref"]: item["old"]
        for item in manifest.get("refs", [])
    }
    if not old_refs or set(old_refs) != set(restored):
        raise RewriteError("restore manifest contains no complete ref set")
    update_refs(root, old_refs, restored)
    print("restored " + str(len(restored)) + " refs")
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument(
        "--repo", type=Path, default=Path.cwd(),
        help="repository root (default: current directory)",
    )
    subparsers = result.add_subparsers(dest="command", required=True)
    plan_parser = subparsers.add_parser("plan")
    plan_parser.add_argument("--ref", action="append", default=[])
    rewrite_parser = subparsers.add_parser("rewrite")
    rewrite_parser.add_argument("--ref", action="append", default=[])
    rewrite_parser.add_argument("--confirm", required=True)
    restore_parser = subparsers.add_parser("restore")
    restore_parser.add_argument("--artifact-dir", type=Path, required=True)
    restore_parser.add_argument("--confirm", required=True)
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    root = args.repo.resolve()
    try:
        run_git(root, "rev-parse", "--show-toplevel")
        if args.command == "plan":
            return plan(root, args.ref)
        if args.command == "rewrite":
            return rewrite(root, args.ref, args.confirm)
        if args.command == "restore":
            return restore(root, args.artifact_dir, args.confirm)
    except RewriteError as exc:
        print(f"history rewrite error: {exc}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
