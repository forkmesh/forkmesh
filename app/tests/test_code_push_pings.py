"""Code pushed to a repository reaches the phone: the code_push ping.

A push becomes observable to this relay in exactly two places — an
owner-signed catalog publication whose refs moved, and a hosted repository's
receive-pack pin — and both call notify_code_push. These tests run the real
notify_code_push and enqueue_notification from entry.py against an in-memory
notifications table, so the storm guard's merge and read-state handling are
exercised on the rows themselves, and then the real APNs payload rendering for
what the phone is sent.
"""

import ast
import asyncio
import importlib
import json
import re
import sys
import types
from pathlib import Path
from urllib.parse import quote

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY = SRC / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
CATALOG_ROUTES = (SRC / "catalog_routes.py").read_text(encoding="utf-8")
HOSTED_GIT = (SRC / "hosted_git.py").read_text(encoding="utf-8")
CATALOG_TEXT = (SRC / "catalog.py").read_text(encoding="utf-8")

sys.path.insert(0, str(SRC))
import organization_rbac  # noqa: E402  (pure module, no Worker imports)

FUNCS = {
    "notify_code_push", "code_push_ping_text", "code_push_merge",
    "code_push_slot_keys", "_code_push_pending", "_org_repo_readers",
    "enqueue_notification", "notification_payload", "team_permission_rank",
    "push_account_apns",
}
CONSTANTS = {
    "CODE_PUSH_WINDOW_MS", "CODE_PUSH_MAX_ORG_RECIPIENTS",
    "CODE_PUSH_OTHER_REFS", "NOTIFICATION_KINDS", "NOTIFICATION_BODY_MAX",
    "SECURITY_ALERT_BODY_MAX",
    "NOTIFICATION_RETAIN_MS", "MAX_NOTIFICATIONS_PER_RECIPIENT",
    "TEAM_PERMISSIONS",
}
ALERT_FUNCS = {
    "clean_string", "_notification_display_repo",
    "_notification_digest_title", "_notification_item_record",
    "_notification_item_path", "_notification_stored_path",
    "_notification_code_push_tail", "_push_alert_item",
}
ALERT_CONSTANTS = {
    "MAX_NODE_NAME", "MAX_AVATAR_B64", "MAX_AVATAR_BYTES", "AVATAR_B64_RE",
    "NOTIFICATION_ITEM_URL_SEGMENTS", "NOTIFICATION_HREF_RE",
    "PUSH_ALERT_AVATAR_MAX_B64", "PUSH_ALERT_BODY_MAX",
}


def _select(text, funcs, constants):
    tree = ast.parse(text, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in funcs):
            selected.append(node)
        elif (isinstance(node, ast.Assign) and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id in constants):
            selected.append(node)
    names = {getattr(n, "name", None) or n.targets[0].id for n in selected}
    assert names >= funcs | constants, sorted((funcs | constants) - names)
    return ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))


def _alert_namespace():
    ns = {"re": re, "quote": quote}
    exec(compile(_select(CATALOG_TEXT + "\n" + ENTRY_TEXT, ALERT_FUNCS,
                         ALERT_CONSTANTS), str(ENTRY), "exec"), ns)
    return ns


