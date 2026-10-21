#!/usr/bin/env python3
"""Two-way forge sync contracts (tools/forge_sync.py, tools/forge_providers.py).

The engine drives the real GitHub and Gitea adapters against an in-memory forge
that speaks each provider's REST shape, so every scenario certifies both the
common provider contract and the adapter."""

import base64
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import sys
from urllib.error import HTTPError
from urllib.parse import parse_qs, urlsplit

import pytest

pytest.importorskip("cryptography")
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import forge_sync as fs  # noqa: E402

providers = fs.providers
REPO = "octo/app"
BASES = {"github": "https://api.github.test", "gitea": "https://gitea.test"}
KINDS = ("github", "gitea")
T0 = 1_800_000_000_000


def iso(ms):
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).isoformat().replace("+00:00", "Z")


# ---- in-memory forge ----------------------------------------------------------


class Response:
    def __init__(self, payload, headers=None):
        self._raw = b"" if payload is None else json.dumps(payload).encode()
        self.headers = headers or {}

    def read(self):
        return self._raw

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class FakeForge:
    """Enough of the GitHub and Gitea issue APIs to certify the adapters."""

    def __init__(self, kind, viewer="forkmesh-bot"):
        self.kind = kind
        self.viewer = viewer
        self.now = T0
        self.next_id = 5000
        self.issues = {}
        self.comments = {}
        self.labels = {}
        self.failures = []
        self.log = []
        self.sleeps = []

    # fixtures
    def tick(self):
        self.now += 60_000
        return self.now

    def _id(self):
        self.next_id += 1
        return self.next_id

    def add_issue(self, number=None, *, title="Crash on start", body="Steps", author="alice",
                  state="open", labels=(), assignees=()):
        number = number or (max(self.issues, default=0) + 1)
        stamp = self.tick()
        self.issues[number] = {"id": self._id(), "number": number, "title": title, "body": body,
                               "state": state, "user": {"login": author}, "labels": list(labels),
                               "assignees": list(assignees), "created_at": stamp,
                               "updated_at": stamp}
        return self.issues[number]

    def add_comment(self, number, body, author="bob"):
        stamp = self.tick()
        cid = self._id()
        self.comments[cid] = {"id": cid, "issue": number, "body": body, "user": {"login": author},
                              "created_at": stamp, "updated_at": stamp}
        self.issues[number]["updated_at"] = stamp
        return self.comments[cid]

    def edit_issue(self, number, **fields):
        self.issues[number].update(fields)
        self.issues[number]["updated_at"] = self.tick()

    def fail(self, code, times=1, headers=None, match=""):
        self.failures.append({"code": code, "times": times, "headers": headers or {}, "match": match})

    def writes(self):
        return [entry for entry in self.log if entry[0] != "GET"]

    # provider wiring
    def provider(self, **extra):
        kwargs = {"opener": self.open, "sleeper": self.sleeps.append}
        if self.kind == "github":
            return providers.GitHubProvider(REPO, "tok-secret", base_url=BASES["github"], **kwargs, **extra)
        return providers.GiteaProvider(REPO, "tok-secret", base_url=BASES["gitea"], **kwargs, **extra)

    # HTTP
    def _issue_json(self, issue):
        count = sum(1 for c in self.comments.values() if c["issue"] == issue["number"])
        labels = [{"name": n, "color": "ededed"} for n in issue["labels"]]
        if self.kind == "gitea":
            labels = [{"id": self.labels.get(n.casefold(), 0), "name": n} for n in issue["labels"]]
        return {**issue, "labels": labels, "assignees": [{"login": a} for a in issue["assignees"]],
                "created_at": iso(issue["created_at"]), "updated_at": iso(issue["updated_at"]),
                "comments": count}

    @staticmethod
    def _comment_json(comment):
        return {**comment, "created_at": iso(comment["created_at"]),
                "updated_at": iso(comment["updated_at"])}

    def _page(self, items, query):
        page = int(query.get("page", ["1"])[0])
        size = int(query.get("per_page" if self.kind == "github" else "limit", ["30"])[0])
        chunk = items[(page - 1) * size: page * size]
        headers = {}
        if self.kind == "github" and page * size < len(items):
            headers["Link"] = '<next>; rel="next"'
        return chunk, headers

    def open(self, request, timeout=None):
        method = request.get_method()
        url = urlsplit(request.full_url)
        path, query = url.path, parse_qs(url.query)
        if self.kind == "gitea":
            assert path.startswith("/api/v1"), path
            path = path[len("/api/v1"):]
            assert request.get_header("Authorization") == "token tok-secret"
        else:
            assert request.get_header("Authorization") == "Bearer tok-secret"
        body = json.loads(request.data) if request.data else None
        self.log.append((method, path))
        for failure in self.failures:
            if failure["times"] and failure["match"] in f"{method} {path}":
                failure["times"] -= 1
                raise HTTPError(request.full_url, failure["code"], "fail", failure["headers"],
                                io.BytesIO(b"{}"))
        prefix = f"/repos/{REPO}"
        if path == "/user":
            return Response({"login": self.viewer})
        assert path.startswith(prefix), path
        rest = path[len(prefix):].strip("/").split("/")

        if rest == ["issues"] and method == "GET":
            items = [self._issue_json(i) for _, i in sorted(self.issues.items())]
            chunk, headers = self._page(items, query)
            return Response(chunk, headers)
        if rest == ["issues"] and method == "POST":
            names = body.get("labels") or []
            if self.kind == "gitea":
                by_id = {v: k for k, v in self.labels.items()}
                names = [by_id[i] for i in names]
            issue = self.add_issue(title=body["title"], body=body["body"], author=self.viewer,
                                   labels=names, assignees=body.get("assignees") or [])
            return Response(self._issue_json(issue))
        if rest == ["labels"] and method == "GET":
            return Response([{"id": v, "name": k} for k, v in self.labels.items()])
        if rest == ["labels"] and method == "POST":
            self.labels[body["name"].casefold()] = self._id()
            return Response({"id": self.labels[body["name"].casefold()], "name": body["name"]})
        if rest[0] == "issues" and rest[1] == "comments":
            comment = self.comments.get(int(rest[2]))
            if comment is None:
                raise HTTPError(request.full_url, 404, "missing", {}, io.BytesIO(b"{}"))
            if method == "DELETE":
                del self.comments[comment["id"]]
                self.issues[comment["issue"]]["updated_at"] = self.tick()
                return Response(None)
            comment.update(body=body["body"], updated_at=self.tick())
            self.issues[comment["issue"]]["updated_at"] = self.now
            return Response(self._comment_json(comment))
        number = int(rest[1])
        if number not in self.issues:
            raise HTTPError(request.full_url, 404, "missing", {}, io.BytesIO(b"{}"))
        if len(rest) == 2 and method == "PATCH":
            self.edit_issue(number, **{k: v for k, v in body.items()
                                       if k in ("title", "body", "state", "labels", "assignees")})
            return Response(self._issue_json(self.issues[number]))
        if rest[2:] == ["labels"] and method == "PUT":
            by_id = {v: k for k, v in self.labels.items()}
            self.edit_issue(number, labels=[by_id[i] for i in body["labels"]])
            return Response([])
        if rest[2:] == ["comments"] and method == "GET":
            items = [self._comment_json(c) for c in sorted(self.comments.values(), key=lambda c: c["id"])
                     if c["issue"] == number]
            chunk, headers = self._page(items, query)
            return Response(chunk, headers)
        if rest[2:] == ["comments"] and method == "POST":
            return Response(self._comment_json(self.add_comment(number, body["body"], self.viewer)))
        raise AssertionError(f"unrouted {method} {path}")


