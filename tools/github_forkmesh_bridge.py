#!/usr/bin/env python3
"""Connect a ForkMesh repository to a GitHub repository and keep them in sync.

Three pieces, all driven from a GitHub Actions workflow because ForkMesh
Actions run without egress:

* ``git-sync`` moves branches and tags both ways between GitHub and the
  repository's ForkMesh SSH remote. It only ever creates a missing ref or
  fast-forwards the side that is behind; it never forces, never deletes, and
  reports a diverged ref instead of choosing a winner.
* ``export`` converts GitHub issues, discussions, and pull requests into the
  repository-native ``.forkmesh`` records the desktop, relay, and mirrors read:
  ``.forkmesh/issues/{open,closed}/<n>/issue-<n>.json``,
  ``.forkmesh/discussions/<n>/`` and the ``forkmesh/pulls`` ledger branch.
  Every record is signed with a dedicated bridge Ed25519 key using the exact
  canonical strings of desktop/src/{IssueStore,DiscussionStore,PullStore}.cpp,
  so the records pass the same strict validation as natively authored ones.
  Output is deterministic (stable ids, GitHub timestamps, deterministic
  Ed25519), so an unchanged GitHub item rewrites nothing. Events a ForkMesh
  user adds to an imported item are kept; only ``github-*`` events are owned
  by the bridge.
* ``run`` chains both and commits/pushes the records to both sides; ``connect``
  scaffolds the configuration, a copy of this script, and the workflow into a
  checkout.

This file is intentionally self-contained (stdlib plus ``cryptography``) so
``connect`` can copy it into any repository.
"""

from __future__ import annotations

import argparse
import base64
from dataclasses import dataclass, field
from datetime import datetime
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from typing import Any, Callable, Iterable
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode
from urllib.request import Request, urlopen


API_BASE = "https://api.github.com"
API_VERSION = "2022-11-28"
CONFIG_REL = ".forkmesh/github-sync.json"
CONFIG_SCHEMA = "forkmesh-github-sync-v1"
PULLS_BRANCH = "forkmesh/pulls"
WORKFLOW_REL = ".github/workflows/forkmesh-github-sync.yml"
DEFAULT_SCRIPT_REL = ".forkmesh/github-bridge.py"
BRIDGE_ID_PREFIX = "github-"
SYNC_NAMESPACE = "refs/forkmesh-sync"
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
RETRYABLE_HTTP = frozenset({408, 425, 429, 500, 502, 503, 504})
MAX_PATCH_BYTES = 8 * 1024 * 1024
BRIDGE_GIT_NAME = "ForkMesh GitHub bridge"
BRIDGE_GIT_EMAIL = "github-bridge@forkmesh.invalid"


class BridgeError(RuntimeError):
    """A failure that is safe to print (never carries a credential)."""


# ---- GitHub API -------------------------------------------------------------


class GitHubClient:
    """Minimal REST + GraphQL client with pagination and bounded retry."""

    def __init__(
        self,
        token: str,
        *,
        base_url: str = API_BASE,
        opener: Callable[..., Any] = urlopen,
        sleeper: Callable[[float], None] = time.sleep,
    ) -> None:
        if not token:
            raise BridgeError("GitHub token is empty")
        self._token = token
        self.base_url = base_url.rstrip("/")
        self._opener = opener
        self._sleep = sleeper

    def __repr__(self) -> str:  # pragma: no cover - logging guard
        return "GitHubClient(token=<redacted>)"

    def _request(
        self, method: str, url: str, body: Any = None, accept: str | None = None
    ) -> tuple[bytes, dict[str, str]]:
        payload = None if body is None else json.dumps(body).encode("utf-8")
        headers = {
            "Accept": accept or "application/vnd.github+json",
            "Authorization": f"Bearer {self._token}",
            "X-GitHub-Api-Version": API_VERSION,
            "User-Agent": "forkmesh-github-bridge/1",
        }
        if payload is not None:
            headers["Content-Type"] = "application/json"
        for attempt in range(5):
            try:
                request = Request(url, data=payload, method=method, headers=headers)
                with self._opener(request, timeout=60) as response:
                    return response.read(), {
                        key.lower(): value for key, value in response.headers.items()
                    }
            except HTTPError as exc:
                limited = exc.code == 403 and exc.headers is not None and (
                    exc.headers.get("Retry-After") is not None
                    or exc.headers.get("X-RateLimit-Remaining") == "0"
                )
                if (exc.code in RETRYABLE_HTTP or limited) and attempt < 4:
                    retry_after = exc.headers.get("Retry-After") if exc.headers else None
                    try:
                        delay = min(60.0, max(0.0, float(retry_after)))
                    except (TypeError, ValueError):
                        delay = min(30.0, 1.5 * (2**attempt))
                    self._sleep(delay)
                    continue
                message = ""
                try:
                    message = str(json.loads(exc.read()).get("message") or "")
                except (ValueError, AttributeError):
                    pass
                suffix = f": {message}" if message else ""
                raise BridgeError(
                    f"GitHub API returned HTTP {exc.code}{suffix}".replace(
                        self._token, "<redacted>"
                    )
                ) from exc
            except URLError as exc:
                if attempt < 4:
                    self._sleep(min(30.0, 1.5 * (2**attempt)))
                    continue
                raise BridgeError(f"GitHub API connection failed: {exc.reason}") from exc
        raise BridgeError("GitHub API retry budget exhausted")

    def get(self, path: str, query: dict[str, Any] | None = None) -> Any:
        url = self.base_url + path
        if query:
            url += "?" + urlencode(query)
        raw, _ = self._request("GET", url)
        return json.loads(raw) if raw else None

    def paginate(
        self, path: str, query: dict[str, Any] | None = None
    ) -> Iterable[dict[str, Any]]:
        page = 1
        while True:
            params = dict(query or {})
            params.update({"per_page": 100, "page": page})
            raw, headers = self._request(
                "GET", self.base_url + path + "?" + urlencode(params)
            )
            items = json.loads(raw) if raw else []
            if not isinstance(items, list):
                raise BridgeError(f"GitHub API pagination expected a list at {path}")
            yield from (item for item in items if isinstance(item, dict))
            if len(items) < 100 or 'rel="next"' not in headers.get("link", ""):
                return
            page += 1

    def graphql(self, query: str, variables: dict[str, Any]) -> dict[str, Any]:
        raw, _ = self._request(
            "POST", self.base_url + "/graphql", {"query": query, "variables": variables}
        )
        decoded = json.loads(raw) if raw else {}
        if decoded.get("errors"):
            first = decoded["errors"][0]
            raise BridgeError(f"GitHub GraphQL error: {first.get('message', first)}")
        return decoded.get("data") or {}


def repo_path(repository: str) -> str:
    owner, name = repository.split("/", 1)
    return f"/repos/{quote(owner, safe='')}/{quote(name, safe='')}"


def validate_repository(value: str, label: str) -> str:
    repository = (value or "").strip()
    if not REPOSITORY_RE.fullmatch(repository):
        raise BridgeError(f"{label} must use the owner/name form")
    return repository


# ---- Signing (byte-for-byte with the desktop stores) -------------------------


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def _b64url_decode(text: str) -> bytes:
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


