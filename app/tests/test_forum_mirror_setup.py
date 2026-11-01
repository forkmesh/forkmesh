import asyncio
import json
import sys
from pathlib import Path

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))

import dashboard_shell  # noqa: E402
import forum_mirror_setup as setup  # noqa: E402
import market_packages  # noqa: E402
import static_routes  # noqa: E402

ORIGIN = "https://forum.example.org"
SITE = {"categories": [{"id": 1, "name": "General"}, {"id": 2, "name": "Help"}]}
ABOUT = {"about": {"title": "Example Forum", "description": "Tests", "stats": {"topics_count": 900, "posts_count": 40000}}}


class _Response:
    def __init__(self, status, text):
        self.status = status
        self._text = text

    async def text(self):
        return self._text


class _Date:
    @staticmethod
    def now():
        return 1_800_000_000_000


class Fake:
    def __init__(self):
        self.robots = "User-agent: *\nDisallow: /admin/\n"
        self.site_status = 200
        self.fetched = []
        self.commits = []
        self.features = {"issues": True, "forum": False}
        self.saved_features = []
        self.dispatches = []
        self.audits = []
        self.admins = {"ada"}
        self.permission = "write"
        self.session_name = "ada"

    async def fetch(self, url, init, timeout):
        self.fetched.append((url, init))
        assert init["redirect"] == "manual" and init["headers"]["user-agent"].startswith("ForkMeshDiscourseMirror/")
        if url.endswith("/robots.txt"):
            return _Response(200, self.robots)
        if url.endswith("/site.json"):
            return _Response(self.site_status, json.dumps(SITE) if self.site_status == 200 else "<html>")
        if url.endswith("/about.json"):
            return _Response(200, json.dumps(ABOUT))
        return _Response(404, "")

    def runtime(self, env_actions=True):
        fake = self

        async def session_record(env, request, data):
            return None, {"name": fake.session_name} if fake.session_name else None

        async def is_admin(env, name):
            return name in fake.admins

        async def features_get(env, owner, repo):
            return dict(fake.features)

        async def features_bi(env, owner, repo):
            return "bi:%s/%s" % (owner, repo)

        async def d1_run(env, sql, *args):
            fake.saved_features.append((sql, args))

        async def audit(env, actor, action, kind, target, outcome, details):
            fake.audits.append((actor, action, target, outcome, details))

        return {
            "js_fetch_with_timeout": fake.fetch,
            "_account_session_record": session_record,
            "_is_admin": is_admin,
            "_repo_features_get": features_get,
            "_repo_features_bi": features_bi,
            "d1_run": d1_run,
            "_audit_sensitive_action": audit,
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "MAX_NODE_NAME": 39,
            "Date": _Date,
        }


@pytest.fixture
def fake(monkeypatch):
    fake = Fake()
    for name, value in fake.runtime().items():
        monkeypatch.setitem(setup.__dict__, name, value)

    async def permission(env, request, owner, repo, actor):
        return fake.permission

    async def commit_paths(env, request, owner, repo, actor, branch, files, message):
        fake.commits.append({"owner": owner, "repo": repo, "actor": actor, "branch": branch,
                             "files": files, "message": message})
        return "c" * 40

    async def dispatch(env, owner, repo, branch, actor):
        fake.dispatches.append((owner, repo, branch, actor))
        return {"dispatched": True, "runId": 7000}

    monkeypatch.setattr(setup.edits, "_permission", permission)
    monkeypatch.setattr(setup.edits, "commit_paths", commit_paths)
    monkeypatch.setattr(setup, "_dispatch", dispatch)
    return fake


def run(coroutine):
    return asyncio.run(coroutine)


def test_forum_addresses_must_be_public_https():
    assert setup.clean_origin("forum.example.org") == ORIGIN
    assert setup.clean_origin("https://Forum.Example.org/community/") == "https://forum.example.org/community"
    for bad in ("http://forum.example.org", "https://localhost", "https://10.0.0.1", "https://x.internal",
                "https://u:p@forum.example.org", "https://forum.example.org/?a=1", "https://forkmesh.com",
                "https://forum.example.org/a/b/c/d/e/f", "javascript:alert(1)", ""):
        assert setup.clean_origin(bad) == "", bad


def test_the_workflow_runs_the_committed_importer_twice_an_hour_and_pushes_back():
    text = setup.workflow_text(minute=7)
    workflow = yaml.safe_load(text)
    triggers = workflow.get("on", workflow.get(True))
    assert triggers["schedule"] == [{"cron": "7,37 * * * *"}]
    assert "workflow_dispatch" in triggers
    assert workflow["permissions"] == {"contents": "write"}
    steps = workflow["jobs"]["mirror"]["steps"]
    assert steps[0]["run"] == "python3 .forkmesh/forum/tool/discourse_mirror.py ci --max-minutes 45"
    assert steps[1]["run"].startswith('git push origin "HEAD:$GITHUB_REF_NAME"')
    assert workflow["jobs"]["mirror"]["timeout-minutes"] == 60
    assert setup.WORKFLOW_PATH == ".forkmesh/discourse-mirror.yml"


def test_the_importer_is_found_through_the_package_catalog():
    path = market_packages.entrypoint_file("discourse-mirror", "mirror")
    assert path == REPO / "marketplace/packages/discourse-mirror/content/mirror/discourse_mirror.py"
    assert setup.importer_source().startswith(b"#!/usr/bin/env python3")
    assert market_packages.entrypoint_file("discourse-mirror", "settings") is None
    assert market_packages.entrypoint_file("no-such-package", "mirror") is None


