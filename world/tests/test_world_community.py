#!/usr/bin/env python3
"""Executable privacy, authorization, and persistence tests for World APIs."""

import asyncio
import json
from pathlib import Path
import sqlite3
import sys

import pytest
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import world_community as policy  # noqa: E402
import world_community_api as api  # noqa: E402


NOW = 1_800_000_000_000


def run_async_test(function):
    def wrapper():
        return asyncio.run(function())
    wrapper.__name__ = function.__name__
    wrapper.__doc__ = function.__doc__
    return wrapper


def mastodon_record(**updates):
    record = {
        "kind": "mastodon",
        "name": "Public Makers",
        "url": "https://social.forkmesh.dev/",
        "icon": "https://social.forkmesh.dev/icon.png",
        "description": "A public software community.",
        "languages": ["en", "fr-CA"],
        "topics": ["software", "open source"],
        "registration": "approval-required",
        "publicActivity": "moderate",
        "relationships": [{"name": "Peer Community", "public": True}],
        "consentedFollowers": [{
            "handle": "@alice@social.forkmesh.dev",
            "avatar": "https://cdn.forkmesh.dev/alice.png",
            "public": True,
            "consent": True,
            "accountVisibility": "public",
        }],
        "publicOnly": True,
        "consentConfirmed": True,
        "consentSource": "instance-operator",
        "consentCheckedAt": NOW,
    }
    record.update(updates)
    return record


def lemmy_record(**updates):
    record = {
        "kind": "lemmy",
        "name": "Code Forum",
        "url": "https://lemmy.forkmesh.dev/",
        "description": "Public development discussions.",
        "languages": ["en"],
        "topics": ["programming"],
        "registration": "open",
        "publicActivity": "low",
        "moderation": "community-moderated",
        "communities": [{"name": "Python", "public": True}],
        "approvedSubscriptions": [{
            "name": "!rust@lemmy.forkmesh.dev",
            "public": True,
            "consent": True,
        }],
        "publicOnly": True,
        "consentConfirmed": True,
        "consentSource": "moderator-reviewed",
        "consentCheckedAt": NOW,
    }
    record.update(updates)
    return record


def social_record(kind="x", **updates):
    platform = (
        {
            "name": "Public X profiles",
            "url": "https://x.com/",
            "profile": "https://x.com/forkmesh",
            "handle": "@forkmesh",
        }
        if kind == "x"
        else {
            "name": "Public Reddit profiles",
            "url": "https://www.reddit.com/",
            "profile": "https://www.reddit.com/user/forkmesh/",
            "handle": "u/forkmesh",
        }
    )
    record = {
        "kind": kind,
        "name": platform["name"],
        "url": platform["url"],
        "description": "Explicitly consented public profiles.",
        "languages": ["en"],
        "topics": ["software"],
        "publicActivity": "moderate",
        "consentedProfiles": [{
            "handle": platform["handle"],
            "profileUrl": platform["profile"],
            "avatar": "",
            "public": True,
            "consent": True,
            "accountVisibility": "public",
            "consentSource": "user-approved",
            "consentCheckedAt": NOW,
        }],
        "publicOnly": True,
        "consentConfirmed": True,
        "consentSource": "moderator-reviewed",
        "consentCheckedAt": NOW,
    }
    record.update(updates)
    return record


def test_fediverse_record_is_bounded_https_public_and_consent_only():
    record, error = policy.normalize_fediverse_record(
        mastodon_record(), NOW)
    assert error == ""
    assert record["url"] == "https://social.forkmesh.dev/"
    assert record["host"] == "social.forkmesh.dev"
    assert record["consentedFollowers"] == [{
        "handle": "@alice@social.forkmesh.dev",
        "avatar": "https://cdn.forkmesh.dev/alice.png",
        "profileUrl": "",
        "public": True,
        "consent": True,
        "consentEvidence": {
            "type": "operator-attestation",
            "source": "instance-operator",
            "checkedAt": NOW,
            "oauthVerifiedByForkMesh": False,
        },
    }]
    assert record["publicOnly"] is True
    assert record["consentConfirmed"] is True
    assert "private" not in json.dumps(record).lower()

    row = {
        "instance_id": "a" * 32,
        "kind": "mastodon",
        "data": json.dumps(record),
        "updated_at": NOW,
    }
    payload = policy.fediverse_directory_payload([row], NOW)
    assert payload["mastodon"][0]["id"] == "a" * 32
    assert payload["lemmy"] == []
    assert payload["privacy"]["privateFollowersExcluded"] is True
    assert payload["privacy"][
        "operatorAttestationsAreNotOAuthVerification"] is True


