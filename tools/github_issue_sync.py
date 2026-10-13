#!/usr/bin/env python3
"""Idempotent one-way GitHub issue synchronization.

The destination issue body carries a machine-readable HTML comment mapping it
to the source issue.  That mapping survives between workflow runs without a
database, avoids duplicates, and records a digest of the synchronized fields so
unchanged issues do not generate API writes.

Tokens are accepted only through environment variables.  Reports contain issue
metadata and counters, never tokens or issue bodies.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import random
import re
import sys
import time
from typing import Any, Callable, Iterable
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode
from urllib.request import Request, urlopen


API_BASE = "https://api.github.com"
API_VERSION = "2022-11-28"
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
MAPPING_RE = re.compile(
    r"<!--\s*forkmesh-issue-sync:v1\s+(\{[^\r\n]*\})\s*-->",
    re.IGNORECASE,
)
FAILURE_MARKER = "<!-- forkmesh-issue-sync-failure:v1 -->"
RETRYABLE_HTTP = frozenset({408, 409, 425, 429, 500, 502, 503, 504})


class SyncError(RuntimeError):
    """A safe-to-display synchronization error."""


class GitHubAPI:
    """Minimal GitHub REST client with pagination and bounded retry."""

    def __init__(
        self,
        token: str,
        *,
        base_url: str = API_BASE,
        opener: Callable[..., Any] = urlopen,
        sleeper: Callable[[float], None] = time.sleep,
    ) -> None:
        if not token:
            raise SyncError("GitHub token is empty")
        self._token = token
        self.base_url = base_url.rstrip("/")
        self._opener = opener
        self._sleep = sleeper

    def __repr__(self) -> str:  # pragma: no cover - defensive logging guard
        return "GitHubAPI(token=<redacted>)"

    def request(
        self,
        method: str,
        path: str,
        *,
        query: dict[str, Any] | None = None,
        body: dict[str, Any] | None = None,
        attempts: int = 5,
    ) -> tuple[Any, dict[str, str]]:
        url = path if path.startswith("https://") else self.base_url + path
        if query:
            url += ("&" if "?" in url else "?") + urlencode(
                [(key, value) for key, value in query.items() if value is not None]
            )
        payload = None if body is None else json.dumps(body).encode("utf-8")
        headers = {
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self._token}",
            "X-GitHub-Api-Version": API_VERSION,
            "User-Agent": "forkmesh-issue-sync/1",
        }
        if payload is not None:
            headers["Content-Type"] = "application/json"

        for attempt in range(attempts):
            request = Request(url, data=payload, method=method, headers=headers)
            try:
                with self._opener(request, timeout=30) as response:
                    raw = response.read()
                    decoded = json.loads(raw.decode("utf-8")) if raw else None
                    return decoded, {
                        key.lower(): value for key, value in response.headers.items()
                    }
            except HTTPError as exc:
                raw = exc.read().decode("utf-8", "replace")
                retry_after = exc.headers.get("Retry-After") if exc.headers else None
                rate_remaining = (
                    exc.headers.get("X-RateLimit-Remaining") if exc.headers else None
                )
                retryable = exc.code in RETRYABLE_HTTP or (
                    exc.code == 403
                    and (retry_after is not None or rate_remaining == "0")
                )
                if retryable and attempt + 1 < attempts:
                    try:
                        delay = min(30.0, max(0.0, float(retry_after)))
                    except (TypeError, ValueError):
                        reset = (
                            exc.headers.get("X-RateLimit-Reset")
                            if exc.headers
                            else None
                        )
                        try:
                            delay = min(
                                30.0,
                                max(0.0, float(reset) - time.time()),
                            )
                        except (TypeError, ValueError):
                            delay = min(
                                12.0,
                                0.75 * (2**attempt) + random.random() / 4,
                            )
                    self._sleep(delay)
                    continue
                message = ""
                try:
                    decoded = json.loads(raw)
                    message = str(decoded.get("message", "")).strip()
                except (json.JSONDecodeError, AttributeError):
                    pass
                suffix = f": {message}" if message else ""
                raise SyncError(
                    f"GitHub API returned HTTP {exc.code}{suffix}".replace(
                        self._token, "<redacted>"
                    )
                ) from exc
            except URLError as exc:
                if attempt + 1 < attempts:
                    self._sleep(min(12.0, 0.75 * (2**attempt)))
                    continue
                raise SyncError(f"GitHub API connection failed: {exc.reason}") from exc
        raise SyncError("GitHub API retry budget exhausted")

    def paginate(
        self, path: str, *, query: dict[str, Any] | None = None
    ) -> Iterable[dict[str, Any]]:
        page = 1
        while True:
            page_query = dict(query or {})
            page_query.update({"page": page, "per_page": 100})
            result, headers = self.request("GET", path, query=page_query)
            if not isinstance(result, list):
                raise SyncError(f"GitHub API pagination expected a list at {path}")
            for item in result:
                if isinstance(item, dict):
                    yield item
            link = headers.get("link", "")
            if len(result) < 100 or 'rel="next"' not in link:
                break
            page += 1


def validate_repository(value: str, label: str) -> str:
    repository = value.strip()
    if not REPOSITORY_RE.fullmatch(repository):
        raise SyncError(f"{label} must use the owner/repository form")
    return repository


def _repo_path(repository: str) -> str:
    owner, name = repository.split("/", 1)
    return f"/repos/{quote(owner, safe='')}/{quote(name, safe='')}"


def source_digest(issue: dict[str, Any]) -> str:
    labels = sorted(
        str(label.get("name", "") if isinstance(label, dict) else label)
        for label in issue.get("labels", [])
    )
    user = issue.get("user") if isinstance(issue.get("user"), dict) else {}
    payload = {
        "title": str(issue.get("title") or ""),
        "body": str(issue.get("body") or ""),
        "state": str(issue.get("state") or "open"),
        "labels": labels,
        "author": str(user.get("login") or ""),
        "createdAt": str(issue.get("created_at") or ""),
        "updatedAt": str(issue.get("updated_at") or ""),
    }
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def mapping_payload(
    source_repository: str, issue: dict[str, Any], digest: str
) -> dict[str, Any]:
    return {
        "source": source_repository,
        "number": int(issue["number"]),
        "nodeId": str(issue.get("node_id") or ""),
        "digest": digest,
        "updatedAt": str(issue.get("updated_at") or ""),
    }


def mapping_marker(payload: dict[str, Any]) -> str:
    compact = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return f"<!-- forkmesh-issue-sync:v1 {compact} -->"


def parse_mapping(body: str | None) -> dict[str, Any] | None:
    matches = list(MAPPING_RE.finditer(body or ""))
    if not matches:
        return None
    match = matches[-1]
    try:
        payload = json.loads(match.group(1))
    except json.JSONDecodeError:
        return None
    if (
        not isinstance(payload, dict)
        or not isinstance(payload.get("source"), str)
        or not isinstance(payload.get("number"), int)
    ):
        return None
    return payload


def destination_body(
    source_repository: str, issue: dict[str, Any], digest: str
) -> str:
    user = issue.get("user") if isinstance(issue.get("user"), dict) else {}
    login = str(user.get("login") or "unknown")
    user_url = str(user.get("html_url") or f"https://github.com/{login}")
    issue_url = str(
        issue.get("html_url")
        or f"https://github.com/{source_repository}/issues/{issue['number']}"
    )
    # A source author cannot inject a forged mapping ahead of the marker this
    # synchronizer appends. Destination parsing also uses the final marker.
    original = MAPPING_RE.sub(
        "<!-- source issue-sync marker removed -->",
        str(issue.get("body") or ""),
    ).rstrip()
    attribution = (
        "---\n"
        f"_Synchronized one-way from [{source_repository}#{issue['number']}]"
        f"({issue_url}). Originally opened by [@{login}]({user_url})._\n\n"
        f"- Source created: `{issue.get('created_at') or 'unknown'}`\n"
        f"- Source updated: `{issue.get('updated_at') or 'unknown'}`\n"
        "- Replies on this copy are not written back to the source."
    )
    pieces = [piece for piece in (original, attribution) if piece]
    pieces.append(mapping_marker(mapping_payload(source_repository, issue, digest)))
    return "\n\n".join(pieces) + "\n"


@dataclass
class SyncReport:
    source: str
    destination: str
    dry_run: bool
    started_at: str
    completed_at: str = ""
    scanned: int = 0
    created: int = 0
    updated: int = 0
    unchanged: int = 0
    skipped_pull_requests: int = 0
    labels_created: int = 0
    mappings: list[dict[str, Any]] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def finish(self) -> None:
        self.completed_at = datetime.now(timezone.utc).isoformat()

    def as_dict(self) -> dict[str, Any]:
        return {
            "schemaVersion": 1,
            "direction": "github-to-github-one-way",
            "sourceRepository": self.source,
            "destinationRepository": self.destination,
            "dryRun": self.dry_run,
            "startedAt": self.started_at,
            "completedAt": self.completed_at,
            "counts": {
                "scanned": self.scanned,
                "created": self.created,
                "updated": self.updated,
                "unchanged": self.unchanged,
                "skippedPullRequests": self.skipped_pull_requests,
                "labelsCreated": self.labels_created,
            },
            "mappings": self.mappings,
            "warnings": self.warnings,
        }

    def markdown(self) -> str:
        mode = "Dry run" if self.dry_run else "Applied"
        return (
            "## ForkMesh issue synchronization\n\n"
            f"- Direction: `{self.source}` → `{self.destination}` (one-way)\n"
            f"- Mode: {mode}\n"
            f"- Scanned source issues: {self.scanned}\n"
            f"- Created: {self.created}\n"
            f"- Updated: {self.updated}\n"
            f"- Unchanged: {self.unchanged}\n"
            f"- Pull requests skipped: {self.skipped_pull_requests}\n"
            f"- Destination labels created: {self.labels_created}\n"
            f"- Warnings: {len(self.warnings)}\n"
        )


class IssueSynchronizer:
    def __init__(
        self,
        source_api: GitHubAPI,
        destination_api: GitHubAPI,
        *,
        source_repository: str,
        destination_repository: str,
        dry_run: bool = False,
        logger: Callable[[str], None] = print,
    ) -> None:
        self.source_api = source_api
        self.destination_api = destination_api
        self.source_repository = validate_repository(
            source_repository, "source repository"
        )
        self.destination_repository = validate_repository(
            destination_repository, "destination repository"
        )
        if self.source_repository.casefold() == self.destination_repository.casefold():
            raise SyncError("source and destination repositories must be different")
        self.dry_run = dry_run
        self.log = logger
        self._destination_labels: dict[str, dict[str, Any]] | None = None

    def _source_issues(self) -> list[dict[str, Any]]:
        path = _repo_path(self.source_repository) + "/issues"
        return list(
            self.source_api.paginate(
                path, query={"state": "all", "sort": "updated", "direction": "asc"}
            )
        )

    def _destination_mappings(
        self,
    ) -> tuple[dict[tuple[str, int], tuple[dict[str, Any], dict[str, Any]]], list[str]]:
        path = _repo_path(self.destination_repository) + "/issues"
        mappings: dict[
            tuple[str, int], tuple[dict[str, Any], dict[str, Any]]
        ] = {}
        warnings: list[str] = []
        for issue in self.destination_api.paginate(path, query={"state": "all"}):
            if "pull_request" in issue:
                continue
            mapping = parse_mapping(issue.get("body"))
            if not mapping:
                continue
            key = (mapping["source"].casefold(), int(mapping["number"]))
            if key in mappings:
                first = mappings[key][0].get("number")
                warnings.append(
                    f"duplicate destination mapping for {mapping['source']}"
                    f"#{mapping['number']}: kept #{first}, ignored #{issue.get('number')}"
                )
                continue
            mappings[key] = (issue, mapping)
        return mappings, warnings

    def _load_destination_labels(self) -> dict[str, dict[str, Any]]:
        if self._destination_labels is None:
            path = _repo_path(self.destination_repository) + "/labels"
            self._destination_labels = {
                str(label.get("name") or "").casefold(): label
                for label in self.destination_api.paginate(path)
                if label.get("name")
            }
        return self._destination_labels

    def _ensure_labels(
        self, source_labels: list[Any], report: SyncReport
    ) -> list[str]:
        destination = self._load_destination_labels()
        names: list[str] = []
        for raw in source_labels:
            source = raw if isinstance(raw, dict) else {"name": str(raw)}
            name = str(source.get("name") or "").strip()
            if not name:
                continue
            names.append(name)
            key = name.casefold()
            if key in destination:
                continue
            report.labels_created += 1
            if self.dry_run:
                destination[key] = source
                continue
            created, _ = self.destination_api.request(
                "POST",
                _repo_path(self.destination_repository) + "/labels",
                body={
                    "name": name,
                    "color": str(source.get("color") or "6f42c1").lstrip("#")[:6],
                    "description": str(source.get("description") or "")[:100],
                },
            )
            destination[key] = created if isinstance(created, dict) else source
        return names

    def run(self) -> SyncReport:
        report = SyncReport(
            source=self.source_repository,
            destination=self.destination_repository,
            dry_run=self.dry_run,
            started_at=datetime.now(timezone.utc).isoformat(),
        )
        mappings, mapping_warnings = self._destination_mappings()
        report.warnings.extend(mapping_warnings)
        source_issues = self._source_issues()
        for source in source_issues:
            if "pull_request" in source:
                report.skipped_pull_requests += 1
                continue
            report.scanned += 1
            number = int(source["number"])
            key = (self.source_repository.casefold(), number)
            digest = source_digest(source)
            existing_pair = mappings.get(key)
            if existing_pair and existing_pair[1].get("digest") == digest:
                report.unchanged += 1
                report.mappings.append(
                    {
                        "sourceIssue": number,
                        "destinationIssue": int(existing_pair[0]["number"]),
                        "action": "unchanged",
                        "digest": digest,
                    }
                )
                continue

            labels = self._ensure_labels(list(source.get("labels") or []), report)
            payload = {
                "title": str(source.get("title") or "(untitled issue)"),
                "body": destination_body(self.source_repository, source, digest),
                "labels": labels,
            }
            desired_state = "closed" if source.get("state") == "closed" else "open"

            if existing_pair:
                destination = existing_pair[0]
                destination_number = int(destination["number"])
                action = "updated"
                report.updated += 1
                if not self.dry_run:
                    payload["state"] = desired_state
                    updated, _ = self.destination_api.request(
                        "PATCH",
                        _repo_path(self.destination_repository)
                        + f"/issues/{destination_number}",
                        body=payload,
                    )
                    if isinstance(updated, dict):
                        destination = updated
                mappings[key] = (
                    destination,
                    mapping_payload(self.source_repository, source, digest),
                )
            else:
                action = "created"
                report.created += 1
                if self.dry_run:
                    destination_number = 0
                else:
                    created, _ = self.destination_api.request(
                        "POST",
                        _repo_path(self.destination_repository) + "/issues",
                        body=payload,
                    )
                    if not isinstance(created, dict) or not created.get("number"):
                        raise SyncError(
                            f"destination did not return a number for source issue #{number}"
                        )
                    destination_number = int(created["number"])
                    if desired_state == "closed":
                        created, _ = self.destination_api.request(
                            "PATCH",
                            _repo_path(self.destination_repository)
                            + f"/issues/{destination_number}",
                            body={"state": "closed"},
                        )
                    mappings[key] = (
                        created if isinstance(created, dict) else {},
                        mapping_payload(self.source_repository, source, digest),
                    )
            self.log(
                f"{'[dry-run] ' if self.dry_run else ''}{action}: "
                f"{self.source_repository}#{number} -> "
                f"{self.destination_repository}#{destination_number or 'new'}"
            )
            report.mappings.append(
                {
                    "sourceIssue": number,
                    "destinationIssue": destination_number or None,
                    "action": action,
                    "digest": digest,
                }
            )
        report.finish()
        return report


def notify_failure(
    api: GitHubAPI,
    destination_repository: str,
    *,
    run_url: str,
    message: str,
) -> dict[str, Any]:
    """Create or update one administrator-visible issue for workflow failures."""

    repository = validate_repository(destination_repository, "destination repository")
    path = _repo_path(repository) + "/issues"
    safe_message = re.sub(r"(?i)(token|authorization|secret)=[^\s]+", r"\1=<redacted>", message)
    body = (
        f"{FAILURE_MARKER}\n\n"
        "The scheduled one-way GitHub issue synchronization failed.\n\n"
        f"- Workflow run: {run_url or 'unavailable'}\n"
        f"- Time: {datetime.now(timezone.utc).isoformat()}\n"
        f"- Summary: {safe_message[:1000] or 'See the workflow logs.'}\n\n"
        "No credentials are included in this notification."
    )
    existing = None
    for issue in api.paginate(path, query={"state": "open"}):
        if FAILURE_MARKER in str(issue.get("body") or ""):
            existing = issue
            break
    if existing:
        updated, _ = api.request(
            "PATCH",
            path + f"/{int(existing['number'])}",
            body={"body": body, "title": "[automation] GitHub issue sync failed"},
        )
        return updated
    created, _ = api.request(
        "POST",
        path,
        body={"title": "[automation] GitHub issue sync failed", "body": body},
    )
    return created


def _token_from_env(name: str) -> str:
    token = os.environ.get(name, "")
    if not token:
        raise SyncError(f"required token environment variable {name} is unset")
    return token


def _write_report(report: SyncReport, report_path: Path, summary_path: Path | None) -> None:
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(
        json.dumps(report.as_dict(), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    if summary_path:
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        summary_path.write_text(report.markdown(), encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    sync = subparsers.add_parser("sync", help="perform a one-way issue sync")
    sync.add_argument("--source", required=True)
    sync.add_argument("--destination", required=True)
    sync.add_argument("--source-token-env", default="GITHUB_TOKEN")
    sync.add_argument("--destination-token-env", default="GITHUB_TOKEN")
    sync.add_argument("--dry-run", action="store_true")
    sync.add_argument("--report", type=Path, required=True)
    sync.add_argument("--summary", type=Path)

    notify = subparsers.add_parser(
        "notify-failure", help="create/update a destination failure issue"
    )
    notify.add_argument("--destination", required=True)
    notify.add_argument("--token-env", default="GITHUB_TOKEN")
    notify.add_argument("--run-url", default="")
    notify.add_argument("--message", default="See the workflow logs.")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    tokens: list[str] = []
    try:
        if args.command == "sync":
            source_token = _token_from_env(args.source_token_env)
            destination_token = _token_from_env(args.destination_token_env)
            tokens.extend((source_token, destination_token))
            synchronizer = IssueSynchronizer(
                GitHubAPI(source_token),
                GitHubAPI(destination_token),
                source_repository=args.source,
                destination_repository=args.destination,
                dry_run=args.dry_run,
            )
            report = synchronizer.run()
            _write_report(report, args.report, args.summary)
            print(report.markdown())
            return 0
        token = _token_from_env(args.token_env)
        tokens.append(token)
        result = notify_failure(
            GitHubAPI(token),
            args.destination,
            run_url=args.run_url,
            message=args.message,
        )
        print(f"failure notification issue: #{result.get('number', 'unknown')}")
        return 0
    except (SyncError, OSError) as exc:
        message = str(exc)
        for token in tokens:
            message = message.replace(token, "<redacted>")
        print(f"issue sync failed: {message}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
