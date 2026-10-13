#!/usr/bin/env python3
"""Per-account PR/issue/commit/discussion tallies in the user directory.

The Qt app's admin Users page shows one row per account with a column for
each kind of contribution that account has made. Issues, PRs, and
discussions come from ``contributor_activity`` — the same running tally the
leaderboards use — joined onto the directory read. Commits cannot: nothing
increments that column, because commits reach the mesh as whole-repo
snapshots rather than one signed event apiece, so they are summed out of
``profile_contribution_days`` instead.

Contracts pinned here:

  * ``_account_chat_user_payload`` always emits ``pulls``/``issues``/
    ``commits``/``discussions`` as integers, defaulting to 0 for an account
    the tally has never seen (a LEFT JOIN miss hands the caller NULL).
  * ``_account_users_directory`` LEFT JOINs ``contributor_activity`` on the
    account blind index for the three event-tallied kinds, and takes commits
    from ``_contribution_commit_totals`` keyed by the same blind index.
  * ``_contribution_commit_totals`` counts only public projects on public
    repositories, and one source repo per project, so an unauthenticated
    read of the directory leaks no private activity and a mirrored project
    is not counted once per mirror.
  * Discussions are tallied at all: ``_record_contributor`` accepts the kind
    and ``discussions_handler`` calls it on an accepted event.
  * ``contributor_activity`` carries a ``discussions`` column on fresh
    databases and gains one on already-deployed ones.
"""

import ast
import asyncio
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
ACCOUNT_USERS = ROOT / "src" / "account_users.py"
ACCOUNT_USERS_TEXT = ACCOUNT_USERS.read_text(encoding="utf-8")
SCHEMA_TEXT = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")


def _function_source(name):
    for text in (ENTRY_TEXT, ACCOUNT_USERS_TEXT):
        tree = ast.parse(text)
        for node in tree.body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and \
                    node.name == name:
                return ast.get_source_segment(text, node)
    raise AssertionError(f"Worker source must define {name}")


def _load_function(name, namespace):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    function = next(
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name == name
    )
    scope = dict(namespace)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[function], type_ignores=[])),
        str(ENTRY), "exec"), scope)
    return scope[name]


def _run(coro):
    return asyncio.run(coro)


TALLY_KEYS = ("issues", "pulls", "commits", "discussions")


def _payload_fn():
    return _load_function("_account_chat_user_payload", {
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 63,
        "SOLANA_RE": re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$"),
        "_owned_nodes": lambda rec: list(rec.get("nodes", [])),
        "_world_public_total_active_ms": lambda ms: int(ms or 0),
        "_world_public_total_steps": lambda steps: int(steps or 0),
        "_world_public_office_visits": _load_function(
            "_world_public_office_visits", {}),
        "_clean_mastodon_handle": lambda value: str(value or ""),
        "_mastodon_url": lambda value: (
            "https://mastodon.example/@" + str(value).lstrip("@")
            if value else ""),
        "_account_public_last_email": lambda rec: {},
        "_account_world_client_fields": lambda rec: {},
        "_contribution_tally": _load_function("_contribution_tally", {}),
    })


# --- payload shape -----------------------------------------------------------

def test_directory_payload_carries_every_contribution_tally():
    payload = _payload_fn()(
        {"name": "Ada"}, 0, "", issues=9, pulls=2, commits=40, discussions=1)
    assert payload["issues"] == 9
    assert payload["pulls"] == 2
    assert payload["commits"] == 40
    assert payload["discussions"] == 1


def test_directory_payload_carries_a_bounded_office_visit_aggregate():
    payload = _payload_fn()({"name": "Ada"}, office_visits=12)
    assert payload["officeVisits"] == 12

    bounded = _load_function("_world_public_office_visits", {})
    assert bounded(None) == 0
    assert bounded(-1) == 0
    assert bounded("7") == 7
    assert bounded(10_000) == 999


def test_directory_payload_carries_public_mastodon_identity():
    payload = _payload_fn()({"name": "Ada", "mastodon": "@ada@example.social"})
    assert payload["mastodon"] == "@ada@example.social"
    assert payload["mastodonUrl"] == (
        "https://mastodon.example/@ada@example.social")


def test_directory_payload_derives_wearable_activity_badges_for_old_and_new_data():
    # Contributor/Discord data existed before badge_awards, while Chat starts
    # as a durable award. The public World roster needs one normalized list
    # that recognizes both without exposing a Discord id or chat content.
    payload = _payload_fn()(
        {"name": "Ada"},
        issues=1,
        pulls=2,
        discussions=3,
        discord_linked=True,
        achievement_badges=["chat", "unknown", "chat"],
    )
    assert payload["achievementBadges"] == [
        "qa", "coder", "discord", "poster", "chat"]


def test_untallied_account_reports_zero_not_null():
    # An account that has never contributed has no contributor_activity row,
    # so the LEFT JOIN hands every tally column back as SQL NULL — None in
    # Python, which bare int() raises on. One such account would have 500-ed
    # the entire directory read (and with it chat's roster and the World
    # campfire), so NULL has to land as a plain 0.
    payload = _payload_fn()(
        {"name": "ada"}, 0, "",
        issues=None, pulls=None, commits=None, discussions=None)
    assert [payload[key] for key in TALLY_KEYS] == [0, 0, 0, 0]
    assert all(isinstance(payload[key], int) for key in TALLY_KEYS)
    # Defaults (no tallies passed at all) behave the same way.
    assert [_payload_fn()({"name": "ada"})[key] for key in TALLY_KEYS] == \
        [0, 0, 0, 0]