@pytest.mark.parametrize("kind", ["x", "reddit"])
def test_public_social_profiles_require_consent_platform_urls_and_evidence(kind):
    record, error = policy.normalize_fediverse_record(
        social_record(kind), NOW)
    assert error == ""
    profile = record["consentedProfiles"][0]
    assert profile["public"] is True
    assert profile["consent"] is True
    assert profile["consentEvidence"] == {
        "type": "operator-attestation",
        "source": "user-approved",
        "checkedAt": NOW,
        "oauthVerifiedByForkMesh": False,
    }
    assert "token" not in json.dumps(profile).lower()
    payload = policy.fediverse_directory_payload([{
        "instance_id": "b" * 32,
        "data": json.dumps(record),
        "updated_at": NOW,
    }], NOW)
    assert payload[kind][0]["consentedProfiles"][0]["handle"]

    wrong_host = "https://www.reddit.com/user/forkmesh/"
    if kind == "reddit":
        wrong_host = "https://x.com/forkmesh"
    invalid = social_record(kind)
    invalid["consentedProfiles"][0]["profileUrl"] = wrong_host
    assert policy.normalize_fediverse_record(
        invalid, NOW)[1] == "invalid_consented_profiles"

    missing_consent = social_record(kind)
    missing_consent["consentedProfiles"][0]["consent"] = False
    assert policy.normalize_fediverse_record(
        missing_consent, NOW)[1] == "invalid_consented_profiles"


def test_media_item_never_accepts_user_supplied_live_provider_metadata():
    item, error = policy.normalize_media_item({
        "title": "User playlist label",
        "provider": "somafm",
        "url": "https://somafm.com/groovesalad/",
        "termsConfirmed": True,
        "noRebroadcast": True,
        "providerMetadata": {
            "status": "available",
            "title": "Unverified claim",
            "permissionConfirmed": True,
        },
    })
    assert error == ""
    assert item["providerMetadata"] == {
        "status": "unavailable",
        "reason": "not_received",
    }


@run_async_test
async def test_social_directory_api_persists_consent_only_x_records():
    runtime = FakeRuntime()
    created = await api.handle_fediverse(
        runtime.use("POST", "admin", social_record("x")),
        "/api/world/fediverse",
    )
    assert created["status"] == 201
    listed = await api.handle_fediverse(
        runtime.use("GET"), "/api/world/fediverse")
    assert listed["data"]["x"][0]["consentedProfiles"][0]["handle"] == (
        "@forkmesh")
    assert listed["data"]["x"][0]["consentedProfiles"][0][
        "consentEvidence"]["oauthVerifiedByForkMesh"] is False


@pytest.mark.parametrize("updates,error", [
    ({"url": "http://social.forkmesh.dev/"}, "invalid_public_https_url"),
    ({"url": "https://127.0.0.1/"}, "invalid_public_https_url"),
    ({"url": "https://social.local/"}, "invalid_public_https_url"),
    ({"url": "https://user:pass@social.forkmesh.dev/"},
     "invalid_public_https_url"),
    ({"url": "https://social.forkmesh.dev/private"},
     "invalid_public_https_url"),
    ({"icon": "https://tracking.example.org/pixel"},
     "invalid_icon_url"),
    ({"publicOnly": False}, "public_consent_attestation_required"),
    ({"consentConfirmed": False}, "public_consent_attestation_required"),
    ({"consentSource": "scraped"}, "invalid_consent_source"),
    ({"consentCheckedAt": NOW + 60 * 60 * 1000},
     "invalid_consent_timestamp"),
    ({"followers": [{"handle": "@private"}]},
     "sensitive_fields_not_allowed"),
    ({"privateAccounts": ["@private"]}, "sensitive_fields_not_allowed"),
    ({"searchTerms": ["secret"]}, "sensitive_fields_not_allowed"),
    ({"unknown": "field"}, "unsupported_field"),
    ({"languages": ["en"] * 9}, "invalid_languages"),
    ({"topics": ["x"] * 13}, "invalid_topics"),
])
def test_fediverse_record_rejects_nonpublic_unbounded_or_sensitive_input(
        updates, error):
    assert policy.normalize_fediverse_record(
        mastodon_record(**updates), NOW) == (None, error)