# ---- helpers --------------------------------------------------------------------


@pytest.fixture(params=KINDS)
def forge(request):
    return FakeForge(request.param)


@pytest.fixture
def sync_key():
    return fs.bridge.BridgeSigner.generate()


@pytest.fixture
def native_key():
    return fs.bridge.BridgeSigner.generate()


def connected(tmp_path, forge, **ledger):
    checkout = tmp_path / "repo"
    checkout.mkdir()
    fs.connect(checkout, provider=forge.kind, repository=REPO,
               base_url=BASES[forge.kind] if forge.kind == "gitea" else "", export_from=T0 - 1)
    if ledger:
        data = fs.load_ledger(checkout)
        data.update(ledger)
        fs.save_ledger(checkout, data)
    return checkout


class Clock:
    def __init__(self, forge):
        self.forge = forge

    def __call__(self):
        return self.forge.now


def run(checkout, forge, key):
    return fs.Engine(forge.provider(), key, checkout=checkout, clock=Clock(forge)).run()


def local_issue(checkout, number, key, *, title="Native bug", body="Native body",
                ts=T0 + 1, comments=()):
    store = fs.LocalIssues(checkout, key)
    store.create(number, ts, "carol")
    store.append(number, "open", f"open-{number}", ts, "carol", title=title, body=body, attachments=[])
    for index, text in enumerate(comments):
        store.append(number, "comment", f"c-{number}-{index}", ts + 1 + index, "carol",
                     body=text, attachments=[])
    store.flush()


