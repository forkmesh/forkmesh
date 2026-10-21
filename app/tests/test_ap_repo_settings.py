#!/usr/bin/env python3
"""Per-repo ActivityPub settings + follower list (adhoc #49).

The repo About endpoint now exposes WHO follows the repo actor
(`fediverse.followersList`) and the owner's per-repo federation switches
(`fediverse.settings`: federate / broadcastEvents / acceptComments), editable
from the website's repo settings gear and the Qt client's repo-details dialog.

These tests load the real handlers out of src/entry.py and pin:

  * missing ap_repo_settings row means every switch defaults ON (repos that
    predate migration 0035 keep federating exactly as before);
  * _ap_repo_federates honours the owner's per-repo federate switch;
  * GET /about returns the newest followers (handle + remote actor URL) and
    the current switches;
  * the Qt desktop can save switches with the owner-key signed token
    (ownerSig/ts body fields, "forkmesh-repo-about-v1" canonical) — and a bad
    signature is rejected before anything is written;
  * an unchanged settings save writes nothing and purges no edge caches;
  * a settings-only save never blanks the description (absent = unchanged);
  * the event-publish and inbound-reply paths gate on the switches
    (source-level contract, same style as the profile-broadcast pin).

Run: python3 -m pytest cloudflare_worker/tests/test_ap_repo_settings.py
"""

import ast
import asyncio
import importlib.util
import json
import re
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse

import read_only_mirrors
from worker_test_helpers import json_from_request_double

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"


def _sibling_module(name):
    """The real (js-free) helper module the handler under test delegates to."""
    spec = importlib.util.spec_from_file_location(
        name, ROOT / "src" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


activitypub = _sibling_module("activitypub")
activitypub_threads = _sibling_module("activitypub_threads")
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {n.name for n in selected}
    missing = set(names) - found
    assert not missing, "missing functions: %s" % sorted(missing)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals or {})
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


async def _passthrough_alias(env, owner, repo):
    # Non-org repos resolve to themselves; org-alias resolution is pinned
    # separately in test_orgs_teams.py.
    return owner


def _json_response(data, status=200, cache_seconds=None, cache_control=None,
                   extra_headers=None):
    return {"status": status, "data": data}


def _defaults_constant(name="AP_REPO_SETTING_DEFAULTS"):
    # These defaults are module-level assignments, not functions, so the AST
    # extraction above never picks them up — evaluate them directly.
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    for node in tree.body:
        if (isinstance(node, ast.Assign) and
                any(getattr(t, "id", "") == name for t in node.targets)):
            return ast.literal_eval(node.value)
    raise AssertionError(name + " not found")


AP_REPO_SETTING_DEFAULTS = _defaults_constant()
REPO_ALERT_SETTING_DEFAULTS = _defaults_constant(
    "REPO_ALERT_SETTING_DEFAULTS")
REPO_FEATURE_DEFAULTS = _defaults_constant("REPO_FEATURE_DEFAULTS")


def _settings_ns(settings_rows):
    """Load the real settings reader against a fake ap_repo_settings table
    keyed by the blind index string."""

    async def blind_index(env, value):
        return "bi:" + str(value or "")

    async def d1_first(env, sql, *args):
        assert "FROM ap_repo_settings" in sql
        return settings_rows.get(args[0])

    return _load("_ap_repo_settings_get", "_ap_repo_settings_bi",
                 extra_globals={
                     "blind_index": blind_index,
                     "d1_first": d1_first,
                     "json": json,
                     "_ap_org_alias_owner": _passthrough_alias,
                     "AP_REPO_SETTING_DEFAULTS": AP_REPO_SETTING_DEFAULTS,
                 })


def test_missing_row_means_every_switch_defaults_on():
    ns = _settings_ns({})
    settings = _run(ns["_ap_repo_settings_get"](None, "Alice", "Proj"))
    assert settings == {"federate": True, "broadcastEvents": True,
                        "acceptComments": True}


def test_stored_row_overrides_and_key_is_case_insensitive():
    rows = {"bi:ap-repo-settings:alice/proj":
            {"data": json.dumps({"federate": True, "broadcastEvents": False,
                                 "acceptComments": False})}}
    ns = _settings_ns(rows)
    # URL-cased caller (About handler) and handle-cased caller (AP gates)
    # must land on the same row.
    for owner, repo in (("Alice", "Proj"), ("alice", "proj")):
        settings = _run(ns["_ap_repo_settings_get"](None, owner, repo))
        assert settings == {"federate": True, "broadcastEvents": False,
                            "acceptComments": False}