def test_relationship_entries_require_explicit_public_and_user_consent():
    missing_public = mastodon_record(consentedFollowers=[{
        "handle": "@alice@example.social",
        "consent": True,
    }])
    assert policy.normalize_fediverse_record(
        missing_public, NOW)[1] == "invalid_consented_followers"

    missing_consent = mastodon_record(consentedFollowers=[{
        "handle": "@alice@example.social",
        "public": True,
    }])
    assert policy.normalize_fediverse_record(
        missing_consent, NOW)[1] == "invalid_consented_followers"

    private_account = mastodon_record(consentedFollowers=[{
        "handle": "@alice@example.social",
        "public": True,
        "consent": True,
        "accountVisibility": "private",
    }])
    assert policy.normalize_fediverse_record(
        private_account, NOW)[1] == "invalid_consented_followers"

    record, error = policy.normalize_fediverse_record(lemmy_record(), NOW)
    assert error == ""
    assert record["approvedSubscriptions"][0] == {
        "name": "!rust@lemmy.forkmesh.dev",
        "public": True,
        "consent": True,
    }


@pytest.mark.parametrize("provider,url", [
    ("somafm", "https://somafm.com/groovesalad/"),
    ("youtube", "https://www.youtube.com/watch?v=public-video"),
    ("vimeo", "https://vimeo.com/123456"),
    ("soundcloud", "https://soundcloud.com/public-artist/public-track"),
    ("twitch", "https://www.twitch.tv/public_channel"),
    ("internet-archive", "https://archive.org/details/public-recording"),
    ("peertube", "https://video.forkmesh.dev/w/abcdEFGH1234"),
])
def test_media_provider_pages_are_external_https_only(provider, url):
    item, error = policy.normalize_media_item({
        "title": "Community program",
        "provider": provider,
        "url": url,
        "termsConfirmed": True,
        "noRebroadcast": True,
        "autoplay": False,
    })
    assert error == ""
    assert item["provider"] == provider
    assert item["url"].startswith("https://")
    assert item["autoplay"] is False
    assert item["externalPlaybackOnly"] is True


@pytest.mark.parametrize("value,error", [
    ({
        "title": "Track",
        "provider": "somafm",
        "url": "http://somafm.com/",
        "termsConfirmed": True,
        "noRebroadcast": True,
    }, "invalid_provider_url"),
    ({
        "title": "Raw stream",
        "provider": "somafm",
        "url": "https://somafm.com/live.mp3",
        "termsConfirmed": True,
        "noRebroadcast": True,
    }, "invalid_provider_url"),
    ({
        "title": "Internal",
        "provider": "peertube",
        "url": "https://video.local/w/abcdEFGH",
        "termsConfirmed": True,
        "noRebroadcast": True,
    }, "invalid_provider_url"),
    ({
        "title": "Unknown",
        "provider": "external",
        "url": "https://unknown.forkmesh.dev/watch/1",
        "termsConfirmed": True,
        "noRebroadcast": True,
    }, "invalid_provider_url"),
    ({
        "title": "Wrong provider",
        "provider": "vimeo",
        "url": "https://www.youtube.com/watch?v=abc",
        "termsConfirmed": True,
        "noRebroadcast": True,
    }, "invalid_provider_url"),
    ({
        "title": "Query leak",
        "provider": "somafm",
        "url": "https://somafm.com/?search=private",
        "termsConfirmed": True,
        "noRebroadcast": True,
    }, "invalid_provider_url"),
    ({
        "title": "Autoplay",
        "provider": "vimeo",
        "url": "https://vimeo.com/1234",
        "termsConfirmed": True,
        "noRebroadcast": True,
        "autoplay": True,
    }, "autoplay_not_allowed"),
    ({
        "title": "No terms",
        "provider": "vimeo",
        "url": "https://vimeo.com/1234",
        "noRebroadcast": True,
    }, "provider_terms_confirmation_required"),
    ({
        "title": "Rebroadcast",
        "provider": "vimeo",
        "url": "https://vimeo.com/1234",
        "termsConfirmed": True,
    }, "no_rebroadcast_confirmation_required"),
])
def test_media_items_reject_streams_unsafe_links_and_missing_affirmations(
        value, error):
    assert policy.normalize_media_item(value) == (None, error)