def local_edit(checkout, number, key, kind, event_id, ts, **fields):
    store = fs.LocalIssues(checkout, key)
    store.append(number, kind, event_id, ts, "carol", **fields)
    store.flush()


def record(checkout, number):
    for side in ("open", "closed"):
        path = checkout / ".forkmesh" / "issues" / side / str(number) / f"issue-{number}.json"
        if path.exists():
            return json.loads(path.read_text())
    return None


def snapshot(checkout):
    return {str(p.relative_to(checkout)): p.read_bytes()
            for p in sorted((checkout / ".forkmesh").rglob("*")) if p.is_file()}


def verifies(number, event):
    raw = lambda t: base64.urlsafe_b64decode(t + "=" * (-len(t) % 4))  # noqa: E731
    content = fs.event_content(event)
    message = (f"forkmesh-issue-event-v1\n{event['type']}\n{number}\n{event['author']}\n"
               f"{event['ts']}\n{hashlib.sha256(content.encode()).hexdigest()}").encode()
    try:
        Ed25519PublicKey.from_public_bytes(raw(event["author"])).verify(raw(event["sig"]), message)
        return True
    except Exception:
        return False


# ---- provider contract ------------------------------------------------------------


def test_every_registered_adapter_implements_the_whole_contract():
    required = [name for name, value in vars(providers.ForgeProvider).items()
                if callable(value) and not name.startswith("_") and name != "viewer"]
    tiers = set()
    for name, adapter in providers.ADAPTERS.items():
        assert adapter.name == name and ":" not in name
        for method in required:
            assert getattr(adapter, method) is not getattr(providers.ForgeProvider, method), (name, method)
        tiers.add(adapter.tier)
    assert tiers == {"centralized", "self-hosted"}


def test_adapter_round_trip_normalizes_the_provider_shape(forge):
    provider = forge.provider()
    created = provider.create_issue(title="  Two   words ", body="a\r\nb\r\n", state="closed",
                                    labels=["bug"], assignees=["dana"])
    assert (created.title, created.state, created.labels, created.assignees) == (
        "Two words", "closed", ["bug"], ["dana"])
    comment = provider.create_comment(created, "hi")
    provider.update_comment(created, comment.id, "hello")
    [listed] = list(provider.list_issues())
    assert listed.id == created.id and listed.body == "a\nb"
    assert [c.body for c in provider.list_comments(listed)] == ["hello"]
    provider.delete_comment(listed, comment.id)
    assert provider.list_comments(listed) == []
    assert provider.viewer() == "forkmesh-bot"


def test_adapter_paginates_past_one_page(forge):
    for _ in range(120):
        forge.add_issue()
    assert len(list(forge.provider().list_issues())) == 120


def test_transport_refuses_plain_http_and_redacts_the_token():
    with pytest.raises(providers.ForgeError, match="https"):
        providers.GiteaProvider(REPO, "t", base_url="http://forge.example")
    forge = FakeForge("github")
    forge.fail(422, match="POST")
    with pytest.raises(providers.ForgeError) as err:
        forge.provider().create_issue(title="x", body="", state="open", labels=[], assignees=[])
    assert "tok-secret" not in str(err.value)


# ---- idempotence and loop prevention -------------------------------------------------


def test_provider_issue_imports_signed_and_repeated_runs_write_nothing(tmp_path, forge, sync_key):
    checkout = connected(tmp_path, forge)
    forge.add_issue(7, labels=["bug"], assignees=["dana"])
    forge.add_comment(7, "Me too")

    first = run(checkout, forge, sync_key)
    rec = record(checkout, 7)
    assert first["completed"] and first["counts"]["toForkmesh"]["created"] == 2
    assert rec["title"] == "Crash on start" and rec["labels"] == ["bug"]
    assert rec["assignees"] == ["github:dana" if forge.kind == "github" else "gitea:dana"]
    assert {ev["authorName"] for ev in rec["events"]} == {f"{forge.kind}:alice", f"{forge.kind}:bob"}
    assert all(verifies(7, ev) for ev in rec["events"])
    assert forge.writes() == []

    run(checkout, forge, sync_key)
    settled = snapshot(checkout)
    for _ in range(3):
        again = run(checkout, forge, sync_key)
        assert again["counts"]["toForkmesh"] == {"created": 0, "updated": 0, "deleted": 0, "linked": 0}
        assert again["counts"]["toProvider"] == {"created": 0, "updated": 0, "deleted": 0, "linked": 0}
    assert forge.writes() == []
    assert snapshot(checkout) == settled
    assert again["measures"]["convergenceRate"] == 1.0 and again["status"]["state"] == "verified"