def test_garbage_row_falls_back_to_defaults():
    rows = {"bi:ap-repo-settings:alice/proj": {"data": "not json"}}
    ns = _settings_ns(rows)
    settings = _run(ns["_ap_repo_settings_get"](None, "alice", "proj"))
    assert settings == AP_REPO_SETTING_DEFAULTS


# --- the federation gate honours the per-repo switch --------------------------

def _federates_ns(is_private, federate):
    async def blind_index(env, value):
        return "bi:" + str(value or "")

    async def d1_first(env, sql, *args):
        if "FROM repositories" in sql:
            return {"is_private": 1 if is_private else 0}
        raise AssertionError("unexpected d1_first: " + sql)

    async def _ap_repo_settings_get(env, owner, repo):
        return dict(AP_REPO_SETTING_DEFAULTS, federate=federate)

    return _load("_ap_repo_federates", extra_globals={
        "blind_index": blind_index,
        "d1_first": d1_first,
        "_ap_org_alias_owner": _passthrough_alias,
        "_ap_repo_settings_get": _ap_repo_settings_get,
    })


def test_repo_federates_only_when_public_and_switch_on():
    assert _run(_federates_ns(False, True)["_ap_repo_federates"](
        None, "alice", "proj")) is True
    assert _run(_federates_ns(False, False)["_ap_repo_federates"](
        None, "alice", "proj")) is False
    assert _run(_federates_ns(True, True)["_ap_repo_federates"](
        None, "alice", "proj")) is False


# --- GET /about: follower list + switches -------------------------------------

