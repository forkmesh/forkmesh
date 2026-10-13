#!/usr/bin/env python3
"""The repository circle's real star total and its ActivityPub follower gallery.

The World draws two things above and below the selected repository's circle,
and both must be live relay data rather than decoration:

  * the star on top carries the stored repo_stars total (black digits inside
    the star, "—" while unknown — never a fabricated number);
  * the seats below it are the repository actor's real ap_followers rows,
    joined with the cached remote actor document so each one shows the avatar,
    display name, @handle, home instance, bio, and follow date the fediverse
    already publishes for that account.

Pinned here: the migration/lazy-schema agreement for the cached profile
columns, the sanitization every remote field passes through before it reaches a
browser, and the client contracts (one /about read per repository, live star
and follower state merged into the scene records so a catalog refresh cannot
revert the totals).

Run: python3 -m pytest cloudflare_worker/tests/test_world_repository_followers.py
"""

import ast
import asyncio
import importlib.util
import re
import sqlite3
from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY_PATH = ROOT / "src" / "entry.py"
ENTRY = ENTRY_PATH.read_text(encoding="utf-8")
ROUTES_PATH = ROOT / "src" / "fediverse_routes.py"
ROUTES = ROUTES_PATH.read_text(encoding="utf-8")
SCHEMA = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
MIGRATION = ROOT / "migrations" / "0078_ap_remote_actor_profile.sql"
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


def _sibling_module(name):
    spec = importlib.util.spec_from_file_location(
        name, ROOT / "src" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ap = _sibling_module("activitypub")


def _load_function(name, namespace):
    for node in ast.parse(ROUTES, filename=str(ROUTES_PATH)).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) \
                and node.name == name:
            module = ast.fix_missing_locations(
                ast.Module(body=[node], type_ignores=[]))
            exec(compile(module, str(ROUTES_PATH), "exec"), namespace)
            return namespace[name]
    raise AssertionError(name + " not found")


# --- storage ------------------------------------------------------------------

def test_migration_and_lazy_schema_agree_on_cached_profile_columns():
    db = sqlite3.connect(":memory:")
    # Replay the pre-0078 table, then the migration: an existing deployment and
    # a lazily-ensured fresh database must end up with the same columns.
    db.executescript(
        "CREATE TABLE ap_remote_actors ("
        " actor_id TEXT PRIMARY KEY, inbox TEXT, shared_inbox TEXT,"
        " pubkey_pem TEXT, handle TEXT, display_name TEXT, url TEXT,"
        " updated_at INTEGER NOT NULL);")
    db.executescript(MIGRATION.read_text(encoding="utf-8"))
    migrated = {
        row[1]
        for row in db.execute("PRAGMA table_info(ap_remote_actors)").fetchall()
    }
    assert {"avatar_url", "summary"} <= migrated

    fresh = sqlite3.connect(":memory:")
    ddl = re.search(
        r'"""(CREATE TABLE IF NOT EXISTS ap_remote_actors.*?)"""',
        SCHEMA,
        re.DOTALL,
    )
    assert ddl, "ap_remote_actors DDL not found in schema.py"
    fresh.executescript(ddl.group(1))
    assert migrated == {
        row[1]
        for row in fresh.execute(
            "PRAGMA table_info(ap_remote_actors)").fetchall()
    }


def test_cached_actor_write_sanitizes_avatar_and_bio():
    write = ROUTES.split("INSERT INTO ap_remote_actors", 1)
    assert len(write) == 2
    prelude = write[0][-800:]
    # The avatar is validated as a public https media URL and the bio is
    # flattened from untrusted remote HTML, before either is persisted.
    assert "ap.public_media_url(ess.get(\"icon\"))" in prelude
    assert "ap_threads.sanitize_remote_content(ess.get(\"summary\")" in prelude
    assert "avatar_url=excluded.avatar_url" in write[1]
    assert "summary=excluded.summary" in write[1]


def _profile_sweep(rows, fetched, fails=()):
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(
        "CREATE TABLE ap_remote_actors ("
        " actor_id TEXT PRIMARY KEY, avatar_url TEXT, summary TEXT,"
        " updated_at INTEGER NOT NULL);"
        "CREATE TABLE ap_followers ("
        " actor_bi TEXT, follower_id TEXT);")
    for actor_id, avatar, summary, updated_at in rows:
        db.execute(
            "INSERT INTO ap_remote_actors VALUES (?,?,?,?)",
            (actor_id, avatar, summary, updated_at))
        db.execute(
            "INSERT INTO ap_followers VALUES ('actor:repo', ?)", (actor_id,))

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in db.execute(sql, args).fetchall()]

    async def _ap_remote_actor(_env, actor_id, force_refresh=False):
        assert force_refresh is True
        fetched.append(actor_id)
        # An unreachable server leaves the cached row exactly as it was.
        return None if actor_id in fails else {"actor_id": actor_id}

    return _load_function("_ap_refresh_follower_profiles", {
        "d1_all": d1_all,
        "_ap_remote_actor": _ap_remote_actor,
    })