class BridgeSigner:
    """The bridge's own Ed25519 identity; its public key authors every record."""

    def __init__(self, private_key: Any) -> None:
        from cryptography.hazmat.primitives import serialization

        self._key = private_key
        self.public_key = b64url(
            private_key.public_key().public_bytes(
                serialization.Encoding.Raw, serialization.PublicFormat.Raw
            )
        )

    @classmethod
    def generate(cls) -> "BridgeSigner":
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

        return cls(Ed25519PrivateKey.generate())

    @classmethod
    def from_text(cls, text: str) -> "BridgeSigner":
        """Accept a PKCS#8 PEM key or a base64url 32-byte seed."""
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PrivateKey,
        )

        stripped = (text or "").strip()
        if not stripped:
            raise BridgeError("bridge signing key is empty")
        try:
            if stripped.startswith("-----BEGIN"):
                key = serialization.load_pem_private_key(
                    stripped.encode("ascii"), password=None
                )
            else:
                key = Ed25519PrivateKey.from_private_bytes(_b64url_decode(stripped))
        except (ValueError, TypeError) as exc:
            raise BridgeError("bridge signing key is not a valid Ed25519 key") from exc
        if not isinstance(key, Ed25519PrivateKey):
            raise BridgeError("bridge signing key must be Ed25519")
        return cls(key)

    def pem(self) -> str:
        from cryptography.hazmat.primitives import serialization

        return self._key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        ).decode("ascii")

    def sign(self, message: bytes) -> str:
        return b64url(self._key.sign(message))


def _event_canonical(domain: str, kind: str, number: int, author: str, ts: int,
                     content: str) -> bytes:
    digest = hashlib.sha256(content.encode("utf-8")).hexdigest()
    return f"{domain}\n{kind}\n{number}\n{author}\n{ts}\n{digest}".encode("utf-8")


def issue_event_content(ev: dict[str, Any]) -> str:
    """desktop/src/IssueStore.cpp::contentForSigning for the bridge's types."""
    nul = "\x00"
    kind = ev.get("type", "")
    attachments = ",".join(ev.get("attachments") or [])
    if kind == "open":
        return ev.get("title", "") + nul + ev.get("body", "") + nul + attachments
    if kind in ("comment", "edit"):
        return ev.get("body", "") + nul + attachments
    if kind == "title":
        return ev.get("title", "")
    if kind == "status":
        return ev.get("status", "")
    if kind == "labels":
        return ",".join(ev.get("labels") or [])
    if kind == "milestone":
        return ev.get("milestone", "")
    if kind == "assignees":
        return ",".join(ev.get("assignees") or [])
    raise BridgeError(f"the bridge does not sign issue events of type {kind!r}")


def sign_issue_event(signer: BridgeSigner, number: int, ev: dict[str, Any]) -> str:
    return signer.sign(_event_canonical(
        "forkmesh-issue-event-v1", ev["type"], number, ev["author"], ev["ts"],
        issue_event_content(ev)))


def discussion_event_content(ev: dict[str, Any]) -> str:
    if ev["type"] == "open":
        return ev["title"] + "\x00" + ev["body"] + "\x00" + ev["category"]
    return ev["body"]


def sign_discussion_event(signer: BridgeSigner, number: int,
                          ev: dict[str, Any]) -> str:
    return signer.sign(_event_canonical(
        "forkmesh-discussion-event-v1", ev["type"], number, ev["author"],
        ev["ts"], discussion_event_content(ev)))


def pull_event_content(ev: dict[str, Any]) -> str:
    nul = "\x00"
    kind = ev["type"]
    if kind == "comment":
        return ev["body"]
    if kind == "review":
        return ev["state"] + nul + ev["body"]
    if kind == "line-comment":
        return ev["path"] + nul + ev["side"] + nul + str(ev["line"]) + nul + ev["body"]
    raise BridgeError(f"the bridge does not sign pull events of type {kind!r}")


def sign_pull_event(signer: BridgeSigner, number: int, ev: dict[str, Any]) -> str:
    return signer.sign(_event_canonical(
        "forkmesh-pull-comment-v1", ev["type"], number, ev["author"], ev["ts"],
        pull_event_content(ev)))


def sign_pull(signer: BridgeSigner, title: str, base: str, head: str,
              patch: bytes, commits: bytes, ts: int) -> str:
    content = (title.encode() + b"\x00" + base.encode() + b"\x00" + head.encode()
               + b"\x00" + patch + b"\x00" + commits)
    canonical = (b"forkmesh-pull-event-v1\n" + signer.public_key.encode() + b"\n"
                 + str(ts).encode() + b"\n"
                 + hashlib.sha256(content).hexdigest().encode())
    return signer.sign(canonical)


# ---- Record text helpers ----------------------------------------------------


def iso_ms(value: Any) -> int:
    """GitHub ISO-8601 timestamp -> epoch milliseconds (0 when absent)."""
    if not value:
        return 0
    text = str(value).replace("Z", "+00:00")
    return int(datetime.fromisoformat(text).timestamp() * 1000)


def normalize_body(text: Any) -> str:
    """Line endings normalized and edge newlines stripped (stripEdgeNewlines)."""
    return str(text or "").replace("\r\n", "\n").replace("\r", "\n").strip("\n")


def single_line(text: Any) -> str:
    """Front matter values are one line; a newline would end the value."""
    return " ".join(str(text or "").split())


def github_name(user: Any) -> str:
    """Display credit for a GitHub account. The ``github:`` prefix can never
    collide with a ForkMesh account name, so imported text cannot pass itself
    off as a native user's."""
    login = user.get("login") if isinstance(user, dict) else None
    return f"github:{login or 'ghost'}"


def parse_front_matter(text: str) -> tuple[list[tuple[str, str]], str]:
    """Mirror of parseFrontMatter: ordered ``key: value`` pairs and the body."""
    lines = text.split("\n")
    if not lines or lines[0].strip() != "---":
        return [], text
    pairs: list[tuple[str, str]] = []
    index = 1
    while index < len(lines) and lines[index].strip() != "---":
        line = lines[index]
        sep = line.find(": ")
        if sep >= 0:
            pairs.append((line[:sep], line[sep + 2:]))
        elif line.endswith(":"):
            pairs.append((line[:-1], ""))
        index += 1
    return pairs, "\n".join(lines[index + 1:]).strip("\n")


def front_matter_text(pairs: Iterable[tuple[str, Any]], body: str) -> str:
    lines = ["---"] + [f"{key}: {value}" for key, value in pairs] + ["---", ""]
    return "\n".join(lines) + "\n" + body + "\n"


def write_if_changed(path: Path, data: str | bytes) -> bool:
    raw = data.encode("utf-8") if isinstance(data, str) else data
    if path.is_file() and path.read_bytes() == raw:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw)
    return True


def numeric_dirs(root: Path) -> dict[int, Path]:
    if not root.is_dir():
        return {}
    return {
        int(entry.name): entry
        for entry in root.iterdir()
        if entry.is_dir() and entry.name.isdigit()
    }


# ---- Connection config ------------------------------------------------------


def load_config(checkout: Path) -> dict[str, Any]:
    path = checkout / CONFIG_REL
    try:
        config = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        config = {}
    except ValueError as exc:
        raise BridgeError(f"{CONFIG_REL} is not valid JSON") from exc
    if not isinstance(config, dict):
        raise BridgeError(f"{CONFIG_REL} must hold a JSON object")
    config["schema"] = CONFIG_SCHEMA
    for kind in ("issues", "discussions", "pulls"):
        if not isinstance(config.get(kind), dict):
            config[kind] = {}
    return config