def test_about_get_returns_followers_list_and_settings():
    async def _repo_is_private(env, owner, repo):
        return False

    async def blind_index(env, value):
        return "bi:" + str(value or "")

    async def d1_first(env, sql, *args):
        if "FROM repositories" in sql:
            return {"data": {"description": "d", "website": ""}}
        if "COUNT(*)" in sql and "ap_followers" in sql:
            return {"c": 2}
        if "FROM ap_repo_settings" in sql:
            return {"data": json.dumps({"acceptComments": False})}
        # Operational-alert switches (adhoc #445) ride the same About payload;
        # no row means every alert email stays off.
        if "FROM repo_alert_settings" in sql:
            return None
        if "FROM repo_feature_settings" in sql:
            assert args == ("bi:repo-feature-settings:mirror2/proj",)
            return {"data": json.dumps({"pulls": False})}
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_all(env, sql, *args):
        if "FROM repo_media" in sql:
            return []
        if "FROM ap_followers" in sql:
            assert "ORDER BY created_at DESC LIMIT 50" in sql
            # The cached remote actor document joins in each follower's public
            # presentation; a follower whose actor was never fetched (or whose
            # row predates migration 0078) joins as NULL.
            assert "LEFT JOIN ap_remote_actors" in sql
            return [
                {"follower_id": "https://mastodon.social/users/kate",
                 "follower_handle": "@kate@mastodon.social",
                 "created_at": 1700,
                 "display_name": "Kate",
                 "avatar_url": "https://files.mastodon.social/kate.png",
                 "summary": "<p>Rust &amp; embedded</p>",
                 "url": "https://mastodon.social/@kate"},
                # Older row without the resolved handle: derived from the URL.
                {"follower_id": "https://fosstodon.org/users/sam",
                 "follower_handle": "",
                 "created_at": 1600},
            ]
        raise AssertionError("unexpected d1_all: " + sql)

    async def decrypt_row(env, data):
        return dict(data)

    async def _ap_enabled(env):
        return True

    async def _ap_actor_bi(env, kind, handle):
        assert handle == "forkmesh.proj"
        return "actor:" + handle

    async def _org_repo_node(env, owner, repo):
        if (owner, repo) == ("forkmesh", "proj"):
            return "mirror2"
        return ""

    github_mirror = {"provider": "github", "label": "GitHub",
                     "url": "https://github.com/octo/proj", "role": "mirror"}

    async def _public_about_link(env, owner, repo):
        assert (owner, repo) == ("mirror2", "proj")
        return github_mirror

    ns = _load("_repo_about_public", "_ap_repo_settings_get",
               "_ap_repo_settings_bi", "_repo_alert_settings_get",
               "_repo_alert_settings_bi", "_repo_features_get",
               "_repo_features_bi", "_read_only_mirror_fields",
               extra_globals={
                   "_read_only_mirrors": read_only_mirrors,
                   "_repository_provider_mirror": SimpleNamespace(
                       public_about_link=_public_about_link),
                   "_instance_solo_flagship_node": lambda env: "",
                   "REPO_FEATURE_DEFAULTS": REPO_FEATURE_DEFAULTS,
                   "REPO_ALERT_SETTING_DEFAULTS":
                       REPO_ALERT_SETTING_DEFAULTS,
                   "json_response": _json_response,
                   "_repo_is_private": _repo_is_private,
                   "blind_index": blind_index,
                   "d1_first": d1_first,
                   "d1_all": d1_all,
                   "decrypt_row": decrypt_row,
                   "_ap_org_alias_owner": _passthrough_alias,
                   "_ap_enabled": _ap_enabled,
                   "_ap_actor_bi": _ap_actor_bi,
                   "_org_repo_node": _org_repo_node,
                   "_ap_origin": lambda env, request=None:
                       "https://forkmesh.com",
                   "_ap_domain_of": lambda origin: "forkmesh.com",
                   "_ap_actor_url": lambda origin, kind, handle:
                       origin + "/ap/repos/alice/proj",
                   "ap": SimpleNamespace(
                       repo_handle=lambda owner, repo: owner + "." + repo,
                       public_media_url=activitypub.public_media_url),
                   "ap_threads": SimpleNamespace(
                       sanitize_remote_content=(
                           activitypub_threads.sanitize_remote_content)),
                   "clean_string": lambda value, cap: str(value or "")[:cap],
                   "quote": lambda value: value,
                   "safe_segment": lambda value: value,
                   "urlparse": urlparse,
                   "REPO_ABOUT_RE": re.compile(
                       r"^/api/repo/([^/]+)/([^/]+)/about/?$"),
                   "json": json,
                   "AP_ACTOR_REPO": "repo",
                   "AP_AVATAR_PATH": "/assets/fediverse-avatar.png",
                   "AP_BANNER_PATH": "/assets/fediverse-banner.png",
                   "AP_REPO_SETTING_DEFAULTS": AP_REPO_SETTING_DEFAULTS,
               })
    request = SimpleNamespace(
        url="https://forkmesh.com/api/repo/forkmesh/proj/about")
    resp = _run(ns["_repo_about_public"](
        None, request, "mirror2", "proj"))
    assert resp["status"] == 200
    fediverse = resp["data"]["fediverse"]
    assert fediverse["followers"] == 2
    assert fediverse["followersList"] == [
        {"handle": "@kate@mastodon.social",
         "url": "https://mastodon.social/users/kate",
         "name": "Kate",
         "avatarUrl": "https://files.mastodon.social/kate.png",
         "about": "Rust & embedded",
         "instance": "mastodon.social",
         "profileUrl": "https://mastodon.social/@kate",
         "followedAt": 1700},
        # No cached actor document yet: identity fields stay empty and the
        # profile URL falls back to the actor id.
        {"handle": "@sam@fosstodon.org",
         "url": "https://fosstodon.org/users/sam",
         "name": "",
         "avatarUrl": "",
         "about": "",
         "instance": "fosstodon.org",
         "profileUrl": "https://fosstodon.org/users/sam",
         "followedAt": 1600},
    ]
    assert fediverse["settings"] == {"federate": True,
                                     "broadcastEvents": True,
                                     "acceptComments": False}
    assert fediverse["enabled"] is True
    assert resp["data"]["features"] == {
        "issues": True, "pulls": False, "discussions": True,
        "actions": True, "protectDefaultBranch": False}
    assert resp["data"]["mirrorOf"] == "" and resp["data"]["readOnly"] is False
    assert resp["data"]["externalMirrors"] == [github_mirror]
    # Platform operational-alert state is not exposed from a public repo API.
    assert "alerts" not in resp["data"]


# --- POST /about: owner-key auth + settings save -------------------------------

