#!/usr/bin/env python3
"""A push banner shows the event, not "New activity" (adhoc #199).

The alert this Worker hands Apple carries this rendering (_apns_ping_alert),
and the phone's notification service extension reads GET /api/push/alert the
moment one lands to add the portrait and badge. The composition lives
in entry.py rather than in Swift for two reasons: it is the same helper set the
digest email already uses, so a phone and an inbox describe one event
identically, and — unlike Swift on a Linux node — it can be tested here.
"""

import ast
import re
import time
from pathlib import Path
from urllib.parse import quote


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
CATALOG = ENTRY.parent / "catalog.py"
RUNTIME = ENTRY.parent / "app_runtime.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
CATALOG_TEXT = CATALOG.read_text(encoding="utf-8")

FUNCS = {
    "clean_string",
    "_notification_display_repo",
    "_notification_digest_title",
    "_notification_item_record",
    "_notification_item_path",
    "_notification_stored_path",
    "_notification_stored_tail",
    "_push_alert_item",
}
CONSTANTS = {
    "MAX_NODE_NAME",
    "MAX_AVATAR_B64",
    "MAX_AVATAR_BYTES",
    "AVATAR_B64_RE",
    "NOTIFICATION_ITEM_URL_SEGMENTS",
    "NOTIFICATION_HREF_RE",
    "PUSH_ALERT_AVATAR_MAX_B64",
    "PUSH_ALERT_BODY_MAX",
    "PUSH_ALERT_DEFAULT_ITEMS",
    "PUSH_ALERT_MAX_ITEMS",
    "PUSH_ALERT_FRESH_MS",
}


def _load():
    tree = ast.parse(CATALOG_TEXT + "\n" + ENTRY_TEXT, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in FUNCS):
            selected.append(node)
        elif (isinstance(node, ast.Assign) and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id in CONSTANTS):
            selected.append(node)
    found = {
        node.name for node in selected
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }
    assert found == FUNCS, "missing functions: %s" % sorted(FUNCS - found)
    names = {
        node.targets[0].id for node in selected
        if isinstance(node, ast.Assign)
    }
    assert names == CONSTANTS, "missing constants: %s" % sorted(
        CONSTANTS - names)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    ns = {"time": time, "re": re, "quote": quote}
    exec(compile(module, str(ENTRY), "exec"), ns)
    return ns


def _issue_ping(**overrides):
    item = {
        "id": "a" * 64,
        "kind": "subscribed",
        "title": "Issue submitted for acme/widgets",
        "body": "Search box is broken on mobile",
        "repo": "acme/widgets",
        "actor": "bob",
        "ts": 1783607520000,
        "meta": {"number": 42, "source": "issue"},
    }
    item.update(overrides)
    return item


def test_a_banner_names_the_record_the_repo_the_actor_and_the_text():
    ns = _load()
    alert = ns["_push_alert_item"](_issue_ping())
    assert alert["title"] == "\U0001F440 bob \xb7 acme/widgets #42"
    assert alert["subtitle"] == ""
    assert alert["body"] == "Search box is broken on mobile"
    assert alert["actor"] == "bob"
    assert alert["repo"] == "acme/widgets"
    assert alert["id"] == "a" * 64
    assert alert["ts"] == 1783607520000


def test_the_repository_is_named_once_not_twice():
    # The stored title reads "... for acme/widgets" so an inbox row makes
    # sense alone. A banner already prints the repository on the subtitle
    # line, and a lock screen has far less room than a list row.
    ns = _load()
    alert = ns["_push_alert_item"](_issue_ping())
    assert alert["title"].count("acme/widgets") == 1
    assert "acme/widgets" not in alert["subtitle"] + alert["body"]
    plain = ns["_push_alert_item"](_issue_ping(actor=""))
    assert "acme/widgets" not in plain["title"]
    assert plain["subtitle"].count("acme/widgets") == 1


