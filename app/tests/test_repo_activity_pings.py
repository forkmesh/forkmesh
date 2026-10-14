"""Activity on a repository raises a ping, not only an outbound webhook.

adhoc #295. A push, a new branch or tag, a merged pull request and a published
release were all observable to the relay and all of them stopped at the
outbound webhook rail: an owner with no registered endpoint learned nothing.
Issues, pull requests and comments already raised pings of their own; these
contracts cover the rest of the activity a repository sees.
"""

import ast
import asyncio
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
CATALOG_ROUTES = ROOT / "src" / "catalog_routes.py"
CATALOG_SOURCE = CATALOG_ROUTES.read_text(encoding="utf-8")
HOSTED_GIT = (ROOT / "src" / "hosted_git.py").read_text(encoding="utf-8")
ROUTE_HANDLERS = (ROOT / "src" / "route_handlers.py").read_text(encoding="utf-8")
FEDIVERSE = (ROOT / "src" / "fediverse_routes.py").read_text(encoding="utf-8")
DESKTOP = ROOT.parent / "desktop" / "src"
ANDROID = (ROOT.parent / "native" / "android" / "core" / "src" / "main" /
           "kotlin" / "com" / "forkmesh" / "nativeapp")


def _load_notifier(name, captured, maintainers=()):
    """One entry.py notifier with its collaborators stubbed.

    Same shape as test_notifications._load_pending_inbox_notifier: the point is
    to read back the exact strings the helper composes without standing up D1,
    crypto or the Worker runtime.
    """
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    body = [node for node in tree.body
            if isinstance(node, ast.AsyncFunctionDef) and node.name in (
                name, "_node_owner_among", "_org_fanout_recipients")]
    assert len(body) == 3, name + " not found in entry.py"
    body.extend(
        node for node in tree.body
        if isinstance(node, ast.Assign) and {
            t.id for t in node.targets if isinstance(t, ast.Name)
        } & {"REPO_ACTIVITY_LABELS", "NOTIFICATION_BODY_MAX"})
    mod = ast.Module(body=body, type_ignores=[])
    ast.fix_missing_locations(mod)

    async def enqueue_notification(_env, recipient, kind, title, **kwargs):
        captured.append(dict(kwargs, recipient=recipient, kind=kind,
                             title=title))

    async def org_repo_maintainers(_env, _host_owner, _repo):
        return list(maintainers)

    async def account_row(_env, _name):
        return None, {}

    ns = {
        "enqueue_notification": enqueue_notification,
        "_org_repo_maintainers": org_repo_maintainers,
        "_account_row": account_row,
        "clean_string": lambda value, limit=240: (
            value.strip()[:limit] if isinstance(value, str) else ""),
        "repo_web_href": lambda owner, repo: "/" + owner + "/" + repo,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]*", value or "")),
        "MAX_NODE_NAME": 63,
        "MAX_REPO_SEGMENT": 100,
    }
    exec(compile(mod, str(ENTRY), "exec"), ns)
    return ns[name]


def _catalog_namespace():
    # Module level is only constants and defs; nothing external runs at exec.
    namespace = {
        "re": re,
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
    }
    exec(compile(CATALOG_SOURCE, str(CATALOG_ROUTES), "exec"), namespace)
    return namespace


PUBLIC_PUSH = {
    "owner": "alice", "name": "atlas", "visibility": "public",
    "source": "local-node", "commit": "a" * 40, "stateHash": "b" * 64,
}


# --- the helper itself -------------------------------------------------------

def test_a_moved_default_head_pings_the_owner_and_the_org_maintainers():
    captured = []
    notify = _load_notifier("notify_repo_activity", captured,
                            maintainers=["mona", "jett"])
    asyncio.run(notify(object(), "alice", "atlas", "branch.push",
                       body="main advanced to " + "c" * 12,
                       key="c" * 40, meta={"branch": "main"}))

    assert [ping["recipient"] for ping in captured] == ["alice", "mona", "jett"]
    ping = captured[0]
    assert ping["kind"] == "repo_activity"
    assert ping["title"] == "New push in alice/atlas"
    assert ping["body"] == "main advanced to " + "c" * 12
    assert ping["repo"] == "alice/atlas"
    assert ping["href"] == "/alice/atlas"
    assert ping["meta"]["event"] == "branch.push"
    assert ping["meta"]["branch"] == "main"
    # The commit is the event's identity: republishing the same state collapses
    # onto this row instead of banner-ing every device a second time.
    assert ping["dedupe"] == "repo-activity:alice/atlas:branch.push:" + "c" * 40
    assert {p["dedupe"] for p in captured} == {ping["dedupe"]}