def test_media_schedule_is_utc_bounded_and_item_id_is_opaque():
    schedule, error = policy.normalize_media_schedule({
        "title": "Release watch party",
        "sessionType": "watch-party",
        "itemId": "b" * 32,
        "startsAt": NOW + 60_000,
        "endsAt": NOW + 3_600_000,
    }, NOW)
    assert error == ""
    assert schedule["startsAt"] == NOW + 60_000
    assert schedule["endsAt"] == NOW + 3_600_000
    assert policy.normalize_media_schedule({
        "title": "Too long",
        "startsAt": NOW,
        "endsAt": NOW + policy.MEDIA_MAX_SCHEDULE_DURATION_MS + 1,
    }, NOW)[1] == "invalid_schedule_window"
    assert policy.normalize_media_schedule({
        "title": "Bad item",
        "itemId": "../other-space",
        "startsAt": NOW,
        "endsAt": NOW + 60_000,
    }, NOW)[1] == "invalid_item_id"


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT / "migrations" / "0052_world_fediverse_media.sql")
            .read_text(encoding="utf-8")
        )
        self.db.executescript(
            (ROOT / "migrations" / "0059_world_media_playback.sql")
            .read_text(encoding="utf-8")
        )
        self.request_method = "GET"
        self.request_data = {}
        self.actor = ""
        self.clock = NOW
        self.ids = 0
        self.admins = {"admin"}
        self.users = {
            name: {"bi": "bi-" + name, "name": name}
            for name in ("admin", "alice", "bob", "mallory")
        }
        self.audits = []

    def use(self, method, actor="", data=None, now=None):
        self.request_method = method
        self.actor = actor
        self.request_data = {} if data is None else data
        if now is not None:
            self.clock = now
        return self

    def method(self):
        return self.request_method

    def now(self):
        return self.clock

    def new_id(self):
        self.ids += 1
        return f"{self.ids:032x}"

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {
            "status": status,
            "data": data,
            "cache_control": cache_control,
            "headers": dict(extra_headers or {}),
        }

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        if not isinstance(self.request_data, dict):
            return None, "invalid_json"
        return self.request_data, ""

    async def session(self, _data):
        user = self.users.get(self.actor)
        return ((user or {}).get("bi", ""), user)

    async def is_admin(self, name):
        return name in self.admins

    async def account(self, name):
        user = self.users.get(str(name or "").lower())
        return ((user or {}).get("bi", ""), (user or {}).get("name", ""))

    async def audit(self, actor, action, target_type="", target="",
                    outcome="success", details=None):
        self.audits.append({
            "actor": actor,
            "action": action,
            "target_type": target_type,
            "target": target,
            "outcome": outcome,
            "details": details or {},
        })

    async def d1_all(self, sql, *args):
        return [
            dict(row) for row in self.db.execute(sql, args).fetchall()
        ]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()


@run_async_test
async def test_fediverse_api_is_public_read_admin_write_and_audited():
    runtime = FakeRuntime()
    empty = await api.handle_fediverse(
        runtime.use("GET"), "/api/world/fediverse")
    assert empty["status"] == 200
    assert empty["data"]["mastodon"] == []
    assert empty["cache_control"].startswith("public")

    denied = await api.handle_fediverse(
        runtime.use("POST", "alice", mastodon_record()),
        "/api/world/fediverse",
    )
    assert denied["status"] == 403
    assert runtime.audits[-1]["outcome"] == "denied"

    created = await api.handle_fediverse(
        runtime.use("POST", "admin", mastodon_record()),
        "/api/world/fediverse",
    )
    assert created["status"] == 201
    instance_id = created["data"]["instance"]["id"]
    assert runtime.audits[-1]["action"] == "world.fediverse.create"

    listed = await api.handle_fediverse(
        runtime.use("GET"), "/api/world/fediverse")
    assert listed["data"]["mastodon"][0]["id"] == instance_id
    assert listed["data"]["mastodon"][0]["url"] == (
        "https://social.forkmesh.dev/")

    no_reaffirmation = await api.handle_fediverse(
        runtime.use("PATCH", "admin", {"description": "Changed"}),
        f"/api/world/fediverse/{instance_id}",
    )
    assert no_reaffirmation["status"] == 400
    assert no_reaffirmation["data"]["error"] == (
        "public_consent_attestation_required")

    updated = await api.handle_fediverse(
        runtime.use("PATCH", "admin", {
            "description": "Updated public description",
            "publicOnly": True,
            "consentConfirmed": True,
            "consentCheckedAt": NOW,
        }),
        f"/api/world/fediverse/{instance_id}",
    )
    assert updated["status"] == 200
    assert updated["data"]["instance"]["description"] == (
        "Updated public description")
    assert runtime.audits[-1]["action"] == "world.fediverse.update"

    deleted = await api.handle_fediverse(
        runtime.use("DELETE", "admin"),
        f"/api/world/fediverse/{instance_id}",
    )
    assert deleted["status"] == 200
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM world_fediverse_instances").fetchone()[0] == 0
    assert runtime.audits[-1]["action"] == "world.fediverse.delete"