def test_an_alias_fronted_repo_shows_the_organization_not_the_mirror():
    # Same rule issue #652 established for the digest: the stored slug is the
    # node that holds the bytes, the organization is the public address. The
    # mirror name must not survive in the title either.
    ns = _load()
    alert = ns["_push_alert_item"](
        _issue_ping(title="Issue submitted for Mirror23/ForkMesh",
                    repo="mirror23/forkmesh"),
        {"mirror23/forkmesh": "forkmesh/forkmesh"},
    )
    assert alert["title"] == "\U0001F440 bob \xb7 forkmesh/forkmesh #42"
    assert "mirror23" not in alert["title"].lower()
    plain = ns["_push_alert_item"](
        _issue_ping(title="Issue submitted for Mirror23/ForkMesh",
                    repo="mirror23/forkmesh", actor=""),
        {"mirror23/forkmesh": "forkmesh/forkmesh"},
    )
    assert plain["title"] == "\U0001F440 #42 Issue submitted"
    assert plain["subtitle"] == "forkmesh/forkmesh"
    assert alert["threadId"] == "forkmesh/forkmesh"


def test_missing_pieces_drop_out_instead_of_rendering_placeholders():
    # An older row with no actor, a system notification about no repository,
    # a kind with no body: each piece simply goes away. "None", "#0" and an
    # orphan separator are all lock-screen-visible bugs.
    ns = _load()
    alert = ns["_push_alert_item"]({
        "id": "b" * 64,
        "kind": "credits_refilled",
        "title": "Credits refilled",
        "body": "",
        "repo": "",
        "actor": "",
        "meta": {},
    })
    assert alert["title"] == "\U0001F50B Credits refilled"
    assert alert["subtitle"] == ""
    assert alert["body"] == ""
    assert alert["threadId"] == "credits_refilled"
    for field in ("title", "subtitle", "body"):
        assert "None" not in alert[field]
        assert "#0" not in alert[field]
        assert "\xb7" not in alert[field]

    solo = ns["_push_alert_item"]({"title": "Bounty funded", "actor": "carol"})
    assert solo["title"] == "\U0001F514 carol"
    assert solo["subtitle"] == ""
    assert solo["body"] == "Bounty funded"

    # And nothing at all still produces a drawable banner.
    empty = ns["_push_alert_item"]({})
    assert empty["title"] == "\U0001F514 Notification"
    assert empty["threadId"] == "forkmesh"
    assert empty["ts"] == 0


def test_a_submission_without_a_number_keeps_its_plain_headline():
    # A brand-new submission has no durable number until the owner drains it.
    ns = _load()
    alert = ns["_push_alert_item"](_issue_ping(meta={"source": "issue"}))
    assert alert["title"] == "\U0001F440 bob \xb7 acme/widgets"
    assert "#" not in alert["title"]
    plain = ns["_push_alert_item"](
        _issue_ping(meta={"source": "issue"}, actor=""))
    assert plain["title"] == "\U0001F440 Issue submitted"


def test_a_long_body_is_bounded_but_still_several_lines():
    # iOS shows about four lines collapsed and the rest when the banner is
    # pulled open, so this has to carry a real comment — and stop well short
    # of an essay, because the extension has seconds and a small heap.
    ns = _load()
    alert = ns["_push_alert_item"](_issue_ping(body="x" * 5000))
    assert len(alert["body"]) == ns["PUSH_ALERT_BODY_MAX"]
    assert ns["PUSH_ALERT_BODY_MAX"] >= 400


