#!/usr/bin/env python3
"""External repository import/stub/invitation/logo contract tests."""

import asyncio
import base64
import concurrent.futures
import copy
import importlib.util
import json
import sqlite3
import threading
from pathlib import Path
from urllib.parse import unquote

import pytest


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "src" / "repository_imports.py"
ENTRY_TEXT = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
RUNTIME_TEXT = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
SCHEMA_TEXT = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
REPOS_VIEW = (
    ROOT / "public" / "dashboard" / "partials" / "views" / "repos.html"
).read_text(encoding="utf-8")
NEW_REPO_MODAL = (
    ROOT / "public" / "dashboard" / "partials" / "new-repo-modal.html"
).read_text(encoding="utf-8")
REPO_LIST_JS = (
    ROOT / "public" / "dashboard" / "js" / "05-repo-list-explorer.js"
).read_text(encoding="utf-8")
REPO_INIT_JS = (
    ROOT / "public" / "dashboard" / "js" / "08-repo-detail-network.js"
).read_text(encoding="utf-8")
REPO_CONTENT_JS = (
    ROOT / "public" / "dashboard" / "js" / "06-repo-content.js"
).read_text(encoding="utf-8")
BUILT_DASHBOARD = (
    ROOT / "public" / "dashboard" / "repos" / "index.html"
).read_text(encoding="utf-8")

spec = importlib.util.spec_from_file_location(
    "forkmesh_repository_imports", MODULE_PATH)
imports = importlib.util.module_from_spec(spec)
spec.loader.exec_module(imports)


def run(awaitable):
    return asyncio.run(awaitable)


def github_root(private=False, admin=False):
    return {
        "id": 12345,
        "full_name": "octo/widget",
        "html_url": "https://github.com/octo/widget",
        "private": private,
        "description": "A useful widget",
        "license": {
            "spdx_id": "MIT",
            "name": "MIT License",
            "html_url": "https://api.github.com/licenses/mit",
        },
        "topics": ["developer-tools", "mesh"],
        "language": "Python",
        "default_branch": "main",
        "permissions": {
            "admin": admin,
            "maintain": False,
            "push": admin,
            "pull": True,
        },
        "owner": {
            "login": "octo",
            "type": "Organization",
            "html_url": "https://github.com/octo",
            "avatar_url": "https://avatars.githubusercontent.com/u/1",
        },
        "created_at": "2024-01-02T03:04:05Z",
        "updated_at": "2026-07-22T03:04:05Z",
        "archived": False,
        "stargazers_count": 10,
        "forks_count": 2,
        "open_issues_count": 3,
        "subscribers_count": 4,
    }


def github_extras():
    return {
        "topics": {"names": ["developer-tools", "mesh"]},
        "languages": {"Python": 900, "JavaScript": 100},
        "structure": {
            "tree": [
                {"path": "src", "type": "tree"},
                {"path": "src/app.py", "type": "blob"},
                {"path": "manage.py", "type": "blob"},
                {"path": ".github/workflows/test.yml", "type": "blob"},
            ],
        },
        "branches": [{
            "name": "main",
            "protected": True,
            "commit": {"sha": "a" * 40},
        }],
        "contributors": [{
            "login": "contrib",
            "public_email": "contrib@example.test",
            "contributions": 17,
            "html_url": "https://github.com/contrib",
            "avatar_url": "https://avatars.githubusercontent.com/u/2",
            "type": "User",
        }],
        "commits": [{
            "sha": "b" * 40,
            "html_url": "https://github.com/octo/widget/commit/" + "b" * 40,
            "author": {"login": "contrib", "type": "User"},
            "commit": {
                "message": "Add feature\n\nDetails",
                "author": {
                    "name": "Contributor",
                    "date": "2026-07-20T00:00:00Z",
                },
            },
        }],
        "issues": [
            {
                "number": 1,
                "title": "Real issue",
                "state": "open",
                "user": {"login": "contrib"},
                "created_at": "2026-07-20T00:00:00Z",
                "updated_at": "2026-07-21T00:00:00Z",
                "html_url": "https://github.com/octo/widget/issues/1",
            },
            {
                "number": 2,
                "title": "Pull in issue result",
                "pull_request": {"url": "https://api.github.com/pulls/2"},
            },
        ],
        "pullRequests": [{
            "number": 2,
            "title": "Improve widget",
            "state": "open",
            "draft": False,
            "user": {"login": "contrib"},
            "html_url": "https://github.com/octo/widget/pull/2",
        }],
        "releases": [{
            "tag_name": "v1.0.0",
            "name": "First release",
            "draft": False,
            "prerelease": False,
            "published_at": "2026-07-01T00:00:00Z",
            "html_url": "https://github.com/octo/widget/releases/tag/v1.0.0",
        }],
    }


def test_provider_url_parser_is_canonical_and_not_an_ssrf_surface():
    github = imports.parse_provider_source(
        "https://github.com/octo/widget.git")
    assert github == {
        "provider": "github",
        "host": "github.com",
        "owner": "octo",
        "name": "widget",
        "fullName": "octo/widget",
        "canonicalUrl": "https://github.com/octo/widget",
        "metadataPath": "/repos/octo/widget",
    }
    gitlab = imports.parse_provider_source(
        "gitlab.com/group/subgroup/project")
    assert gitlab["provider"] == "gitlab"
    assert gitlab["owner"] == "group/subgroup"
    assert gitlab["metadataPath"] == \
        "/projects/group%2Fsubgroup%2Fproject?license=true"
    codeberg = imports.parse_provider_source(
        "https://codeberg.org/m33/tootbot-py.git")
    assert codeberg["provider"] == "codeberg"
    assert codeberg["owner"] == "m33"
    assert codeberg["name"] == "tootbot-py"
    assert codeberg["metadataPath"] == "/repos/m33/tootbot-py"

    rejected = (
        "http://github.com/octo/widget",
        "https://user:pass@github.com/octo/widget",
        "https://api.github.com/repos/octo/widget",
        "https://github.com/octo/widget/issues",
        "https://github.com/octo/widget?token=secret",
        "https://github.com:444/octo/widget",
        "https://127.0.0.1/octo/widget",
        "https://github.com:bogus/octo/widget",
    )
    for value in rejected:
        with pytest.raises(imports.ProviderSourceError):
            imports.parse_provider_source(value)


def test_namespace_parser_accepts_only_canonical_organization_urls():
    assert imports.parse_provider_namespace("https://codeberg.org/m33") == {
        "provider": "codeberg", "namespace": "m33"}
    assert imports.parse_provider_namespace("codeberg.org/m33/") == {
        "provider": "codeberg", "namespace": "m33"}
    assert imports.parse_provider_namespace("https://gitlab.com/acme") == {
        "provider": "gitlab", "namespace": "acme"}
    # A GitLab organization nests subgroups, so a deeper path is a namespace
    # in its own right rather than an error.
    assert imports.parse_provider_namespace(
        "https://gitlab.com/acme/platform/tools") == {
            "provider": "gitlab", "namespace": "acme/platform/tools"}
    for value in (
            "https://codeberg.org/m33/repository",
            "https://codeberg.org/m33?tab=repositories",
            "https://user:secret@codeberg.org/m33",
            "https://gitlab.com/acme?tab=projects",
            "https://gitlab.com/",
            "https://github.com/m33",
            "http://codeberg.org/m33"):
        with pytest.raises(imports.ProviderSourceError):
            imports.parse_provider_namespace(value)


def test_gitlab_group_discovery_paths_walk_subgroups_before_users():
    top = imports.namespace_discovery_paths("gitlab", "acme", 1)
    assert top == [
        "/groups/acme/projects?include_subgroups=true&archived=false"
        "&order_by=path&sort=asc&per_page=50&page=1",
        "/users/acme/projects?archived=false&order_by=path&sort=asc"
        "&per_page=50&page=1",
    ]
    # A subgroup path can never name a user, so no request is spent probing it.
    nested = imports.namespace_discovery_paths("gitlab", "acme/platform", 2)
    assert nested == [
        "/groups/acme%2Fplatform/projects?include_subgroups=true"
        "&archived=false&order_by=path&sort=asc&per_page=50&page=2",
    ]


def test_provider_fetch_plan_is_metadata_only():
    source = imports.parse_provider_source("https://github.com/octo/widget")
    paths = imports.provider_extra_paths(source, github_root())
    assert set(paths) == {
        "topics", "languages", "structure", "branches", "contributors",
        "commits", "issues", "pullRequests", "releases",
    }
    joined = "\n".join(paths.values()).lower()
    for forbidden in ("/contents", "/archive", "/tarball", "/zipball", "/raw/"):
        assert forbidden not in joined


def test_provider_snapshot_stops_immediately_when_rate_budget_is_exhausted():
    calls = []

    async def fetch(env, provider, path, token):
        calls.append((provider, path, token))
        return {
            "status": 200,
            "data": github_root(),
            "headers": {
                "x-ratelimit-limit": "60",
                "x-ratelimit-remaining": "0",
                "x-ratelimit-reset": "1900000000",
            },
        }

    source = imports.parse_provider_source("https://github.com/octo/widget")
    root, extras, rate, incomplete = run(
        imports.fetch_provider_snapshot(fetch, None, source, ""))
    assert root["id"] == 12345
    assert extras == {}
    assert rate["remaining"] == "0"
    assert len(calls) == 1
    assert {item.split(":", 1)[0] for item in incomplete} == {
        "topics", "languages", "structure", "branches", "contributors",
        "commits", "issues", "pullRequests", "releases",
    }