def test_profile_sweep_only_refreshes_followers_missing_their_presentation():
    fetched = []
    sweep = _profile_sweep(
        [
            # Oldest cache first, and only rows still missing avatar/bio.
            ("https://m.s/users/old", None, None, 10),
            ("https://f.o/users/mid", "", None, 20),
            ("https://h.io/users/done", "https://h.io/a.png", "bio", 5),
        ],
        fetched,
    )
    assert asyncio.run(sweep(object(), 3)) == 2
    assert fetched == ["https://m.s/users/old", "https://f.o/users/mid"]


def test_profile_sweep_window_rotates_past_unreachable_servers():
    # Two dead servers hold the oldest cache entries forever (a failed fetch
    # never updates the row), so window 0 must not be the only batch that runs.
    rows = [
        ("https://dead.one/users/a", None, None, 1),
        ("https://dead.two/users/b", None, None, 2),
        ("https://live.one/users/c", None, None, 3),
        ("https://live.two/users/d", None, None, 4),
    ]
    dead = {"https://dead.one/users/a", "https://dead.two/users/b"}

    first = []
    assert asyncio.run(
        _profile_sweep(rows, first, dead)(object(), 2, 0)) == 0
    assert first == ["https://dead.one/users/a", "https://dead.two/users/b"]

    rotated = []
    assert asyncio.run(
        _profile_sweep(rows, rotated, dead)(object(), 2, 1)) == 2
    assert rotated == ["https://live.one/users/c", "https://live.two/users/d"]

    # A window past the end of a short backlog sweeps from the front instead of
    # idling the tick.
    beyond = []
    assert asyncio.run(
        _profile_sweep(rows, beyond, dead)(object(), 2, 9)) == 0
    assert beyond == ["https://dead.one/users/a", "https://dead.two/users/b"]


def test_profile_sweep_runs_on_its_own_bounded_cron_slot():
    due_phases = ENTRY[
        ENTRY.index("def _cron_due_phases"):
        ENTRY.index("async def _run_cron_phase")
    ]
    profile_slot = due_phases.split("if minute % 20 == 12:", 1)
    assert len(profile_slot) == 2
    assert 'phases.append("ap-profiles")' in profile_slot[1][:100]

    phase_runner = ENTRY[
        ENTRY.index("async def _run_cron_phase"):
        ENTRY.index("def _cron_tick_phase_order")
    ]
    profile_phase = phase_runner.split('if phase == "ap-profiles":', 1)
    assert len(profile_phase) == 2
    mapping = profile_phase[1].split('if phase == "federation":', 1)[0]
    assert "_ap_refresh_follower_profiles(" in mapping
    assert "AP_PROFILE_REFRESH_BATCH" in mapping
    assert "AP_PROFILE_REFRESH_WINDOWS" in mapping
    assert "AP_PROFILE_REFRESH_BATCH = 3" in ENTRY


# --- the public follower list -------------------------------------------------

def test_about_follower_query_joins_the_cached_actor_document():
    followers = ENTRY.split("SELECT follower_id, follower_handle", 1)
    assert len(followers) == 2
    query = followers[1][:2400]
    assert "LEFT JOIN ap_remote_actors ON actor_id = follower_id" in query
    # Newest first, still capped: a popular repo never ships thousands of rows.
    assert "ORDER BY created_at DESC LIMIT 50" in query
    for field in ("\"avatarUrl\"", "\"about\"", "\"instance\"",
                  "\"profileUrl\"", "\"followedAt\""):
        assert field in query, field
    assert "ap.public_media_url(" in query
    assert "ap_threads.sanitize_remote_content(" in query


def test_public_media_url_is_the_only_avatar_gate_the_client_trusts():
    # The relay refuses anything a browser should not fetch on the viewer's
    # behalf, so an actor document cannot smuggle a scheme or a private host in.
    assert ap.public_media_url("https://files.m.s/a.png") == \
        "https://files.m.s/a.png"
    for hostile in ("javascript:alert(1)", "data:image/png;base64,AA",
                    "http://files.m.s/a.png", "https://10.0.0.4/a.png",
                    "https://box.local/a.png"):
        assert ap.public_media_url(hostile) == "", hostile


# --- the World client ---------------------------------------------------------

def test_world_reads_followers_from_the_public_about_card():
    loader = WORLD.split("async loadRepositoryFollowers(", 1)
    assert len(loader) == 2
    body = loader[1][:2600]
    assert "/about" in body
    assert "normalizeRepositoryFollowers(fediverse.followersList)" in body
    # The relay's reported total stays authoritative for the caption; the list
    # itself is capped.
    assert "followers.length" in body
    # A private repository is never probed for followers.
    assert "status: \"unavailable\"" in body
    assert "isPrivate" in body
    # Both repository-map entry paths hydrate the gallery.
    assert WORLD.count("this.loadRepositoryFollowers(") >= 2