def test_the_actor_portrait_rides_along_and_oversize_ones_are_dropped():
    # The image becomes a UNNotificationAttachment thumbnail. Half a base64
    # blob would decode to a broken attachment and iOS then drops the picture
    # from the banner entirely, so an over-budget portrait is rejected whole.
    ns = _load()
    portrait = "iVBORw0KGgo" + "A" * 200
    alert = ns["_push_alert_item"](_issue_ping(), None, {"bob": portrait})
    assert alert["avatarPng"] == portrait

    huge = "A" * (ns["PUSH_ALERT_AVATAR_MAX_B64"] + 4)
    assert ns["_push_alert_item"](
        _issue_ping(), None, {"bob": huge})["avatarPng"] == ""
    assert ns["_push_alert_item"](
        _issue_ping(), None, {"bob": "not base64!!"})["avatarPng"] == ""
    # The byline never depends on the picture arriving.
    assert ns["_push_alert_item"](
        _issue_ping(), None, {"bob": huge})["title"] == (
            "\U0001F440 bob \xb7 acme/widgets #42")

    # A phone thumbnail may be larger than the digest's inline email portrait,
    # and still small enough to read inside an extension's budget.
    assert ns["PUSH_ALERT_AVATAR_MAX_B64"] > 12 * 1024
    assert ns["PUSH_ALERT_AVATAR_MAX_B64"] < ns["MAX_AVATAR_B64"]


def test_banners_from_one_repository_group_together():
    ns = _load()
    assert ns["_push_alert_item"](_issue_ping())["threadId"] == "acme/widgets"


def test_a_tap_lands_on_the_record_the_banner_names():
    # Issue #724: the extension keeps this on the delivered notification and
    # the app routes the tap with it, so it has to be the exact record.
    ns = _load()
    assert ns["_push_alert_item"](_issue_ping())["href"] == (
        "/acme/widgets/issues/42")
    pull = _issue_ping(meta={"number": 7, "source": "pull"})
    assert ns["_push_alert_item"](pull)["href"] == "/acme/widgets/pull/7"
    # No number yet: the repository itself.
    fresh = _issue_ping(meta={"source": "issue"})
    assert ns["_push_alert_item"](fresh)["href"] == "/acme/widgets"
    # The organization, not the mirror that holds the bytes.
    aliased = ns["_push_alert_item"](
        _issue_ping(repo="mirror23/forkmesh"),
        {"mirror23/forkmesh": "forkmesh/forkmesh"})
    assert aliased["href"] == "/forkmesh/forkmesh/issues/42"


def test_a_tap_on_a_desktop_ping_lands_on_its_run_or_agent():
    ns = _load()
    run = ns["_push_alert_item"]({
        "kind": "desktop_ping", "title": "Workflow build failed",
        "repo": "mirror23/forkmesh",
        "href": "/mirror23/forkmesh/actions/runs/41",
        "meta": {"desktopKind": "action", "runId": "41"}},
        {"mirror23/forkmesh": "forkmesh/forkmesh"})
    assert run["href"] == "/forkmesh/forkmesh/actions/runs/41"
    agent = ns["_push_alert_item"]({
        "kind": "desktop_ping", "title": "Agent #12", "repo": "jett/forkmesh",
        "href": "/jett/forkmesh/agents/12",
        "meta": {"desktopKind": "agent-done", "sessionId": "12"}})
    assert agent["href"] == "/jett/forkmesh/agents/12"


def test_a_tap_without_a_repository_uses_only_a_same_site_stored_path():
    # Stored data: a full URL or a scheme-relative one would let a ping send
    # the app somewhere else entirely.
    ns = _load()
    item = ns["_push_alert_item"]
    assert item({"title": "Invite", "href": "/calendar/42"})["href"] == (
        "/calendar/42")
    assert item({"title": "x", "href": "https://evil.example/"})["href"] == ""
    assert item({"title": "x", "href": "//evil.example/x"})["href"] == ""
    assert item({})["href"] == ""