def config_text(config: dict[str, Any]) -> str:
    return json.dumps(config, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


@dataclass
class ExportReport:
    counts: dict[str, dict[str, int]] = field(default_factory=lambda: {
        kind: {"created": 0, "updated": 0, "unchanged": 0}
        for kind in ("issues", "discussions", "pulls")
    })
    warnings: list[str] = field(default_factory=list)

    def count(self, kind: str, action: str) -> None:
        self.counts[kind][action] += 1

    def markdown(self) -> str:
        rows = "\n".join(
            f"| {kind} | {c['created']} | {c['updated']} | {c['unchanged']} |"
            for kind, c in self.counts.items()
        )
        warnings = "".join(f"- {warning}\n" for warning in self.warnings)
        return (
            "### GitHub records → .forkmesh\n\n"
            "| Kind | Created | Updated | Unchanged |\n| --- | --- | --- | --- |\n"
            f"{rows}\n\n" + (f"Warnings:\n\n{warnings}" if warnings else "")
        )


# ---- Export: GitHub collaboration records -> .forkmesh ----------------------


DISCUSSIONS_QUERY = """
query($owner: String!, $name: String!, $after: String) {
  repository(owner: $owner, name: $name) {
    hasDiscussionsEnabled
    discussions(first: 25, after: $after, orderBy: {field: CREATED_AT, direction: ASC}) {
      pageInfo { hasNextPage endCursor }
      nodes {
        id number title body createdAt updatedAt closed
        author { login }
        category { name }
        comments(first: 100) {
          pageInfo { hasNextPage endCursor }
          nodes { ...BridgeComment replies(first: 100) { nodes { ...BridgeReply } } }
        }
      }
    }
  }
}
fragment BridgeComment on DiscussionComment { databaseId body createdAt author { login } }
fragment BridgeReply on DiscussionComment { databaseId body createdAt author { login } }
"""

DISCUSSION_COMMENTS_QUERY = """
query($id: ID!, $after: String) {
  node(id: $id) {
    ... on Discussion {
      comments(first: 100, after: $after) {
        pageInfo { hasNextPage endCursor }
        nodes { ...BridgeComment replies(first: 100) { nodes { ...BridgeReply } } }
      }
    }
  }
}
fragment BridgeComment on DiscussionComment { databaseId body createdAt author { login } }
fragment BridgeReply on DiscussionComment { databaseId body createdAt author { login } }
"""


class Exporter:
    def __init__(
        self,
        client: Any,
        signer: BridgeSigner,
        *,
        github_repository: str,
        checkout: Path,
        pulls_root: Path | None = None,
        git_dir: Path | None = None,
        github_remote: str = "origin",
        kinds: Iterable[str] = ("issues", "discussions", "pulls"),
        logger: Callable[[str], None] = print,
    ) -> None:
        self.client = client
        self.signer = signer
        self.repository = validate_repository(github_repository, "GitHub repository")
        self.checkout = Path(checkout)
        self.pulls_root = Path(pulls_root) if pulls_root else self.checkout
        self.git_dir = Path(git_dir) if git_dir else self.checkout
        self.github_remote = github_remote
        self.kinds = tuple(kinds)
        self.log = logger
        self.report = ExportReport()
        self.config = load_config(self.checkout)
        self.config["github"] = self.repository
        if self.config.get("signer") != signer.public_key:
            # A rotated key re-signs everything the bridge owns.
            for kind in ("issues", "discussions", "pulls"):
                for entry in self.config[kind].values():
                    entry.pop("updatedAt", None)
            self.config["signer"] = signer.public_key

    # -- shared --------------------------------------------------------------

    def run(self) -> ExportReport:
        if "issues" in self.kinds:
            self.export_issues()
        if "discussions" in self.kinds:
            self.export_discussions()
        if "pulls" in self.kinds:
            self.export_pulls()
        write_if_changed(self.checkout / CONFIG_REL, config_text(self.config))
        return self.report

    def _allocate(self, kind: str, gh_number: int, github_numbers: set[int],
                  existing: set[int]) -> int:
        mapping = self.config[kind]
        entry = mapping.get(str(gh_number))
        if isinstance(entry, dict) and isinstance(entry.get("number"), int):
            return entry["number"]
        claimed = {
            e["number"] for e in mapping.values()
            if isinstance(e, dict) and isinstance(e.get("number"), int)
        }
        if gh_number not in existing and gh_number not in claimed:
            return gh_number
        # A native ForkMesh record already owns the GitHub number. Allocate
        # past every GitHub number seen so the next GitHub item is unlikely to
        # need remapping too.
        number = max(existing | claimed | github_numbers | {0}) + 1
        while number in existing or number in claimed:
            number += 1
        self.report.warnings.append(
            f"{kind[:-1]} {self.repository}#{gh_number} is stored as #{number}: "
            f"#{gh_number} already belongs to a ForkMesh record"
        )
        return number

    def _unchanged(self, kind: str, gh_number: int, updated_at: str,
                   exists: bool) -> bool:
        entry = self.config[kind].get(str(gh_number))
        return bool(exists and isinstance(entry, dict)
                    and entry.get("updatedAt") == updated_at)

    def _remember(self, kind: str, gh_number: int, number: int,
                  updated_at: str) -> None:
        self.config[kind][str(gh_number)] = {"number": number, "updatedAt": updated_at}

    # -- issues --------------------------------------------------------------

    def _issue_event(self, number: int, kind: str, event_id: str, ts: int,
                     author_name: str, **fields: Any) -> dict[str, Any]:
        event = {"type": kind, "id": event_id, "author": self.signer.public_key,
                 "authorName": author_name, "ts": ts, **fields}
        event["sig"] = sign_issue_event(self.signer, number, event)
        return event

    def issue_record(self, number: int, gh: dict[str, Any],
                     comments: list[dict[str, Any]],
                     existing: dict[str, Any] | None) -> dict[str, Any]:
        gid = gh.get("id") or gh["number"]
        created = iso_ms(gh.get("created_at"))
        author_name = github_name(gh.get("user"))
        bridge = [self._issue_event(
            number, "open", f"github-issue-{gid}", created, author_name,
            title=single_line(gh.get("title")) or "(untitled issue)",
            body=normalize_body(gh.get("body")), attachments=[])]
        labels = [str(l.get("name") if isinstance(l, dict) else l)
                  for l in gh.get("labels") or []]
        if labels:
            bridge.append(self._issue_event(
                number, "labels", f"github-labels-{gid}", created + 1,
                author_name, labels=labels))
        milestone = gh.get("milestone")
        if isinstance(milestone, dict) and milestone.get("title"):
            bridge.append(self._issue_event(
                number, "milestone", f"github-milestone-{gid}", created + 2,
                author_name, milestone=single_line(milestone["title"])))
        assignees = [github_name(a) for a in gh.get("assignees") or []]
        if assignees:
            bridge.append(self._issue_event(
                number, "assignees", f"github-assignees-{gid}", created + 3,
                author_name, assignees=assignees))
        for comment in comments:
            bridge.append(self._issue_event(
                number, "comment", f"github-comment-{comment['id']}",
                iso_ms(comment.get("created_at")), github_name(comment.get("user")),
                body=normalize_body(comment.get("body")), attachments=[]))

        preserved = [
            ev for ev in (existing or {}).get("events", [])
            if isinstance(ev, dict) and ev.get("type") != "open"
            and not str(ev.get("id", "")).startswith(BRIDGE_ID_PREFIX)
        ]
        native_status = sorted(
            (ev for ev in preserved if ev.get("type") == "status"),
            key=lambda ev: int(ev.get("ts") or 0))
        if gh.get("state") == "closed":
            closed_at = iso_ms(gh.get("closed_at")) or iso_ms(gh.get("updated_at"))
            bridge.append(self._issue_event(
                number, "status", f"github-status-{gid}", closed_at,
                author_name, status="closed"))
        elif native_status and native_status[-1].get("status") == "closed":
            # GitHub is authoritative for an imported issue's state.
            reopened = max(iso_ms(gh.get("updated_at")),
                           int(native_status[-1].get("ts") or 0) + 1)
            bridge.append(self._issue_event(
                number, "status", f"github-status-{gid}", reopened,
                author_name, status="open"))

        opener = bridge[0]
        rest = sorted(bridge[1:] + preserved,
                      key=lambda ev: (int(ev.get("ts") or 0), str(ev.get("id"))))
        events = [opener] + rest

        title, status, milestone_title = opener["title"], "open", ""
        folded_labels: list[str] = []
        folded_assignees: list[str] = []
        votes = 0
        for ev in events:
            kind = ev.get("type")
            if kind == "title" and ev.get("title"):
                title = ev["title"]
            elif kind == "status":
                status = ev.get("status", status)
            elif kind == "labels":
                folded_labels = list(ev.get("labels") or [])
            elif kind == "milestone":
                milestone_title = ev.get("milestone", "")
            elif kind == "assignees":
                folded_assignees = list(ev.get("assignees") or [])
            elif kind == "vote" and ev.get("author"):
                votes += 1

        previous = existing or {}
        record: dict[str, Any] = {
            "schema": "forkmesh-issue-v1",
            "number": number,
            "title": title,
            "status": status,
            "labels": folded_labels,
            "milestone": milestone_title,
            "priority": previous.get("priority", 0),
            "progress": previous.get("progress", 0),
            "assignees": folded_assignees,
            "createdAt": created,
            "updatedAt": max(int(ev.get("ts") or 0) for ev in events),
            "author": self.signer.public_key,
            "authorName": author_name,
            "bountyUsd": previous.get("bountyUsd", 0.0),
            "bountyAddress": previous.get("bountyAddress", ""),
            "bountyStatus": previous.get("bountyStatus", ""),
            "votes": votes,
        }
        for key, value in previous.items():
            if key not in record and key != "events":
                record[key] = value
        record["events"] = events
        return record

    def export_issues(self) -> None:
        root = self.checkout / ".forkmesh" / "issues"
        items = [
            item for item in self.client.paginate(
                repo_path(self.repository) + "/issues",
                {"state": "all", "sort": "created", "direction": "asc"})
            if "pull_request" not in item
        ]
        github_numbers = {int(item["number"]) for item in items}
        dirs: dict[int, Path] = {}
        for side in (root, root / "open", root / "closed"):
            dirs.update(numeric_dirs(side))
        existing_numbers = set(dirs)
        labels_changed = milestones_changed = False
        labels_catalog = self._load_json_list(root / "labels.json")
        milestones_catalog = self._load_json_list(root / "milestones.json")

        for gh in items:
            gh_number = int(gh["number"])
            number = self._allocate("issues", gh_number, github_numbers, existing_numbers)
            existing_numbers.add(number)
            current = dirs.get(number)
            record_path = current / f"issue-{number}.json" if current else None
            if self._unchanged("issues", gh_number, str(gh.get("updated_at")),
                               bool(record_path and record_path.is_file())):
                self.report.count("issues", "unchanged")
                continue
            existing = None
            if record_path and record_path.is_file():
                try:
                    existing = json.loads(record_path.read_text(encoding="utf-8"))
                except ValueError:
                    self.report.warnings.append(
                        f"issue #{number} was unreadable and has been rebuilt")
            comments = list(self.client.paginate(
                repo_path(self.repository) + f"/issues/{gh_number}/comments"
            )) if gh.get("comments") else []
            record = self.issue_record(number, gh, comments, existing)

            side = "closed" if record["status"] == "closed" else "open"
            target = root / side / str(number)
            if current and current != target:
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(current), str(target))
            dirs[number] = target
            changed = write_if_changed(
                target / f"issue-{number}.json",
                json.dumps(record, indent=2, ensure_ascii=False) + "\n")
            action = "unchanged" if not changed and current == target else (
                "updated" if existing else "created")
            self.report.count("issues", action)
            self._remember("issues", gh_number, number, str(gh.get("updated_at")))

            for label in gh.get("labels") or []:
                if isinstance(label, dict) and label.get("name") and not any(
                        str(known.get("name", "")).casefold()
                        == str(label["name"]).casefold()
                        for known in labels_catalog):
                    labels_catalog.append({
                        "name": str(label["name"]),
                        "color": "#" + str(label.get("color") or "6f42c1").lstrip("#")[:6],
                    })
                    labels_changed = True
            milestone = gh.get("milestone")
            if isinstance(milestone, dict) and milestone.get("title") and not any(
                    known.get("title") == single_line(milestone["title"])
                    for known in milestones_catalog):
                milestones_catalog.append({
                    "description": normalize_body(milestone.get("description")),
                    "due": iso_ms(milestone.get("due_on")),
                    "status": "closed" if milestone.get("state") == "closed" else "open",
                    "title": single_line(milestone["title"]),
                })
                milestones_changed = True

        if labels_changed:
            write_if_changed(root / "labels.json",
                             json.dumps(labels_catalog, indent=2) + "\n")
        if milestones_changed:
            write_if_changed(root / "milestones.json",
                             json.dumps(milestones_catalog, indent=4, sort_keys=True) + "\n")

    @staticmethod
    def _load_json_list(path: Path) -> list[dict[str, Any]]:
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (FileNotFoundError, ValueError):
            return []
        return [item for item in value if isinstance(item, dict)] if isinstance(value, list) else []

    # -- event files shared by discussions and pulls ---------------------------

    @staticmethod
    def _sync_event_files(directory: Path, pattern: re.Pattern[str],
                          bridge: list[tuple[int, str, str, str]]) -> bool:
        """Rewrite numbered event files: bridge-owned entries are regenerated,
        native files are kept byte-for-byte, and all are ordered by timestamp
        from 0002 (the head record is conceptually event 1)."""
        entries = list(bridge)
        stale: set[str] = set()
        if directory.is_dir():
            for path in directory.iterdir():
                if not path.is_file() or not pattern.fullmatch(path.name):
                    continue
                stale.add(path.name)
                text = path.read_text(encoding="utf-8")
                values = dict(parse_front_matter(text)[0])
                if values.get("id", "").startswith(BRIDGE_ID_PREFIX):
                    continue
                try:
                    ts = int(values.get("ts") or 0)
                except ValueError:
                    ts = 0
                entries.append((ts, values.get("id", path.name),
                                values.get("type") or "comment", text))
        entries.sort(key=lambda entry: (entry[0], entry[1]))
        changed = False
        wanted: set[str] = set()
        for index, (_, _, kind, text) in enumerate(entries, start=2):
            name = f"{index:04d}-{kind}.md"
            wanted.add(name)
            changed |= write_if_changed(directory / name, text)
        for name in stale - wanted:
            (directory / name).unlink()
            changed = True
        return changed

    # -- discussions ---------------------------------------------------------

    def _discussion_comments(self, discussion: dict[str, Any]) -> list[dict[str, Any]]:
        connection = discussion.get("comments") or {}
        nodes = list(connection.get("nodes") or [])
        page = connection.get("pageInfo") or {}
        while page.get("hasNextPage"):
            data = self.client.graphql(DISCUSSION_COMMENTS_QUERY, {
                "id": discussion["id"], "after": page.get("endCursor")})
            connection = ((data.get("node") or {}).get("comments")) or {}
            nodes.extend(connection.get("nodes") or [])
            page = connection.get("pageInfo") or {}
        flat: list[dict[str, Any]] = []
        for comment in nodes:
            flat.append(comment)
            flat.extend((comment.get("replies") or {}).get("nodes") or [])
        return [c for c in flat if isinstance(c, dict) and c.get("databaseId")]

    def discussion_files(self, number: int, discussion: dict[str, Any],
                         comments: list[dict[str, Any]]) -> tuple[str, list[tuple[int, str, str, str]]]:
        pub = self.signer.public_key
        opener = {
            "type": "open",
            "number": number,
            "title": single_line(discussion.get("title")) or "(untitled discussion)",
            "category": single_line((discussion.get("category") or {}).get("name")) or "General",
            "body": normalize_body(discussion.get("body")),
            "author": pub,
            "ts": iso_ms(discussion.get("createdAt")),
        }
        sig = sign_discussion_event(self.signer, number, opener)
        head = front_matter_text([
            ("schema", "forkmesh-discussion-v1"),
            ("number", number),
            ("title", opener["title"]),
            ("category", opener["category"]),
            ("status", "closed" if discussion.get("closed") else "open"),
            ("createdAt", opener["ts"]),
            ("author", pub),
            ("authorName", github_name(discussion.get("author"))),
            ("type", "open"),
            ("id", f"github-discussion-{discussion.get('number')}"),
            ("ts", opener["ts"]),
            ("sig", sig),
        ], opener["body"])
        events = []
        for comment in comments:
            event = {"type": "comment", "author": pub,
                     "ts": iso_ms(comment.get("createdAt")),
                     "body": normalize_body(comment.get("body"))}
            event_id = f"github-discussion-comment-{comment['databaseId']}"
            text = front_matter_text([
                ("type", "comment"), ("id", event_id), ("author", pub),
                ("authorName", github_name(comment.get("author"))),
                ("ts", event["ts"]),
                ("sig", sign_discussion_event(self.signer, number, event)),
            ], event["body"])
            events.append((event["ts"], event_id, "comment", text))
        return head, events

    def export_discussions(self) -> None:
        owner, name = self.repository.split("/", 1)
        discussions: list[dict[str, Any]] = []
        after = None
        while True:
            data = self.client.graphql(DISCUSSIONS_QUERY,
                                       {"owner": owner, "name": name, "after": after})
            repository = data.get("repository") or {}
            if not repository.get("hasDiscussionsEnabled"):
                return
            connection = repository.get("discussions") or {}
            discussions.extend(n for n in connection.get("nodes") or [] if isinstance(n, dict))
            page = connection.get("pageInfo") or {}
            if not page.get("hasNextPage"):
                break
            after = page.get("endCursor")

        root = self.checkout / ".forkmesh" / "discussions"
        github_numbers = {int(d["number"]) for d in discussions}
        existing_numbers = set(numeric_dirs(root))
        comment_re = re.compile(r"\d{4}-comment\.md")
        for discussion in discussions:
            gh_number = int(discussion["number"])
            number = self._allocate("discussions", gh_number, github_numbers,
                                    existing_numbers)
            existing_numbers.add(number)
            directory = root / str(number)
            existed = (directory / "discussion.md").is_file()
            # Discussions arrive with their comments in the listing query, so
            # they are always regenerated and compared rather than skipped by
            # updatedAt (which does not move on every new comment).
            head, events = self.discussion_files(
                number, discussion, self._discussion_comments(discussion))
            changed = write_if_changed(directory / "discussion.md", head)
            changed |= self._sync_event_files(directory, comment_re, events)
            self.report.count("discussions", "unchanged" if not changed else (
                "updated" if existed else "created"))
            self._remember("discussions", gh_number, number,
                           str(discussion.get("updatedAt")))

    # -- pull requests -------------------------------------------------------

    def _git(self, *args: str, check: bool = True) -> subprocess.CompletedProcess[bytes]:
        result = subprocess.run(["git", "-C", str(self.git_dir), *args],
                                capture_output=True)
        if check and result.returncode != 0:
            raise BridgeError(
                f"git {args[0]} failed: "
                + result.stderr.decode("utf-8", "replace").strip()[:500])
        return result

    def pull_payload(self, gh_number: int, base_ref: str, base_sha: str,
                     head_sha: str) -> tuple[bytes, bytes]:
        """changes.patch / commits.mbox exactly as PullStore derives them."""
        have = lambda sha: self._git("cat-file", "-e", f"{sha}^{{commit}}",
                                     check=False).returncode == 0
        try:
            if not (have(base_sha) and have(head_sha)):
                self._git("fetch", "--no-tags", "--quiet", self.github_remote,
                          f"+refs/pull/{gh_number}/head:{SYNC_NAMESPACE}/pull/{gh_number}",
                          f"+refs/heads/{base_ref}:{SYNC_NAMESPACE}/pull-base/{gh_number}")
            patch = self._git("diff", "--binary", f"{base_sha}...{head_sha}").stdout
            commits = self._git("format-patch", "--binary", "--stdout",
                                f"{base_sha}..{head_sha}").stdout
        except BridgeError as exc:
            self.report.warnings.append(
                f"pull request #{gh_number}: patch unavailable ({exc})")
            return b"", b""
        if len(patch) > MAX_PATCH_BYTES or len(commits) > MAX_PATCH_BYTES:
            self.report.warnings.append(
                f"pull request #{gh_number}: patch exceeds {MAX_PATCH_BYTES} bytes "
                "and was not copied")
            return b"", b""
        return patch, commits

    def _pull_event(self, number: int, kind: str, event_id: str, ts: int,
                    user: Any, body: Any, **fields: Any) -> tuple[int, str, str, str]:
        event = {"type": kind, "author": self.signer.public_key, "ts": ts,
                 "body": normalize_body(body), **fields}
        pairs: list[tuple[str, Any]] = [
            ("type", kind), ("id", event_id), ("author", self.signer.public_key),
            ("authorName", github_name(user)), ("ts", ts)]
        if kind == "review":
            pairs.append(("state", fields["state"]))
        if kind == "line-comment":
            pairs += [("path", fields["path"]), ("side", fields["side"]),
                      ("line", fields["line"])]
        pairs.append(("sig", sign_pull_event(self.signer, number, event)))
        return ts, event_id, kind, front_matter_text(pairs, event["body"])

    def pull_events(self, number: int, comments: list[dict[str, Any]],
                    reviews: list[dict[str, Any]],
                    review_comments: list[dict[str, Any]]) -> list[tuple[int, str, str, str]]:
        events = [
            self._pull_event(number, "comment", f"github-comment-{c['id']}",
                             iso_ms(c.get("created_at")), c.get("user"), c.get("body"))
            for c in comments
        ]
        states = {"APPROVED": "approved", "CHANGES_REQUESTED": "changes_requested"}
        for review in reviews:
            state = str(review.get("state") or "")
            if state == "PENDING" or (state == "COMMENTED" and not normalize_body(review.get("body"))):
                continue
            events.append(self._pull_event(
                number, "review", f"github-review-{review['id']}",
                iso_ms(review.get("submitted_at")), review.get("user"),
                review.get("body"), state=states.get(state, "commented")))
        for comment in review_comments:
            events.append(self._pull_event(
                number, "line-comment", f"github-review-comment-{comment['id']}",
                iso_ms(comment.get("created_at")), comment.get("user"),
                comment.get("body"), path=single_line(comment.get("path")),
                side="old" if comment.get("side") == "LEFT" else "new",
                line=int(comment.get("line") or comment.get("original_line") or 0)))
        return events

    def export_pulls(self) -> None:
        base_path = repo_path(self.repository)
        pulls = list(self.client.paginate(
            base_path + "/pulls", {"state": "all", "sort": "created", "direction": "asc"}))
        root = self.pulls_root / ".forkmesh" / "pulls"
        github_numbers = {int(p["number"]) for p in pulls}
        existing_numbers = set(numeric_dirs(root))
        known_keys = {"schema", "number", "title", "base", "head", "status",
                      "mergeBase", "mergeHead", "ts", "author", "authorName", "sig"}
        event_re = re.compile(r"\d{4}-[a-z-]+\.md")
        for pr in pulls:
            gh_number = int(pr["number"])
            number = self._allocate("pulls", gh_number, github_numbers, existing_numbers)
            existing_numbers.add(number)
            directory = root / str(number)
            pull_md = directory / "pull.md"
            existed = pull_md.is_file()
            if self._unchanged("pulls", gh_number, str(pr.get("updated_at")), existed):
                self.report.count("pulls", "unchanged")
                continue
            base = pr.get("base") or {}
            head = pr.get("head") or {}
            head_repo = (head.get("repo") or {}).get("full_name") or ""
            head_name = (str(head.get("ref") or "")
                         if head_repo.casefold() == self.repository.casefold()
                         else str(head.get("label") or f"unknown:{head.get('ref', '')}"))
            status = ("merged" if pr.get("merged_at")
                      else "closed" if pr.get("state") == "closed" else "open")
            title = single_line(pr.get("title")) or "(untitled pull request)"
            base_name = single_line(base.get("ref"))
            head_name = single_line(head_name)
            patch = commits = b""
            if status == "open":
                patch, commits = self.pull_payload(
                    gh_number, base_name, str(base.get("sha")), str(head.get("sha")))
            ts = iso_ms(pr.get("created_at"))
            pairs: list[tuple[str, Any]] = [
                ("schema", "forkmesh-pull-v1"), ("number", number), ("title", title),
                ("base", base_name), ("head", head_name), ("status", status)]
            if status == "merged":
                pairs += [("mergeBase", base.get("sha")), ("mergeHead", head.get("sha"))]
            pairs += [
                ("ts", ts), ("author", self.signer.public_key),
                ("authorName", github_name(pr.get("user"))),
                ("sig", sign_pull(self.signer, title, base_name, head_name,
                                  patch, commits, ts))]
            if existed:
                previous = parse_front_matter(pull_md.read_text(encoding="utf-8"))[0]
                pairs += [(k, v) for k, v in previous if k not in known_keys]
            changed = write_if_changed(
                pull_md, front_matter_text(pairs, normalize_body(pr.get("body"))))
            for name, payload in (("changes.patch", patch), ("commits.mbox", commits)):
                path = directory / name
                if status == "open" and (payload or name == "changes.patch"):
                    changed |= write_if_changed(path, payload)
                elif path.exists():
                    path.unlink()
                    changed = True
            events = self.pull_events(
                number,
                list(self.client.paginate(base_path + f"/issues/{gh_number}/comments")),
                list(self.client.paginate(base_path + f"/pulls/{gh_number}/reviews")),
                list(self.client.paginate(base_path + f"/pulls/{gh_number}/comments")))
            changed |= self._sync_event_files(directory, event_re, events)
            self.report.count("pulls", "unchanged" if not changed else (
                "updated" if existed else "created"))
            self._remember("pulls", gh_number, number, str(pr.get("updated_at")))