@run_async_test
async def test_media_api_enforces_authentication_owner_and_moderator_roles():
    runtime = FakeRuntime()
    unauthenticated = await api.handle_media(
        runtime.use("GET"), "/api/world/media/spaces")
    assert unauthenticated["status"] == 401

    created = await api.handle_media(
        runtime.use("POST", "alice", {
            "name": "Town Square Listening Room",
            "description": "Official provider links only.",
            "sessionType": "listening-room",
        }),
        "/api/world/media/spaces",
    )
    assert created["status"] == 201
    space_id = created["data"]["space"]["id"]
    assert created["data"]["space"]["viewerRole"] == "owner"

    visible = await api.handle_media(
        runtime.use("GET", "bob"),
        f"/api/world/media/spaces/{space_id}",
    )
    assert visible["status"] == 200
    assert visible["data"]["space"]["viewerRole"] == ""
    assert visible["data"]["space"]["canModerate"] is False

    denied = await api.handle_media(
        runtime.use("POST", "bob", {
            "title": "SomaFM",
            "provider": "somafm",
            "url": "https://somafm.com/groovesalad/",
            "termsConfirmed": True,
            "noRebroadcast": True,
        }),
        f"/api/world/media/spaces/{space_id}/items",
    )
    assert denied["status"] == 403
    assert runtime.audits[-1]["outcome"] == "denied"

    granted = await api.handle_media(
        runtime.use("POST", "alice", {"account": "bob"}),
        f"/api/world/media/spaces/{space_id}/roles",
    )
    assert granted["status"] == 201
    role_id = granted["data"]["role"]["id"]
    assert granted["data"]["role"]["role"] == "moderator"

    item = await api.handle_media(
        runtime.use("POST", "bob", {
            "title": "SomaFM Groove Salad",
            "provider": "somafm",
            "url": "https://somafm.com/groovesalad/",
            "termsConfirmed": True,
            "noRebroadcast": True,
            "autoplay": False,
        }),
        f"/api/world/media/spaces/{space_id}/items",
    )
    assert item["status"] == 201
    item_id = item["data"]["item"]["id"]
    assert item["data"]["item"]["externalPlaybackOnly"] is True

    schedule = await api.handle_media(
        runtime.use("POST", "bob", {
            "title": "Shared listening hour",
            "sessionType": "listening-room",
            "itemId": item_id,
            "startsAt": NOW + 60_000,
            "endsAt": NOW + 3_600_000,
        }),
        f"/api/world/media/spaces/{space_id}/schedules",
    )
    assert schedule["status"] == 201
    assert schedule["data"]["schedule"]["itemId"] == item_id

    stopped = await api.handle_media(
        runtime.use("POST", "bob", {"itemId": item_id}),
        f"/api/world/media/spaces/{space_id}/stop",
    )
    assert stopped["status"] == 200
    assert stopped["data"]["playbackState"] == "stopped"
    assert runtime.db.execute(
        "SELECT status FROM world_media_items WHERE item_id=?",
        (item_id,),
    ).fetchone()[0] == "stopped"

    removed = await api.handle_media(
        runtime.use("DELETE", "bob"),
        f"/api/world/media/spaces/{space_id}/items/{item_id}",
    )
    assert removed["status"] == 200
    assert runtime.db.execute(
        "SELECT status FROM world_media_items WHERE item_id=?",
        (item_id,),
    ).fetchone()[0] == "removed"

    cannot_manage_roles = await api.handle_media(
        runtime.use("POST", "bob", {"account": "mallory"}),
        f"/api/world/media/spaces/{space_id}/roles",
    )
    assert cannot_manage_roles["status"] == 403

    revoked = await api.handle_media(
        runtime.use("DELETE", "alice"),
        f"/api/world/media/spaces/{space_id}/roles/{role_id}",
    )
    assert revoked["status"] == 200
    denied_again = await api.handle_media(
        runtime.use("POST", "bob", {}),
        f"/api/world/media/spaces/{space_id}/stop",
    )
    assert denied_again["status"] == 403