def test_forkmesh_issue_exports_once_and_its_echo_never_duplicates(tmp_path, forge, sync_key, native_key):
    checkout = connected(tmp_path, forge)
    local_issue(checkout, 3, native_key, comments=["First native comment"])

    run(checkout, forge, sync_key)
    assert len(forge.issues) == 1 and len(forge.comments) == 1
    [remote] = forge.issues.values()
    assert "forkmesh-forge-sync:v1" in remote["body"] and "Synced from ForkMesh · carol" in remote["body"]

    for _ in range(3):
        run(checkout, forge, sync_key)
    assert len(forge.issues) == 1 and len(forge.comments) == 1
    assert [n for n in fs.LocalIssues(checkout, sync_key).records] == [3]
    assert len(fs.fold(record(checkout, 3))["comments"]) == 1
    assert [w for w in forge.writes() if w[0] == "POST" and w[1].endswith("/issues")] == [
        ("POST", f"/repos/{REPO}/issues")]


def test_issues_opened_before_connecting_are_not_exported(tmp_path, forge, sync_key, native_key):
    checkout = connected(tmp_path, forge)
    local_issue(checkout, 1, native_key, ts=T0 - 10)
    run(checkout, forge, sync_key)
    assert forge.issues == {}


def test_lost_ledger_relinks_by_marker_instead_of_duplicating(tmp_path, forge, sync_key, native_key):
    checkout = connected(tmp_path, forge)
    local_issue(checkout, 4, native_key, comments=["native"])
    forge.add_issue(9, body="from provider")
    run(checkout, forge, sync_key)
    before = (len(forge.issues), len(forge.comments), sorted(fs.LocalIssues(checkout, sync_key).records))

    ledger = fs.load_ledger(checkout)
    ledger["issues"] = {}
    fs.save_ledger(checkout, ledger)
    result = run(checkout, forge, sync_key)
    run(checkout, forge, sync_key)

    assert (len(forge.issues), len(forge.comments), sorted(fs.LocalIssues(checkout, sync_key).records)) == before
    assert result["counts"]["toForkmesh"]["linked"] >= 2
    assert len(fs.fold(record(checkout, 4))["comments"]) == 1


# ---- edits, conflicts, deletions ----------------------------------------------------


def test_edits_flow_in_both_directions(tmp_path, forge, sync_key, native_key):
    checkout = connected(tmp_path, forge)
    forge.add_issue(5, labels=["bug"])
    run(checkout, forge, sync_key)

    forge.edit_issue(5, title="Crash on start (macOS)", state="closed")
    run(checkout, forge, sync_key)
    rec = record(checkout, 5)
    assert rec["title"] == "Crash on start (macOS)" and rec["status"] == "closed"

    opener = fs.fold(rec)["openerId"]
    local_edit(checkout, 5, native_key, "edit", "native-edit", forge.tick(),
               target=opener, body="Clearer steps", attachments=[])
    local_edit(checkout, 5, native_key, "labels", "native-labels", forge.tick(), labels=["bug", "macos"])
    run(checkout, forge, sync_key)
    assert forge.issues[5]["body"] == "Clearer steps"
    assert sorted(forge.issues[5]["labels"]) == ["bug", "macos"]

    settled = len(forge.writes())
    run(checkout, forge, sync_key)
    run(checkout, forge, sync_key)
    assert len(forge.writes()) == settled


def test_concurrent_edits_become_a_visible_conflict_and_resolve_recovers(tmp_path, forge, sync_key, native_key):
    checkout = connected(tmp_path, forge)
    forge.add_issue(6, body="original")
    run(checkout, forge, sync_key)
    opener = fs.fold(record(checkout, 6))["openerId"]

    forge.edit_issue(6, body="provider wording")
    local_edit(checkout, 6, native_key, "edit", "native-edit", forge.tick(),
               target=opener, body="forkmesh wording", attachments=[])
    result = run(checkout, forge, sync_key)
    assert result["status"]["state"] == "conflicted"
    assert result["conflicts"] == [{"issue": 6, "field": "body", "forkmesh": "forkmesh wording",
                                    "provider": "provider wording",
                                    "detectedAt": result["conflicts"][0]["detectedAt"]}]
    assert forge.issues[6]["body"] == "provider wording"
    assert fs.fold(record(checkout, 6))["body"] == "forkmesh wording"
    # Still visible, still untouched, on every later run.
    assert run(checkout, forge, sync_key)["conflicts"][0]["field"] == "body"
    assert fs.main(["--checkout", str(checkout), "status"]) == 1

    fs.resolve(checkout, issue=6, name="body", take="forkmesh")
    result = run(checkout, forge, sync_key)
    assert result["conflicts"] == [] and forge.issues[6]["body"] == "forkmesh wording"
    assert fs.load_ledger(checkout)["history"][-1]["discarded"] == "provider wording"