# ---- Two-way ref sync -------------------------------------------------------


def git(repo: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True,
                            text=True, encoding="utf-8", errors="replace")
    if check and result.returncode != 0:
        raise BridgeError(f"git {args[0]} failed: {result.stderr.strip()[:500]}")
    return result


@dataclass
class RefSyncReport:
    github: dict[str, str] = field(default_factory=dict)
    forkmesh: dict[str, str] = field(default_factory=dict)
    to_github: list[dict[str, str]] = field(default_factory=list)
    to_forkmesh: list[dict[str, str]] = field(default_factory=list)
    diverged: list[str] = field(default_factory=list)
    rejected: list[str] = field(default_factory=list)
    unchanged: int = 0

    def tip(self, ref: str) -> str | None:
        """The ref's value after the sync (both sides agree unless diverged)."""
        return self.github.get(ref) or self.forkmesh.get(ref)

    def as_dict(self) -> dict[str, Any]:
        return {"toGitHub": self.to_github, "toForkMesh": self.to_forkmesh,
                "diverged": self.diverged, "rejected": self.rejected,
                "unchanged": self.unchanged}

    def markdown(self) -> str:
        lines = ["### Git refs", "",
                 f"- GitHub → ForkMesh: {len(self.to_forkmesh)}",
                 f"- ForkMesh → GitHub: {len(self.to_github)}",
                 f"- Unchanged: {self.unchanged}"]
        lines += [f"- ⚠️ Diverged (not synced): `{ref}`" for ref in self.diverged]
        lines += [f"- ⚠️ Rejected push: `{ref}`" for ref in self.rejected]
        return "\n".join(lines) + "\n"