def test_github_record_normalizes_all_requested_metadata_and_truthful_status():
    source = imports.parse_provider_source("https://github.com/octo/widget")
    record = imports.build_repository_record(
        source, github_root(), github_extras(), "alice", "stub",
        1_700_000_000_000)
    assert record["status"] == "stub_only"
    assert record["statusLabel"] == "Stub only"
    assert record["mirrored"] is False
    assert "not mirrored" in record["mirrorNotice"].lower()
    assert "does not own or control" in record["ownershipNotice"]
    assert record["metadata"]["license"]["spdxId"] == "MIT"
    assert record["metadata"]["topics"] == ["developer-tools", "mesh"]
    assert record["metadata"]["languages"]["Python"] == 900
    assert record["metadata"]["fileStructure"] == [".github", "manage.py", "src"]
    assert record["metadata"]["frameworks"] == ["Django"]
    assert record["metadata"]["projectCategory"] == "web application"
    assert record["metadata"]["branches"][0]["protected"] is True
    assert record["metadata"]["contributors"][0]["login"] == "contrib"
    assert record["metadata"]["commits"][0]["sha"] == "b" * 40
    assert [item["number"] for item in record["metadata"]["issues"]] == [1]
    assert record["metadata"]["pullRequests"][0]["number"] == 2
    assert record["metadata"]["releases"][0]["tag"] == "v1.0.0"
    assert record["metadata"]["organization"]["name"] == "octo"
    assert record["attribution"]["provider"] == "GitHub"
    assert record["sourceCodeFetched"] is False
    assert record["privateSourceSentToExternalModel"] is False


def test_private_import_requires_explicit_request_token_and_stores_no_token():
    source = imports.parse_provider_source("https://github.com/octo/widget")
    with pytest.raises(imports.ProviderRequestError) as raised:
        imports.build_repository_record(
            source, github_root(private=True), github_extras(), "alice",
            "import", 1, token_present=False)
    assert raised.value.code == "private_authorization_required"

    record = imports.build_repository_record(
        source, github_root(private=True, admin=True), github_extras(),
        "alice", "import", 1, token_present=True)
    serialized = json.dumps(record, sort_keys=True)
    assert record["isPrivate"] is True
    assert record["authorization"]["privateAccessVerified"] is True
    assert record["authorization"]["administratorVerified"] is True
    assert record["authorization"]["credentialStored"] is False
    assert "ghp_super_secret" not in serialized
    assert not any("token" in key.casefold() for key in record)


def test_gitlab_contributor_emails_are_not_imported():
    source = imports.parse_provider_source(
        "https://gitlab.com/group/subgroup/widget")
    root = {
        "id": 99,
        "path_with_namespace": "group/subgroup/widget",
        "web_url": "https://gitlab.com/group/subgroup/widget",
        "visibility": "public",
        "namespace": {
            "full_path": "group/subgroup",
            "kind": "group",
            "web_url": "https://gitlab.com/group/subgroup",
        },
        "permissions": {},
        "topics": ["mesh"],
        "default_branch": "main",
    }
    extras = {
        "contributors": [{
            "name": "Contributor",
            "email": "private@example.test",
            "commits": 3,
        }],
        "languages": {"Rust": 100.0},
    }
    record = imports.build_repository_record(
        source, root, extras, "alice", "import", 2)
    serialized = json.dumps(record)
    assert "private@example.test" not in serialized
    assert record["metadata"]["contributors"][0]["login"] == "Contributor"
    assert record["metadata"]["primaryLanguage"] == "Rust"


def test_status_transitions_require_real_mirror_states():
    assert imports.status_transition_allowed("stub_only", "mirror_requested")
    assert imports.status_transition_allowed(
        "mirror_requested", "actively_mirrored")
    assert not imports.status_transition_allowed(
        "stub_only", "actively_mirrored")
    assert not imports.status_transition_allowed(
        "actively_mirrored", "stub_only")


def test_external_identity_survives_provider_repository_rename():
    before = imports.external_repository_id("github", "12345", "octo/widget")
    after = imports.external_repository_id(
        "github", "12345", "octo/renamed-widget")
    assert before == after


def test_no_logo_is_invented_for_a_repository_without_one():
    # adhoc #23: every card used to carry a machine-generated abstract mark,
    # which read as the project's logo without ever being one. A repository
    # gets its own committed root logo or owner-approved artwork, or nothing.
    assert not hasattr(imports, "deterministic_logo")
    source = imports.parse_provider_source("https://github.com/octo/widget")
    record = imports.build_repository_record(
        source, github_root(), github_extras(), "alice", "import", 1)
    assert imports.public_repository_record(record)["logo"] is None

    approved = {"dataUrl": "data:image/png;base64,AAAA", "approved": True}
    assert imports.public_repository_record(
        record, approved)["logo"] == approved


def test_uploaded_logos_reject_svg_and_signature_spoofing():
    png = _fake_png_data_url()
    cleaned = imports.validate_uploaded_logo(png)
    assert cleaned["contentType"] == "image/png"
    assert cleaned["sizeBytes"] > 8
    with pytest.raises(ValueError, match="unsupported_logo_type"):
        imports.validate_uploaded_logo(
            "data:image/svg+xml;base64,"
            + base64.b64encode(b"<svg/>").decode())
    with pytest.raises(ValueError, match="logo_signature_mismatch"):
        imports.validate_uploaded_logo(
            "data:image/png;base64,"
            + base64.b64encode(b"not png").decode())


def test_invitation_preview_identifies_inviter_and_has_controls():
    source = imports.parse_provider_source("https://github.com/octo/widget")
    record = imports.build_repository_record(
        source, github_root(), github_extras(), "alice", "import", 1)
    preview = imports.invitation_preview(
        record, "alice", "contrib", "c@example.test", "prior_consent",
        "https://forkmesh.test", "inv_" + "a" * 24, "token")
    assert preview["inviter"] == "alice"
    assert "alice invited" in preview["subject"]
    assert "does not own or control" in preview["text"]
    assert "/opt-out?token=" in preview["optOutUrl"]
    assert "/report?token=" in preview["reportUrl"]


def test_schema_and_migration_define_all_import_privacy_tables():
    tables = (
        "repository_imports",
        "repository_mirror_volunteers",
        "contributor_invitations",
        "contributor_invitation_rate",
        "contributor_invitation_optouts",
        "contributor_invitation_abuse",
        "contributor_invitation_provenance",
        "repository_logo_suggestions",
    )
    migration_0044 = (
        ROOT / "migrations" / "0044_repository_imports.sql"
    ).read_text(encoding="utf-8")
    migration_0060 = (
        ROOT / "migrations" / "0060_contributor_invitation_provenance.sql"
    ).read_text(encoding="utf-8")
    for table in tables:
        assert "CREATE TABLE IF NOT EXISTS " + table in SCHEMA_TEXT
        assert (
            "CREATE TABLE IF NOT EXISTS " + table in migration_0044
            or "CREATE TABLE IF NOT EXISTS " + table in migration_0060)
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(migration_0044)
        connection.executescript(migration_0060)
        actual = {
            row[0] for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type='table'")
        }
        assert set(tables).issubset(actual)
    finally:
        connection.close()