def test_published_source_of_truth_rule_settles_and_keeps_the_discarded_value(tmp_path, forge, sync_key, native_key):
    checkout = connected(tmp_path, forge, sourceOfTruth={"title": "provider"})
    forge.add_issue(8, title="Old")
    run(checkout, forge, sync_key)
    forge.edit_issue(8, title="Provider title")
    local_edit(checkout, 8, native_key, "title", "native-title", forge.tick(), title="ForkMesh title")
    result = run(checkout, forge, sync_key)
    assert result["resolvedByRule"] == 1 and result["conflicts"] == []
    assert record(checkout, 8)["title"] == "Provider title"
    assert fs.load_ledger(checkout)["history"][-1] == {
        "at": forge.now, "remoteId": str(forge.issues[8]["id"]), "field": "title",
        "took": "provider", "discarded": "ForkMesh title"}


def test_comment_deletions_propagate_both_ways(tmp_path, forge, sync_key, native_key):
    checkout = connected(tmp_path, forge)
    forge.add_issue(2)
    remote_comment = forge.add_comment(2, "spam")
    run(checkout, forge, sync_key)
    local_edit(checkout, 2, native_key, "comment", "native-c", forge.tick(), body="keep?", attachments=[])
    run(checkout, forge, sync_key)
    assert len(forge.comments) == 2

    del forge.comments[remote_comment["id"]]
    forge.edit_issue(2)
    result = run(checkout, forge, sync_key)
    assert result["counts"]["toForkmesh"]["deleted"] == 1
    events = record(checkout, 2)["events"]
    [delete] = [ev for ev in events if ev["type"] == "delete"]
    assert delete["target"] == f"forge-{forge.kind}-comment-{remote_comment['id']}" and verifies(2, delete)

    local_edit(checkout, 2, native_key, "delete", "native-del", forge.tick(), target="native-c")
    run(checkout, forge, sync_key)
    assert forge.comments == {}
    assert run(checkout, forge, sync_key)["conflicts"] == []


def test_deletion_racing_an_edit_is_a_conflict_not_a_loss(tmp_path, forge, sync_key, native_key):
    checkout = connected(tmp_path, forge)
    forge.add_issue(2)
    comment = forge.add_comment(2, "draft")
    run(checkout, forge, sync_key)
    eid = f"forge-{forge.kind}-comment-{comment['id']}"
    local_edit(checkout, 2, native_key, "edit", "native-edit", forge.tick(), target=eid,
               body="improved", attachments=[])
    del forge.comments[comment["id"]]
    forge.edit_issue(2)
    result = run(checkout, forge, sync_key)
    assert [(c["field"], c["forkmesh"], c["provider"]) for c in result["conflicts"]] == [
        (f"comment:{eid}", "improved", "deleted")]
    assert fs.fold(record(checkout, 2))["comments"][eid]["body"] == "improved"

    fs.resolve(checkout, issue=2, name=f"comment:{eid}", take="forkmesh")
    run(checkout, forge, sync_key)
    assert [c["body"].split("\n\n<sub>")[0] for c in forge.comments.values()] == ["improved"]


def test_force_updated_provider_issue_is_never_overwritten_into_the_old_record(tmp_path, forge, sync_key):
    checkout = connected(tmp_path, forge)
    forge.add_issue(1, title="Original")
    run(checkout, forge, sync_key)

    del forge.issues[1]
    forge.add_issue(1, title="Replacement", author="mallory")
    result = run(checkout, forge, sync_key)
    assert record(checkout, 1)["title"] == "Original"
    assert record(checkout, 2)["title"] == "Replacement"
    assert [(c["issue"], c["field"]) for c in result["conflicts"]] == [(1, "issue")]
    assert any("stored as #2" in w for w in result["warnings"])

    fs.resolve(checkout, issue=1, name="issue", take="provider")
    assert run(checkout, forge, sync_key)["conflicts"] == []


