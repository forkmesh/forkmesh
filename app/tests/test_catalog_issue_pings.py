#!/usr/bin/env python3
import ast
import asyncio
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
CATALOG_SOURCE = (ROOT / "src" / "catalog_routes.py").read_text(encoding="utf-8")


def _load(captured, maintainers=(), next_number=None, seq_error=False):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    body = [node for node in tree.body
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in ("notify_catalog_issues_opened",
                              "_catalog_issue_max")]
    assert len(body) == 2
    body.extend(
        node for node in tree.body
        if isinstance(node, ast.Assign) and {
            t.id for t in node.targets if isinstance(t, ast.Name)
        } & {"PING_CATALOG_ISSUES_MAX"})
    mod = ast.Module(body=body, type_ignores=[])
    ast.fix_missing_locations(mod)
    queries = []

    async def enqueue_notification(_env, recipient, kind, title, **kwargs):
        captured.append(dict(kwargs, recipient=recipient, kind=kind,
                             title=title))
        return True

    async def org_repo_maintainers(_env, _owner, _repo):
        return list(maintainers)

    async def d1_first(_env, sql, *args):
        queries.append((sql, args))
        if seq_error:
            raise RuntimeError("d1 down")
        if next_number is None:
            return None
        return {"next_number": next_number}

    ns = {
        "enqueue_notification": enqueue_notification,
        "_org_repo_maintainers": org_repo_maintainers,
        "d1_first": d1_first,
        "clean_string": lambda value, limit=240: (
            value.strip()[:limit] if isinstance(value, str) else ""),
        "repo_web_href": lambda owner, repo: "/" + owner + "/" + repo,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]*", value or "")),
        "MAX_NODE_NAME": 63,
        "MAX_REPO_SEGMENT": 100,
    }
    exec(compile(mod, str(ENTRY), "exec"), ns)
    return ns["notify_catalog_issues_opened"], queries


def _run(notify, before, after, owner="fmnext-n1"):
    return asyncio.run(notify(
        object(), owner, "forkmesh", "repo-bi",
        {"issueMaxNumber": before}, {"issueMaxNumber": after}))


def test_an_issue_made_on_the_node_pings_the_organization():
    captured = []
    notify, queries = _load(captured, maintainers=["jett", "maya"],
                            next_number=40)
    assert _run(notify, "41", "42") == 2
    assert queries[0][1] == ("repo-bi",)
    assert [p["recipient"] for p in captured] == ["jett", "maya"]
    for ping in captured:
        assert ping["kind"] == "issue_opened"
        assert ping["title"] == "New issue in fmnext-n1/forkmesh"
        assert ping["body"] == "Issue #42"
        assert ping["href"] == "/fmnext-n1/forkmesh/issues/42"
        assert ping["actor"] == "fmnext-n1"
        assert ping["source"] == "issue"
        assert ping["meta"]["number"] == 42
        assert ping["dedupe"] == "issue-opened:fmnext-n1/forkmesh:42"


def test_numbers_the_relay_reserved_already_pinged_through_the_inbox():
    captured = []
    notify, _ = _load(captured, maintainers=["jett"], next_number=45)
    assert _run(notify, "41", "44") == 0
    assert captured == []
    assert _run(notify, "41", "46") == 2
    assert [p["meta"]["number"] for p in captured] == [45, 46]


def test_a_repo_without_relay_reservations_pings_every_new_number():
    captured = []
    notify, _ = _load(captured, maintainers=["jett"])
    assert _run(notify, "0", "2") == 2
    assert [p["meta"]["number"] for p in captured] == [1, 2]


def test_a_burst_is_capped():
    captured = []
    notify, _ = _load(captured, maintainers=["jett"])
    _run(notify, "10", "40")
    assert [p["meta"]["number"] for p in captured] == [36, 37, 38, 39, 40]


def test_nothing_is_sent_without_a_rise_or_a_known_previous_maximum():
    captured = []
    notify, queries = _load(captured, maintainers=["jett"])
    assert _run(notify, "5", "5") == 0
    assert _run(notify, "6", "5") == 0
    assert _run(notify, "", "5") == 0
    assert asyncio.run(notify(object(), "fmnext-n1", "forkmesh", "bi",
                              {}, {"issueMaxNumber": "5"})) == 0
    assert captured == [] and queries == []


def test_only_an_organization_fronted_host_fans_out():
    captured = []
    notify, queries = _load(captured, maintainers=[])
    assert _run(notify, "1", "2", owner="some-mirror") == 0
    assert captured == [] and queries == []


def test_the_publishing_node_is_not_told_about_its_own_issue():
    captured = []
    notify, _ = _load(captured, maintainers=["fmnext-n1", "maya"])
    _run(notify, "1", "2")
    assert [p["recipient"] for p in captured] == ["maya"]


def test_an_unreadable_counter_sends_nothing_rather_than_a_duplicate():
    captured = []
    notify, _ = _load(captured, maintainers=["jett"], seq_error=True)
    assert _run(notify, "1", "2") == 0
    assert captured == []


def test_the_catalog_route_calls_it_best_effort_for_an_update():
    index = CATALOG_SOURCE.index("await notify_catalog_issues_opened(")
    before = CATALOG_SOURCE[max(0, index - 120):index]
    assert "if exists:" in before and "try:" in before
    assert "key_bi, prior, record)" in CATALOG_SOURCE[index:index + 200]