def test_atomic_limit_migration_serializes_concurrent_logo_and_invite_writes(
        tmp_path):
    database = tmp_path / "repository-import-limits.sqlite3"
    migration_0044 = (
        ROOT / "migrations" / "0044_repository_imports.sql"
    ).read_text(encoding="utf-8")
    migration_0056 = (
        ROOT / "migrations" / "0056_repository_import_atomic_limits.sql"
    ).read_text(encoding="utf-8")
    migration_0060 = (
        ROOT / "migrations" / "0060_contributor_invitation_provenance.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(database)
    connection.executescript(migration_0044)
    connection.executescript(migration_0056)
    connection.executescript(migration_0056)
    connection.executescript(migration_0060)
    connection.execute("PRAGMA journal_mode=WAL")
    connection.close()

    def concurrent_writes(count, insert):
        barrier = threading.Barrier(count)

        def write(index):
            db = sqlite3.connect(database, timeout=30)
            try:
                barrier.wait(timeout=10)
                insert(db, index)
                db.commit()
                return True
            except sqlite3.IntegrityError:
                db.rollback()
                return False
            finally:
                db.close()

        with concurrent.futures.ThreadPoolExecutor(
                max_workers=count) as executor:
            return list(executor.map(write, range(count)))

    def proposer_logo_insert(db, index):
        db.execute(
            "INSERT INTO repository_logo_suggestions "
            "(id,repo_id,proposer_bi,status,official,data,created_at) "
            "VALUES (?,?,?,?,?,?,?)",
            (
                "logo-proposer-" + str(index),
                "repo-proposer",
                "bi:bob",
                "pending",
                0,
                "{}",
                index,
            ),
        )

    assert sum(concurrent_writes(12, proposer_logo_insert)) == \
        imports.MAX_PENDING_LOGO_SUGGESTIONS_PER_PROPOSER

    def repository_logo_insert(db, index):
        db.execute(
            "INSERT INTO repository_logo_suggestions "
            "(id,repo_id,proposer_bi,status,official,data,created_at) "
            "VALUES (?,?,?,?,?,?,?)",
            (
                "logo-repository-" + str(index),
                "repo-community",
                "bi:proposer-" + str(index),
                "pending",
                0,
                "{}",
                index,
            ),
        )

    assert sum(concurrent_writes(55, repository_logo_insert)) == \
        imports.MAX_SUGGESTIONS_PER_REPOSITORY - 1

    def official_logo_insert(db, index):
        db.execute(
            "INSERT INTO repository_logo_suggestions "
            "(id,repo_id,proposer_bi,status,official,data,created_at) "
            "VALUES (?,?,?,?,?,?,?)",
            (
                "logo-official-" + str(index),
                "repo-community",
                "bi:owner",
                "approved",
                1,
                "{}",
                1000 + index,
            ),
        )

    assert sum(concurrent_writes(8, official_logo_insert)) == 8
    connection = sqlite3.connect(database)
    assert connection.execute(
        "SELECT COUNT(*) FROM repository_logo_suggestions "
        "WHERE repo_id='repo-community'").fetchone()[0] == \
        imports.MAX_SUGGESTIONS_PER_REPOSITORY
    assert connection.execute(
        "SELECT COUNT(*) FROM repository_logo_suggestions "
        "WHERE repo_id='repo-community' AND official=1").fetchone()[0] == 1
    connection.close()

    invitation_now = 1_800_000_000_000

    def daily_invitation_insert(db, index):
        provenance_id = "prov-daily-" + str(index)
        repo_id = "invite-repo-" + str(index)
        email_bi = "bi:email-" + str(index)
        contributor_bi = "bi:contributor-" + str(index)
        db.execute(
            "INSERT INTO contributor_invitation_provenance "
            "(id,repo_id,contributor_bi,email_bi,basis,created_by_bi,data,"
            "created_at) VALUES (?,?,?,?,?,?,?,?)",
            (
                provenance_id, repo_id, contributor_bi, email_bi,
                "owner_supplied", "bi:alice", "{}", invitation_now + index,
            ),
        )
        db.execute(
            "INSERT INTO contributor_invitations "
            "(id,repo_id,inviter_bi,email_bi,contributor_bi,provenance_id,"
            "status,data,created_at) VALUES (?,?,?,?,?,?,?,?,?)",
            (
                "invite-daily-" + str(index),
                repo_id,
                "bi:alice",
                email_bi,
                contributor_bi,
                provenance_id,
                "pending",
                "{}",
                invitation_now + index,
            ),
        )

    assert sum(concurrent_writes(25, daily_invitation_insert)) == \
        imports.INVITATION_DAILY_LIMIT
    connection = sqlite3.connect(database)
    day = invitation_now // (24 * 60 * 60 * 1000)
    assert connection.execute(
        "SELECT sent_count FROM contributor_invitation_rate "
        "WHERE inviter_bi='bi:alice' AND day_bucket=? AND repo_id='*'",
        (day,),
    ).fetchone()[0] == imports.INVITATION_DAILY_LIMIT
    connection.close()

    def cooldown_invitation_insert(db, index):
        provenance_id = "prov-cooldown-" + str(index)
        repo_id = "cooldown-repo-" + str(index)
        contributor_bi = "bi:contributor-" + str(index)
        db.execute(
            "INSERT INTO contributor_invitation_provenance "
            "(id,repo_id,contributor_bi,email_bi,basis,created_by_bi,data,"
            "created_at) VALUES (?,?,?,?,?,?,?,?)",
            (
                provenance_id, repo_id, contributor_bi,
                "bi:shared-recipient", "prior_consent",
                "bi:actor-" + str(index), "{}",
                invitation_now + 100 + index,
            ),
        )
        db.execute(
            "INSERT INTO contributor_invitations "
            "(id,repo_id,inviter_bi,email_bi,contributor_bi,provenance_id,"
            "status,data,created_at) VALUES (?,?,?,?,?,?,?,?,?)",
            (
                "invite-cooldown-" + str(index),
                repo_id,
                "bi:actor-" + str(index),
                "bi:shared-recipient",
                contributor_bi,
                provenance_id,
                "pending",
                "{}",
                invitation_now + 100 + index,
            ),
        )

    assert sum(concurrent_writes(8, cooldown_invitation_insert)) == 1


def test_atomic_limit_triggers_are_in_runtime_schema_and_upgrade_migration():
    migration = (
        ROOT / "migrations" / "0056_repository_import_atomic_limits.sql"
    ).read_text(encoding="utf-8")
    triggers = (
        "trg_logo_pending_proposer_limit",
        "trg_logo_community_repository_limit",
        "trg_logo_official_insert",
        "trg_logo_official_approval",
        "trg_invitation_recipient_cooldown",
        "trg_invitation_daily_limit",
        "trg_invitation_repository_daily_limit",
        "trg_invitation_reserve_rate",
        "trg_invitation_release_rate",
    )
    for trigger in triggers:
        assert trigger in migration
        assert trigger in SCHEMA_TEXT
    assert "RAISE(ABORT, 'logo_pending_proposer_limit')" in migration
    assert "AFTER INSERT ON contributor_invitations" in migration
    assert "AFTER DELETE ON contributor_invitations" in migration
    provenance_migration = (
        ROOT / "migrations" / "0060_contributor_invitation_provenance.sql"
    ).read_text(encoding="utf-8")
    assert "trg_invitation_provenance_reservation" in provenance_migration
    assert "trg_invitation_provenance_reservation" in SCHEMA_TEXT
    assert "invitation_provenance_invalid" in provenance_migration


def test_worker_adapter_uses_primary_apis_and_never_logs_provider_exception():
    assert "def _repository_import_module():" in ENTRY_TEXT
    assert "import repository_imports" in ENTRY_TEXT
    assert "repository_import.provider_api_origin(provider) + path" in ENTRY_TEXT
    assert 'headers["x-github-api-version"] = "2026-03-10"' in ENTRY_TEXT
    assert '"private-token"' in ENTRY_TEXT
    assert '"redirect": "manual"' in ENTRY_TEXT
    assert '"authorization" + ' not in ENTRY_TEXT
    adapter = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _repository_provider_fetch"):
        ENTRY_TEXT.index("async def _repository_import_operator_eligible")
    ]
    assert "repr(" not in adapter
    assert "provider_response_too_large" in adapter
    assert 'url.path.startswith("/api/repository-imports/")' in RUNTIME_TEXT


def test_dashboard_separates_external_stubs_from_live_mirror_catalog():
    assert "External repositories &amp; stubs" in REPOS_VIEW
    assert "separate from live mirrors" in REPOS_VIEW
    assert "does not own or control external repositories" in REPOS_VIEW
    assert "data-external-repo-list" in REPOS_VIEW
    assert 'fetchJson("/api/repository-imports"' in REPO_LIST_JS
    assert "statusLabel" in REPO_LIST_JS
    assert "mirrorNotice" in REPO_LIST_JS
    assert "data-external-mirror-volunteer" in REPO_LIST_JS
    assert "loadExternalRepositories();" in REPO_INIT_JS
    # The generated, actually served page must include the source partial.
    assert "External repositories &amp; stubs" in BUILT_DASHBOARD


def test_dashboard_provider_import_token_is_explicitly_ephemeral():
    assert 'data-new-repo-source="provider"' in NEW_REPO_MODAL
    assert "data-new-repo-provider-token" in NEW_REPO_MODAL
    assert 'type="password"' in NEW_REPO_MODAL
    assert "only for this request" in NEW_REPO_MODAL
    assert "it is not persisted" in NEW_REPO_MODAL
    assert "source files are not fetched" in NEW_REPO_MODAL
    assert 'fetch("/api/repository-imports"' in REPO_LIST_JS
    assert "providerToken" in REPO_LIST_JS
    assert 'tokenInput.value = ""' in REPO_LIST_JS
    # Tokens must never be placed in persistent browser storage.
    token_section = REPO_LIST_JS[
        REPO_LIST_JS.index("const providerToken"):
        REPO_LIST_JS.index("tokenInput.value =") + len('tokenInput.value = ""')
    ]
    assert "localStorage" not in token_section


def test_create_and_mirror_advances_to_an_explicit_desktop_handoff():
    assert "data-new-repo-details" in NEW_REPO_MODAL
    assert 'data-new-repo-steps role="status" aria-live="polite"' in NEW_REPO_MODAL
    assert "Desktop handoff required" in NEW_REPO_MODAL
    assert "The browser cannot create or queue this mirror" in NEW_REPO_MODAL
    assert "Nothing has been published yet" in NEW_REPO_MODAL
    assert "function setNewRepoHandoffOpen(open, copy = {})" in REPO_LIST_JS
    assert 'setNewRepoHandoffOpen(true);' in REPO_LIST_JS
    assert '$("[data-new-repo-details]")?.classList.toggle("hidden", open)' in REPO_LIST_JS
    assert '$("[data-new-repo-submit]")?.classList.toggle("hidden", open)' in REPO_LIST_JS
    assert 'cancelLabel.textContent = open ? "Close" : "Cancel"' in REPO_LIST_JS
    assert "form.scrollTop = 0" in REPO_LIST_JS
    # The generated page is what production serves, so it must carry the state
    # markers and copy after the dashboard asset build runs.
    assert "data-new-repo-details" in BUILT_DASHBOARD


def test_native_repositories_share_one_logo_and_public_review_ui():
    assert "REPO_LOGO_RE" in ENTRY_TEXT
    assert "REPO_LOGO_SUGGESTIONS_RE" in ENTRY_TEXT
    assert "native_repository_logo_handler" in ENTRY_TEXT
    # A repository's own committed root logo, or artwork its owner approved.
    # Nothing else is served as a logo (adhoc #23).
    assert "_committed_repository_logo_url" in ENTRY_TEXT
    assert "deterministic_logo" not in ENTRY_TEXT
    # The owner changes the logo from the Settings tab and reviews suggested
    # artwork there; the repository page no longer offers a suggestion form.
    assert "data-repo-settings-logo-form" in REPO_INIT_JS
    assert "data-repo-logo-suggestion-form" not in REPO_INIT_JS
    assert "/logo-suggestions" in REPO_CONTENT_JS
    assert "data-repo-logo-review" in REPO_CONTENT_JS
    assert "nativeRepositoryLogoMarkup" in REPO_LIST_JS
    assert "data-native-repo-logo" in REPO_LIST_JS
    assert "hydrateNativeRepositoryLogos" in REPO_LIST_JS
    # Built through repoApiBase, so the logo is read from the repository's
    # public address rather than from the mirror that published its catalog
    # row (see test_repo_api_address_frontend.py).
    assert "${repoApiBase(repo)}/logo" in REPO_LIST_JS


