#!/usr/bin/env python3
"""Continuous two-way issue synchronization between ForkMesh and a forge.

``github_forkmesh_bridge.py`` imports GitHub records one way. This engine keeps
a ForkMesh repository's ``.forkmesh/issues`` records and a forge's issues and
comments converged in both directions, through the provider contract in
``forge_providers.py`` (GitHub and Gitea/Forgejo are certified).

Safety rules, all enforced here and pinned by ``app/tests/test_forge_sync.py``:

* **Three-way merge per field.** ``.forkmesh/forge-sync.json`` records the
  digest each field (title, body, state, labels, assignees) and each comment
  had when both sides last agreed. Only the side that moved is copied; when
  both moved, the field becomes a visible conflict and neither side is
  written, unless the repository published a source-of-truth rule for that
  field, in which case the discarded value is kept in the ledger history.
* **Loop prevention.** A write records the written value as the new base, so
  its echo compares equal and writes nothing. Records the engine creates on
  the forge carry a ``forkmesh-forge-sync:v1`` marker, and records it creates
  in ForkMesh use ``forge-<provider>-`` event ids, so a lost or rolled-back
  ledger re-links instead of duplicating.
* **Deletions** are signed ``delete`` events in ForkMesh and comment deletions
  on the forge; a deletion that races an edit on the other side is a conflict.
* **Identities** from the forge are always shown as ``<provider>:<login>``
  and signed by the sync key, so they can never pass as a ForkMesh account.
  Owner-approved links only translate assignees, and ambiguous links are
  ignored and reported.
* **Failures.** Rate limits and outages are retried with backoff; when retries
  run out the run stops, keeps its progress, and reports a retryable error.
  ``pause`` stops all provider traffic.

Everything the engine writes is deterministic, so repeated runs over
unchanged state write nothing on either side.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import sys
import time
from typing import Any, Callable


_TOOLS = Path(__file__).resolve().parent


def _load(name: str, path: Path) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:  # pragma: no cover
        raise RuntimeError(f"{path.name} is unavailable")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


providers = _load("forkmesh_forge_providers", _TOOLS / "forge_providers.py")
bridge = _load("forkmesh_forge_bridge", _TOOLS / "github_forkmesh_bridge.py")
sync_status = _load("forkmesh_forge_sync_status", _TOOLS.parent / "app" / "src" / "sync_status.py")

LEDGER_REL = ".forkmesh/forge-sync.json"
LEDGER_ENV = "FORKMESH_FORGE_LEDGER"
SOURCE_REL = ".forkmesh/forge-source.json"
SOURCE_SCHEMA = "forkmesh-forge-source-v1"
ISSUE_URL_TEMPLATES = {"github": "https://github.com/{repository}/issues/{number}",
                       "gitea": "{base}/{repository}/issues/{number}"}
SCHEMA = "forkmesh-forge-sync-v1"
REPORT_SCHEMA = "forkmesh-forge-sync-report-v1"
DEFAULT_SLO_MS = 15 * 60 * 1000
FIELDS = ("title", "body", "state", "labels", "assignees")
RULES = ("manual", "forkmesh", "provider")
MODES = ("sync", "mirror")
PROVIDER_WRITES = ("create_issue", "update_issue", "create_comment", "update_comment",
                   "delete_comment")
SIDES = ("forkmesh", "provider")
FOREIGN_EVENT_PREFIXES = ("forge-", "github-")
HISTORY_LIMIT = 100
MARKER_RE = re.compile(
    r"\n*(?:<sub>Synced from ForkMesh[^\n]*</sub>\n)?"
    r"<!--\s*forkmesh-forge-sync:v1\s+(\{[^\r\n]*\})\s*-->\s*$")


class SyncError(RuntimeError):
    """A configuration or usage failure that is safe to print."""


class ReadOnlyProvider:
    def __init__(self, provider: Any) -> None:
        self._provider = provider

    def __getattr__(self, name: str) -> Any:
        if name in PROVIDER_WRITES:
            raise SyncError(f"mirror mode never writes to the provider ({name})")
        return getattr(self._provider, name)


def digest(value: Any) -> str:
    raw = json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"))
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()[:32]


def split_marker(body: str) -> tuple[str, dict[str, Any] | None]:
    match = MARKER_RE.search(body or "")
    if not match:
        return body or "", None
    try:
        marker = json.loads(match.group(1))
    except ValueError:
        marker = None
    return (body[:match.start()], marker if isinstance(marker, dict) else None)


def with_marker(body: str, author: str, **marker: Any) -> str:
    credit = f"<sub>Synced from ForkMesh · {bridge.single_line(author) or 'ForkMesh'}</sub>"
    tag = json.dumps(marker, sort_keys=True, separators=(",", ":"))
    return f"{body}\n\n{credit}\n<!-- forkmesh-forge-sync:v1 {tag} -->"


def label_key(labels: list[str]) -> list[str]:
    return sorted({str(label).casefold() for label in labels})


# ---- ledger -----------------------------------------------------------------


def ledger_path(checkout: Path) -> Path:
    override = os.environ.get(LEDGER_ENV, "").strip()
    return Path(override) if override else Path(checkout) / LEDGER_REL


def load_ledger(checkout: Path) -> dict[str, Any]:
    path = ledger_path(checkout)
    try:
        ledger = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise SyncError(f"{LEDGER_REL} is missing; run `forge_sync.py connect` first") from None
    except ValueError as exc:
        raise SyncError(f"{LEDGER_REL} is not valid JSON") from exc
    if not isinstance(ledger, dict) or ledger.get("schema") != SCHEMA:
        raise SyncError(f"{LEDGER_REL} is not a {SCHEMA} ledger")
    ledger.setdefault("issues", {})
    ledger.setdefault("identities", {})
    ledger.setdefault("history", [])
    ledger.setdefault("sourceOfTruth", {})
    for name, rule in ledger["sourceOfTruth"].items():
        if name not in FIELDS or rule not in RULES:
            raise SyncError(f"sourceOfTruth.{name} must be one of {', '.join(RULES)}")
    if ledger.setdefault("mode", "sync") not in MODES:
        raise SyncError(f"mode must be one of {', '.join(MODES)}")
    return ledger


def save_ledger(checkout: Path, ledger: dict[str, Any]) -> bool:
    return bridge.write_if_changed(
        ledger_path(checkout),
        json.dumps(ledger, indent=2, sort_keys=True, ensure_ascii=False) + "\n")


def identity_links(ledger: dict[str, Any], provider: str) -> tuple[dict[str, str], dict[str, str], list[str]]:
    """Owner-approved ``<provider>:<login>`` → account links, both directions.
    A login or account that appears in more than one link is ambiguous and
    ignored."""
    prefix = provider + ":"
    by_login: dict[str, set[str]] = {}
    by_account: dict[str, set[str]] = {}
    for key, account in (ledger.get("identities") or {}).items():
        if not str(key).startswith(prefix) or not isinstance(account, str) or not account:
            continue
        login = str(key)[len(prefix):].casefold()
        by_login.setdefault(login, set()).add(account)
        by_account.setdefault(account, set()).add(login)
    warnings = []
    to_account, to_login = {}, {}
    for login, accounts in sorted(by_login.items()):
        if len(accounts) > 1:
            warnings.append(f"{prefix}{login} is linked to several accounts and is ignored")
            continue
        account = next(iter(accounts))
        if len(by_account[account]) > 1:
            warning = f"{account} is linked to several {provider} logins and is ignored"
            if warning not in warnings:
                warnings.append(warning)
            continue
        to_account[login] = account
        to_login[account] = login
    return to_account, to_login, warnings


# ---- ForkMesh issue records -------------------------------------------------


def event_content(ev: dict[str, Any]) -> str:
    if ev.get("type") == "delete":
        return str(ev.get("target", ""))
    return bridge.issue_event_content(ev)


def fold(record: dict[str, Any]) -> dict[str, Any]:
    events = sorted((ev for ev in record.get("events") or [] if isinstance(ev, dict)),
                    key=lambda ev: (int(ev.get("ts") or 0), str(ev.get("id", ""))))
    opener = next((ev for ev in events if ev.get("type") == "open"), {})
    state = {"title": opener.get("title") or record.get("title", ""), "status": "open",
             "labels": [], "assignees": [], "milestone": ""}
    edits: dict[str, str] = {}
    deleted: set[str] = set()
    for ev in events:
        kind = ev.get("type")
        if kind == "title" and ev.get("title"):
            state["title"] = ev["title"]
        elif kind == "status":
            state["status"] = ev.get("status", state["status"])
        elif kind in ("labels", "assignees"):
            state[kind] = list(ev.get(kind) or [])
        elif kind == "milestone":
            state["milestone"] = ev.get("milestone", "")
        elif kind == "edit" and ev.get("target"):
            edits[str(ev["target"])] = ev.get("body", "")
        elif kind == "delete" and ev.get("target"):
            deleted.add(str(ev["target"]))
    comments = {
        str(ev["id"]): {"body": edits.get(str(ev["id"]), ev.get("body", "")),
                        "authorName": ev.get("authorName", ""), "ts": int(ev.get("ts") or 0)}
        for ev in events if ev.get("type") == "comment" and ev.get("id")}
    opener_id = str(opener.get("id", ""))
    return {**state, "openerId": opener_id,
            "body": edits.get(opener_id, opener.get("body", "")),
            "attachments": list(opener.get("attachments") or []),
            "comments": {k: v for k, v in comments.items() if k not in deleted},
            "deletedComments": {k for k in comments if k in deleted},
            "lastTs": max((int(ev.get("ts") or 0) for ev in events), default=0),
            "createdAt": int(record.get("createdAt") or opener.get("ts") or 0),
            "authorName": record.get("authorName", "")}


class LocalIssues:
    def __init__(self, checkout: Path, signer: Any) -> None:
        self.root = Path(checkout) / ".forkmesh" / "issues"
        self.signer = signer
        self.paths: dict[int, Path] = {}
        self.records: dict[int, dict[str, Any]] = {}
        self.dirty: set[int] = set()
        for side in (self.root, self.root / "open", self.root / "closed"):
            for number, directory in bridge.numeric_dirs(side).items():
                path = directory / f"issue-{number}.json"
                try:
                    record = json.loads(path.read_text(encoding="utf-8"))
                except (FileNotFoundError, ValueError):
                    continue
                if isinstance(record, dict):
                    self.paths[number], self.records[number] = directory, record

    def append(self, number: int, kind: str, event_id: str, ts: int,
               author_name: str, **fields: Any) -> dict[str, Any] | None:
        record = self.records[number]
        events = record.setdefault("events", [])
        if any(ev.get("id") == event_id for ev in events if isinstance(ev, dict)):
            return None
        last = max((int(ev.get("ts") or 0) for ev in events if isinstance(ev, dict)), default=0)
        event = {"type": kind, "id": event_id, "author": self.signer.public_key,
                 "authorName": author_name, "ts": max(int(ts), last + 1) if events else int(ts),
                 **fields}
        event["sig"] = self.signer.sign(bridge._event_canonical(
            "forkmesh-issue-event-v1", kind, number, event["author"], event["ts"],
            event_content(event)))
        events.append(event)
        self.dirty.add(number)
        return event

    def create(self, number: int, created_at: int, author_name: str) -> None:
        self.records[number] = {
            "schema": "forkmesh-issue-v1", "number": number, "createdAt": created_at,
            "author": self.signer.public_key, "authorName": author_name,
            "priority": 0, "progress": 0, "bountyUsd": 0.0, "bountyAddress": "",
            "bountyStatus": "", "votes": 0, "events": []}
        self.dirty.add(number)

    def flush(self) -> list[int]:
        written = []
        for number in sorted(self.dirty):
            record = self.records[number]
            folded = fold(record)
            record.update({"title": folded["title"], "status": folded["status"],
                           "labels": folded["labels"], "assignees": folded["assignees"],
                           "milestone": folded["milestone"], "updatedAt": folded["lastTs"]})
            side = "closed" if folded["status"] == "closed" else "open"
            target = self.root / side / str(number)
            current = self.paths.get(number)
            if current and current != target and current.exists():
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(current), str(target))
            self.paths[number] = target
            if bridge.write_if_changed(target / f"issue-{number}.json",
                                       json.dumps(record, indent=4, sort_keys=True,
                                                  ensure_ascii=False) + "\n"):
                written.append(number)
        self.dirty.clear()
        return written


# ---- engine -----------------------------------------------------------------


@dataclass
class SyncReport:
    counts: dict[str, dict[str, int]] = field(default_factory=lambda: {
        side: {"created": 0, "updated": 0, "deleted": 0, "linked": 0}
        for side in ("toForkmesh", "toProvider")})
    unchanged: int = 0
    resolved_by_rule: int = 0
    warnings: list[str] = field(default_factory=list)
    error: str | None = None
    error_message: str = ""
    completed: bool = True

    def bump(self, direction: str, action: str) -> None:
        self.counts[direction][action] += 1


class Engine:
    def __init__(self, provider: Any, signer: Any, *, checkout: Path,
                 clock: Callable[[], int] = lambda: int(time.time() * 1000)) -> None:
        self.checkout = Path(checkout)
        self.clock = clock
        self.ledger = load_ledger(self.checkout)
        self.mirror = self.ledger["mode"] == "mirror"
        self.provider = ReadOnlyProvider(provider) if self.mirror else provider
        self.p = provider.name
        if self.ledger.get("provider") != self.p or \
                self.ledger.get("repository") != provider.repository:
            raise SyncError(f"{LEDGER_REL} is connected to "
                            f"{self.ledger.get('provider')}:{self.ledger.get('repository')}")
        self.local = LocalIssues(self.checkout, signer)
        self.report = SyncReport()
        self.to_account, self.to_login, identity_warnings = identity_links(self.ledger, self.p)
        self.report.warnings.extend(identity_warnings)
        self.rules = {f: self.ledger["sourceOfTruth"].get(f, "manual") for f in FIELDS}
        self.sync_name = f"{self.p}:~sync"

    # -- comparable field values ------------------------------------------------

    def local_values(self, folded: dict[str, Any]) -> dict[str, Any]:
        logins = []
        for name in folded["assignees"]:
            if name.startswith(self.p + ":"):
                logins.append(name[len(self.p) + 1:].casefold())
            elif name in self.to_login:
                logins.append(self.to_login[name])
        return {"title": folded["title"], "body": folded["body"], "state": folded["status"],
                "labels": label_key(folded["labels"]), "assignees": sorted(set(logins))}

    @staticmethod
    def remote_values(issue: Any) -> dict[str, Any]:
        return {"title": issue.title, "body": split_marker(issue.body)[0], "state": issue.state,
                "labels": label_key(issue.labels),
                "assignees": sorted({a.casefold() for a in issue.assignees})}

    def local_assignee(self, login: str) -> str:
        return self.to_account.get(login.casefold(), f"{self.p}:{login}")

    # -- run -------------------------------------------------------------------

    def run(self) -> dict[str, Any]:
        now = self.clock()
        if self.ledger.get("paused"):
            return self.result(now)
        try:
            remote = list(self.provider.list_issues())
            self.viewer = self.provider.viewer()
            by_id = {issue.id: issue for issue in remote}
            linked = {str(e.get("remoteId")): int(n) for n, e in self.ledger["issues"].items()
                      if e.get("remoteId")}
            for number, entry in sorted(self.ledger["issues"].items(), key=lambda kv: int(kv[0])):
                if entry.get("detached") or not entry.get("remoteId"):
                    continue
                issue = by_id.get(str(entry["remoteId"]))
                if issue is None:
                    self.remote_missing(int(number), entry, now)
                    continue
                self.guard(int(number), entry, now, lambda: self.sync_pair(int(number), entry, issue, now))
            for issue in remote:
                if issue.id not in linked:
                    self.adopt_or_import(issue, linked, now)
            for number in sorted(self.local.records):
                if not self.mirror:
                    self.export_if_new(number, now)
        except providers.ForgeError as exc:
            self.report.completed = False
            self.report.error = exc.code
            self.report.error_message = str(exc)
        finally:
            self.local.flush()
        return self.result(now)

    def guard(self, number: int, entry: dict[str, Any], now: int, work: Callable[[], None]) -> None:
        try:
            work()
            entry.pop("lastError", None)
            entry.pop("retries", None)
        except providers.ForgeError as exc:
            entry["lastError"] = exc.code
            entry["retries"] = int(entry.get("retries") or 0) + 1
            entry.setdefault("pendingSince", now)
            if exc.retryable:
                raise
            self.report.warnings.append(f"issue #{number}: {exc}")

    # -- linked pairs ------------------------------------------------------------

    def sync_pair(self, number: int, entry: dict[str, Any], issue: Any, now: int) -> None:
        if number not in self.local.records:
            self.conflict(entry, "issue", {"forkmesh": "missing", "provider": "present"}, now)
            return
        folded = fold(self.local.records[number])
        if (issue.updated_at == entry.get("remoteUpdatedAt")
                and folded["lastTs"] == entry.get("localUpdatedAt")
                and not entry.get("conflicts")):
            self.report.unchanged += 1
            return
        lv, rv = self.local_values(folded), self.remote_values(issue)
        bases = entry.setdefault("fields", {})
        push: dict[str, Any] = {}
        wrote = False
        for name in FIELDS:
            winner = self.merge(entry, name, lv[name], rv[name], bases, now)
            if winner == "provider":
                self.apply_local_field(number, folded, issue, name, now)
                wrote = True
            elif winner == "forkmesh":
                push[name] = lv[name]
        if push:
            fields = {}
            for name, value in push.items():
                if name == "labels":
                    fields["labels"] = folded["labels"]
                elif name == "body" and entry.get("origin") == "forkmesh":
                    fields["body"] = with_marker(value, folded["authorName"], issue=number)
                else:
                    fields[name] = value
            issue = self.provider.update_issue(issue, fields)
            self.report.bump("toProvider", "updated")
        if wrote:
            self.report.bump("toForkmesh", "updated")
        self.sync_comments(number, entry, issue, now)
        entry["remoteUpdatedAt"] = issue.updated_at
        entry["localUpdatedAt"] = fold(self.local.records[number])["lastTs"]
        if not entry.get("conflicts"):
            entry.pop("pendingSince", None)
        if not push and not wrote:
            self.report.unchanged += 1

    def merge(self, entry: dict[str, Any], name: str, local: Any, remote: Any,
              bases: dict[str, str], now: int) -> str | None:
        """Return the side whose value must be copied, or None."""
        l, r, base = digest(local), digest(remote), bases.get(name)
        conflicts = entry.setdefault("conflicts", {})
        if l == r:
            bases[name] = l
            conflicts.pop(name, None)
            self.tidy(entry)
            return None
        if self.mirror:
            bases[name] = r
            conflicts.pop(name, None)
            self.tidy(entry)
            return "provider"
        if l == base:
            bases[name] = r
            conflicts.pop(name, None)
            self.tidy(entry)
            return "provider"
        if r == base:
            bases[name] = l
            conflicts.pop(name, None)
            self.tidy(entry)
            return "forkmesh"
        rule = self.rules.get(name.split(":", 1)[0], "manual")
        if rule in SIDES:
            loser = "provider" if rule == "forkmesh" else "forkmesh"
            self.remember(entry, name, rule, local if loser == "forkmesh" else remote, now)
            bases[name] = l if rule == "forkmesh" else r
            conflicts.pop(name, None)
            self.tidy(entry)
            self.report.resolved_by_rule += 1
            return rule
        self.conflict(entry, name, {"forkmesh": local, "provider": remote}, now)
        return None

    def conflict(self, entry: dict[str, Any], name: str, values: dict[str, Any], now: int) -> None:
        conflicts = entry.setdefault("conflicts", {})
        previous = conflicts.get(name) or {}
        conflicts[name] = {**values, "detectedAt": previous.get("detectedAt", now)}
        entry.setdefault("pendingSince", now)

    @staticmethod
    def tidy(entry: dict[str, Any]) -> None:
        if not entry.get("conflicts"):
            entry.pop("conflicts", None)

    def remember(self, entry: dict[str, Any], name: str, took: str, discarded: Any, now: int) -> None:
        history = self.ledger["history"]
        history.append({"at": now, "remoteId": entry.get("remoteId"), "field": name,
                        "took": took, "discarded": discarded})
        del history[:-HISTORY_LIMIT]

    def apply_local_field(self, number: int, folded: dict[str, Any], issue: Any,
                          name: str, now: int) -> None:
        stamp = max(issue.updated_at, folded["lastTs"] + 1)
        eid = f"forge-{self.p}-{name}-{issue.id}-{stamp}"
        if name == "title":
            self.local.append(number, "title", eid, stamp, self.sync_name, title=issue.title)
        elif name == "body":
            self.local.append(number, "edit", eid, stamp, self.sync_name,
                              target=folded["openerId"], body=split_marker(issue.body)[0],
                              attachments=folded["attachments"])
        elif name == "state":
            self.local.append(number, "status", eid, stamp, self.sync_name, status=issue.state)
        elif name == "labels":
            self.local.append(number, "labels", eid, stamp, self.sync_name, labels=list(issue.labels))
        elif name == "assignees":
            hidden = [a for a in folded["assignees"]
                      if not a.startswith(self.p + ":") and a not in self.to_login]
            self.local.append(number, "assignees", eid, stamp, self.sync_name,
                              assignees=hidden + [self.local_assignee(a) for a in issue.assignees])

    # -- comments ----------------------------------------------------------------

    def sync_comments(self, number: int, entry: dict[str, Any], issue: Any, now: int) -> None:
        folded = fold(self.local.records[number])
        mapped = entry.setdefault("comments", {})
        remote = {c.id: c for c in self.provider.list_comments(issue)} \
            if (issue.comment_count or mapped or folded["comments"]) else {}
        by_remote = {str(m.get("remoteId")): eid for eid, m in mapped.items()}
        conflicts = entry.setdefault("conflicts", {})
        for eid, link in list(mapped.items()):
            if link.get("deleted"):
                continue
            key = f"comment:{eid}"
            local = folded["comments"].get(eid)
            if link.get("reexport"):
                if self.mirror:
                    continue
                if local is None:
                    link["deleted"] = True
                    continue
                created = self.provider.create_comment(
                    issue, with_marker(local["body"], local["authorName"], issue=number, event=eid))
                mapped[eid] = link = {"remoteId": created.id, "origin": "forkmesh",
                                      "bases": {key: digest(local["body"])}}
                by_remote[created.id] = eid
                self.report.bump("toProvider", "created")
                continue
            comment = remote.get(str(link.get("remoteId")))
            if local is not None and comment is not None:
                winner = self.merge(entry, key, local["body"], split_marker(comment.body)[0],
                                    link.setdefault("bases", {}), now)
                if winner == "provider":
                    self.local.append(number, "edit", f"forge-{self.p}-edit-{comment.id}-{comment.updated_at}",
                                      comment.updated_at, self.sync_name,
                                      target=eid, body=split_marker(comment.body)[0], attachments=[])
                    self.report.bump("toForkmesh", "updated")
                elif winner == "forkmesh":
                    body = local["body"]
                    if link.get("origin") == "forkmesh":
                        body = with_marker(body, local["authorName"], issue=number, event=eid)
                    self.provider.update_comment(issue, comment.id, body)
                    self.report.bump("toProvider", "updated")
                continue
            base = (link.get("bases") or {}).get(key)
            if local is None and comment is None:
                link["deleted"] = True
            elif local is None:
                if self.mirror:
                    continue
                if digest(split_marker(comment.body)[0]) != base:
                    self.conflict(entry, key, {"forkmesh": "deleted",
                                               "provider": split_marker(comment.body)[0]}, now)
                    continue
                self.provider.delete_comment(issue, comment.id)
                link["deleted"] = True
                self.report.bump("toProvider", "deleted")
            else:
                if not self.mirror and digest(local["body"]) != base:
                    self.conflict(entry, key, {"forkmesh": local["body"], "provider": "deleted"}, now)
                    continue
                self.local.append(number, "delete", f"forge-{self.p}-delete-{link['remoteId']}",
                                  now, self.sync_name, target=eid)
                link["deleted"] = True
                self.report.bump("toForkmesh", "deleted")
            conflicts.pop(key, None)
        self.tidy(entry)

        for comment in sorted(remote.values(), key=lambda c: (c.created_at, c.id)):
            if comment.id in by_remote:
                continue
            body, marker = split_marker(comment.body)
            if self.mirror:
                marker = None
            eid = str((marker or {}).get("event") or f"forge-{self.p}-comment-{comment.id}")
            if eid in mapped:
                self.report.warnings.append(
                    f"issue #{number}: {self.p} comment {comment.id} duplicates a synced comment")
                continue
            if eid not in folded["comments"] and eid not in folded["deletedComments"]:
                self.local.append(number, "comment", eid, comment.created_at,
                                  f"{self.p}:{comment.author}", body=body, attachments=[])
                self.report.bump("toForkmesh", "created")
            else:
                self.report.bump("toForkmesh", "linked")
            mapped[eid] = {"remoteId": comment.id, "origin": "forkmesh" if marker else "provider",
                           "bases": {f"comment:{eid}": digest(body)}}
            by_remote[comment.id] = eid

        if self.mirror:
            return
        folded = fold(self.local.records[number])
        for eid, local in sorted(folded["comments"].items(), key=lambda kv: (kv[1]["ts"], kv[0])):
            if eid in mapped or eid.startswith(FOREIGN_EVENT_PREFIXES):
                continue
            created = self.provider.create_comment(
                issue, with_marker(local["body"], local["authorName"], issue=number, event=eid))
            mapped[eid] = {"remoteId": created.id, "origin": "forkmesh",
                           "bases": {f"comment:{eid}": digest(local["body"])}}
            self.report.bump("toProvider", "created")

    # -- unlinked records -----------------------------------------------------------

    def remote_missing(self, number: int, entry: dict[str, Any], now: int) -> None:
        self.conflict(entry, "issue", {"forkmesh": "present", "provider": "missing"}, now)

    def adopt_or_import(self, issue: Any, linked: dict[str, int], now: int) -> None:
        body, marker = split_marker(issue.body)
        claimed = marker.get("issue") if marker and not self.mirror else None
        if isinstance(claimed, int) and claimed in self.local.records:
            entry = self.ledger["issues"].get(str(claimed))
            if entry and entry.get("remoteId") and str(entry["remoteId"]) != issue.id:
                self.report.warnings.append(
                    f"{self.p} issue #{issue.number} duplicates ForkMesh issue #{claimed}; not imported")
                return
            # A crash or rolled-back ledger lost this link: re-link, never duplicate.
            entry = {"remoteId": issue.id, "remoteNumber": issue.number, "origin": "forkmesh",
                     "fields": {name: digest(value) for name, value in self.remote_values(issue).items()}}
            self.ledger["issues"][str(claimed)] = entry
            linked[issue.id] = claimed
            self.report.bump("toForkmesh", "linked")
            self.guard(claimed, entry, now, lambda: self.sync_pair(claimed, entry, issue, now))
            return
        opener = f"forge-{self.p}-issue-{issue.id}"
        existing = next((n for n, r in self.local.records.items()
                         if fold(r)["openerId"] == opener), None)
        if existing is None:
            taken = set(self.local.records) | {int(n) for n in self.ledger["issues"]}
            number = issue.number
            if number in taken:
                number = max(taken | {issue.number}) + 1
                self.report.warnings.append(
                    f"{self.p} issue #{issue.number} is stored as #{number}: "
                    f"#{issue.number} already belongs to a ForkMesh record")
            author = f"{self.p}:{issue.author}"
            self.local.create(number, issue.created_at, author)
            self.local.append(number, "open", opener, issue.created_at, author,
                              title=issue.title or "(untitled issue)", body=body, attachments=[])
            if issue.labels:
                self.local.append(number, "labels", f"forge-{self.p}-labels-{issue.id}",
                                  issue.created_at + 1, author, labels=list(issue.labels))
            if issue.assignees:
                self.local.append(number, "assignees", f"forge-{self.p}-assignees-{issue.id}",
                                  issue.created_at + 2, author,
                                  assignees=[self.local_assignee(a) for a in issue.assignees])
            if issue.state == "closed":
                self.local.append(number, "status", f"forge-{self.p}-status-{issue.id}",
                                  issue.updated_at, author, status="closed")
            self.report.bump("toForkmesh", "created")
        else:
            number = existing
            self.report.bump("toForkmesh", "linked")
        folded = fold(self.local.records[number])
        entry = {"remoteId": issue.id, "remoteNumber": issue.number, "origin": "provider",
                 "fields": {name: digest(value) for name, value in self.local_values(folded).items()}}
        self.ledger["issues"][str(number)] = entry
        linked[issue.id] = number
        self.guard(number, entry, now, lambda: self.sync_pair(number, entry, issue, now))

    def export_if_new(self, number: int, now: int) -> None:
        entry = self.ledger["issues"].get(str(number))
        if entry is not None and not entry.get("reexport"):
            return
        folded = fold(self.local.records[number])
        if not folded["openerId"] or folded["openerId"].startswith(FOREIGN_EVENT_PREFIXES):
            return
        if entry is None and folded["createdAt"] < int(self.ledger.get("exportFrom") or 0):
            return
        values = self.local_values(folded)
        entry = {"origin": "forkmesh"}
        self.ledger["issues"][str(number)] = entry

        def create() -> None:
            issue = self.provider.create_issue(
                title=folded["title"], state=folded["status"], labels=folded["labels"],
                assignees=values["assignees"],
                body=with_marker(folded["body"], folded["authorName"], issue=number))
            entry.update({"remoteId": issue.id, "remoteNumber": issue.number,
                          "fields": {name: digest(value) for name, value in values.items()}})
            self.report.bump("toProvider", "created")
            self.sync_pair(number, entry, issue, now)

        try:
            self.guard(number, entry, now, create)
        finally:
            if not entry.get("remoteId"):
                entry["reexport"] = True

    # -- result ----------------------------------------------------------------------

    def result(self, now: int) -> dict[str, Any]:
        if self.report.completed and not self.ledger.get("paused"):
            self.ledger["lastSuccessAt"] = now
        self.ledger["lastRun"] = {"at": now, "completed": self.report.completed,
                                  "error": self.report.error}
        retries = getattr(self.provider.transport, "retries", 0)
        recovered = getattr(self.provider.transport, "recovered", 0)
        save_ledger(self.checkout, self.ledger)
        save_source(self.checkout, self.ledger)
        return build_report(self.ledger, now, report=self.report, retries=retries,
                            recovered=recovered)


def source_record(ledger: dict[str, Any]) -> dict[str, Any] | None:
    provider = str(ledger.get("provider") or "")
    template = ISSUE_URL_TEMPLATES.get(provider)
    repository = str(ledger.get("repository") or "")
    if not template or not repository:
        return None
    base = str(ledger.get("baseUrl") or "").rstrip("/")
    if "{base}" in template and not base:
        return None
    renumbered = {}
    for number, entry in (ledger.get("issues") or {}).items():
        remote = entry.get("remoteNumber")
        if isinstance(remote, int) and str(remote) != str(number):
            renumbered[str(number)] = remote
    return {"schema": SOURCE_SCHEMA, "provider": provider, "repository": repository,
            "baseUrl": base,
            "issueUrl": template.replace("{repository}", repository).replace("{base}", base),
            "renumbered": dict(sorted(renumbered.items(), key=lambda kv: int(kv[0])))}


def save_source(checkout: Path, ledger: dict[str, Any]) -> bool:
    record = source_record(ledger)
    if record is None:
        return False
    return bridge.write_if_changed(
        Path(checkout) / SOURCE_REL,
        json.dumps(record, indent=2, sort_keys=True, ensure_ascii=False) + "\n")


# ---- status & telemetry ---------------------------------------------------------


def build_report(ledger: dict[str, Any], now: int, *, report: SyncReport | None = None,
                 retries: int = 0, recovered: int = 0) -> dict[str, Any]:
    issues = ledger.get("issues") or {}
    open_conflicts = [
        {"issue": int(number), "field": name, **{side: values.get(side) for side in SIDES},
         "detectedAt": values.get("detectedAt")}
        for number, entry in sorted(issues.items(), key=lambda kv: int(kv[0]))
        for name, values in sorted((entry.get("conflicts") or {}).items())]
    linked = [e for e in issues.values() if e.get("remoteId") and not e.get("detached")]
    converged = [e for e in linked if not e.get("conflicts") and not e.get("lastError")]
    pending = [int(e["pendingSince"]) for e in issues.values() if e.get("pendingSince")]
    last_success = ledger.get("lastSuccessAt")
    slo = int(ledger.get("sloMs") or DEFAULT_SLO_MS)
    lag_candidates = pending + ([int(last_success)] if last_success else [])
    lag = 0 if not pending and last_success and now == last_success else (
        max(0, now - min(lag_candidates)) if lag_candidates else None)
    error = report.error if report else (ledger.get("lastRun") or {}).get("error")
    provider_current = bool(last_success) and not error and not pending
    status = sync_status.evaluate({
        "now": now, "paused": bool(ledger.get("paused")), "conflicts": len(open_conflicts),
        "lastError": error, "requiredCopies": 2,
        "sourceUpdatedAt": min(pending) if pending else None,
        "copies": [
            {"status": "current", "verified": True, "confirmedAt": last_success},
            {"status": "current" if provider_current else ("unreachable" if error else "behind"),
             "verified": provider_current, "confirmedAt": last_success},
        ]})
    return {
        "schema": REPORT_SCHEMA,
        "provider": ledger.get("provider"), "repository": ledger.get("repository"),
        "paused": bool(ledger.get("paused")), "pausedReason": ledger.get("pausedReason", ""),
        "completed": report.completed if report else None,
        "error": error, "errorMessage": report.error_message if report else "",
        "counts": report.counts if report else None,
        "unchanged": report.unchanged if report else None,
        "resolvedByRule": report.resolved_by_rule if report else 0,
        "warnings": report.warnings if report else [],
        "conflicts": open_conflicts,
        "failing": sorted(int(n) for n, e in issues.items() if e.get("lastError")),
        "measures": {
            "linkedIssues": len(linked),
            "convergenceRate": round(len(converged) / len(linked), 4) if linked else 1.0,
            "conflictRate": round(len(open_conflicts) / len(linked), 4) if linked else 0.0,
            "retries": retries, "retrySuccesses": recovered,
            "lagMs": lag, "sloMs": slo, "sloMet": lag is not None and lag <= slo,
        },
        "status": status,
    }


def markdown(result: dict[str, Any]) -> str:
    lines = [f"## ForkMesh ⇄ {result['provider']} sync ({result['repository']})", "",
             f"**{result['status']['label']}** — {result['status']['summary']}", ""]
    if result.get("counts"):
        lines += ["| Direction | Created | Updated | Deleted | Linked |", "| --- | --- | --- | --- | --- |"]
        for side, c in result["counts"].items():
            lines.append(f"| {side} | {c['created']} | {c['updated']} | {c['deleted']} | {c['linked']} |")
        lines.append("")
    m = result["measures"]
    lag = "not synced yet" if m["lagMs"] is None else f"{m['lagMs']} ms"
    lines.append(f"Lag: {lag} (SLO {m['sloMs']} ms, {'met' if m['sloMet'] else 'missed'}); "
                 f"convergence {m['convergenceRate']:.0%}; retries {m['retries']} "
                 f"({m['retrySuccesses']} recovered).")
    if result["conflicts"]:
        lines += ["", "Unresolved conflicts (resolve with `forge_sync.py resolve`):", ""]
        lines += [f"- issue #{c['issue']} `{c['field']}`" for c in result["conflicts"]]
    if result["warnings"]:
        lines += ["", "Warnings:", ""] + [f"- {w}" for w in result["warnings"]]
    return "\n".join(lines) + "\n"


# ---- operator controls ------------------------------------------------------------


def connect(checkout: Path, *, provider: str, repository: str, base_url: str = "",
            export_from: int, slo_ms: int = DEFAULT_SLO_MS,
            mode: str | None = None) -> dict[str, Any]:
    if provider not in providers.ADAPTERS:
        raise SyncError(f"provider must be one of {', '.join(providers.ADAPTERS)}")
    repository = providers.validate_repository(repository)
    path = ledger_path(checkout)
    ledger = load_ledger(checkout) if path.exists() else {"schema": SCHEMA, "issues": {},
                                                          "identities": {}, "history": [],
                                                          "sourceOfTruth": {}}
    if ledger.get("provider") not in (None, provider) or \
            ledger.get("repository") not in (None, repository):
        raise SyncError(f"{LEDGER_REL} is already connected to another repository")
    if mode is not None and mode not in MODES:
        raise SyncError(f"mode must be one of {', '.join(MODES)}")
    ledger.update({"provider": provider, "repository": repository, "sloMs": slo_ms})
    if mode is not None:
        ledger["mode"] = mode
    ledger.setdefault("exportFrom", export_from)
    ledger.setdefault("paused", False)
    if base_url:
        ledger["baseUrl"] = base_url
    save_ledger(checkout, ledger)
    return ledger


def set_paused(checkout: Path, paused: bool, reason: str = "") -> dict[str, Any]:
    ledger = load_ledger(checkout)
    ledger["paused"] = paused
    if paused and reason:
        ledger["pausedReason"] = bridge.single_line(reason)
    else:
        ledger.pop("pausedReason", None)
    save_ledger(checkout, ledger)
    return ledger


def resolve(checkout: Path, *, issue: int, name: str, take: str,
            clock: Callable[[], int] = lambda: int(time.time() * 1000)) -> dict[str, Any]:
    """Settle one conflict. The next run copies the chosen side over the other;
    the discarded value stays in the ledger history."""
    if take not in SIDES:
        raise SyncError("--take must be forkmesh or provider")
    ledger = load_ledger(checkout)
    entry = ledger["issues"].get(str(issue))
    values = ((entry or {}).get("conflicts") or {}).get(name)
    if not values:
        raise SyncError(f"issue #{issue} has no conflict on {name}")
    other = "provider" if take == "forkmesh" else "forkmesh"
    entry["conflicts"].pop(name)
    if not entry["conflicts"]:
        entry.pop("conflicts")
        entry.pop("pendingSince", None)
    ledger["history"].append({"at": clock(), "remoteId": entry.get("remoteId"), "field": name,
                              "took": take, "discarded": values.get(other)})
    del ledger["history"][:-HISTORY_LIMIT]
    if name == "issue":
        if take == "forkmesh":
            ledger["issues"][str(issue)] = {"origin": "forkmesh", "reexport": True}
        else:
            entry["detached"] = True
    elif name.startswith("comment:"):
        eid = name[len("comment:"):]
        link = entry.setdefault("comments", {}).setdefault(eid, {})
        if values.get(take) == "deleted":
            # The deletion wins: the next pass deletes the surviving copy.
            link.setdefault("bases", {})[name] = digest(values.get(other))
        elif values.get(other) == "deleted":
            if take == "forkmesh":
                # Recreate the comment on the provider on the next pass.
                entry["comments"][eid] = {"reexport": True}
            else:
                # Restore it in ForkMesh by forgetting the link; the provider
                # copy is re-imported as a comment.
                entry["comments"].pop(eid, None)
        else:
            link.setdefault("bases", {})[name] = digest(values.get(other))
    else:
        entry.setdefault("fields", {})[name] = digest(values.get(other))
    entry.pop("remoteUpdatedAt", None)
    save_ledger(checkout, ledger)
    return ledger


# ---- CLI ---------------------------------------------------------------------------


def build_provider(ledger: dict[str, Any], token: str, **transport: Any) -> Any:
    adapter = providers.ADAPTERS.get(str(ledger.get("provider")))
    if adapter is None:
        raise SyncError("the ledger names no supported provider")
    kwargs = dict(transport)
    if ledger.get("baseUrl"):
        kwargs["base_url"] = ledger["baseUrl"]
    elif adapter is providers.GiteaProvider:
        raise SyncError("a Gitea connection needs --base-url")
    return adapter(ledger["repository"], token, **kwargs)


def main(argv: list[str] | None = None) -> int:
    previous = os.environ.get(LEDGER_ENV)
    try:
        return _main(argv)
    finally:
        if previous is None:
            os.environ.pop(LEDGER_ENV, None)
        else:
            os.environ[LEDGER_ENV] = previous


def _main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--checkout", type=Path, default=Path("."))
    parser.add_argument("--ledger", type=Path, default=None,
                        help="keep the sync ledger at this path instead of the checkout's "
                             ".forkmesh/forge-sync.json")
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("connect", help="create or update the sync ledger")
    c.add_argument("--provider", required=True, choices=sorted(providers.ADAPTERS))
    c.add_argument("--repository", required=True)
    c.add_argument("--base-url", default="")
    c.add_argument("--slo-minutes", type=int, default=DEFAULT_SLO_MS // 60000)
    c.add_argument("--export-existing", action="store_true",
                   help="also create provider issues for ForkMesh issues opened before connecting")
    c.add_argument("--mirror", action="store_const", const="mirror", dest="mode",
                   help="import only: copy the provider's issues and comments exactly and "
                        "never write to the provider")
    c.add_argument("--two-way", action="store_const", const="sync", dest="mode",
                   help="sync both ways (the default for a new ledger)")
    s = sub.add_parser("sync", help="run one two-way pass")
    s.add_argument("--report", type=Path)
    sub.add_parser("status", help="show lag, conflicts and failures without contacting the provider")
    p = sub.add_parser("pause")
    p.add_argument("--reason", default="")
    sub.add_parser("resume")
    r = sub.add_parser("resolve", help="settle a conflict by choosing a side")
    r.add_argument("--issue", type=int, required=True)
    r.add_argument("--field", required=True, help="title, body, state, labels, assignees, "
                                                   "issue, or comment:<event id>")
    r.add_argument("--take", required=True, choices=SIDES)
    args = parser.parse_args(argv)
    if args.ledger is not None:
        os.environ[LEDGER_ENV] = str(args.ledger)
    now = int(time.time() * 1000)
    try:
        if args.command == "connect":
            connect(args.checkout, provider=args.provider, repository=args.repository,
                    base_url=args.base_url, slo_ms=args.slo_minutes * 60000,
                    export_from=0 if args.export_existing else now, mode=args.mode)
            result = build_report(load_ledger(args.checkout), now)
        elif args.command in ("pause", "resume"):
            result = build_report(set_paused(args.checkout, args.command == "pause",
                                             getattr(args, "reason", "")), now)
        elif args.command == "resolve":
            result = build_report(resolve(args.checkout, issue=args.issue, name=args.field,
                                          take=args.take), now)
        elif args.command == "status":
            result = build_report(load_ledger(args.checkout), now)
        else:
            token = os.environ.get("FORKMESH_FORGE_TOKEN", "")
            key = os.environ.get("FORKMESH_BRIDGE_SIGNING_KEY", "")
            if not key:
                raise SyncError("FORKMESH_BRIDGE_SIGNING_KEY is not set")
            ledger = load_ledger(args.checkout)
            engine = Engine(build_provider(ledger, token), bridge.BridgeSigner.from_text(key),
                            checkout=args.checkout)
            result = engine.run()
            if args.report:
                args.report.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                                       encoding="utf-8")
    except (SyncError, providers.ForgeError, bridge.BridgeError) as exc:
        print(f"forge-sync: {exc}", file=sys.stderr)
        return 2
    print(markdown(result), end="")
    if result.get("completed") is False:
        return 75  # EX_TEMPFAIL: retryable, progress kept
    return 1 if result["conflicts"] else 0


if __name__ == "__main__":
    sys.exit(main())