def test_robots_rules_are_honoured():
    assert setup.robots_allows("User-agent: *\nDisallow: /admin/\n", [ORIGIN + "/t/1.json"])
    assert not setup.robots_allows("User-agent: *\nDisallow: /\n", [ORIGIN + "/site.json"])
    assert not setup.robots_allows("User-agent: ForkMeshDiscourseMirror\nDisallow: /t/\n", [ORIGIN + "/t/1.json"])


def test_check_reports_size_and_readiness(fake):
    report = run(setup.check(ORIGIN))
    assert report["ready"] and report["discourse"] and report["public"] and report["robotsAllow"]
    assert report["title"] == "Example Forum" and report["categories"] == 2
    assert report["topics"] == 900 and report["posts"] == 40000
    assert report["estimate"] == {"requests": 2030, "hours": 0.6, "runs": 3}
    fake.robots = "User-agent: *\nDisallow: /\n"
    assert run(setup.check(ORIGIN))["problem"] == "The forum's robots.txt does not allow copying."
    fake.robots = ""
    fake.site_status = 403
    report = run(setup.check(ORIGIN))
    assert not report["ready"] and report["problem"] == "This address does not answer like a Discourse forum."


def test_only_an_instance_admin_may_check_or_start(fake):
    fake.session_name = ""
    with pytest.raises(setup.SetupError) as error:
        run(setup._actor(None, None, {}))
    assert error.value.status == 401
    fake.session_name = "bob"
    with pytest.raises(setup.SetupError) as error:
        run(setup._actor(None, None, {}))
    assert error.value.code == "admin_required" and error.value.status == 403
    fake.session_name = "ada"
    assert run(setup._actor(None, None, {})) == "ada"


def test_start_commits_the_workflow_importer_and_settings_and_turns_the_tab_on(fake):
    result = run(setup.start(None, None, {"owner": "ada", "repo": "forum-mirror", "branch": "main",
                                          "origin": "forum.example.org/", "agree": True, "uploads": False}, "ada"))
    assert result["ok"] and result["commit"] == "c" * 40 and result["dispatched"] and result["runId"] == 7000
    assert result["title"] == "Example Forum" and result["origin"] == ORIGIN
    commit = fake.commits[0]
    assert commit["branch"] == "main" and commit["actor"] == "ada"
    assert commit["message"] == "Mirror forum.example.org into the Forum tab"
    assert set(commit["files"]) == {setup.WORKFLOW_PATH, setup.TOOL_PATH, setup.SETTINGS_PATH}
    settings = json.loads(commit["files"][setup.SETTINGS_PATH])
    assert settings["origin"] == ORIGIN and settings["uploads"] is False and settings["startedBy"] == "ada"
    assert commit["files"][setup.TOOL_PATH] == setup.importer_source()
    sql, args = fake.saved_features[-1]
    assert "repo_feature_settings" in sql and json.loads(args[1])["forum"] is True
    assert fake.dispatches == [("ada", "forum-mirror", "main", "ada")]
    assert fake.audits[0][1] == "repository.forum_mirror"


@pytest.mark.parametrize("data,code", [
    ({"agree": False}, "agreement_required"),
    ({"origin": "http://forum.example.org"}, "invalid_origin"),
    ({"repo": "../x"}, "invalid_repository"),
    ({"branch": "bad..branch"}, "invalid_repository"),
])
def test_start_refuses_bad_requests(fake, data, code):
    body = {"owner": "ada", "repo": "forum", "branch": "main", "origin": ORIGIN, "agree": True, **data}
    with pytest.raises(setup.SetupError) as error:
        run(setup.start(None, None, body, "ada"))
    assert error.value.code == code
    assert not fake.commits


def test_start_refuses_a_forum_that_is_not_ready_or_a_repository_it_cannot_write(fake):
    body = {"owner": "ada", "repo": "forum", "branch": "main", "origin": ORIGIN, "agree": True}
    fake.robots = "User-agent: *\nDisallow: /\n"
    with pytest.raises(setup.SetupError) as error:
        run(setup.start(None, None, body, "ada"))
    assert error.value.code == "forum_not_ready"
    fake.robots = ""
    fake.permission = "read"
    with pytest.raises(setup.SetupError) as error:
        run(setup.start(None, None, body, "ada"))
    assert error.value.code == "not_authorized"
    assert not fake.commits


def test_the_page_and_api_are_routed():
    assert "start-discourse-mirror" in static_routes.CORE_RESERVED_ROUTE_PREFIXES
    assert "/start-discourse-mirror.html" in static_routes.BLOCKED_STATIC_HTML_PATHS
    assert "start-discourse-mirror.js" in dashboard_shell.CACHE_BUSTED_ASSETS
    runtime = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
    assert 'if url.path in ("/api/forum-mirrors", "/api/forum-mirrors/"):' in runtime
    assert '"!/start-discourse-mirror/"' in (ROOT / "wrangler.toml").read_text(encoding="utf-8")
    assert '"/start-discourse-mirror"' in (REPO / "www" / "worker.js").read_text(encoding="utf-8")
    assert "/start-discourse-mirror https://app.forkmesh.com/start-discourse-mirror 308" in (
        REPO / "www" / "public" / "_redirects").read_text(encoding="utf-8")
    start = (ROOT / "public" / "start.html").read_text(encoding="utf-8")
    assert 'href="/start-discourse-mirror" data-start-discourse-link' in start
    page = (ROOT / "public" / "start-discourse-mirror.html").read_text(encoding="utf-8")
    assert '<script src="/start-discourse-mirror.js?v=${version}" defer></script>' in page
    script = (ROOT / "public" / "start-discourse-mirror.js").read_text(encoding="utf-8")
    assert "innerHTML" not in script
    assert 'api("/api/forum-mirrors"' in script and "/api/hosted-repositories" in script