def test_native_private_logo_cards_send_bearer_and_never_cache_auth_misses():
    section = REPO_LIST_JS[
        REPO_LIST_JS.index("const nativeRepositoryLogoCache")
        :REPO_LIST_JS.index("function repoActivitySparkline")
    ]
    assert 'const token = String(state.session?.sessionToken || "")' in section
    assert "headers.Authorization = `Bearer ${token}`" in section
    assert "nativeRepositoryLogoCache.clear()" in section
    assert "nativeRepositoryLogoSessionEpoch += 1" in section
    assert "if (!response.ok) throw new Error" in section
    assert 'String(state.session?.sessionToken || "") !== token' in section
    assert "nativeRepositoryLogoCache.delete(cacheKey)" in section


def _fake_png_data_url():
    raw = b"\x89PNG\r\n\x1a\n" + b"\x00" * 32
    return "data:image/png;base64," + base64.b64encode(raw).decode()


class Request:
    def __init__(self, method="GET", body=None, actor="", url=None):
        self.method = method
        self._body = body
        self.actor = actor
        self.url = url or "https://forkmesh.test/api/repository-imports"

    async def json(self):
        if self._body is None:
            raise ValueError("no JSON")
        return copy.deepcopy(self._body)


class ServiceHarness:
    def __init__(
            self, private=False, send_email=True,
            provider_admin_for_token=True):
        self.now = 1_800_000_000_000
        self.private = private
        self.provider_admin_for_token = provider_admin_for_token
        self.send_email_result = send_email
        self.sent = []
        self.repository_imports = {}
        self.volunteers = {}
        self.invitations = {}
        self.provenance = {}
        self.rates = {}
        self.optouts = {}
        self.abuse = {}
        self.logos = {}
        self.provider_tokens_seen = []
        self.audit_events = []
        self.service = imports.RepositoryImportService({
            "json_response": self.json_response,
            "ensure_schema": self.ensure_schema,
            "d1_all": self.d1_all,
            "d1_first": self.d1_first,
            "d1_run": self.d1_run,
            "encrypt_row": self.encrypt_row,
            "decrypt_row": self.decrypt_row,
            "blind_index": self.blind_index,
            "session_record": self.session_record,
            "provider_fetch": self.provider_fetch,
            "send_email": self.send_email,
            "audit": self.audit,
            "invitation_token": self.invitation_token,
            "operator_eligible": self.operator_eligible,
            "is_moderator": self.is_moderator,
            "mirror_status": self.mirror_status,
            "public_origin": lambda env, request: "https://forkmesh.test",
            "now_ms": lambda: self.now,
        })

    @staticmethod
    def json_response(data, status=200, **kwargs):
        return {"status": status, "data": data, "options": kwargs}

    @staticmethod
    async def ensure_schema(env):
        return None

    @staticmethod
    async def encrypt_row(env, value):
        return copy.deepcopy(value)

    @staticmethod
    async def decrypt_row(env, value, key=None):
        return copy.deepcopy(value) if isinstance(value, dict) else None

    @staticmethod
    async def blind_index(env, value):
        return "bi:" + str(value).strip().lower()

    @staticmethod
    async def session_record(env, request, data=None):
        actor = getattr(request, "actor", "")
        if not actor and isinstance(data, dict) and data.get("sessionToken"):
            actor = str(data["sessionToken"])
        return (
            "bi:" + actor,
            {
                "name": actor,
                "status": "active",
                "email": actor + "@example.test",
                "email_verified": True,
            } if actor else None,
        )

    async def audit(
            self, env, actor, action, target_type, target, outcome, details):
        self.audit_events.append({
            "actor": actor,
            "action": action,
            "targetType": target_type,
            "target": target,
            "outcome": outcome,
            "details": copy.deepcopy(details),
        })

    async def provider_fetch(self, env, provider, path, token):
        self.provider_tokens_seen.append(token)
        if path.startswith("/repos/"):
            root = github_root(
                private=self.private,
                admin=bool(token) and self.provider_admin_for_token)
            suffix_map = {
                "/topics?": "topics",
                "/languages": "languages",
                "/git/trees/": "structure",
                "/branches?": "branches",
                "/contributors?": "contributors",
                "/commits?": "commits",
                "/issues?": "issues",
                "/pulls?": "pullRequests",
                "/releases?": "releases",
            }
            for marker, key in suffix_map.items():
                if marker in path:
                    return {
                        "status": 200,
                        "data": github_extras()[key],
                        "headers": {
                            "x-ratelimit-limit": "5000",
                            "x-ratelimit-remaining": "4999",
                        },
                    }
            return {
                "status": 200,
                "data": root,
                "headers": {
                    "x-ratelimit-limit": "5000",
                    "x-ratelimit-remaining": "4999",
                },
            }
        raise AssertionError(path)

    async def send_email(self, env, to, subject, text, html):
        self.sent.append({
            "to": to, "subject": subject, "text": text, "html": html})
        return self.send_email_result

    @staticmethod
    async def invitation_token(env, repo_id, invitation_id, email):
        return ("t" + imports.opaque_record_id(
            "token", repo_id, invitation_id, email))[:64]

    @staticmethod
    async def operator_eligible(env, actor, node):
        return node == actor + "-node"

    @staticmethod
    async def is_moderator(env, actor):
        return actor == "moderator"

    @staticmethod
    async def mirror_status(env, actor, record, target, data):
        return {
            "verified": bool(data.get("proof")),
            "owner": actor,
            "name": record["name"],
            "live": bool(data.get("live")),
        }

    async def d1_first(self, env, sql, *args):
        if "FROM repository_imports WHERE id=?" in sql:
            return copy.deepcopy(self.repository_imports.get(args[0]))
        if "FROM repository_logo_suggestions" in sql and "official=1" in sql:
            for row in self.logos.values():
                if row["repo_id"] == args[0] and row["official"]:
                    if "SELECT id" in sql:
                        return {"id": row["id"]}
                    return {"data": copy.deepcopy(row["data"])}
            return None
        if "COUNT(*) AS n FROM repository_mirror_volunteers" in sql:
            return {"n": sum(
                1 for (repo_id, _), row in self.volunteers.items()
                if repo_id == args[0] and row["status"] == "requested")}
        if "FROM repository_mirror_volunteers" in sql:
            return copy.deepcopy(self.volunteers.get((args[0], args[1])))
        if "FROM contributor_invitation_optouts" in sql:
            return copy.deepcopy(self.optouts.get(args[0]))
        if (
                "FROM contributor_invitation_provenance" in sql
                and "WHERE id=? AND repo_id=?" in sql):
            row = self.provenance.get(args[0])
            if row and row["repo_id"] == args[1]:
                return copy.deepcopy(row)
            return None
        if "FROM contributor_invitation_rate" in sql:
            repo_id = "*" if "repo_id='*'" in sql else args[2]
            return {
                "sent_count": self.rates.get((args[0], args[1], repo_id), 0)}
        if "FROM contributor_invitations" in sql and "email_bi=?" in sql:
            email_bi, cutoff = args
            rows = [
                row for row in self.invitations.values()
                if row["email_bi"] == email_bi
                and row["status"] in ("pending", "sent")
                and row["created_at"] > cutoff]
            return copy.deepcopy(max(
                rows, key=lambda row: row["created_at"], default=None))
        if "COUNT(*) AS n FROM contributor_invitations" in sql:
            return {"n": sum(
                row["repo_id"] == args[0]
                for row in self.invitations.values())}
        if "FROM contributor_invitations" in sql and "id=?" in sql:
            row = self.invitations.get(args[0])
            if row and row["repo_id"] == args[1]:
                return copy.deepcopy(row)
            return None
        if "COUNT(*) AS n FROM repository_logo_suggestions" in sql:
            if "proposer_bi=?" in sql:
                return {"n": sum(
                    row["repo_id"] == args[0]
                    and row["proposer_bi"] == args[1]
                    and row["status"] == "pending"
                    for row in self.logos.values())}
            return {"n": sum(
                row["repo_id"] == args[0] for row in self.logos.values())}
        if "SELECT id FROM repository_logo_suggestions" in sql:
            row = self.logos.get(args[0])
            return {"id": args[0]} if row and row["repo_id"] == args[1] else None
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_all(self, env, sql, *args):
        if "FROM repository_imports" in sql:
            owner_filter = "owner_bi=?" in sql
            owner_bi = args[0] if owner_filter else ""
            rows = []
            for row in self.repository_imports.values():
                if row["is_private"] and row["owner_bi"] != owner_bi:
                    continue
                rows.append(copy.deepcopy(row))
            rows.sort(key=lambda row: row["updated_at"], reverse=True)
            return rows
        if "FROM contributor_invitations" in sql:
            return [
                copy.deepcopy(row) for row in self.invitations.values()
                if row["repo_id"] == args[0]]
        if "FROM contributor_invitation_provenance" in sql:
            return [
                copy.deepcopy(row) for row in self.provenance.values()
                if row["repo_id"] == args[0] and not row["revoked_at"]]
        if "FROM repository_logo_suggestions" in sql:
            if "official=1 AND repo_id IN" in sql:
                wanted = set(args)
                return [
                    copy.deepcopy(row) for row in self.logos.values()
                    if row["repo_id"] in wanted and row["official"]]
            return [
                copy.deepcopy(row) for row in self.logos.values()
                if row["repo_id"] == args[0]]
        raise AssertionError("unexpected d1_all: " + sql)

    async def d1_run(self, env, sql, *args):
        if sql.startswith("DELETE FROM repository_mirror_volunteers"):
            self.volunteers = {
                key: row for key, row in self.volunteers.items()
                if key[0] != args[0]
            }
            return
        if (
                sql.startswith("DELETE FROM contributor_invitation_provenance")
                and "WHERE repo_id=?" in sql):
            self.provenance = {
                key: row for key, row in self.provenance.items()
                if row["repo_id"] != args[0]
            }
            return
        if (
                sql.startswith("DELETE FROM contributor_invitations")
                and "WHERE repo_id=?" in sql):
            self.invitations = {
                key: row for key, row in self.invitations.items()
                if row["repo_id"] != args[0]
            }
            return
        if sql.startswith("DELETE FROM contributor_invitation_rate"):
            self.rates = {
                key: value for key, value in self.rates.items()
                if key[0] != args[0]
            }
            return
        if sql.startswith("DELETE FROM repository_logo_suggestions WHERE repo_id"):
            self.logos = {
                key: row for key, row in self.logos.items()
                if row["repo_id"] != args[0]
            }
            return
        if sql.startswith("DELETE FROM repository_imports"):
            self.repository_imports.pop(args[0], None)
            return
        if sql.startswith("INSERT INTO repository_imports"):
            (
                repo_id, provider, external_id, owner_bi, is_private, status,
                data, created_at, updated_at,
            ) = args
            prior = self.repository_imports.get(repo_id)
            self.repository_imports[repo_id] = {
                "id": repo_id,
                "provider": provider,
                "external_id": external_id,
                "owner_bi": owner_bi,
                "is_private": is_private,
                "status": status,
                "data": copy.deepcopy(data),
                "created_at": prior["created_at"] if prior else created_at,
                "updated_at": updated_at,
            }
            return
        if sql.startswith("INSERT INTO repository_mirror_volunteers"):
            repo_id, operator_bi, node, status, created_at, updated_at = args
            self.volunteers[(repo_id, operator_bi)] = {
                "node_label": node, "status": status,
                "created_at": created_at, "updated_at": updated_at}
            return
        if sql.startswith(
                "UPDATE repository_mirror_volunteers SET status='withdrawn'"):
            updated_at, repo_id, operator_bi = args
            row = self.volunteers.get((repo_id, operator_bi))
            if row:
                row.update({"status": "withdrawn", "updated_at": updated_at})
            return
        if sql.startswith(
                "INSERT INTO contributor_invitation_provenance"):
            (
                provenance_id, repo_id, contributor_bi, email_bi, basis,
                created_by_bi, data, created_at,
            ) = args
            self.provenance[provenance_id] = {
                "id": provenance_id,
                "repo_id": repo_id,
                "contributor_bi": contributor_bi,
                "email_bi": email_bi,
                "basis": basis,
                "created_by_bi": created_by_bi,
                "data": copy.deepcopy(data),
                "created_at": created_at,
                "revoked_at": 0,
            }
            return
        if sql.startswith(
                "UPDATE contributor_invitation_provenance SET revoked_at=? "
                "WHERE repo_id=?"):
            revoked_at, repo_id = args
            for row in self.provenance.values():
                if (
                        row["repo_id"] == repo_id
                        and row["basis"] == "public_for_invitations"
                        and not row["revoked_at"]):
                    row["revoked_at"] = revoked_at
            return
        if sql.startswith(
                "UPDATE contributor_invitation_provenance "
                "SET revoked_at=? WHERE id=?"):
            revoked_at, provenance_id, repo_id, *rest = args
            row = self.provenance.get(provenance_id)
            if (
                    row and row["repo_id"] == repo_id
                    and (not rest or row["created_by_bi"] == rest[0])):
                row["revoked_at"] = revoked_at
            return
        if sql.startswith("INSERT INTO contributor_invitations"):
            (
                invitation_id, repo_id, inviter_bi, email_bi,
                contributor_bi, provenance_id, status, data, created_at,
            ) = args
            day = created_at // (24 * 60 * 60 * 1000)
            evidence = self.provenance.get(provenance_id)
            if (
                    not evidence or evidence["repo_id"] != repo_id
                    or evidence["contributor_bi"] != contributor_bi
                    or evidence["email_bi"] != email_bi
                    or evidence["revoked_at"]):
                raise RuntimeError("invitation_provenance_invalid")
            if email_bi in self.optouts:
                raise RuntimeError("invitation_recipient_opted_out")
            if any(
                    row["email_bi"] == email_bi
                    and row["status"] in ("pending", "sent")
                    and row["created_at"] > (
                        created_at
                        - imports.INVITATION_RECIPIENT_COOLDOWN_MS)
                    for row in self.invitations.values()):
                raise RuntimeError("invitation_recipient_cooldown")
            if self.rates.get((inviter_bi, day, "*"), 0) >= \
                    imports.INVITATION_DAILY_LIMIT:
                raise RuntimeError("invitation_daily_limit")
            if self.rates.get((inviter_bi, day, repo_id), 0) >= \
                    imports.INVITATION_REPO_DAILY_LIMIT:
                raise RuntimeError("invitation_repository_daily_limit")
            if sum(
                    row["repo_id"] == repo_id
                    for row in self.invitations.values()) >= \
                    imports.MAX_INVITATIONS_PER_REPOSITORY:
                raise RuntimeError("invitation_repository_history_limit")
            self.invitations[invitation_id] = {
                "id": invitation_id, "repo_id": repo_id,
                "inviter_bi": inviter_bi, "email_bi": email_bi,
                "contributor_bi": contributor_bi,
                "provenance_id": provenance_id,
                "status": status, "data": copy.deepcopy(data),
                "created_at": created_at, "sent_at": 0,
            }
            for rate_repo_id in ("*", repo_id):
                key = (inviter_bi, day, rate_repo_id)
                self.rates[key] = self.rates.get(key, 0) + 1
            return
        if sql.startswith(
                "UPDATE contributor_invitations SET status=?, sent_at=?"):
            status, sent_at, invitation_id = args
            self.invitations[invitation_id].update(
                {"status": status, "sent_at": sent_at})
            return
        if sql.startswith("INSERT INTO contributor_invitation_rate"):
            inviter_bi, day, repo_id = args
            key = (inviter_bi, day, repo_id)
            self.rates[key] = self.rates.get(key, 0) + 1
            return
        if sql.startswith("DELETE FROM contributor_invitations"):
            invitation_id = args[0]
            row = self.invitations.get(invitation_id)
            if row and row["status"] == "pending":
                day = row["created_at"] // (24 * 60 * 60 * 1000)
                for rate_repo_id in ("*", row["repo_id"]):
                    key = (row["inviter_bi"], day, rate_repo_id)
                    remaining = max(0, self.rates.get(key, 0) - 1)
                    if remaining:
                        self.rates[key] = remaining
                    else:
                        self.rates.pop(key, None)
                del self.invitations[invitation_id]
            return
        if sql.startswith("INSERT INTO contributor_invitation_optouts"):
            email_bi, opted_out_at, invitation_id = args
            self.optouts[email_bi] = {
                "opted_out_at": opted_out_at,
                "source_invitation_id": invitation_id,
            }
            return
        if "SET status='opted_out'" in sql:
            self.invitations[args[0]]["status"] = "opted_out"
            return
        if sql.startswith("INSERT INTO contributor_invitation_abuse"):
            report_id, invitation_id, data, created_at, status = args
            self.abuse[report_id] = {
                "id": report_id, "invitation_id": invitation_id,
                "data": copy.deepcopy(data), "created_at": created_at,
                "status": status,
            }
            return
        if "SET status='reported'" in sql:
            self.invitations[args[0]]["status"] = "reported"
            return
        if sql.startswith(
                "UPDATE repository_logo_suggestions SET official=0"):
            for row in self.logos.values():
                if row["repo_id"] == args[0]:
                    row["official"] = 0
                    if "status=CASE" in sql and row["status"] == "approved":
                        row["status"] = "superseded"
            return
        if sql.startswith(
                "DELETE FROM repository_logo_suggestions WHERE id IN ("):
            repo_id, cleanup_count = args
            candidates = sorted(
                (
                    row for row in self.logos.values()
                    if row["repo_id"] == repo_id and not row["official"]
                ),
                key=lambda row: (
                    0 if row["status"] in ("rejected", "superseded") else 1,
                    row["created_at"],
                ),
            )
            for row in candidates[:cleanup_count]:
                del self.logos[row["id"]]
            return
        if sql.startswith("INSERT INTO repository_logo_suggestions"):
            (
                suggestion_id, repo_id, proposer_bi, status, official, data,
                created_at, reviewed_at, reviewed_by_bi,
            ) = args
            if status == "pending" and not official:
                pending_by_proposer = sum(
                    row["repo_id"] == repo_id
                    and row["proposer_bi"] == proposer_bi
                    and row["status"] == "pending"
                    for row in self.logos.values())
                if pending_by_proposer >= \
                        imports.MAX_PENDING_LOGO_SUGGESTIONS_PER_PROPOSER:
                    raise RuntimeError("logo_pending_proposer_limit")
                if sum(
                        row["repo_id"] == repo_id
                        for row in self.logos.values()) >= \
                        imports.MAX_SUGGESTIONS_PER_REPOSITORY - 1:
                    raise RuntimeError("logo_community_repository_limit")
            if official:
                candidates = sorted(
                    (
                        row for row in self.logos.values()
                        if row["repo_id"] == repo_id and not row["official"]
                    ),
                    key=lambda row: (
                        0 if row["status"] in (
                            "rejected", "superseded") else 1,
                        row["created_at"],
                    ),
                )
                repo_count = sum(
                    row["repo_id"] == repo_id
                    for row in self.logos.values())
                for row in candidates[:max(0, repo_count - 49)]:
                    del self.logos[row["id"]]
                for row in self.logos.values():
                    if row["repo_id"] == repo_id and row["official"]:
                        row["official"] = 0
                        if row["status"] == "approved":
                            row["status"] = "superseded"
            self.logos[suggestion_id] = {
                "id": suggestion_id, "repo_id": repo_id,
                "proposer_bi": proposer_bi, "status": status,
                "official": official, "data": copy.deepcopy(data),
                "created_at": created_at, "reviewed_at": reviewed_at,
                "reviewed_by_bi": reviewed_by_bi,
            }
            return
        if "status='approved', official=1" in sql:
            reviewed_at, reviewer_bi, suggestion_id, repo_id = args
            row = self.logos[suggestion_id]
            assert row["repo_id"] == repo_id
            for other in self.logos.values():
                if (other["repo_id"] == repo_id
                        and other["id"] != suggestion_id
                        and other["official"]):
                    other["official"] = 0
                    if other["status"] == "approved":
                        other["status"] = "superseded"
            row.update({
                "status": "approved", "official": 1,
                "reviewed_at": reviewed_at,
                "reviewed_by_bi": reviewer_bi,
            })
            return
        if "status='rejected', official=0" in sql:
            reviewed_at, reviewer_bi, suggestion_id, repo_id = args
            row = self.logos[suggestion_id]
            assert row["repo_id"] == repo_id
            row.update({
                "status": "rejected", "official": 0,
                "reviewed_at": reviewed_at,
                "reviewed_by_bi": reviewer_bi,
            })
            return
        raise AssertionError("unexpected d1_run: " + sql)

    def call(self, request, path=None):
        return run(self.service.handle(
            None, request, path or request.url.split("?", 1)[0].replace(
                "https://forkmesh.test", "")))