class FakeRuntime:
    """The Worker primitives notify_code_push touches, in memory."""

    def __init__(self, now=1_800_000_000_000, org=(), muted=None):
        self.now = now
        self.rows = {}          # dedupe_bi -> {recipient_bi, ts, read_at, data}
        self.org = list(org)    # [(name, role)]
        self.muted = dict(muted or {})
        self.events = []        # (recipient, topic, push_record or None)
        self.org_lookups = 0
        runtime = self

        async def d1_run(_env, sql, *args):
            if sql.lstrip().startswith("INSERT INTO notifications"):
                dedupe_bi, recipient_bi, ts, read_at, data = args
                row = runtime.rows.get(dedupe_bi)
                if row is None:
                    runtime.rows[dedupe_bi] = {
                        "recipient_bi": recipient_bi, "ts": ts,
                        "read_at": read_at, "data": data}
                else:  # ON CONFLICT keeps read_at
                    row.update(recipient_bi=recipient_bi, ts=ts, data=data)

        async def d1_all(_env, sql, *args):
            if "FROM org_repos" in sql:
                runtime.org_lookups += 1
                return [{"name": n, "role": r} for n, r in runtime.org]
            assert "FROM notifications" in sql and "dedupe_bi IN" in sql
            recipient_bi, *wanted = args
            return [dict(row, dedupe_bi=bi) for bi, row in runtime.rows.items()
                    if bi in wanted and row["recipient_bi"] == recipient_bi]

        async def blind_index(_env, value):
            return "bi:" + value

        async def encrypt_row(_env, payload):
            return json.dumps(payload)

        async def decrypt_row(_env, data):
            return json.loads(data) if data else None

        async def notify_account_event(_env, recipient, topic, record=None):
            runtime.events.append((recipient, topic, record))

        async def noop(*_args, **_kwargs):
            return None

        async def ping_delivery_settings(_env, recipient, _now):
            return set(runtime.muted.get(recipient, ())), False

        self.ns = {
            "re": re, "quote": quote,
            "Date": types.SimpleNamespace(now=lambda: runtime.now),
            "d1_run": d1_run, "d1_all": d1_all,
            "blind_index": blind_index, "encrypt_row": encrypt_row,
            "decrypt_row": decrypt_row,
            "notify_account_event": notify_account_event,
            "_queue_instant_notification": noop, "ensure_schema": noop,
            "_ping_delivery_settings": ping_delivery_settings,
            "_org_rbac": organization_rbac,
            "clean_string": lambda value, limit=240: (
                str(value).strip()[:limit] if value is not None else ""),
            "valid_node_name": lambda value: bool(
                re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}", value or "")),
            "repo_web_href": lambda owner, repo: (
                "/" + quote(owner) + "/" + quote(repo)),
            "MAX_NODE_NAME": 63, "MAX_REPO_SEGMENT": 100,
        }
        exec(compile(_select(ENTRY_TEXT, FUNCS, CONSTANTS), str(ENTRY),
                     "exec"), self.ns)

    def push(self, owner="alice", repo="atlas", **kwargs):
        return asyncio.run(self.ns["notify_code_push"](
            object(), owner, repo, **kwargs))

    def rows_for(self, recipient):
        found = []
        for bi, row in self.rows.items():
            if row["recipient_bi"] == "bi:" + recipient:
                found.append(dict(json.loads(row["data"]), dedupe_bi=bi,
                                  read_at=row["read_at"], row_ts=row["ts"]))
        return sorted(found, key=lambda r: r["row_ts"])

    def banners(self, recipient=None):
        return [(who, record) for who, topic, record in self.events
                if topic == "pings" and record is not None
                and (recipient is None or who == recipient)]

    def mark_all_read(self, recipient):
        for row in self.rows.values():
            if row["recipient_bi"] == "bi:" + recipient:
                row["read_at"] = self.now


PUSH = dict(branch="main", commit="c" * 40, commits=3,
            subject="Fix relay retries\n\nLong description",
            pusher="alice-laptop", key="c" * 40, refs_digest="d" * 64)


# --- one push, one ping ------------------------------------------------------

def test_one_push_files_one_ping_that_banners_the_owners_phone():
    rt = FakeRuntime()
    assert rt.push(**PUSH) == 1
    rows = rt.rows_for("alice")
    assert len(rows) == 1
    ping = rows[0]
    assert ping["kind"] == "code_push"
    assert ping["title"] == "Pushed to alice/atlas"
    assert ping["body"] == ("alice-laptop: 3 commits on main — "
                            "Fix relay retries")
    assert ping["href"] == "/alice/atlas/commits/main"
    assert ping["repo"] == "alice/atlas"
    # No actor: naming the owner as actor would make enqueue_notification
    # drop the owner's own push, and their phone is the point.
    assert ping["actor"] == ""
    assert ping["meta"]["commits"] == 3 and ping["meta"]["pushes"] == 1
    assert len(rt.banners("alice")) == 1