def _fetch_refs(repo: Path, remote: str, side: str) -> dict[str, str]:
    namespace = f"{SYNC_NAMESPACE}/{side}"
    git(repo, "fetch", "--prune", "--no-tags", "--quiet", remote,
        f"+refs/heads/*:{namespace}/heads/*", f"+refs/tags/*:{namespace}/tags/*")
    listing = git(repo, "for-each-ref", "--format=%(refname) %(objectname)",
                  f"{namespace}/").stdout
    refs = {}
    for line in listing.splitlines():
        name, _, oid = line.partition(" ")
        refs["refs/" + name[len(namespace) + 1:]] = oid
    return refs


def _push(repo: Path, remote: str, updates: list[tuple[str, str]]) -> list[str]:
    """Plain (never forced) push; returns the refs the remote rejected."""
    if not updates:
        return []
    result = git(repo, "push", "--porcelain", remote,
                 *[f"{oid}:{ref}" for oid, ref in updates], check=False)
    rejected = [
        line.split("\t")[1].split(":", 1)[1]
        for line in result.stdout.splitlines()
        if line.startswith("!\t") and ":" in line.split("\t")[1]
    ]
    if result.returncode != 0 and not rejected:
        raise BridgeError(f"git push to {'the ForkMesh remote' if remote != 'origin' else remote} "
                          f"failed: {result.stderr.strip()[:500]}")
    return rejected