def _create_with(harness, actor="alice", token=""):
    return harness.call(Request(
        "POST",
        {
            "sourceUrl": "https://github.com/octo/widget",
            "providerToken": token,
            "mode": "stub",
            "sessionToken": actor,
        },
        actor=actor,
    ))


def _provenance_id(harness, basis):
    return next(
        row["id"] for row in harness.provenance.values()
        if row["basis"] == basis and not row["revoked_at"])


def test_codeberg_namespace_discovery_paginates_and_filters_provider_urls():
    harness = ServiceHarness()
    calls = []

    async def provider_fetch(env, provider, path, token):
        calls.append((provider, path, token))
        page = int(path.rsplit("=", 1)[-1])
        if page == 1:
            return {
                "status": 200,
                "data": [
                    {
                        "html_url": f"https://codeberg.org/m33/repo-{index}",
                        "private": False,
                    }
                    for index in range(50)
                ],
                "headers": {},
            }
        return {
            "status": 200,
            "data": [
                {
                    "html_url": "https://codeberg.org/m33/final-repo",
                    "private": False,
                },
                {
                    "html_url": "https://codeberg.org/another/not-m33",
                    "private": False,
                },
                {
                    "html_url": "https://codeberg.org/m33/private-repo",
                    "private": True,
                },
            ],
            "headers": {},
        }

    harness.service.d["provider_fetch"] = provider_fetch
    response = harness.call(Request(
        "POST",
        {
            "sourceUrl": "https://codeberg.org/m33",
            "sessionToken": "alice",
        },
        actor="alice",
    ), "/api/repository-imports/discover")
    assert response["status"] == 200
    assert response["data"]["count"] == 51
    assert response["data"]["repositories"][-1] == \
        "https://codeberg.org/m33/final-repo"
    assert [call[1] for call in calls] == [
        "/users/m33/repos?limit=50&page=1",
        "/users/m33/repos?limit=50&page=2",
    ]