def test_the_same_state_observed_twice_is_not_a_second_push():
    rt = FakeRuntime()
    rt.push(**PUSH)
    rt.now += 5_000
    assert rt.push(**PUSH) == 0
    assert len(rt.rows_for("alice")) == 1
    assert rt.rows_for("alice")[0]["meta"]["commits"] == 3
    assert len(rt.banners()) == 1


def test_text_falls_back_to_what_the_observation_carried():
    text = FakeRuntime().ns["code_push_ping_text"]
    # One commit, no subject: the short sha.
    assert text("alice", "atlas", branch="feature/x", commits=1,
                commit="ABCDEF1234" + "0" * 30, pusher="bot")[1] == (
        "bot: 1 commit on feature/x — abcdef1")
    # Unknown count.
    assert text("alice", "atlas", branch="main", commit="a" * 40)[1] == (
        "alice: new commits on main — aaaaaaa")
    # A branch with a slash stays one path segment for the deep link.
    assert text("alice", "atlas", branch="feature/x")[2] == (
        "/alice/atlas/commits/feature%2Fx")
    # A feature branch or tag moved and the catalog cannot say which.
    title, body, href = text("alice", "atlas", pusher="mona",
                             refs_digest="e" * 64)
    assert title == "Pushed to alice/atlas"
    assert body == "mona: a branch or tag was updated (refs " + "e" * 12 + ")"
    assert href == "/alice/atlas/commits"


# --- recipients --------------------------------------------------------------

def test_org_members_who_can_read_hear_guests_do_not():
    rt = FakeRuntime(org=[("mona", "member"), ("jett", "maintainer"),
                          ("audra", "auditor"), ("gus", "guest"),
                          ("alice", "owner")])
    assert rt.push(**PUSH) == 4
    heard = sorted({who for who, _record in rt.banners()})
    assert heard == ["alice", "audra", "jett", "mona"]
    assert rt.rows_for("gus") == []


def test_an_identified_pusher_is_not_told_about_their_own_push_but_the_owner_is():
    rt = FakeRuntime(org=[("mona", "member"), ("jett", "member")])
    # mona pushed over hosted git: jett and the owner hear, mona does not.
    rt.push(key="d" * 64, refs_digest="d" * 64, pusher="mona",
            pusher_account="mona")
    assert sorted({who for who, _r in rt.banners()}) == ["alice", "jett"]
    # The owner's own push still reaches the owner's phone.
    rt2 = FakeRuntime()
    rt2.push(key="d" * 64, refs_digest="d" * 64, pusher="alice",
             pusher_account="alice")
    assert [who for who, _r in rt2.banners()] == ["alice"]


def test_switching_code_pushes_off_files_nothing_and_buzzes_nothing():
    rt = FakeRuntime(org=[("mona", "member")], muted={"alice": {"code_push"}})
    assert rt.push(**PUSH) == 1
    assert rt.rows_for("alice") == []
    assert [who for who, _r in rt.banners()] == ["mona"]
    # Muting another kind does not mute code pushes.
    rt = FakeRuntime(muted={"alice": {"repo_activity"}})
    assert rt.push(**PUSH) == 1


# --- storm guard ------------------------------------------------------------

def test_pushes_inside_the_window_fold_into_the_unread_ping():
    rt = FakeRuntime()
    rt.push(**PUSH)
    rt.now += 20_000
    rt.push(**dict(PUSH, commit="f" * 40, key="f" * 40, commits=2,
                   subject="Second"))
    rt.now += 30_000
    rt.push(**dict(PUSH, commit="9" * 40, key="9" * 40, commits=1,
                   subject="Third"))
    rows = rt.rows_for("alice")
    assert len(rows) == 1
    ping = rows[0]
    assert ping["meta"]["commits"] == 6
    assert ping["meta"]["pushes"] == 3
    assert ping["body"] == "alice-laptop: 6 commits on main — Third"
    assert ping["read_at"] == 0
    # Moved to the top of the inbox, one banner only.
    assert ping["row_ts"] == rt.now
    assert len(rt.banners()) == 1
    # The later pushes still refreshed the bell over the socket.
    assert len(rt.events) == 3