def test_tally_coercion_survives_junk_column_values():
    tally = _load_function("_contribution_tally", {})
    assert tally(None) == 0
    assert tally("") == 0
    assert tally("nope") == 0
    assert tally(-4) == 0
    assert tally("12") == 12
    assert tally(7.9) == 7


# --- directory read ----------------------------------------------------------

def test_directory_joins_the_contribution_tally_table():
    source = _function_source("_account_users_directory")
    assert "LEFT JOIN contributor_activity c ON c.author_bi=u.user_bi" in \
        source
    for key in ("issues", "pulls", "discussions"):
        assert f"c.{key}" in source
        # Coercion belongs to _contribution_tally; a bare int() here would
        # raise on the NULL a LEFT JOIN miss produces.
        assert f'{key}=row.get("{key}", 0)' in source
        assert f'int(row.get("{key}"' not in source


def test_directory_takes_commits_from_the_contribution_snapshots():
    # contributor_activity.commits is dead weight — no code path increments
    # it — so reading the column back would pin every account at 0 commits.
    source = _function_source("_account_users_directory")
    assert "c.commits" not in source
    assert 'commits=row.get("commits"' not in source
    assert "commit_totals = await _contribution_commit_totals(env)" in source
    assert 'commits=commit_totals.get(row.get("user_bi"), 0)' in source


def test_directory_counts_only_real_office_visits_per_member():
    source = _function_source("_account_users_directory")
    assert "COUNT(*) AS office_visits" in source
    assert "FROM world_office_attendance WHERE visit_scope='office'" in source
    assert "GROUP BY account_bi" in source
    assert "o.account_bi=u.user_bi" in source
    assert 'office_visits=row.get("office_visits", 0)' in source


def test_nothing_increments_the_dead_commits_tally_column():
    # Guards the reasoning above: if a commit tally writer is ever added,
    # this test fails and the directory should go back to the cheap join.
    assert not re.search(
        r'_record_contributor\([^)]*"commits"\)', ENTRY_TEXT)


# --- commit totals -----------------------------------------------------------

def test_commit_totals_are_summed_per_account():
    source = _function_source("_contribution_commit_totals")
    assert "SUM(days.commits) AS commits" in source
    assert "GROUP BY days.subject_user_bi" in source
    # Keyed by the account blind index, the directory's join key.
    assert "days.subject_user_bi AS user_bi" in source
    # Only rows belonging to each project's active snapshot generation.
    assert "days.generation_bi=sources.active_generation_bi" in source


def test_commit_totals_exclude_private_activity():
    # The directory endpoint is unauthenticated (it backs the chat roster and
    # the World campfire), so a commit count drawn from it must stay inside
    # what the public profile graph already shows.
    source = _function_source("_contribution_commit_totals")
    assert "projects.is_public=1 AND repositories.is_private=0" in source


def test_commit_totals_count_a_mirrored_project_once():
    # A project mirrored across several nodes has one contribution snapshot
    # per mirror; ranking picks the latest capture so commits are not
    # multiplied by the mirror count.
    source = _function_source("_contribution_commit_totals")
    assert "PARTITION BY projects.project_bi" in source
    assert "SELECT * FROM ranked_sources WHERE source_rank=1" in source


def test_commit_totals_skip_unkeyed_rows_and_coerce_counts():
    totals_fn = _load_function("_contribution_commit_totals", {
        "d1_all": None,
        "_contribution_tally": _load_function("_contribution_tally", {}),
    })
    rows = [
        {"user_bi": "bi-ada", "commits": "12"},
        {"user_bi": "", "commits": 5},        # unresolved actor: dropped
        {"user_bi": None, "commits": 5},      # same
        {"user_bi": "bi-grace", "commits": None},
    ]

    async def fake_d1_all(env, sql, *params):
        del env, sql, params
        return rows

    totals_fn.__globals__["d1_all"] = fake_d1_all
    totals = _run(totals_fn(object()))
    assert totals == {"bi-ada": 12, "bi-grace": 0}


def test_commit_totals_survive_an_empty_table():
    totals_fn = _load_function("_contribution_commit_totals", {
        "d1_all": None,
        "_contribution_tally": _load_function("_contribution_tally", {}),
    })

    async def fake_d1_all(env, sql, *params):
        del env, sql, params
        return None

    totals_fn.__globals__["d1_all"] = fake_d1_all
    assert _run(totals_fn(object())) == {}


# --- discussion tallying -----------------------------------------------------

def test_record_contributor_accepts_discussions():
    source = _function_source("_record_contributor")
    assert '"issues", "pulls", "commits", "discussions"' in source
    assert "display_name" in source
    assert "blind_index(env, author_key.lower())" in source


def test_accepted_discussion_events_are_tallied():
    source = _function_source("discussions_handler")
    assert 'event.get("authorName", "")' in source


def test_contributor_activity_has_a_discussions_column():
    assert re.search(
        r"CREATE TABLE IF NOT EXISTS contributor_activity[^\"]*?"
        r"discussions INTEGER NOT NULL DEFAULT 0", SCHEMA_TEXT, re.S)
    # Deployed databases predate the column, so the lazy upgrade adds it.
    assert ("ALTER TABLE contributor_activity ADD COLUMN discussions "
            "INTEGER NOT NULL DEFAULT 0") in ENTRY_TEXT