def sync_refs(repo: Path, github_remote: str, forkmesh_remote: str, *,
              exclude: Iterable[str] = (), dry_run: bool = False) -> RefSyncReport:
    repo = Path(repo)
    report = RefSyncReport(github=_fetch_refs(repo, github_remote, "github"),
                           forkmesh=_fetch_refs(repo, forkmesh_remote, "forkmesh"))
    patterns = list(exclude)
    to_github: list[tuple[str, str]] = []
    to_forkmesh: list[tuple[str, str]] = []
    for ref in sorted(set(report.github) | set(report.forkmesh)):
        if any(fnmatch.fnmatchcase(ref, p) or fnmatch.fnmatchcase(ref.split("/", 2)[-1], p)
               for p in patterns):
            continue
        g, f = report.github.get(ref), report.forkmesh.get(ref)
        if g == f:
            report.unchanged += 1
        elif g is None:
            to_github.append((f, ref))
        elif f is None:
            to_forkmesh.append((g, ref))
        elif not ref.startswith("refs/tags/") and git(
                repo, "merge-base", "--is-ancestor", g, f, check=False).returncode == 0:
            to_github.append((f, ref))
        elif not ref.startswith("refs/tags/") and git(
                repo, "merge-base", "--is-ancestor", f, g, check=False).returncode == 0:
            to_forkmesh.append((g, ref))
        else:
            report.diverged.append(ref)

    for remote, updates, target, entries in (
            (github_remote, to_github, report.github, report.to_github),
            (forkmesh_remote, to_forkmesh, report.forkmesh, report.to_forkmesh)):
        rejected = [] if dry_run else _push(repo, remote, updates)
        report.rejected.extend(rejected)
        for oid, ref in updates:
            if ref in rejected:
                continue
            entries.append({"ref": ref, "oid": oid})
            target[ref] = oid
    return report


# ---- run: refs, records, commit, publish ------------------------------------


def _commit_records(worktree: Path, paths: list[str], message: str) -> str | None:
    git(worktree, "add", "-A", "--", *paths)
    if git(worktree, "diff", "--cached", "--quiet", check=False).returncode == 0:
        return None
    git(worktree, "-c", f"user.name={BRIDGE_GIT_NAME}", "-c",
        f"user.email={BRIDGE_GIT_EMAIL}", "commit", "--quiet", "--no-verify",
        "-m", message)
    return git(worktree, "rev-parse", "HEAD").stdout.strip()