def test_follower_normalizer_bounds_every_remote_field():
    normalizer = WORLD.split("function normalizeRepositoryFollowers(", 1)
    assert len(normalizer) == 2
    body = normalizer[1][:1800]
    assert "safePublicHTTPSURL(entry.avatarUrl)" in body
    assert "safePublicHTTPSURL(entry.profileUrl || entry.url)" in body
    for field in ("handle", "name", "about", "instance"):
        assert f"sanitizeNotificationText(entry.{field}" in body, field
    # Deduped and capped, so a hostile relay answer cannot flood the scene.
    assert "seen.has(key)" in body
    assert "followers.length >= 24" in body


def test_scene_records_carry_live_star_and_follower_state():
    merge = WORLD.split("repositoriesWithLiveSocialState() {", 1)
    assert len(merge) == 2
    body = merge[1][:1800]
    assert "this.repositoryStarStates.get(key)" in body
    assert "this.repositoryFollowerStates.get(key)" in body
    assert "starCount:" in body
    assert "fediverseFollowers:" in body
    # The scene is handed the merged records, not the raw catalog snapshot.
    assert "this.repositoriesWithLiveSocialState()," in WORLD
    # The coalescing timer must not be cancelled by an unrelated presence sync:
    # a stale non-zero handle would silently freeze every later rebuild.
    presence = WORLD.split("syncInactivePresence() {", 1)[1][:600]
    assert "clearTimeout(this.repositoryStarSceneSyncTimer)" not in presence


# --- the scene ----------------------------------------------------------------

def test_star_draws_the_stored_total_in_black():
    star = re.search(
        r"function repositoryStar\w*Texture\(THREE, count.*?\n}\n",
        SCENE,
        re.DOTALL,
    )
    assert star, "repository star texture not found"
    body = star.group(0)
    assert "#07120e" in body                      # black digits
    assert "count.toLocaleString(\"en-US\")" in body
    # An unknown total renders a dash; it is never invented or rounded to zero.
    assert '"—"' in body
    # The digits shrink to stay inside the star instead of overflowing it.
    assert "measureText(value).width" in body


def test_follower_gallery_orbits_real_follower_icons_around_file_circle():
    gallery = SCENE.split(
        "// Who follows this repository over ActivityPub", 1)
    assert len(gallery) == 2
    body = gallery[1][:5200]
    assert "repository-fediverse-follower-icons:" in body
    assert "record.fediverseFollowers" in body
    assert "makeRepositoryFollowerIcon(THREE, follower)" in body
    assert "const orbitRadius = 4.25" in body
    assert "gallery.position.set(0, 0.55, 0.18)" in body
    assert "caption.material.depthTest = false" in body
    assert "node.add(gallery)" in body
    # Followers are social records, not in-world physical visitors.
    assert "makeRepositoryFollowerFigure" not in body
    assert "figure.rotation.y" not in body
    # The caption reports the authoritative total and says when it is showing
    # only part of it.
    assert "FEDIVERSE ${" in body
    assert "SHOWING ${visibleFollowers.length}" in body
    assert "NOBODY FOLLOWS THIS REPOSITORY YET" in body
    # Only the selected public repository draws the icon orbit.
    assert "isActive && !record.isPrivate" in body


def test_follower_card_shows_the_whole_public_profile():
    card = SCENE.split("function repositoryFollowerCardTexture(", 1)
    assert len(card) == 2
    body = card[1][:2600]
    for field in ("follower?.name", "follower?.handle", "follower?.instance",
                  "follower?.about"):
        assert field in body, field
    assert "repositoryFollowerFollowedLabel(follower?.followedAt)" in body
    assert "No public bio." in body


def test_remote_avatar_load_is_anonymous_and_optional():
    loader = SCENE.split("function loadRepositoryFollowerAvatar(", 1)
    assert len(loader) == 2
    body = loader[1][:900]
    assert "image.crossOrigin = \"anonymous\"" in body
    assert "image.referrerPolicy = \"no-referrer\"" in body
    # A server without CORS headers simply leaves the generated initials plate.
    assert "image.onerror" in body
    icon = SCENE.split("function makeRepositoryFollowerIcon(", 1)[1][:1600]
    # An icon detached by a catalog rebuild is dropped, never repainted.
    assert "if (!icon.parent) return;" in icon
    # File wedges never occlude the identity badge at common camera angles.
    assert "depthTest: false" in icon
    assert "icon.renderOrder = 40" in icon
    assert "new THREE.PlaneGeometry(1, 1)" in icon


def test_repository_social_graph_has_outer_followers_inner_contributors_and_follow():
    assert "repository-contributor-inner-ring" in SCENE
    assert "makeRepositoryContributorIcon(THREE, contributor)" in SCENE
    assert "const radius = 2.7" in SCENE
    assert "contributorOrbit.position.set(0, 0.55, 0.17)" in SCENE
    assert "repository-fediverse-follow:" in SCENE
    assert "repositoryFollowButtonTexture" in SCENE
    assert "repositoryFediverseFollow" in SCENE
    assert "followRepositoryOnFediverse" in WORLD
    assert "Copy Mastodon handle" in WORLD