# ---- failures -----------------------------------------------------------------------


def test_rate_limit_is_retried_and_counted_as_a_retry_success(tmp_path, forge, sync_key):
    checkout = connected(tmp_path, forge)
    forge.add_issue(1)
    forge.fail(429, headers={"Retry-After": "2"}, match="GET")
    result = run(checkout, forge, sync_key)
    assert result["completed"] and forge.sleeps == [2.0]
    assert result["measures"]["retries"] == 1 and result["measures"]["retrySuccesses"] == 1


def test_exhausted_rate_limit_stops_with_a_retryable_quota_error(tmp_path, forge, sync_key):
    checkout = connected(tmp_path, forge)
    forge.add_issue(1)
    forge.fail(403, times=10, headers={"X-RateLimit-Remaining": "0"})
    result = run(checkout, forge, sync_key)
    assert not result["completed"] and result["error"] == "quota_exceeded"
    assert result["status"]["state"] == "failed"


def test_outage_keeps_progress_and_the_next_run_converges_without_duplicates(tmp_path, forge, sync_key, native_key):
    checkout = connected(tmp_path, forge)
    forge.add_issue(1)
    local_issue(checkout, 10, native_key, comments=["one"])
    forge.fail(503, times=100, match="POST")
    down = run(checkout, forge, sync_key)
    assert not down["completed"] and down["error"] == "provider_unavailable"
    assert record(checkout, 1) is not None  # the import that could run was kept
    assert fs.load_ledger(checkout)["issues"]["10"]["reexport"] is True
    assert fs.main(["--checkout", str(checkout), "status"]) == 0

    forge.failures.clear()
    up = run(checkout, forge, sync_key)
    run(checkout, forge, sync_key)
    assert up["completed"] and len(forge.issues) == 2 and len(forge.comments) == 1
    assert run(checkout, forge, sync_key)["status"]["state"] == "verified"


def test_unauthorized_token_fails_fast_without_retrying(tmp_path, forge, sync_key):
    checkout = connected(tmp_path, forge)
    forge.fail(401, times=10)
    result = run(checkout, forge, sync_key)
    assert result["error"] == "unauthorized" and forge.sleeps == [] and len(forge.log) == 1


# ---- identities ---------------------------------------------------------------------


def test_identity_links_translate_assignees_but_never_authorship(tmp_path, forge, sync_key, native_key):
    p = forge.kind
    checkout = connected(tmp_path, forge, identities={f"{p}:dana": "dana-fm"})
    forge.add_issue(1, author="dana", assignees=["dana"])
    run(checkout, forge, sync_key)
    rec = record(checkout, 1)
    assert rec["assignees"] == ["dana-fm"]
    assert rec["authorName"] == f"{p}:dana" and rec["events"][0]["author"] == sync_key.public_key

    local_edit(checkout, 1, native_key, "assignees", "native-assign", forge.tick(),
               assignees=["dana-fm", "unlinked-fm"])
    run(checkout, forge, sync_key)
    assert forge.issues[1]["assignees"] == ["dana"]
    assert run(checkout, forge, sync_key)["conflicts"] == []


def test_ambiguous_identity_links_are_ignored_and_reported(tmp_path, forge, sync_key):
    p = forge.kind
    checkout = connected(tmp_path, forge, identities={
        f"{p}:dana": "shared", f"{p}:erin": "shared", f"{p}:Frank": "f1", f"{p}:frank": "f2"})
    forge.add_issue(1, assignees=["dana", "frank"])
    result = run(checkout, forge, sync_key)
    assert sorted(record(checkout, 1)["assignees"]) == [f"{p}:dana", f"{p}:frank"]
    assert len(result["warnings"]) == 2


def test_forge_display_names_cannot_collide_with_accounts():
    assert all(":" not in adapter.name for adapter in providers.ADAPTERS.values())
    to_account, _, _ = fs.identity_links({"identities": {"github:x": "acct", "gitea:x": "other"}}, "github")
    assert to_account == {"x": "acct"}


# ---- controls, lag and telemetry -------------------------------------------------------


def test_pause_stops_all_provider_traffic_until_resumed(tmp_path, forge, sync_key):
    checkout = connected(tmp_path, forge)
    forge.add_issue(1)
    fs.set_paused(checkout, True, "migrating labels")
    paused = run(checkout, forge, sync_key)
    assert forge.log == [] and paused["status"]["state"] == "paused"
    assert paused["pausedReason"] == "migrating labels"

    fs.set_paused(checkout, False)
    assert run(checkout, forge, sync_key)["completed"] and record(checkout, 1)