def run_sync(*, repo: Path, github_repository: str, forkmesh_remote: str,
             client: Any, signer: BridgeSigner, github_remote: str = "origin",
             default_branch: str | None = None, dry_run: bool = False,
             logger: Callable[[str], None] = print) -> dict[str, Any]:
    repo = Path(repo).resolve()
    github_repository = validate_repository(github_repository, "GitHub repository")
    refs = sync_refs(repo, github_remote, forkmesh_remote, dry_run=dry_run)
    if not default_branch:
        default_branch = str(client.get(repo_path(github_repository))["default_branch"])
    default_ref = f"refs/heads/{default_branch}"
    if default_ref in refs.diverged:
        raise BridgeError(
            f"{default_branch} has diverged between GitHub and ForkMesh; merge one "
            "side into the other and the next run will import records again")
    tip = refs.tip(default_ref)
    if not tip:
        raise BridgeError(f"neither side has the default branch {default_branch}")

    pulls_ref = f"refs/heads/{PULLS_BRANCH}"
    kinds = ["issues", "discussions", "pulls"]
    warnings: list[str] = []
    if pulls_ref in refs.diverged:
        kinds.remove("pulls")
        warnings.append(f"{PULLS_BRANCH} has diverged; pull requests were not imported")

    git(repo, "checkout", "--quiet", "-B", default_branch, tip)
    pulls_tree = repo / ".git" / "forkmesh-bridge-pulls"
    git(repo, "worktree", "remove", "--force", str(pulls_tree), check=False)
    shutil.rmtree(pulls_tree, ignore_errors=True)
    git(repo, "worktree", "prune")
    pulls_tip = refs.tip(pulls_ref) or tip
    git(repo, "worktree", "add", "--quiet", "--force", "-B", "forkmesh-bridge/pulls",
        str(pulls_tree), pulls_tip)
    try:
        exporter = Exporter(client, signer, github_repository=github_repository,
                            checkout=repo, pulls_root=pulls_tree, git_dir=repo,
                            github_remote=github_remote, kinds=kinds, logger=logger)
        export_report = exporter.run()
        export_report.warnings[:0] = warnings
        published: dict[str, str] = {}
        message = "github-sync: import GitHub issues, discussions and pull requests"
        for worktree, ref, paths in (
                (repo, default_ref, [".forkmesh/issues", ".forkmesh/discussions", CONFIG_REL]),
                (pulls_tree, pulls_ref, [".forkmesh/pulls"])):
            present = [p for p in paths if (worktree / p).exists()]
            if not present:
                continue
            commit = _commit_records(worktree, present, message)
            if not commit or dry_run:
                continue
            for remote in (github_remote, forkmesh_remote):
                if _push(repo, remote, [(commit, ref)]):
                    raise BridgeError(
                        f"{ref} moved on {'GitHub' if remote == github_remote else 'ForkMesh'} "
                        "while records were imported; the next run retries")
            published[ref] = commit
    finally:
        git(repo, "worktree", "remove", "--force", str(pulls_tree), check=False)
    return {"schemaVersion": 1, "githubRepository": github_repository,
            "dryRun": dry_run, "refs": refs.as_dict(),
            "records": {"counts": export_report.counts,
                        "warnings": export_report.warnings},
            "published": published,
            "summary": "## ForkMesh ⇄ GitHub sync\n\n" + refs.markdown() + "\n"
                       + export_report.markdown()}


# ---- connect: scaffold a checkout -------------------------------------------


def render_workflow(script_rel: str = DEFAULT_SCRIPT_REL) -> str:
    return f"""# Generated by `github_forkmesh_bridge.py connect`; see
# www/docs/github-sync.md in the ForkMesh repository.
#
# Keeps this GitHub repository and its ForkMesh repository in sync: branches
# and tags move both ways (create / fast-forward only, never forced), and
# GitHub issues, discussions and pull requests are converted into signed
# .forkmesh records. Nothing runs until FORKMESH_SYNC_REMOTE is set.
name: ForkMesh GitHub sync

on:
  push:
  issues:
  issue_comment:
  discussion:
  discussion_comment:
  # The base-repository context is required so fork pull requests can be
  # recorded. The job never checks out or executes pull request code: the
  # head is only fetched as data for `git diff` / `git format-patch`.
  pull_request_target:
    types: [opened, edited, closed, reopened, synchronize, labeled, unlabeled]
  pull_request_review:
  pull_request_review_comment:
  # ForkMesh cannot call GitHub, so ForkMesh-side changes arrive by polling.
  schedule:
    - cron: "*/15 * * * *"
  workflow_dispatch:
    inputs:
      dry_run:
        description: "Report what would change without pushing"
        required: true
        default: false
        type: boolean

permissions:
  contents: write
  issues: read
  pull-requests: read
  discussions: read

concurrency:
  group: forkmesh-github-sync
  cancel-in-progress: false

jobs:
  sync:
    # Review events from forks run without secrets; the next scheduled run
    # imports them instead.
    if: >-
      vars.FORKMESH_SYNC_REMOTE != '' &&
      (!startsWith(github.event_name, 'pull_request_review') ||
       github.event.pull_request.head.repo.full_name == github.repository)
    runs-on: ubuntu-latest
    timeout-minutes: 30
    steps:
      - name: Check out the default branch
        uses: actions/checkout@de0fac2e4500dabe0009e67214ff5f5447ce83dd # v6.0.2
        with:
          fetch-depth: 0
          ref: ${{{{ github.event.repository.default_branch }}}}

      - name: Set up Python
        uses: actions/setup-python@a309ff8b426b58ec0e2a45f0f869d46889d02405 # v6.2.0
        with:
          python-version: "3.12"

      - name: Install the signing dependency
        run: python -m pip install --disable-pip-version-check --quiet "cryptography==44.0.0"

      - name: Configure ForkMesh SSH access
        shell: bash
        env:
          SSH_KEY: ${{{{ secrets.FORKMESH_SYNC_SSH_KEY }}}}
          KNOWN_HOSTS: ${{{{ vars.FORKMESH_SYNC_KNOWN_HOSTS }}}}
        run: |
          set -euo pipefail
          if [ -z "$SSH_KEY" ]; then
            echo "::error::Set the FORKMESH_SYNC_SSH_KEY secret to an SSH key with write access to the ForkMesh repository."
            exit 1
          fi
          install -d -m 700 "$HOME/.ssh"
          printf '%s\\n' "$SSH_KEY" > "$HOME/.ssh/forkmesh_sync"
          chmod 600 "$HOME/.ssh/forkmesh_sync"
          strict=accept-new
          if [ -n "$KNOWN_HOSTS" ]; then
            printf '%s\\n' "$KNOWN_HOSTS" >> "$HOME/.ssh/known_hosts"
            strict=yes
          fi
          echo "GIT_SSH_COMMAND=ssh -i $HOME/.ssh/forkmesh_sync -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=$strict" >> "$GITHUB_ENV"

      - name: Sync refs, issues, discussions and pull requests
        shell: bash
        env:
          GITHUB_TOKEN: ${{{{ github.token }}}}
          FORKMESH_BRIDGE_SIGNING_KEY: ${{{{ secrets.FORKMESH_BRIDGE_SIGNING_KEY }}}}
          FORKMESH_SYNC_REMOTE: ${{{{ vars.FORKMESH_SYNC_REMOTE }}}}
          DEFAULT_BRANCH: ${{{{ github.event.repository.default_branch }}}}
          DRY_RUN: ${{{{ inputs.dry_run }}}}
        run: |
          set -euo pipefail
          dry_run=()
          if [ "${{DRY_RUN:-false}}" = "true" ]; then
            dry_run=(--dry-run)
          fi
          python {script_rel} run \\
            --repo . \\
            --github "$GITHUB_REPOSITORY" \\
            --default-branch "$DEFAULT_BRANCH" \\
            --report artifacts/forkmesh-github-sync.json \\
            --summary "$GITHUB_STEP_SUMMARY" \\
            "${{dry_run[@]}}"
"""