def test_gitlab_group_discovery_includes_subgroups_and_skips_outsiders():
    harness = ServiceHarness()
    calls = []

    async def provider_fetch(env, provider, path, token):
        calls.append((provider, path, token))
        return {
            "status": 200,
            "data": [
                {
                    "web_url": "https://gitlab.com/acme/website",
                    "visibility": "public",
                },
                {
                    "web_url": "https://gitlab.com/acme/platform/runner",
                    "visibility": "public",
                },
                # Shared from another namespace: listed by the group endpoint,
                # but not part of the organization being imported.
                {
                    "web_url": "https://gitlab.com/partner/sdk",
                    "visibility": "public",
                },
                # Untokenized discovery never advertises a non-public project.
                {
                    "web_url": "https://gitlab.com/acme/secrets",
                    "visibility": "internal",
                },
            ],
            "headers": {},
        }

    harness.service.d["provider_fetch"] = provider_fetch
    response = harness.call(Request(
        "POST",
        {
            "sourceUrl": "https://gitlab.com/acme",
            "sessionToken": "alice",
        },
        actor="alice",
    ), "/api/repository-imports/discover")
    assert response["status"] == 200
    assert response["data"]["provider"] == "gitlab"
    assert response["data"]["namespace"] == "acme"
    assert response["data"]["repositories"] == [
        "https://gitlab.com/acme/website",
        "https://gitlab.com/acme/platform/runner",
    ]
    assert response["data"]["incomplete"] is False
    assert [call[1] for call in calls] == [
        "/groups/acme/projects?include_subgroups=true&archived=false"
        "&order_by=path&sort=asc&per_page=50&page=1",
    ]


def test_gitlab_discovery_falls_back_to_a_personal_namespace():
    harness = ServiceHarness()
    calls = []

    async def provider_fetch(env, provider, path, token):
        calls.append(path)
        if path.startswith("/groups/"):
            return {"status": 404, "data": {"message": "404 Group Not Found"},
                    "headers": {}}
        return {
            "status": 200,
            "data": [{
                "web_url": "https://gitlab.com/jo/dotfiles",
                "visibility": "private",
            }],
            "headers": {},
        }

    harness.service.d["provider_fetch"] = provider_fetch
    response = harness.call(Request(
        "POST",
        {
            "sourceUrl": "https://gitlab.com/jo",
            # A supplied token is what made the private project visible at all,
            # so discovery reports it and the import re-verifies the access.
            "providerToken": "glpat_one_request_only",
            "sessionToken": "alice",
        },
        actor="alice",
    ), "/api/repository-imports/discover")
    assert response["status"] == 200
    assert response["data"]["repositories"] == [
        "https://gitlab.com/jo/dotfiles"]
    assert calls == [
        "/groups/jo/projects?include_subgroups=true&archived=false"
        "&order_by=path&sort=asc&per_page=50&page=1",
        "/users/jo/projects?archived=false&order_by=path&sort=asc"
        "&per_page=50&page=1",
    ]
    assert "glpat_one_request_only" not in json.dumps(
        harness.repository_imports, sort_keys=True)


def test_namespace_discovery_reports_an_exhausted_page_budget():
    harness = ServiceHarness()
    pages = []

    async def provider_fetch(env, provider, path, token):
        pages.append(path)
        page = int(path.rsplit("page=", 1)[-1])
        return {
            "status": 200,
            "data": [
                {
                    "web_url": f"https://gitlab.com/acme/repo-{page}-{index}",
                    "visibility": "public",
                }
                for index in range(50)
            ],
            "headers": {},
        }

    harness.service.d["provider_fetch"] = provider_fetch
    response = harness.call(Request(
        "POST",
        {"sourceUrl": "https://gitlab.com/acme", "sessionToken": "alice"},
        actor="alice",
    ), "/api/repository-imports/discover")
    assert response["status"] == 200
    assert response["data"]["count"] == imports.MAX_NAMESPACE_REPOSITORIES
    assert response["data"]["incomplete"] is True
    assert len(pages) == imports.NAMESPACE_MAX_PAGES


def test_service_creates_truthful_stub_lists_it_and_never_persists_token():
    harness = ServiceHarness()
    response = _create_with(harness, token="ghp_one_request_only")
    assert response["status"] == 201
    repo = response["data"]["repository"]
    assert repo["status"] == "stub_only"
    assert repo["mirrored"] is False
    # No approved artwork yet, and ForkMesh never invents a mark for a project
    # it does not own (adhoc #23), so the card renders its repository icon.
    assert repo["logo"] is None
    assert response["data"]["credentialStored"] is False
    assert "ghp_one_request_only" in harness.provider_tokens_seen
    assert "ghp_one_request_only" not in json.dumps(
        harness.repository_imports, sort_keys=True)
    assert "contrib@example.test" not in json.dumps(
        harness.repository_imports, sort_keys=True)

    listing = harness.call(Request("GET"))
    assert [item["id"] for item in listing["data"]["repositories"]] == [repo["id"]]
    assert listing["data"]["repositories"][0]["statusLabel"] == "Stub only"


def test_service_digest_list_avoids_rich_import_payloads_for_polling():
    harness = ServiceHarness()
    created = _create_with(harness)
    repo = created["data"]["repository"]

    digest = harness.call(Request(
        "GET",
        url="https://forkmesh.test/api/repository-imports?view=digest",
    ))

    assert digest["status"] == 200
    assert digest["data"] == {
        "ok": True,
        "repositories": [{
            "id": repo["id"],
            "status": "stub_only",
            "updatedAt": harness.now,
        }],
    }
    assert len(json.dumps(digest["data"])) < 200


def test_service_bulk_delete_primitive_requires_owner_and_removes_import():
    harness = ServiceHarness()
    created = _create_with(harness, actor="alice")
    repo_id = created["data"]["repository"]["id"]
    forbidden = harness.call(Request(
        "DELETE", {"sessionToken": "bob"}, actor="bob",
    ), "/api/repository-imports/" + repo_id)
    assert forbidden["status"] == 403
    assert repo_id in harness.repository_imports
    deleted = harness.call(Request(
        "DELETE", {"sessionToken": "alice"}, actor="alice",
    ), "/api/repository-imports/" + repo_id)
    assert deleted["status"] == 200
    assert deleted["data"]["deleted"] is True
    assert repo_id not in harness.repository_imports
    assert harness.audit_events[-1]["action"] == "repository_import.delete"


def test_service_private_import_is_not_probeable_by_other_or_anonymous_users():
    harness = ServiceHarness(private=True)
    created = _create_with(harness, token="private-access-token")
    assert created["status"] == 201
    repo_id = created["data"]["repository"]["id"]
    assert harness.provenance == {}

    anonymous = harness.call(Request("GET"))
    assert anonymous["data"]["repositories"] == []
    assert "owner" in anonymous["data"]["privacy"]
    assert "after materialization" in anonymous["data"]["privacy"]
    guessed = harness.call(Request(
        "GET", actor="mallory",
        url="https://forkmesh.test/api/repository-imports/" + repo_id))
    assert guessed == {
        "status": 404,
        "data": {"error": "not_found"},
        "options": {},
    }
    owner = harness.call(Request(
        "GET", actor="alice",
        url="https://forkmesh.test/api/repository-imports/" + repo_id))
    assert owner["status"] == 200
    assert owner["data"]["repository"]["isPrivate"] is True