def test_a_push_carries_no_actor_so_it_reaches_the_owner_who_pushed():
    # A node publishes the refs state it now advertises; there is no
    # receive-pack and so no pusher identity to read. Naming the owner as the
    # actor would make enqueue_notification drop the ping (actor == recipient)
    # and the account would never hear about its own repositories moving.
    captured = []
    notify = _load_notifier("notify_repo_activity", captured)
    asyncio.run(notify(object(), "alice", "atlas", "repository.push",
                       key="b" * 64))

    assert captured[0]["actor"] == ""
    assert captured[0]["recipient"] == "alice"
    # No body supplied: the vocabulary's fallback says what actually moved.
    assert captured[0]["body"] == "A branch or tag was updated."


def test_a_merged_pull_request_names_its_merger_as_the_actor():
    captured = []
    notify = _load_notifier("notify_repo_activity", captured)
    asyncio.run(notify(object(), "alice", "atlas", "pull.merged",
                       body="Pull request #7 was merged.", actor="mona",
                       key="d" * 40, meta={"number": 7}))

    ping = captured[0]
    assert ping["title"] == "Pull request merged in alice/atlas"
    assert ping["actor"] == "mona"
    assert ping["meta"] == {"number": 7, "event": "pull.merged"}


def test_an_unknown_event_or_repo_pings_nobody():
    captured = []
    notify = _load_notifier("notify_repo_activity", captured)
    asyncio.run(notify(object(), "alice", "atlas", "repository.vandalised"))
    asyncio.run(notify(object(), "", "atlas", "repository.push"))
    asyncio.run(notify(object(), "alice", "", "repository.push"))
    assert captured == []


def test_a_published_release_reaches_the_org_that_fronts_the_repo():
    captured = []
    notify = _load_notifier("notify_release_published", captured,
                            maintainers=["mona"])
    asyncio.run(notify(object(), "alice", "atlas",
                       {"tag": "v2.1.0", "name": "Relay retries",
                        "created_by": "jett", "published_at": 1700}))

    assert [ping["recipient"] for ping in captured] == ["alice", "mona"]
    ping = captured[0]
    assert ping["kind"] == "release_published"
    assert ping["title"] == "Release v2.1.0 published"
    assert ping["body"] == "Relay retries"
    assert ping["actor"] == "jett"
    assert ping["ts"] == 1700
    assert ping["dedupe"] == "release:alice/atlas:v2.1.0"


# --- what counts as an observed push ----------------------------------------

def test_a_private_push_is_observed_even_though_no_mirror_wakes_for_it():
    ns = _catalog_namespace()
    observed = ns["catalog_push_observed"]
    mirror = ns["catalog_mirror_update_needed"]
    private = dict(PUBLIC_PUSH, visibility="private")

    # Same push, same moved head: the owner is pinged, the fleet is not.
    assert observed(private, True, "f" * 40, "b" * 64) is True
    assert mirror(private, True, "f" * 40, "b" * 64) is False
    assert observed(PUBLIC_PUSH, True, "f" * 40, "b" * 64) is True
    assert mirror(PUBLIC_PUSH, True, "f" * 40, "b" * 64) is True


def test_a_mirrors_echo_and_an_unchanged_republish_are_not_pushes():
    observed = _catalog_namespace()["catalog_push_observed"]
    # A mirror re-publishes the state it just fetched; reporting that as a push
    # would ping the owner twice for one push.
    assert observed(dict(PUBLIC_PUSH, source="remote-clone"),
                    True, "f" * 40, "b" * 64) is False
    # Nothing moved.
    assert observed(PUBLIC_PUSH, True, "a" * 40, "b" * 64) is False
    # A first publication is a new repository, not activity in an existing one.
    assert observed(PUBLIC_PUSH, False, "", "") is False
    # A feature branch or tag moves the refs digest while the default head
    # stays put, and that is still a push.
    assert observed(dict(PUBLIC_PUSH, stateHash="e" * 64),
                    True, "a" * 40, "b" * 64) is True