@run_async_test
async def test_shared_playback_clock_is_server_authoritative_and_conflict_safe():
    runtime = FakeRuntime()
    created = await api.handle_media(
        runtime.use("POST", "alice", {
            "name": "Synchronized release room",
            "sessionType": "watch-party",
        }),
        "/api/world/media/spaces",
    )
    space_id = created["data"]["space"]["id"]
    added = await api.handle_media(
        runtime.use("POST", "alice", {
            "title": "Release presentation",
            "provider": "vimeo",
            "url": "https://vimeo.com/12345",
            "termsConfirmed": True,
            "noRebroadcast": True,
        }),
        f"/api/world/media/spaces/{space_id}/items",
    )
    item_id = added["data"]["item"]["id"]

    started = await api.handle_media(
        runtime.use("POST", "alice", {
            "state": "playing",
            "itemId": item_id,
            "positionMs": 1500,
            "expectedRevision": 0,
        }),
        f"/api/world/media/spaces/{space_id}/playback",
    )
    assert started["status"] == 200
    assert started["data"]["playback"]["revision"] == 1
    assert started["data"]["playback"]["coordinationOnly"] is True
    assert started["data"]["playback"]["requiresLocalPlaybackConsent"] is True

    # A second authenticated client observes the same UTC-derived clock without
    # receiving media bytes or an autoplay command.
    observed = await api.handle_media(
        runtime.use("GET", "bob", now=NOW + 2500),
        f"/api/world/media/spaces/{space_id}",
    )
    playback = observed["data"]["playback"]
    assert playback["state"] == "playing"
    assert playback["itemId"] == item_id
    assert playback["positionMs"] == 4000
    assert observed["data"]["mediaPolicy"] == {
        "httpsProviderPagesOnly": True,
        "autoplay": False,
        "serverStoresMedia": False,
        "rebroadcastAuthorized": False,
        "serverAuthoritativeCoordination": True,
        "localPlaybackConsentRequired": True,
        "providerTrackMetadataRequiresPermittedReceipt": True,
        "userPlaylistTitlesAreNotProviderTrackMetadata": True,
    }

    denied = await api.handle_media(
        runtime.use("PATCH", "bob", {
            "state": "paused",
            "itemId": item_id,
            "positionMs": 4000,
            "expectedRevision": 1,
        }),
        f"/api/world/media/spaces/{space_id}/playback",
    )
    assert denied["status"] == 403

    stale = await api.handle_media(
        runtime.use("PATCH", "alice", {
            "state": "paused",
            "itemId": item_id,
            "positionMs": 4000,
            "expectedRevision": 0,
        }),
        f"/api/world/media/spaces/{space_id}/playback",
    )
    assert stale["status"] == 409
    assert stale["data"]["error"] == "playback_conflict"
    assert stale["data"]["playback"]["revision"] == 1


def test_media_playback_validation_requires_item_clock_and_revision():
    valid, error = policy.normalize_media_playback({
        "state": "playing",
        "itemId": "a" * 32,
        "positionMs": 1234,
        "expectedRevision": 7,
    })
    assert error == ""
    assert valid["positionMs"] == 1234
    assert policy.normalize_media_playback({
        "state": "playing",
        "positionMs": 0,
        "expectedRevision": 0,
    })[1] == "invalid_item_id"
    assert policy.normalize_media_playback({
        "state": "paused",
        "itemId": "a" * 32,
        "positionMs": 0,
    })[1] == "invalid_playback_revision"


@run_async_test
async def test_media_retention_soft_archives_then_purges_bounded_rows():
    runtime = FakeRuntime()
    created = await api.handle_media(
        runtime.use("POST", "alice", {
            "name": "Old room",
            "sessionType": "watch-party",
        }),
        "/api/world/media/spaces",
    )
    space_id = created["data"]["space"]["id"]
    old = NOW - policy.MEDIA_ACTIVE_RETENTION_MS - 1
    runtime.db.execute(
        "UPDATE world_media_spaces SET last_activity_at=? WHERE space_id=?",
        (old, space_id),
    )
    runtime.db.commit()

    await api.cleanup_media_records(runtime.use("GET", now=NOW))
    archived = runtime.db.execute(
        "SELECT status,archived_at FROM world_media_spaces WHERE space_id=?",
        (space_id,),
    ).fetchone()
    assert tuple(archived) == ("archived", NOW)

    purge_now = NOW + policy.MEDIA_ARCHIVED_RETENTION_MS + 1
    await api.cleanup_media_records(runtime.use("GET", now=purge_now))
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM world_media_spaces WHERE space_id=?",
        (space_id,),
    ).fetchone()[0] == 0