def test_after_the_window_the_next_push_banners_again():
    rt = FakeRuntime()
    rt.push(**PUSH)
    rt.now += 60_000
    rt.push(**dict(PUSH, commit="f" * 40, key="f" * 40, commits=1))
    assert len(rt.rows_for("alice")) == 2
    assert len(rt.banners()) == 2


def test_a_read_ping_is_not_reopened_the_push_is_filed_silently():
    rt = FakeRuntime()
    rt.push(**PUSH)
    rt.mark_all_read("alice")
    rt.now += 10_000
    rt.push(**dict(PUSH, commit="f" * 40, key="f" * 40, commits=2))
    rows = rt.rows_for("alice")
    assert len(rows) == 2
    assert rows[0]["read_at"] and rows[1]["read_at"] == 0
    assert rows[1]["meta"]["commits"] == 2
    assert len(rt.banners()) == 1


def test_the_window_is_per_branch_and_per_repository():
    rt = FakeRuntime()
    rt.push(**PUSH)
    rt.push(**dict(PUSH, branch="dev", key="1" * 40))
    rt.push(repo="other", **PUSH)
    assert len(rt.banners()) == 3


def test_an_unknown_count_makes_the_merged_count_unknown():
    merge = FakeRuntime().ns["code_push_merge"]
    assert merge({"commits": 3}, 2) == 5
    assert merge({"commits": 3}, 0) == 0
    assert merge({}, 2) == 0


# --- what the phone is sent --------------------------------------------------

@pytest.fixture()
def apns(monkeypatch):
    js = types.ModuleType("js")
    js.Date = types.SimpleNamespace(now=lambda: 1_700_000_000_000)
    js.Uint8Array = types.SimpleNamespace(new=lambda data: data)
    js.crypto = types.SimpleNamespace(subtle=None)
    js.Object = types.SimpleNamespace(fromEntries=dict)
    ffi = types.ModuleType("pyodide.ffi")
    ffi.to_js = lambda value, dict_converter=None: value
    pyodide = types.ModuleType("pyodide")
    pyodide.ffi = ffi
    monkeypatch.setitem(sys.modules, "js", js)
    monkeypatch.setitem(sys.modules, "pyodide", pyodide)
    monkeypatch.setitem(sys.modules, "pyodide.ffi", ffi)
    sys.modules.pop("apns", None)
    module = importlib.import_module("apns")
    yield module
    sys.modules.pop("apns", None)


def _phone(rt, apns_module, configured=True):
    """Run the real push_account_apns for each banner the pushes raised."""
    alert_ns = _alert_namespace()
    sent = []
    lookups = []

    async def _apns_account_tokens(_env, owner):
        lookups.append(owner)
        return [{"token_bi": "t", "token": "ab" * 32,
                 "environment": "production"}]

    async def _apns_ping_alert(_env, record):
        item = alert_ns["_push_alert_item"](record, {}, {})
        return {"title": item["title"], "subtitle": item["subtitle"],
                "body": item["body"], "threadId": item["threadId"],
                "alertId": item["id"], "href": item["href"]}

    async def _apns_send_tokens(_env, tokens, topic, alert):
        sent.append((topic, alert))
        return len(tokens)

    rt.ns.update(_apns_module=lambda: apns_module,
                 _apns_account_tokens=_apns_account_tokens,
                 _apns_ping_alert=_apns_ping_alert,
                 _apns_send_tokens=_apns_send_tokens)
    env = types.SimpleNamespace(APP_ORIGIN="https://app.forkmesh.com")
    if configured:
        env.APNS_AUTH_KEY = "key"
        env.APNS_KEY_ID = "7LYZ4MQFW4"
        env.APNS_TEAM_ID = "742ZANCB57"
    for who, record in rt.banners():
        asyncio.run(rt.ns["push_account_apns"](env, who, "pings", record))
    return env, sent, lookups