def test_provider_administrator_can_adopt_community_stub_without_claiming_provider_ownership():
    harness = ServiceHarness()
    community = _create_with(harness, actor="bob")
    repo_id = community["data"]["repository"]["id"]
    adopted = _create_with(
        harness, actor="alice", token="provider-admin-token")
    assert adopted["status"] == 200
    assert adopted["data"]["adoptedByProviderAdministrator"] is True
    repo = adopted["data"]["repository"]
    assert repo["listedBy"] == "alice"
    assert repo["originalListedBy"] == "bob"
    assert harness.repository_imports[repo_id]["owner_bi"] == "bi:alice"
    assert "does not own or control" in repo["ownershipNotice"]


def test_private_read_only_collaborator_cannot_overwrite_import_owner():
    harness = ServiceHarness(private=True)
    original = _create_with(
        harness, actor="alice", token="provider-admin-token")
    repo_id = original["data"]["repository"]["id"]
    before = copy.deepcopy(harness.repository_imports[repo_id])
    harness.provider_admin_for_token = False
    attempted = _create_with(
        harness, actor="bob", token="provider-read-token")
    assert attempted["status"] == 404
    assert attempted["data"]["error"] == "not_found"
    assert harness.repository_imports[repo_id] == before


def test_service_volunteer_request_changes_status_but_hides_operator_names():
    harness = ServiceHarness()
    created = _create_with(harness)
    repo_id = created["data"]["repository"]["id"]
    requested = harness.call(Request(
        "POST", {"node": "bob-node", "sessionToken": "bob"}, actor="bob",
        url=(
            "https://forkmesh.test/api/repository-imports/" + repo_id
            + "/mirror-volunteers")))
    assert requested["status"] == 201
    assert requested["data"]["repository"]["status"] == "mirror_requested"

    public = harness.call(Request(
        "GET", url=(
            "https://forkmesh.test/api/repository-imports/" + repo_id
            + "/mirror-volunteers")))
    assert public["data"]["volunteerCount"] == 1
    assert public["data"]["myRequest"] is None
    assert "bob-node" not in json.dumps(public)


def test_service_active_mirror_status_requires_catalog_proof_and_live_health():
    harness = ServiceHarness()
    created = _create_with(harness, token="provider-admin-token")
    repo_id = created["data"]["repository"]["id"]
    item_url = "https://forkmesh.test/api/repository-imports/" + repo_id

    too_early = harness.call(Request(
        "PATCH",
        {
            "sessionToken": "alice",
            "status": "actively_mirrored",
            "proof": True,
            "live": True,
        },
        actor="alice", url=item_url))
    assert too_early["status"] == 409
    assert too_early["data"]["error"] == "invalid_status_transition"

    volunteers = item_url + "/mirror-volunteers"
    harness.call(Request(
        "POST", {"node": "alice-node"}, actor="alice", url=volunteers))
    offline = harness.call(Request(
        "PATCH",
        {
            "sessionToken": "alice",
            "status": "actively_mirrored",
            "proof": True,
            "live": False,
        },
        actor="alice", url=item_url))
    assert offline["status"] == 409
    assert offline["data"]["error"] == "healthy_live_mirror_required"

    active = harness.call(Request(
        "PATCH",
        {
            "sessionToken": "alice",
            "status": "actively_mirrored",
            "proof": True,
            "live": True,
        },
        actor="alice", url=item_url))
    assert active["status"] == 200
    assert active["data"]["repository"]["mirrored"] is True
    assert active["data"]["repository"]["status"] == "actively_mirrored"


def test_service_invitation_requires_durable_provider_provenance_and_optout():
    harness = ServiceHarness()
    created = _create_with(harness, token="provider-admin-token")
    repo_id = created["data"]["repository"]["id"]
    path = (
        "https://forkmesh.test/api/repository-imports/" + repo_id
        + "/invitations")
    base = {
        "sessionToken": "alice",
        "contributor": "contrib",
        "email": "contrib@example.test",
        "provenanceId": _provenance_id(
            harness, "public_for_invitations"),
    }
    rejected_boolean = harness.call(Request(
        "POST",
        {**base, "basisConfirmed": True, "dryRun": True},
        actor="alice", url=path))
    assert rejected_boolean["status"] == 400
    assert rejected_boolean["data"]["error"] == \
        "invitation_provenance_required"

    preview = harness.call(Request(
        "POST", {**base, "dryRun": True}, actor="alice", url=path))
    assert preview["status"] == 200
    assert preview["data"]["dryRun"] is True
    assert preview["data"]["preview"]["to"].startswith("c**")
    assert preview["data"]["provenance"]["basis"] == \
        "public_for_invitations"
    assert harness.sent == []

    sent = harness.call(Request(
        "POST", {**base, "confirm": True}, actor="alice", url=path))
    assert sent["status"] == 201
    invitation_id = sent["data"]["invitation"]["id"]
    assert sent["data"]["invitation"]["inviter"] == "alice"
    assert harness.sent[0]["to"] == "contrib@example.test"
    stored_invitation = harness.invitations[invitation_id]["data"]
    assert "email" not in stored_invitation
    assert "contrib@example.test" not in json.dumps(stored_invitation)
    assert len(stored_invitation["actionTokenDigest"]) == 64
    token = run(harness.invitation_token(
        None, repo_id, invitation_id, "contrib@example.test"))
    opt_url = (
        path + "/" + invitation_id + "/opt-out?token=" + token)

    confirmation = harness.call(Request("GET", url=opt_url))
    assert confirmation["data"]["confirmationRequired"] is True
    assert harness.optouts == {}
    opted = harness.call(Request("POST", {}, url=opt_url))
    assert opted["data"]["optedOut"] is True
    assert harness.optouts

    report_url = path + "/" + invitation_id + "/report?token=" + token
    report = harness.call(Request(
        "POST", {"reason": "This was not requested."}, url=report_url))
    assert report["status"] == 201
    assert report["data"]["reported"] is True
    assert harness.abuse
    assert any(
        event["action"] == "contributor_invitation.send"
        and event["details"]["provenanceId"] == base["provenanceId"]
        for event in harness.audit_events)

    again = harness.call(Request(
        "POST", {**base, "confirm": True}, actor="alice", url=path))
    assert again["status"] == 409
    assert again["data"]["error"] == "recipient_opted_out"


def test_prior_consent_uses_verified_recipient_record_and_is_revocable():
    harness = ServiceHarness()
    created = _create_with(harness, token="provider-admin-token")
    repo_id = created["data"]["repository"]["id"]
    consent_path = (
        "https://forkmesh.test/api/repository-imports/" + repo_id
        + "/invitation-consent")
    consent = harness.call(Request(
        "POST",
        {"sessionToken": "contrib", "contributor": "contrib"},
        actor="contrib", url=consent_path))
    assert consent["status"] == 201
    evidence = consent["data"]["provenance"]
    assert evidence["basis"] == "prior_consent"
    assert evidence["rawEmailStored"] is False

    invitation_path = (
        "https://forkmesh.test/api/repository-imports/" + repo_id
        + "/invitations")
    payload = {
        "sessionToken": "alice",
        "contributor": "contrib",
        "email": "contrib@example.test",
        "provenanceId": evidence["id"],
        "dryRun": True,
    }
    preview = harness.call(Request(
        "POST", payload, actor="alice", url=invitation_path))
    assert preview["status"] == 200
    assert preview["data"]["provenance"]["basis"] == "prior_consent"

    revoked = harness.call(Request(
        "DELETE",
        {"sessionToken": "contrib", "contributor": "contrib"},
        actor="contrib", url=consent_path))
    assert revoked["status"] == 200
    assert revoked["data"]["consented"] is False
    denied = harness.call(Request(
        "POST", payload, actor="alice", url=invitation_path))
    assert denied["status"] == 409
    assert denied["data"]["error"] == "invitation_provenance_invalid"


def test_owner_supplied_provenance_requires_hashed_source_record():
    harness = ServiceHarness()
    created = _create_with(harness, token="provider-admin-token")
    repo_id = created["data"]["repository"]["id"]
    provenance_path = (
        "https://forkmesh.test/api/repository-imports/" + repo_id
        + "/invitation-provenance")
    missing = harness.call(Request(
        "POST",
        {
            "sessionToken": "alice",
            "contributor": "contrib",
            "email": "owner-contact@example.test",
            "sourceType": "offline_address_book",
            "sourceReference": "short",
        },
        actor="alice", url=provenance_path))
    assert missing["status"] == 400
    assert missing["data"]["error"] == "owner_provenance_required"

    raw_reference = "Owner contact ledger entry 2026-07-23 #1842"
    created_evidence = harness.call(Request(
        "POST",
        {
            "sessionToken": "alice",
            "contributor": "contrib",
            "email": "owner-contact@example.test",
            "sourceType": "offline_address_book",
            "sourceReference": raw_reference,
        },
        actor="alice", url=provenance_path))
    assert created_evidence["status"] == 201
    evidence = created_evidence["data"]["provenance"]
    assert evidence["basis"] == "owner_supplied"
    assert evidence["rawEmailStored"] is False
    assert evidence["rawEvidenceStored"] is False
    assert raw_reference not in json.dumps(harness.provenance)

    invitation_path = (
        "https://forkmesh.test/api/repository-imports/" + repo_id
        + "/invitations")
    preview = harness.call(Request(
        "POST",
        {
            "sessionToken": "alice",
            "contributor": "contrib",
            "email": "owner-contact@example.test",
            "provenanceId": evidence["id"],
            "dryRun": True,
        },
        actor="alice", url=invitation_path))
    assert preview["status"] == 200
    assert preview["data"]["provenance"]["basis"] == "owner_supplied"


