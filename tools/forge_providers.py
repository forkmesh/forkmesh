#!/usr/bin/env python3
"""Forge provider contract and certified adapters for ``forge_sync.py``.

Every forge adapter normalizes its provider into the same small record shapes
(:class:`RemoteIssue`, :class:`RemoteComment`) and the same failure classes, so
the two-way engine never branches on the provider. Two adapters are certified:

* :class:`GitHubProvider` — the centralized provider (github.com or GitHub
  Enterprise Server).
* :class:`GiteaProvider` — the self-hosted forge (Gitea and Forgejo share the
  ``/api/v1`` surface, which also covers Codeberg).

A new adapter is added by subclassing :class:`ForgeProvider`, passing the
shared contract suite in ``app/tests/test_forge_sync.py``, and registering it
in :data:`ADAPTERS`.

Stdlib only. Tokens never appear in errors, reprs, or reports.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
import json
import re
import time
from typing import Any, Callable, Iterable
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode, urlsplit
from urllib.request import Request, urlopen


REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
RETRYABLE_HTTP = frozenset({408, 425, 500, 502, 503, 504})
MAX_ATTEMPTS = 4
MAX_RETRY_AFTER_SECONDS = 60.0
CAPABILITIES = frozenset({
    "issues", "comments", "labels", "assignees", "state",
    "edits", "comment-deletions",
})


# ---- failures ---------------------------------------------------------------


class ForgeError(RuntimeError):
    """A provider failure that is safe to print. ``code`` is one of the
    ``forkmesh-sync-status-v1`` error codes."""

    code = "unknown"
    retryable = False


class RateLimited(ForgeError):
    code = "quota_exceeded"
    retryable = True

    def __init__(self, message: str, retry_after: float | None = None) -> None:
        super().__init__(message)
        self.retry_after = retry_after


class ProviderUnavailable(ForgeError):
    code = "provider_unavailable"
    retryable = True


class Unauthorized(ForgeError):
    code = "unauthorized"


class NotFound(ForgeError):
    code = "rejected"


# ---- normalized records -----------------------------------------------------


@dataclass
class RemoteComment:
    id: str
    author: str
    body: str
    created_at: int
    updated_at: int
    author_avatar: str = ""


@dataclass
class RemoteIssue:
    id: str
    number: int
    title: str
    body: str
    state: str
    author: str
    created_at: int
    updated_at: int
    labels: list[str] = field(default_factory=list)
    assignees: list[str] = field(default_factory=list)
    comment_count: int = 0
    author_avatar: str = ""


def iso_ms(value: Any) -> int:
    if not value:
        return 0
    return int(datetime.fromisoformat(str(value).replace("Z", "+00:00")).timestamp() * 1000)


def normalize_body(text: Any) -> str:
    return str(text or "").replace("\r\n", "\n").replace("\r", "\n").strip("\n")


def single_line(text: Any) -> str:
    return " ".join(str(text or "").split())


def _login(user: Any) -> str:
    return str(user.get("login") or "ghost") if isinstance(user, dict) else "ghost"


def _avatar(user: Any) -> str:
    value = user.get("avatar_url") if isinstance(user, dict) else ""
    return value if isinstance(value, str) and value.startswith("https://") else ""


def validate_repository(value: str) -> str:
    repository = (value or "").strip()
    if not REPOSITORY_RE.fullmatch(repository):
        raise ForgeError("the provider repository must use the owner/name form")
    return repository


# ---- transport --------------------------------------------------------------


class HttpTransport:
    """JSON over HTTPS with bounded retry. Rate limits honour ``Retry-After``;
    outages back off exponentially. ``retries`` / ``recovered`` feed the
    retry-success measure."""

    def __init__(self, *, base_url: str, token: str, auth_scheme: str,
                 extra_headers: dict[str, str] | None = None,
                 opener: Callable[..., Any] = urlopen,
                 sleeper: Callable[[float], None] = time.sleep,
                 label: str = "provider") -> None:
        parts = urlsplit(base_url)
        local = parts.hostname in ("localhost", "127.0.0.1", "::1")
        if parts.scheme != "https" and not (parts.scheme == "http" and local):
            raise ForgeError(f"{label} API origin must use https")
        if not token:
            raise Unauthorized(f"{label} token is empty")
        self.base_url = base_url.rstrip("/")
        self._token = token
        self._headers = {"Accept": "application/json",
                         "Authorization": f"{auth_scheme} {token}",
                         "User-Agent": "forkmesh-forge-sync/1",
                         **(extra_headers or {})}
        self._opener = opener
        self._sleep = sleeper
        self.label = label
        self.retries = 0
        self.recovered = 0
        self.requests = 0

    def __repr__(self) -> str:  # pragma: no cover - logging guard
        return f"HttpTransport({self.label}, token=<redacted>)"

    def _safe(self, text: str) -> str:
        return text.replace(self._token, "<redacted>")

    def request(self, method: str, path: str, query: dict[str, Any] | None = None,
                body: Any = None) -> tuple[Any, dict[str, str]]:
        url = self.base_url + path + ("?" + urlencode(query) if query else "")
        payload = None if body is None else json.dumps(body).encode("utf-8")
        headers = dict(self._headers)
        if payload is not None:
            headers["Content-Type"] = "application/json"
        retried = False
        for attempt in range(MAX_ATTEMPTS):
            self.requests += 1
            try:
                req = Request(url, data=payload, method=method, headers=headers)
                with self._opener(req, timeout=60) as response:
                    raw = response.read()
                    got = {k.lower(): v for k, v in response.headers.items()}
                if retried:
                    self.recovered += 1
                return (json.loads(raw) if raw else None), got
            except HTTPError as exc:
                hdrs = exc.headers or {}
                retry_after = hdrs.get("Retry-After")
                limited = exc.code == 429 or (exc.code == 403 and (
                    retry_after is not None or hdrs.get("X-RateLimit-Remaining") == "0"))
                if exc.code == 401:
                    raise Unauthorized(f"{self.label} rejected the token (HTTP 401)") from exc
                if exc.code == 404:
                    raise NotFound(f"{self.label} returned HTTP 404 for {method} {path}") from exc
                if limited or exc.code in RETRYABLE_HTTP:
                    try:
                        delay = min(MAX_RETRY_AFTER_SECONDS, max(0.0, float(retry_after)))
                    except (TypeError, ValueError):
                        delay = min(30.0, 1.5 * (2 ** attempt))
                    if attempt < MAX_ATTEMPTS - 1:
                        self.retries += 1
                        retried = True
                        self._sleep(delay)
                        continue
                    if limited:
                        raise RateLimited(f"{self.label} rate limit still exhausted",
                                          retry_after=delay) from exc
                    raise ProviderUnavailable(
                        f"{self.label} returned HTTP {exc.code} after {MAX_ATTEMPTS} attempts"
                    ) from exc
                raise ForgeError(self._safe(
                    f"{self.label} returned HTTP {exc.code} for {method} {path}")) from exc
            except URLError as exc:
                if attempt < MAX_ATTEMPTS - 1:
                    self.retries += 1
                    retried = True
                    self._sleep(min(30.0, 1.5 * (2 ** attempt)))
                    continue
                raise ProviderUnavailable(self._safe(
                    f"{self.label} is unreachable: {exc.reason}")) from exc
        raise ProviderUnavailable(f"{self.label} retry budget exhausted")  # pragma: no cover


# ---- contract ---------------------------------------------------------------


class ForgeProvider:
    """The contract every adapter implements.

    ``name`` prefixes provider identities (``github:alice``) and engine-owned
    event ids, so it must be a short lowercase word that can never be a
    ForkMesh account name with a colon. ``tier`` is ``centralized`` or
    ``self-hosted``.
    """

    name = ""
    tier = ""
    capabilities: frozenset[str] = CAPABILITIES

    def __init__(self, transport: HttpTransport, repository: str) -> None:
        self.transport = transport
        self.repository = validate_repository(repository)

    @property
    def repo_path(self) -> str:
        owner, name = self.repository.split("/", 1)
        return f"/repos/{quote(owner, safe='')}/{quote(name, safe='')}"

    def viewer(self) -> str:
        """Login the token acts as, or ``""`` when the provider will not say."""
        try:
            data, _ = self.transport.request("GET", "/user")
        except (NotFound, Unauthorized, ForgeError):
            return ""
        return str((data or {}).get("login") or "")

    def list_issues(self) -> Iterable[RemoteIssue]:
        raise NotImplementedError

    def list_comments(self, issue: RemoteIssue) -> list[RemoteComment]:
        raise NotImplementedError

    def create_issue(self, *, title: str, body: str, state: str,
                     labels: list[str], assignees: list[str]) -> RemoteIssue:
        raise NotImplementedError

    def update_issue(self, issue: RemoteIssue, fields: dict[str, Any]) -> RemoteIssue:
        raise NotImplementedError

    def create_comment(self, issue: RemoteIssue, body: str) -> RemoteComment:
        raise NotImplementedError

    def update_comment(self, issue: RemoteIssue, comment_id: str, body: str) -> RemoteComment:
        raise NotImplementedError

    def delete_comment(self, issue: RemoteIssue, comment_id: str) -> None:
        raise NotImplementedError


class GitHubProvider(ForgeProvider):
    name = "github"
    tier = "centralized"

    def __init__(self, repository: str, token: str, *,
                 base_url: str = "https://api.github.com", **transport: Any) -> None:
        super().__init__(HttpTransport(
            base_url=base_url, token=token, auth_scheme="Bearer", label="GitHub",
            extra_headers={"Accept": "application/vnd.github+json",
                           "X-GitHub-Api-Version": "2022-11-28"},
            **transport), repository)

    @staticmethod
    def _issue(item: dict[str, Any]) -> RemoteIssue:
        return RemoteIssue(
            id=str(item.get("id") or item["number"]), number=int(item["number"]),
            title=single_line(item.get("title")), body=normalize_body(item.get("body")),
            state="closed" if item.get("state") == "closed" else "open",
            author=_login(item.get("user")), created_at=iso_ms(item.get("created_at")),
            updated_at=iso_ms(item.get("updated_at")),
            labels=[str(l.get("name") if isinstance(l, dict) else l)
                    for l in item.get("labels") or []],
            assignees=[_login(a) for a in item.get("assignees") or []],
            comment_count=int(item.get("comments") or 0),
            author_avatar=_avatar(item.get("user")))

    @staticmethod
    def _comment(item: dict[str, Any]) -> RemoteComment:
        return RemoteComment(id=str(item["id"]), author=_login(item.get("user")),
                             body=normalize_body(item.get("body")),
                             created_at=iso_ms(item.get("created_at")),
                             updated_at=iso_ms(item.get("updated_at") or item.get("created_at")),
                             author_avatar=_avatar(item.get("user")))

    def _paginate(self, path: str, query: dict[str, Any]) -> Iterable[dict[str, Any]]:
        page = 1
        while True:
            items, headers = self.transport.request(
                "GET", path, {**query, "per_page": 100, "page": page})
            items = items if isinstance(items, list) else []
            yield from (item for item in items if isinstance(item, dict))
            if len(items) < 100 or 'rel="next"' not in headers.get("link", ""):
                return
            page += 1

    def list_issues(self) -> Iterable[RemoteIssue]:
        for item in self._paginate(self.repo_path + "/issues",
                                   {"state": "all", "sort": "created", "direction": "asc"}):
            if "pull_request" not in item:
                yield self._issue(item)

    def list_comments(self, issue: RemoteIssue) -> list[RemoteComment]:
        return [self._comment(c) for c in self._paginate(
            self.repo_path + f"/issues/{issue.number}/comments", {})]

    def create_issue(self, *, title, body, state, labels, assignees) -> RemoteIssue:
        data, _ = self.transport.request("POST", self.repo_path + "/issues", body={
            "title": title, "body": body, "labels": labels, "assignees": assignees})
        created = self._issue(data)
        if state == "closed":
            created = self.update_issue(created, {"state": "closed"})
        return created

    def update_issue(self, issue, fields) -> RemoteIssue:
        data, _ = self.transport.request(
            "PATCH", self.repo_path + f"/issues/{issue.number}", body=dict(fields))
        return self._issue(data)

    def create_comment(self, issue, body) -> RemoteComment:
        data, _ = self.transport.request(
            "POST", self.repo_path + f"/issues/{issue.number}/comments", body={"body": body})
        return self._comment(data)

    def update_comment(self, issue, comment_id, body) -> RemoteComment:
        data, _ = self.transport.request(
            "PATCH", self.repo_path + f"/issues/comments/{quote(comment_id)}", body={"body": body})
        return self._comment(data)

    def delete_comment(self, issue, comment_id) -> None:
        self.transport.request("DELETE", self.repo_path + f"/issues/comments/{quote(comment_id)}")


class GiteaProvider(ForgeProvider):
    """Gitea / Forgejo ``/api/v1``. Labels are addressed by id, so names are
    resolved (and missing labels created) before they are written."""

    name = "gitea"
    tier = "self-hosted"
    PAGE = 50

    def __init__(self, repository: str, token: str, *, base_url: str,
                 **transport: Any) -> None:
        super().__init__(HttpTransport(
            base_url=base_url.rstrip("/") + "/api/v1", token=token,
            auth_scheme="token", label="Gitea", **transport), repository)
        self._label_ids: dict[str, int] | None = None

    _issue = staticmethod(GitHubProvider._issue)
    _comment = staticmethod(GitHubProvider._comment)

    def _paginate(self, path: str, query: dict[str, Any]) -> Iterable[dict[str, Any]]:
        page = 1
        while True:
            items, _ = self.transport.request(
                "GET", path, {**query, "page": page, "limit": self.PAGE})
            items = items if isinstance(items, list) else []
            yield from (item for item in items if isinstance(item, dict))
            if len(items) < self.PAGE:
                return
            page += 1

    def list_issues(self) -> Iterable[RemoteIssue]:
        for item in self._paginate(self.repo_path + "/issues",
                                   {"state": "all", "type": "issues"}):
            if not item.get("pull_request"):
                yield self._issue(item)

    def list_comments(self, issue) -> list[RemoteComment]:
        return [self._comment(c) for c in self._paginate(
            self.repo_path + f"/issues/{issue.number}/comments", {})]

    def _labels(self, names: list[str]) -> list[int]:
        if self._label_ids is None:
            self._label_ids = {
                str(l["name"]).casefold(): int(l["id"])
                for l in self._paginate(self.repo_path + "/labels", {})
                if l.get("name") and l.get("id") is not None}
        ids = []
        for name in names:
            key = name.casefold()
            if key not in self._label_ids:
                data, _ = self.transport.request("POST", self.repo_path + "/labels",
                                                 body={"name": name, "color": "#6f42c1"})
                self._label_ids[key] = int(data["id"])
            ids.append(self._label_ids[key])
        return ids

    def create_issue(self, *, title, body, state, labels, assignees) -> RemoteIssue:
        data, _ = self.transport.request("POST", self.repo_path + "/issues", body={
            "title": title, "body": body, "assignees": assignees,
            "labels": self._labels(labels)})
        created = self._issue(data)
        if state == "closed":
            created = self.update_issue(created, {"state": "closed"})
        return created

    def update_issue(self, issue, fields) -> RemoteIssue:
        fields = dict(fields)
        labels = fields.pop("labels", None)
        if labels is not None:
            self.transport.request("PUT", self.repo_path + f"/issues/{issue.number}/labels",
                                   body={"labels": self._labels(list(labels))})
        data, _ = self.transport.request(
            "PATCH", self.repo_path + f"/issues/{issue.number}", body=fields)
        return self._issue(data)

    def create_comment(self, issue, body) -> RemoteComment:
        data, _ = self.transport.request(
            "POST", self.repo_path + f"/issues/{issue.number}/comments", body={"body": body})
        return self._comment(data)

    def update_comment(self, issue, comment_id, body) -> RemoteComment:
        data, _ = self.transport.request(
            "PATCH", self.repo_path + f"/issues/comments/{quote(comment_id)}", body={"body": body})
        return self._comment(data)

    def delete_comment(self, issue, comment_id) -> None:
        self.transport.request("DELETE", self.repo_path + f"/issues/comments/{quote(comment_id)}")


ADAPTERS: dict[str, type[ForgeProvider]] = {
    GitHubProvider.name: GitHubProvider,
    GiteaProvider.name: GiteaProvider,
}