def test_the_banner_payload_names_the_push_and_lands_on_the_branch(apns):
    rt = FakeRuntime()
    rt.push(**PUSH)
    env, sent, _lookups = _phone(rt, apns)
    (topic, alert), = sent
    assert topic == "code-push"
    payload = json.loads(apns.alert_payload(alert, topic, apns.apns_origin(env)))
    assert payload["aps"]["alert"]["title"] == "\U0001F4E4 Pushed to alice/atlas"
    assert payload["aps"]["alert"]["body"] == (
        "alice-laptop: 3 commits on main — Fix relay retries")
    assert payload["href"] == "/alice/atlas/commits/main"
    assert payload["topic"] == "code-push"
    assert payload["origin"] == "https://app.forkmesh.com"
    assert payload["alertId"]
    # A ping: every one its own banner, never collapsed.
    assert apns.collapse_id_for(topic) == ""


def test_no_push_is_attempted_when_apns_is_not_configured(apns):
    rt = FakeRuntime()
    rt.push(**PUSH)
    _env, sent, lookups = _phone(rt, apns, configured=False)
    assert sent == [] and lookups == []
    # The ping itself is still filed for the inbox.
    assert len(rt.rows_for("alice")) == 1


def test_an_alias_fronted_push_keeps_the_branch_but_not_the_backing_node():
    ns = _alert_namespace()
    item = ns["_push_alert_item"](
        {"id": "x", "kind": "code_push", "title": "Pushed to node7/atlas",
         "repo": "node7/atlas", "href": "/node7/atlas/commits/main",
         "meta": {}}, {"node7/atlas": "acme/atlas"}, {})
    assert item["href"] == "/acme/atlas/commits/main"
    # Other kinds keep landing on the repository itself.
    other = ns["_push_alert_item"](
        {"id": "y", "kind": "repo_activity", "repo": "alice/atlas",
         "href": "/alice/atlas/commits/main", "meta": {}}, {}, {})
    assert other["href"] == "/alice/atlas"


# --- the call sites ----------------------------------------------------------

def test_both_push_observations_raise_the_code_push_ping_best_effort():
    for source in (CATALOG_ROUTES, HOSTED_GIT):
        index = source.index("await notify_code_push(")
        assert "try:" in source[max(0, index - 200):index]
        assert "notify_repo_activity(" not in source
    # Only a source publication counts, so a mirror's echo is never a push.
    assert "if catalog_push_observed(" in CATALOG_ROUTES
    assert "pusher_account=pusher" in HOSTED_GIT


def test_commit_delta_is_only_trusted_on_the_same_growing_branch():
    ns = {"re": re, "clean_string": lambda v, n: str(v or "")[:n]}
    exec(compile(CATALOG_ROUTES, "catalog_routes.py", "exec"), ns)
    delta = ns["catalog_push_commit_delta"]
    assert delta({"branch": "main", "commitCount": "12"},
                 {"branch": "main", "commitCount": "9"}) == 3
    assert delta({"branch": "main", "commitCount": "9"},
                 {"branch": "main", "commitCount": "12"}) == 0
    assert delta({"branch": "dev", "commitCount": "12"},
                 {"branch": "main", "commitCount": "9"}) == 0
    assert delta({"branch": "main", "commitCount": ""},
                 {"branch": "main", "commitCount": "9"}) == 0


def test_code_pushes_is_a_switch_on_the_pings_settings_page():
    text = (SRC / "ping_settings.py").read_text(encoding="utf-8")
    assert '("code_push", "repositories", "Code pushes"),' in text
    # Filed and pushed, never mailed: a busy repository must not fill inboxes.
    email = ENTRY_TEXT[ENTRY_TEXT.index("NOTIFICATION_EMAIL_KINDS = ("):]
    assert "code_push" not in email[:email.index("NOTIFICATION_DIGEST_INTERVAL_MS")]