def test_the_route_is_mounted_and_reads_only_unread_rows():
    runtime = RUNTIME.read_text(encoding="utf-8")
    assert '"/api/push/alert"' in runtime
    assert "push_alert_handler(self.env, request)" in runtime

    handler = ENTRY_TEXT.split("async def push_alert_handler", 1)[1]
    handler = handler.split("\nasync def ", 1)[0].split("\ndef ", 1)[0]
    # A notification the user already dealt with must never come back as a
    # banner, and the whole answer is a lock screen's worth of rows.
    assert "read_at=0" in handler
    assert "ORDER BY ts DESC" in handler
    # The alert names no row, so the phone assumes the newest unread one is
    # the one that woke it. Outside this window that assumption is wrong —
    # a sealed direct message, or a push that waited out a night of airplane
    # mode — and the answer has to be "nothing", not yesterday's ping.
    assert "AND ts>=?" in handler
    assert "PUSH_ALERT_FRESH_MS" in handler
    assert "PUSH_ALERT_MAX_ITEMS" in handler
    # Private to its owner, on the same gate as reading the inbox itself.
    assert "_alert_inbox_account_name(env, request)" in handler
    assert '{"error": "unauthorized"}, status=401' in handler
    # The extension runs while the app is suspended, so it is the only thing
    # able to set the badge.
    assert '"unread"' in handler
    # Never cached: a banner built from a stale page would describe the wrong
    # event, and the response is somebody's private inbox.
    assert 'cache_control=no_store' in handler


def test_only_a_read_is_exposed_here():
    handler = ENTRY_TEXT.split("async def push_alert_handler", 1)[1]
    handler = handler.split("\nasync def ", 1)[0].split("\ndef ", 1)[0]
    assert '!= "GET"' in handler
    for mutation in ("UPDATE ", "DELETE ", "INSERT "):
        assert mutation not in handler.upper()


def test_the_freshness_window_survives_a_delivery_delay():
    # Long enough to absorb a push that queued behind a lift or a tunnel,
    # short enough that a banner never dresses itself up as a notification
    # from another day.
    ns = _load()
    window = ns["PUSH_ALERT_FRESH_MS"]
    assert 10 * 60 * 1000 <= window <= 6 * 60 * 60 * 1000


def test_an_agent_done_desktop_ping_shows_only_the_agent_and_its_description():
    ns = _load()
    alert = ns["_push_alert_item"]({
        "kind": "desktop_ping", "title": "Agent #12",
        "body": "Fix the login redirect", "repo": "jett/forkmesh",
        "href": "/jett/forkmesh/agents/12",
        "meta": {"desktopKind": "agent-done", "category": "agent-finished",
                 "sessionId": "12"},
    })
    assert alert["title"] == "\U0001F916 Agent #12"
    assert alert["subtitle"] == ""
    assert alert["body"] == "Fix the login redirect"
    assert alert["threadId"] == "jett/forkmesh"

    other = ns["_push_alert_item"]({
        "kind": "desktop_ping", "title": "Agent #12 needs your approval",
        "repo": "jett/forkmesh", "meta": {"desktopKind": "agent"},
    })
    assert other["subtitle"] == "jett/forkmesh"


def test_every_ping_kind_gets_a_little_icon_beside_its_headline():
    import push_icons
    kinds = set()
    for node in ast.parse(ENTRY_TEXT).body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id == "NOTIFICATION_KINDS"):
            kinds = set(ast.literal_eval(node.value.args[0]))
    assert kinds and set(push_icons.PUSH_KIND_ICONS) >= kinds
    icons = list(push_icons.PUSH_KIND_ICONS.values())
    assert all(icon and icon.strip() == icon for icon in icons)
    assert push_icons.alert_icon("no_such_kind") == push_icons.PUSH_DEFAULT_ICON
    assert push_icons.alert_icon(
        "desktop_ping", {"category": "agent-failed"}) == "⚠️"
    assert push_icons.alert_icon(
        "desktop_ping", {"alarm": "power", "category": "agent-failed"}) == (
        "\U0001FAAB")
    assert push_icons.with_icon("Hello", "\U0001F514") == "\U0001F514 Hello"
    assert push_icons.with_icon("\U0001F514 Hello", "\U0001F514") == (
        "\U0001F514 Hello")
    assert push_icons.with_icon("", "\U0001F514") == ""
    alert = _load()["_push_alert_item"](
        {"id": "n1", "kind": "release_published", "title": "v1.0 released"})
    assert alert["title"] == "\U0001F680 v1.0 released"