def test_lag_is_measured_against_the_published_slo(tmp_path, forge, sync_key):
    checkout = connected(tmp_path, forge)
    run(checkout, forge, sync_key)
    ledger = fs.load_ledger(checkout)
    assert fs.build_report(ledger, forge.now)["measures"]["sloMet"] is True
    late = fs.build_report(ledger, forge.now + fs.DEFAULT_SLO_MS + 1)["measures"]
    assert late["lagMs"] == fs.DEFAULT_SLO_MS + 1 and late["sloMet"] is False


def test_report_is_privacy_safe_and_markdown_names_conflicts(tmp_path, forge, sync_key):
    checkout = connected(tmp_path, forge)
    forge.fail(503, times=10)
    result = run(checkout, forge, sync_key)
    text = json.dumps(result) + fs.markdown(result)
    assert "tok-secret" not in text
    assert result["status"]["schema"] == "forkmesh-sync-status-v1"


def test_mirror_mode_imports_every_issue_verbatim_in_any_language(tmp_path, forge, sync_key):
    checkout = connected(tmp_path, forge, mode="mirror")
    long_body = ("Отчёт о блокировке. 关于封锁的报告。 گزارش فیلترینگ. " * 2000).strip()
    forge.add_issue(1, title="ایران: اختلال گسترده در اینترنت", body=long_body, author="روزبه")
    forge.add_issue(2, title="GFW 封锁 TLS 1.3 ESNI", body="闭合问题\r\n  缩进 ✓", state="closed",
                    labels=["China", "Censorship-Measurement", "a", "b", "c", "d", "e", "f", "g"])
    forge.add_comment(1, "שלום עולם 🌍 " * 900, author="alice")
    forge.add_comment(2, "Türkmenistan: ähli VPN petiklendi", author="bob")
    run(checkout, forge, sync_key)

    one, two = record(checkout, 1), record(checkout, 2)
    assert one["title"] == "ایران: اختلال گسترده در اینترنت" and fs.fold(one)["body"] == long_body
    assert fs.fold(one)["authorName"] == f"{forge.kind}:روزبه"
    assert [c["body"] for c in fs.fold(one)["comments"].values()] == ["שלום עולם 🌍 " * 900]
    assert two["status"] == "closed" and fs.fold(two)["body"] == "闭合问题\n  缩进 ✓"
    assert len(fs.fold(two)["labels"]) == 9
    assert all(verifies(1, ev) for ev in one["events"])
    assert forge.writes() == []


def test_mirror_mode_never_writes_to_the_provider_and_the_provider_always_wins(
        tmp_path, forge, sync_key, native_key):
    checkout = connected(tmp_path, forge, mode="mirror")
    forge.add_issue(3)
    kept = forge.add_comment(3, "original")
    gone = forge.add_comment(3, "to be removed")
    run(checkout, forge, sync_key)

    opener = fs.fold(record(checkout, 3))["openerId"]
    cid = f"forge-{forge.kind}-comment-{kept['id']}"
    local_edit(checkout, 3, native_key, "edit", "native-edit", forge.tick(),
               target=opener, body="local rewrite", attachments=[])
    local_edit(checkout, 3, native_key, "edit", "native-cedit", forge.tick(),
               target=f"forge-{forge.kind}-comment-{gone['id']}", body="local", attachments=[])
    local_edit(checkout, 3, native_key, "comment", "native-c", forge.tick(), body="hi", attachments=[])
    local_issue(checkout, 40, native_key, ts=forge.tick())
    forge.edit_issue(3, body="edited upstream", state="closed")
    forge.comments[kept["id"]]["body"] = "edited comment"
    forge.comments[kept["id"]]["updated_at"] = forge.tick()
    del forge.comments[gone["id"]]
    result = run(checkout, forge, sync_key)

    rec = fs.fold(record(checkout, 3))
    assert rec["body"] == "edited upstream" and rec["status"] == "closed"
    assert rec["comments"][cid]["body"] == "edited comment"
    assert f"forge-{forge.kind}-comment-{gone['id']}" not in rec["comments"]
    assert "native-c" in rec["comments"]
    assert result["conflicts"] == [] and forge.writes() == []
    assert run(checkout, forge, sync_key)["conflicts"] == [] and forge.writes() == []