# --- the call sites ----------------------------------------------------------

def test_every_observable_event_raises_the_ping_beside_its_webhook():
    # A push is a code_push ping now (test_code_push_pings.py): one per
    # publication, with its own "Code pushes" switch. The webhook beside it
    # still names the default-branch move for CI.
    assert "if catalog_push_observed(" in CATALOG_SOURCE
    assert "await notify_code_push(" in CATALOG_SOURCE
    # A hosted (web-created) repository has no node to publish for it, so the
    # state pin is the only evidence its push happened.
    assert "await notify_code_push(" in HOSTED_GIT
    # Only a published merge, matching the webhook beside it.
    assert '"pull.merged",' in ROUTE_HANDLERS
    assert "await notify_repo_activity(" in ROUTE_HANDLERS
    # notify_release_published existed since the kind did and nothing called
    # it, so a cut release reached an endpoint and no inbox.
    assert "await notify_release_published(" in FEDIVERSE
    # Every one of them is best effort: an authorized publication, a pin, a
    # merge and an announcement all stand whether or not a ping could be filed.
    for source, call in (
            (CATALOG_SOURCE, "notify_code_push("),
            (HOSTED_GIT, "notify_code_push("),
            (ROUTE_HANDLERS, "notify_repo_activity("),
            (FEDIVERSE, "notify_release_published(")):
        index = source.index("await " + call)
        assert "try:" in source[max(0, index - 200):index]


def test_a_comment_is_not_re_fanned_to_the_organization():
    # notify_pending_inbox fans out only for the event that CREATES an item.
    # Widening that to comments looks right and is not: notify_subscribers
    # already reaches everyone following the thread, and a maintainer who ever
    # replied is auto-subscribed, so the same reply would file two
    # near-identical "New comment in ..." rows for them.
    body = ENTRY_TEXT[ENTRY_TEXT.index("async def notify_pending_inbox"):]
    body = body[:body.index("\nasync def _org_repo_maintainers")]
    assert "if not opened:\n        return" in body
    assert "a second fan-out would file two" in body


def test_the_kind_is_filed_but_never_mailed():
    kinds = ENTRY_TEXT[ENTRY_TEXT.index("NOTIFICATION_KINDS = frozenset({"):]
    assert '"repo_activity",' in kinds[:kinds.index("})")]
    # Absent from the email vocabulary on purpose: notification_email_enabled
    # returns False for any kind without a default, so a busy repository fills
    # the inbox and the phone banner without filling anybody's mailbox.
    email = ENTRY_TEXT[ENTRY_TEXT.index("NOTIFICATION_EMAIL_KINDS = ("):]
    assert "repo_activity" not in email[:email.index("NOTIFICATION_DIGEST_INTERVAL_MS")]


def test_every_ping_surface_can_draw_the_new_kind():
    actions = (DESKTOP / "MainWindowActions.cpp").read_text(encoding="utf-8")
    settings = (DESKTOP / "MainWindowSettings.cpp").read_text(encoding="utf-8")
    assert 'normalized == QLatin1String("repo_activity")' in actions
    assert '{QStringLiteral("repo_activity"), QStringLiteral("Repo activity")},' in actions
    assert '{QStringLiteral("repo_activity"), QStringLiteral("Repo activity")},' in settings
    # ...and it deep-links to the repository, the only page it has.
    assert 'kind == QLatin1String("repo_activity")' in actions

    logic = (ANDROID / "account" / "NotificationsLogic.kt").read_text(encoding="utf-8")
    kt_models = (ANDROID / "model" / "Notifications.kt").read_text(encoding="utf-8")
    assert '"repo_activity" -> "commit_outlined"' in logic
    assert '"repo_activity" -> "Repo activity"' in kt_models

    world = (ROOT.parent / "world" / "public" / "world" /
             "world.js").read_text(encoding="utf-8")
    assert "repo_activity:" in world