def test_invitation_provenance_must_match_exact_recipient_and_contributor():
    harness = ServiceHarness()
    created = _create_with(harness, token="provider-admin-token")
    repo_id = created["data"]["repository"]["id"]
    path = (
        "https://forkmesh.test/api/repository-imports/" + repo_id
        + "/invitations")
    evidence_id = _provenance_id(harness, "public_for_invitations")
    denied = harness.call(Request(
        "POST",
        {
            "sessionToken": "alice",
            "contributor": "contrib",
            "email": "different@example.test",
            "provenanceId": evidence_id,
            "dryRun": True,
        },
        actor="alice", url=path))
    assert denied["status"] == 409
    assert denied["data"]["error"] == "invitation_provenance_invalid"


def test_invitation_delivery_failure_rolls_back_row_cooldown_and_rate_reservation():
    harness = ServiceHarness(send_email=False)
    created = _create_with(harness, token="provider-admin-token")
    repo_id = created["data"]["repository"]["id"]
    path = (
        "https://forkmesh.test/api/repository-imports/" + repo_id
        + "/invitations")
    payload = {
        "sessionToken": "alice",
        "contributor": "contrib",
        "email": "contrib@example.test",
        "provenanceId": _provenance_id(
            harness, "public_for_invitations"),
        "confirm": True,
    }

    failed = harness.call(Request(
        "POST", payload, actor="alice", url=path))
    assert failed["status"] == 202
    assert failed["data"]["deliveryConfigured"] is False
    assert harness.invitations == {}
    assert harness.rates == {}

    harness.send_email_result = True
    retried = harness.call(Request(
        "POST", payload, actor="alice", url=path))
    assert retried["status"] == 201
    assert retried["data"]["invitation"]["status"] == "sent"
    assert len(harness.invitations) == 1
    day = harness.now // (24 * 60 * 60 * 1000)
    assert harness.rates[("bi:alice", day, "*")] == 1
    assert harness.rates[("bi:alice", day, repo_id)] == 1


def test_provider_admin_proof_expires_and_requires_request_time_refresh():
    harness = ServiceHarness()
    created = _create_with(harness, token="provider-admin-token")
    repo_id = created["data"]["repository"]["id"]
    harness.now += imports.PROVIDER_ADMIN_PROOF_TTL_MS + 1
    path = (
        "https://forkmesh.test/api/repository-imports/" + repo_id
        + "/invitations")
    response = harness.call(Request(
        "POST",
        {
            "sessionToken": "alice",
            "contributor": "contrib",
            "email": "contrib@example.test",
            "provenanceId": _provenance_id(
                harness, "public_for_invitations"),
            "dryRun": True,
        },
        actor="alice", url=path))
    assert response["status"] == 403
    assert response["data"]["error"] == "forbidden"


def test_service_logo_suggestion_requires_rights_and_owner_approval():
    harness = ServiceHarness()
    created = _create_with(harness, token="provider-admin-token")
    repo_id = created["data"]["repository"]["id"]
    path = (
        "https://forkmesh.test/api/repository-imports/" + repo_id
        + "/logo-suggestions")
    rejected = harness.call(Request(
        "POST",
        {"sessionToken": "bob", "imageData": _fake_png_data_url()},
        actor="bob", url=path))
    assert rejected["status"] == 400
    assert rejected["data"]["error"] == "logo_rights_confirmation_required"

    proposed = harness.call(Request(
        "POST",
        {
            "sessionToken": "bob",
            "imageData": _fake_png_data_url(),
            "rightsConfirmed": True,
            "attribution": "Original by Bob",
        },
        actor="bob", url=path))
    assert proposed["status"] == 201
    suggestion_id = proposed["data"]["suggestion"]["id"]
    assert proposed["data"]["suggestion"]["status"] == "pending"

    approved = harness.call(Request(
        "PATCH",
        {
            "sessionToken": "alice",
            "suggestionId": suggestion_id,
            "action": "approve",
        },
        actor="alice", url=path))
    assert approved["data"]["official"] is True
    logo = harness.call(Request(
        "GET", url=(
            "https://forkmesh.test/api/repository-imports/" + repo_id
            + "/logo")))
    assert logo["data"]["logo"]["generated"] is False
    assert logo["data"]["logo"]["suggestionId"] == suggestion_id


def test_native_repository_uses_shared_logo_and_review_workflow():
    harness = ServiceHarness()
    repository_id = "native_" + "a" * 24
    record = {
        "id": repository_id,
        "provider": "forkmesh",
        "name": "mesh-city",
        "metadata": {
            "description": "A repository world",
            "languages": {"JavaScript": 100},
            "topics": ["threejs"],
            "fileStructure": ["src", "tests"],
            "frameworks": ["Three.js"],
            "projectCategory": "web application",
        },
    }

    # Nothing approved yet and no committed root logo: the answer is an empty
    # logo, never invented artwork (adhoc #23).
    empty = run(harness.service.logo_for_record(None, repository_id))
    assert empty["data"]["logo"] is None
    assert empty["options"]["cache_control"] == "no-store"

    async def owner_only(actor):
        return actor == "alice"

    proposed = run(harness.service.native_logo_suggestions(
        None,
        Request(
            "POST",
            {
                "imageData": _fake_png_data_url(),
                "rightsConfirmed": True,
                "attribution": "Original community artwork",
            },
            actor="bob",
        ),
        repository_id,
        record,
        owner_only,
    ))
    assert proposed["status"] == 201
    suggestion_id = proposed["data"]["suggestion"]["id"]
    assert proposed["data"]["suggestion"]["status"] == "pending"

    forbidden = run(harness.service.native_logo_suggestions(
        None,
        Request(
            "PATCH",
            {"suggestionId": suggestion_id, "action": "approve"},
            actor="bob",
        ),
        repository_id,
        record,
        owner_only,
    ))
    assert forbidden["status"] == 403

    approved = run(harness.service.native_logo_suggestions(
        None,
        Request(
            "PATCH",
            {"suggestionId": suggestion_id, "action": "approve"},
            actor="alice",
        ),
        repository_id,
        record,
        owner_only,
    ))
    assert approved["data"]["official"] is True
    official = run(harness.service.logo_for_record(None, repository_id))
    assert official["data"]["logo"]["suggestionId"] == suggestion_id


def test_native_logo_suggestions_are_bounded_per_proposer():
    harness = ServiceHarness()
    repository_id = "native_" + "b" * 24
    record = {
        "id": repository_id,
        "provider": "forkmesh",
        "name": "bounded-logo-proposals",
        "metadata": {"languages": {"Python": 1}},
    }

    async def owner_only(actor):
        return actor == "alice"

    for index in range(imports.MAX_PENDING_LOGO_SUGGESTIONS_PER_PROPOSER):
        harness.now += index + 1
        response = run(harness.service.native_logo_suggestions(
            None,
            Request(
                "POST",
                {
                    "imageData": _fake_png_data_url(),
                    "rightsConfirmed": True,
                },
                actor="bob",
            ),
            repository_id,
            record,
            owner_only,
        ))
        assert response["status"] == 201

    harness.now += 100
    exhausted = run(harness.service.native_logo_suggestions(
        None,
        Request(
            "POST",
            {
                "imageData": _fake_png_data_url(),
                "rightsConfirmed": True,
            },
            actor="bob",
        ),
        repository_id,
        record,
        owner_only,
    ))
    assert exhausted["status"] == 429
    assert exhausted["data"]["error"] == \
        "too_many_logo_suggestions_by_proposer"


def test_native_owner_logo_replacement_cannot_be_blocked_by_full_history():
    harness = ServiceHarness()
    repository_id = "native_" + "c" * 24
    record = {
        "id": repository_id,
        "provider": "forkmesh",
        "name": "owner-replacement",
        "metadata": {"languages": {"Rust": 1}},
    }
    old_official_id = "logo_" + "1" * 24
    oldest_rejected_id = "logo_" + "2" * 24
    harness.logos[old_official_id] = {
        "id": old_official_id,
        "repo_id": repository_id,
        "proposer_bi": "bi:alice",
        "status": "approved",
        "official": 1,
        "data": {"id": old_official_id, "image": {}},
        "created_at": 1,
        "reviewed_at": 1,
        "reviewed_by_bi": "bi:alice",
    }
    for index in range(imports.MAX_SUGGESTIONS_PER_REPOSITORY - 1):
        suggestion_id = (
            oldest_rejected_id if index == 0
            else "logo_" + f"{index + 3:024x}"
        )
        harness.logos[suggestion_id] = {
            "id": suggestion_id,
            "repo_id": repository_id,
            "proposer_bi": "bi:attacker",
            "status": "rejected" if index == 0 else "pending",
            "official": 0,
            "data": {"id": suggestion_id, "image": {}},
            "created_at": index + 2,
            "reviewed_at": 0,
            "reviewed_by_bi": "",
        }
    assert len(harness.logos) == imports.MAX_SUGGESTIONS_PER_REPOSITORY

    async def owner_only(actor):
        return actor == "alice"

    replacement = run(harness.service.native_logo_suggestions(
        None,
        Request(
            "POST",
            {
                "action": "replace",
                "imageData": _fake_png_data_url(),
                "rightsConfirmed": True,
                "attribution": "Owner-provided replacement",
            },
            actor="alice",
        ),
        repository_id,
        record,
        owner_only,
    ))
    assert replacement["status"] == 201
    replacement_id = replacement["data"]["suggestion"]["id"]
    assert replacement["data"]["suggestion"]["official"] is True
    assert len(harness.logos) == imports.MAX_SUGGESTIONS_PER_REPOSITORY
    assert oldest_rejected_id not in harness.logos
    assert harness.logos[old_official_id]["status"] == "superseded"
    assert harness.logos[old_official_id]["official"] == 0
    assert harness.logos[replacement_id]["official"] == 1
    assert harness.logos[replacement_id]["data"]["replacesSuggestionId"] == \
        old_official_id