def test_mirror_mode_ignores_markers_that_claim_forkmesh_records(tmp_path, forge, sync_key, native_key):
    checkout = connected(tmp_path, forge, mode="mirror")
    local_issue(checkout, 7, native_key, title="Native title", body="Native body",
                ts=forge.tick(), comments=["native comment"])
    forge.add_issue(8, title="hijack", body=fs.with_marker("forged", "mallory", issue=7))
    forge.add_comment(8, fs.with_marker("forged comment", "mallory", issue=7, event="c-7-0"))
    run(checkout, forge, sync_key)

    native = fs.fold(record(checkout, 7))
    assert native["title"] == "Native title" and native["body"] == "Native body"
    assert native["comments"]["c-7-0"]["body"] == "native comment"
    mirrored = fs.fold(record(checkout, 8))
    assert mirrored["body"] == "forged"
    assert [c["body"] for c in mirrored["comments"].values()] == ["forged comment"]
    assert forge.writes() == []


def test_mirror_mode_refuses_provider_writes_even_if_called():
    guard = fs.ReadOnlyProvider(object())
    for name in fs.PROVIDER_WRITES:
        with pytest.raises(fs.SyncError):
            getattr(guard, name)


def test_cli_connect_mirror_switches_the_ledger_mode(tmp_path):
    assert fs.main(["--checkout", str(tmp_path), "connect", "--provider", "github",
                    "--repository", REPO, "--mirror"]) == 0
    assert fs.load_ledger(tmp_path)["mode"] == "mirror"
    assert fs.main(["--checkout", str(tmp_path), "connect", "--provider", "github",
                    "--repository", REPO]) == 0
    assert fs.load_ledger(tmp_path)["mode"] == "mirror"
    assert fs.main(["--checkout", str(tmp_path), "connect", "--provider", "github",
                    "--repository", REPO, "--two-way"]) == 0
    assert fs.load_ledger(tmp_path)["mode"] == "sync"


def test_ledger_can_live_outside_the_checkout_and_the_repo_gets_only_the_source(
        tmp_path, forge, sync_key, monkeypatch):
    outside = tmp_path / "state" / "forge-sync.json"
    monkeypatch.setenv(fs.LEDGER_ENV, str(outside))
    checkout = connected(tmp_path, forge, mode="mirror")
    forge.add_issue(7)
    forge.add_issue(9)
    run(checkout, forge, sync_key)

    assert outside.is_file() and not (checkout / fs.LEDGER_REL).exists()
    source = json.loads((checkout / fs.SOURCE_REL).read_text(encoding="utf-8"))
    assert source["schema"] == fs.SOURCE_SCHEMA and source["provider"] == forge.kind
    assert source["repository"] == REPO and source["renumbered"] == {}
    assert source["issueUrl"].endswith(f"{REPO}/issues/{{number}}")
    before = snapshot(checkout)
    run(checkout, forge, sync_key)
    assert snapshot(checkout) == before


def test_source_lists_only_issues_whose_number_differs():
    ledger = {"provider": "github", "repository": REPO, "issues": {
        "5": {"remoteNumber": 5}, "12": {"remoteNumber": 3}, "40": {"origin": "forkmesh"}}}
    record = fs.source_record(ledger)
    assert record["renumbered"] == {"12": 3}
    assert record["issueUrl"] == f"https://github.com/{REPO}/issues/{{number}}"
    assert fs.source_record({"provider": "gitea", "repository": REPO, "issues": {}}) is None


def test_cli_ledger_flag_keeps_the_ledger_out_of_the_checkout(tmp_path, monkeypatch):
    monkeypatch.delenv(fs.LEDGER_ENV, raising=False)
    outside = tmp_path / "srv" / "ledger.json"
    checkout = tmp_path / "repo"
    checkout.mkdir()
    assert fs.main(["--checkout", str(checkout), "--ledger", str(outside), "connect",
                    "--provider", "github", "--repository", REPO, "--mirror"]) == 0
    assert outside.is_file() and not (checkout / fs.LEDGER_REL).exists()


def test_cli_rejects_unknown_resolution_and_unconnected_checkout(tmp_path, capsys):
    assert fs.main(["--checkout", str(tmp_path), "status"]) == 2
    assert "connect" in capsys.readouterr().err


def test_documentation_covers_the_engine_and_is_indexed():
    doc = (ROOT / "www/docs/forge-sync.md").read_text(encoding="utf-8")
    for phrase in ("forge-sync.json", "sourceOfTruth", "resolve", "pause", "Gitea", "SLO",
                   "rate limit", "identities", "--mirror", "--ledger", "forge-source.json"):
        assert phrase in doc, phrase
    assert "(forge-sync.md)" in (ROOT / "www/docs/README.md").read_text(encoding="utf-8")