def connect(*, repo: Path, github_repository: str,
            forkmesh_repository: str | None = None,
            forkmesh_remote: str | None = None,
            script_rel: str = DEFAULT_SCRIPT_REL,
            key_out: Path | None = None,
            add_remotes: bool = True) -> dict[str, Any]:
    repo = Path(repo).resolve()
    github_repository = validate_repository(github_repository, "GitHub repository")
    forkmesh_repository = validate_repository(
        forkmesh_repository or github_repository, "ForkMesh repository")
    forkmesh_remote = forkmesh_remote or f"ssh://git@ssh.forkmesh.com/{forkmesh_repository}.git"
    if not re.fullmatch(r"[A-Za-z0-9_./-]+\.py", script_rel) or ".." in script_rel.split("/"):
        raise BridgeError("the script path must be a relative .py path inside the repository")

    config = load_config(repo)
    config.update({"github": github_repository, "forkmesh": forkmesh_repository,
                   "forkmeshRemote": forkmesh_remote})
    write_if_changed(repo / CONFIG_REL, config_text(config))

    source = Path(__file__).resolve()
    script = repo / script_rel
    if script.resolve() != source:
        write_if_changed(script, source.read_bytes())
        script.chmod(0o755)
    write_if_changed(repo / WORKFLOW_REL, render_workflow(script_rel))

    if add_remotes:
        existing = git(repo, "remote", check=False).stdout.split()
        urls = {git(repo, "remote", "get-url", name, check=False).stdout.strip()
                for name in existing}
        for name, url in (("github", f"https://github.com/{github_repository}.git"),
                          ("forkmesh", forkmesh_remote)):
            if name not in existing and url not in urls:
                git(repo, "remote", "add", name, url)

    public_key = None
    if key_out:
        key_out = Path(key_out).expanduser()
        if key_out.exists():
            signer = BridgeSigner.from_text(key_out.read_text(encoding="ascii"))
        else:
            signer = BridgeSigner.generate()
            key_out.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(key_out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w", encoding="ascii") as handle:
                handle.write(signer.pem())
        public_key = signer.public_key

    key_hint = str(key_out) if key_out else "<bridge-key.pem>"
    return {
        "config": CONFIG_REL, "script": script_rel, "workflow": WORKFLOW_REL,
        "forkmeshRemote": forkmesh_remote, "keyFile": str(key_out) if key_out else None,
        "bridgePublicKey": public_key,
        "nextSteps": [
            f"gh secret set FORKMESH_BRIDGE_SIGNING_KEY --repo {github_repository} < {key_hint}",
            f"gh secret set FORKMESH_SYNC_SSH_KEY --repo {github_repository} < <ssh-private-key>",
            f"gh variable set FORKMESH_SYNC_REMOTE --repo {github_repository} --body {forkmesh_remote}",
            "Register the matching SSH public key on ForkMesh with write access to "
            f"{forkmesh_repository}.",
            f"git add {CONFIG_REL} {script_rel} {WORKFLOW_REL} && git commit -m "
            "'Connect to GitHub' and push the commit to both remotes.",
        ],
    }


# ---- CLI --------------------------------------------------------------------


def _env(name: str, label: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise BridgeError(f"{label} environment variable {name} is unset")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def token_args(p: argparse.ArgumentParser) -> None:
        p.add_argument("--token-env", default="GITHUB_TOKEN")
        p.add_argument("--key-env", default="FORKMESH_BRIDGE_SIGNING_KEY")

    def output_args(p: argparse.ArgumentParser) -> None:
        p.add_argument("--report", type=Path)
        p.add_argument("--summary", type=Path)

    refs = sub.add_parser("git-sync", help="two-way create/fast-forward ref sync")
    refs.add_argument("--repo", type=Path, default=Path("."))
    refs.add_argument("--github-remote", default="origin")
    refs.add_argument("--forkmesh-remote", default=os.environ.get("FORKMESH_SYNC_REMOTE", ""))
    refs.add_argument("--exclude", action="append", default=[])
    refs.add_argument("--dry-run", action="store_true")
    refs.add_argument("--fail-on-divergence", action="store_true")
    output_args(refs)

    export = sub.add_parser("export", help="write GitHub records as .forkmesh records")
    export.add_argument("--repo", type=Path, default=Path("."))
    export.add_argument("--github", required=True)
    export.add_argument("--pulls-root", type=Path)
    export.add_argument("--github-remote", default="origin")
    export.add_argument("--kinds", default="issues,discussions,pulls")
    token_args(export)
    output_args(export)

    run = sub.add_parser("run", help="git-sync, export, commit and publish")
    run.add_argument("--repo", type=Path, default=Path("."))
    run.add_argument("--github", required=True)
    run.add_argument("--github-remote", default="origin")
    run.add_argument("--forkmesh-remote", default=os.environ.get("FORKMESH_SYNC_REMOTE", ""))
    run.add_argument("--default-branch", default="")
    run.add_argument("--dry-run", action="store_true")
    token_args(run)
    output_args(run)

    conn = sub.add_parser("connect", help="scaffold the GitHub connection into a checkout")
    conn.add_argument("--repo", type=Path, default=Path("."))
    conn.add_argument("--github", required=True)
    conn.add_argument("--forkmesh")
    conn.add_argument("--forkmesh-remote")
    conn.add_argument("--script-path", default=DEFAULT_SCRIPT_REL)
    conn.add_argument("--key-out", type=Path)
    conn.add_argument("--no-remotes", action="store_true")

    render = sub.add_parser("render-workflow", help="print the GitHub Actions workflow")
    render.add_argument("--script-path", default=DEFAULT_SCRIPT_REL)
    return parser


def _emit(result: dict[str, Any], markdown: str, report: Path | None,
          summary: Path | None) -> None:
    if report:
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8")
    if summary:
        with summary.open("a", encoding="utf-8") as handle:
            handle.write(markdown)
    print(markdown)


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    secrets: list[str] = []
    try:
        if args.command == "render-workflow":
            sys.stdout.write(render_workflow(args.script_path))
            return 0
        if args.command == "connect":
            result = connect(repo=args.repo, github_repository=args.github,
                             forkmesh_repository=args.forkmesh,
                             forkmesh_remote=args.forkmesh_remote,
                             script_rel=args.script_path, key_out=args.key_out,
                             add_remotes=not args.no_remotes)
            print(json.dumps(result, indent=2))
            return 0
        if args.command == "git-sync":
            if not args.forkmesh_remote:
                raise BridgeError("--forkmesh-remote (or FORKMESH_SYNC_REMOTE) is required")
            refs = sync_refs(args.repo, args.github_remote, args.forkmesh_remote,
                             exclude=args.exclude, dry_run=args.dry_run)
            _emit(refs.as_dict(), refs.markdown(), args.report, args.summary)
            return 1 if (refs.rejected or (args.fail_on_divergence and refs.diverged)) else 0

        token = _env(args.token_env, "GitHub token")
        key = _env(args.key_env, "bridge signing key")
        secrets += [token, key]
        client = GitHubClient(token)
        signer = BridgeSigner.from_text(key)
        if args.command == "export":
            exporter = Exporter(client, signer, github_repository=args.github,
                                checkout=args.repo, pulls_root=args.pulls_root,
                                github_remote=args.github_remote,
                                kinds=[k for k in args.kinds.split(",") if k])
            records = exporter.run()
            _emit({"counts": records.counts, "warnings": records.warnings},
                  records.markdown(), args.report, args.summary)
            return 0
        if not args.forkmesh_remote:
            raise BridgeError("--forkmesh-remote (or FORKMESH_SYNC_REMOTE) is required")
        result = run_sync(repo=args.repo, github_repository=args.github,
                          forkmesh_remote=args.forkmesh_remote, client=client,
                          signer=signer, github_remote=args.github_remote,
                          default_branch=args.default_branch or None,
                          dry_run=args.dry_run)
        summary = result.pop("summary")
        _emit(result, summary, args.report, args.summary)
        return 1 if result["refs"]["rejected"] else 0
    except (BridgeError, OSError) as exc:
        message = str(exc)
        for secret in secrets:
            message = message.replace(secret, "<redacted>")
        print(f"github bridge failed: {message}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