def test_worker_schema_routes_auth_audit_and_hourly_cleanup_are_wired():
    entry = (
        (SRC / "entry.py").read_text(encoding="utf-8")
        + (SRC / "app_runtime.py").read_text(encoding="utf-8")
    )
    schema = (SRC / "schema.py").read_text(encoding="utf-8")
    migration = (
        ROOT / "migrations" / "0052_world_fediverse_media.sql"
    ).read_text(encoding="utf-8")
    for table in (
        "world_fediverse_instances",
        "world_media_spaces",
        "world_media_roles",
        "world_media_items",
        "world_media_schedules",
    ):
        assert f"CREATE TABLE IF NOT EXISTS {table}" in migration
        assert f"CREATE TABLE IF NOT EXISTS {table}" in schema
    playback_migration = (
        ROOT / "migrations" / "0059_world_media_playback.sql"
    ).read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS world_media_playback" in (
        playback_migration)
    assert "CREATE TABLE IF NOT EXISTS world_media_playback" in schema
    social_migration = (
        ROOT / "migrations" / "0061_world_social_directory.sql"
    ).read_text(encoding="utf-8")
    assert "'mastodon', 'lemmy', 'x', 'reddit'" in social_migration
    assert 'world_community_api = _LazyModule("world_community_api")' in entry
    assert "world_fediverse_directory_handler" in entry
    assert "world_media_handler" in entry
    assert "_account_session_record(" in entry
    assert "return await _is_admin(self.env, name)" in entry
    assert "_audit_sensitive_action(" in entry
    due_phases = entry[
        entry.index("def _cron_due_phases"):
        entry.index("async def _run_cron_phase")
    ]
    phase_runner = entry[
        entry.index("async def _run_cron_phase"):
        entry.index("def _cron_tick_phase_order")
    ]
    assert '"world-media-cleanup"' in due_phases
    assert "hourly == 52" in due_phases
    assert 'if phase == "world-media-cleanup":' in phase_runner
    assert "return await cleanup_world_media_records(env)" in phase_runner
    inactive = entry.index('"/api/world/inactive"')
    directory = entry.index('url.path == "/api/world/fediverse"')
    media = entry.index('url.path == "/api/world/media"')
    websocket = entry.index('if url.path in ("/api/world/ws"')
    assert inactive < directory < media < websocket


def test_world_client_uses_live_directory_and_server_media_not_local_storage():
    app = (
        ROOT.parent / "world" / "public" / "world" / "world.js"
    ).read_text(encoding="utf-8")
    scene = world_scene_source()
    assert 'this.fetchJSON("/api/world/fediverse"' in app
    assert 'this.fetchJSON("/api/world/media/spaces"' in app
    assert "/world/fediverse-directory.json" not in app
    assert "MEDIA_ROOM_KEY" not in app
    assert "termsConfirmed: true" in app
    assert "noRebroadcast: true" in app
    assert "autoplay: false" in app
    for method in (
        "createMediaSpace",
        "loadMediaSpace",
        "addMediaItem",
        "removeMediaItem",
        "updateMediaPlayback",
        "stopMediaRoom",
        "scheduleMediaRoom",
        "cancelMediaSchedule",
        "grantMediaModerator",
        "removeMediaModerator",
    ):
        assert f"async {method}" in app
    assert "function updateFediverseDirectory" in scene
    assert "Mastodon" in scene
    assert "Lemmy" in scene
    assert "Reddit" in scene
    assert 'network: "X"' in scene
    assert "function updateMediaSpaces" in scene
    assert "shared-media-spaces" in scene
    assert "playlist and schedule become physical" in scene
    assert "providerTrackMetadataRequiresPermittedReceipt" in (
        ROOT.parent / "world" / "src" / "world_community.py"
    ).read_text(encoding="utf-8")
    assert "Current provider track metadata unavailable" in app