def _about_post_env(log, stored_settings=None, good_sig="GOODSIG",
                    stored_alerts=None):
    async def ensure_schema(env):
        return None

    async def _authed_account_name(env, request, data):
        return ""  # no browser session — the desktop path under test

    async def _account_owns_node(env, actor, owner):
        return False

    async def _is_admin(env, actor):
        return False

    async def _owner_pubkey(env, owner):
        return "PUB"

    async def ed25519_verify(pub, sig, canonical):
        log.append(("verify", canonical.decode()))
        return sig == good_sig

    async def blind_index(env, value):
        return "bi:" + str(value or "")

    async def d1_all(env, sql, *args):
        if "FROM repositories" in sql:
            return [{"key_bi": "kb",
                     "data": {"owner": "alice", "name": "proj",
                              "description": "keep me", "website": ""},
                     "is_private": 0}]
        if "FROM repo_media" in sql:
            return []
        raise AssertionError("unexpected d1_all: " + sql)

    async def d1_first(env, sql, *args):
        if "FROM ap_repo_settings" in sql:
            if stored_settings is None:
                return None
            return {"data": json.dumps(stored_settings)}
        if "FROM repo_alert_settings" in sql:
            if stored_alerts is None:
                return None
            return {"data": json.dumps(stored_alerts)}
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_run(env, sql, *args):
        log.append(("d1_run", sql, args))

    async def decrypt_row(env, data):
        return dict(data) if isinstance(data, dict) else data

    async def encrypt_row(env, rec):
        return rec

    async def edge_cache_delete(key):
        log.append(("purge", key))

    return _load("repo_about_handler", "_ap_repo_settings_get",
                 "_ap_repo_settings_bi", "_ts_ok",
                 "_repo_alert_settings_get", "_repo_alert_settings_bi",
                 extra_globals={
                     "REPO_ALERT_SETTING_DEFAULTS":
                         REPO_ALERT_SETTING_DEFAULTS,
                     "json_response": _json_response,
                     "method_name": lambda request: request.method,
                     "ensure_schema": ensure_schema,
                     "_authed_account_name": _authed_account_name,
                     "_account_owns_node": _account_owns_node,
                     "_is_admin": _is_admin,
                     "_owner_pubkey": _owner_pubkey,
                     "ed25519_verify": ed25519_verify,
                     "blind_index": blind_index,
                     "d1_all": d1_all,
                     "d1_first": d1_first,
                     "d1_run": d1_run,
                     "decrypt_row": decrypt_row,
                     "encrypt_row": encrypt_row,
                     "_ap_org_alias_owner": _passthrough_alias,
                     "edge_cache_delete": edge_cache_delete,
                     "_catalog_record_matches_identity":
                         lambda record, owner, repo: bool(record),
                     "clean_string": lambda value, cap: str(value or "")[:cap],
                     "clean_media_png":
                         lambda value, cap: (str(value or ""), ""),
                     "_ap_origin": lambda env, request=None:
                         "https://forkmesh.com",
                     "_ap_domain_of": lambda origin: "forkmesh.com",
                     "_ap_actor_url": lambda origin, kind, handle:
                         origin + "/ap/repos/alice/proj",
                     "re": re,
                     "json": json,
                     "MAX_REPO_LOGO_BYTES": 1 << 20,
                     "MAX_REPO_BANNER_BYTES": 1 << 20,
                     "AP_ACTOR_REPO": "repo",
                     "AP_REPO_SETTING_DEFAULTS": AP_REPO_SETTING_DEFAULTS,
                     "LOGIN_MAX_SKEW_MS": 10 * 60 * 1000,
                     "ap": SimpleNamespace(
                         repo_handle=lambda owner, repo: owner + "." + repo),
                     "Date": SimpleNamespace(now=lambda: 1750000000000),
                 })


def _post(payload):
    async def read_json():
        return payload
    return SimpleNamespace(method="POST", json=read_json,
                           url="https://forkmesh.com/api/repo/alice/proj/about",
                           headers={})


def _settings_writes(log):
    return [e for e in log if isinstance(e, tuple) and e[0] == "d1_run"
            and "ap_repo_settings" in e[1]]


def test_owner_key_signed_settings_save_writes_and_purges():
    log = []
    ns = _about_post_env(log)
    resp = _run(ns["repo_about_handler"](
        None, _post({"ts": "1750000000000", "ownerSig": "GOODSIG",
                     "fediverse": {"federate": False}}),
        "alice", "proj"))
    assert resp["status"] == 200
    # The canonical binds owner AND repo, so an About token for one repo can
    # never be replayed against another.
    assert ("verify",
            "forkmesh-repo-about-v1\nalice\nproj\n1750000000000") in log
    writes = _settings_writes(log)
    assert len(writes) == 1
    assert writes[0][2][0] == "bi:ap-repo-settings:alice/proj"
    assert json.loads(writes[0][2][1]) == {
        "federate": False, "broadcastEvents": True, "acceptComments": True}
    # The switch flip drops the edge-parked actor doc, collections and
    # webfinger so it takes effect now, not after the TTL.
    purged = {e[1] for e in log if isinstance(e, tuple) and e[0] == "purge"}
    assert "https://forkmesh.com/ap/repos/alice/proj" in purged
    assert "https://forkmesh.com/ap/repos/alice/proj/followers" in purged
    assert any("webfinger" in key for key in purged)
    # A settings-only save must not blank the description or ping the desktop.
    assert not any("UPDATE repositories" in e[1] for e in _writes(log))
    assert not any("about_inbox" in e[1] for e in _writes(log))
    assert resp["data"]["description"] == "keep me"
    assert resp["data"]["fediverse"]["settings"]["federate"] is False


def _writes(log):
    return [e for e in log if isinstance(e, tuple) and e[0] == "d1_run"]


def _alert_writes(log):
    return [e for e in _writes(log) if "repo_alert_settings" in e[1]]


def test_repo_owner_cannot_switch_platform_status_alert_mail():
    # Operational mail is a platform is_admin control. Repository owners may
    # still send a legacy alerts object, but the repo About API ignores it.
    log = []
    ns = _about_post_env(log)
    resp = _run(ns["repo_about_handler"](
        None, _post({"ts": "1750000000000", "ownerSig": "GOODSIG",
                     "alerts": {"statusEmails": True}}),
        "alice", "proj"))
    assert resp["status"] == 200
    assert _alert_writes(log) == []
    assert "alerts" not in resp["data"]
    assert _settings_writes(log) == []


def test_bad_signature_is_rejected_before_any_write():
    log = []
    ns = _about_post_env(log)
    resp = _run(ns["repo_about_handler"](
        None, _post({"ts": "1750000000000", "ownerSig": "FORGED",
                     "fediverse": {"federate": False}}),
        "alice", "proj"))
    assert resp["status"] == 403
    assert _writes(log) == []


def test_stale_timestamp_is_rejected():
    log = []
    ns = _about_post_env(log)
    resp = _run(ns["repo_about_handler"](
        None, _post({"ts": "1", "ownerSig": "GOODSIG",
                     "fediverse": {"federate": False}}),
        "alice", "proj"))
    assert resp["status"] == 403
    assert _writes(log) == []


def test_unchanged_settings_save_writes_and_purges_nothing():
    log = []
    ns = _about_post_env(log, stored_settings={"federate": False})
    resp = _run(ns["repo_about_handler"](
        None, _post({"ts": "1750000000000", "ownerSig": "GOODSIG",
                     "fediverse": {"federate": False,
                                   "broadcastEvents": True,
                                   "acceptComments": True}}),
        "alice", "proj"))
    assert resp["status"] == 200
    assert _settings_writes(log) == []
    assert not any(isinstance(e, tuple) and e[0] == "purge" for e in log)


# --- publish / inbound-reply gates (source-level contract) ---------------------

def test_publish_repo_event_gates_repo_actor_on_switches():
    body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _ap_publish_repo_event"):
        ENTRY_TEXT.index("async def _ap_forget_remote")
    ]
    gate = body.index(
        'if (include_repo_actor and settings["federate"]')
    repo_candidate = body.index("candidates.append((AP_ACTOR_REPO,")
    user_candidate = body.index("candidates.append((AP_ACTOR_USER,")
    # The repo actor sits inside the gate; the author's own user actor stays
    # outside it (the switches govern the repo's presence, not the author's).
    assert gate < repo_candidate < user_candidate
    assert "include_repo_actor=True" in body
    assert body.index("_ap_repo_settings_get") < gate


def test_inbound_reply_gates_on_accept_comments():
    body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _ap_handle_create"):
        ENTRY_TEXT.index("async def _ap_handle_delete")
    ]
    gate = body.index('if not reply_settings["federate"] or '
                      'not reply_settings["acceptComments"]:')
    insert = body.index("INSERT OR IGNORE INTO ap_comments")
    assert gate < insert
    # Dropped replies are still 202-acknowledged (redelivery storms must not
    # be invited back with a 4xx).
    ack = body.index('json_response({"ok": True}, status=202)', gate)
    assert ack < insert


def test_schema_ships_ap_repo_settings_table():
    schema_text = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS ap_repo_settings" in schema_text
    migration = ROOT / "migrations" / "0035_ap_repo_settings.sql"
    assert "CREATE TABLE IF NOT EXISTS ap_repo_settings" in migration.read_text(
        encoding="utf-8")
