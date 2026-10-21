import asyncio
import html
import json
import os
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlencode

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))

import admin_shell  # noqa: E402
import repo_short_urls  # noqa: E402
import server_update  # noqa: E402
import server_update_admin as admin  # noqa: E402

HELPER = REPO / "tools" / "instance-update.sh"
NOW = 1_800_000_000_000


def sandbox_env(tmp_path):
    env = {key: value for key, value in os.environ.items()
           if key not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    for name in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
        path = tmp_path / name.lower()
        path.mkdir(exist_ok=True)
        env[name] = str(path)
    env.update(GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1",
               GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.com",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.com")
    return env


def test_short_addresses_map_onto_the_owner():
    owner = "acme"
    assert repo_short_urls.rewrite("/tools", owner) == ("/acme/tools", "tools")
    assert repo_short_urls.rewrite("/tools/src/lib", owner) == (
        "/acme/tools/tree/src/lib", "tools")
    assert repo_short_urls.rewrite("/tools/issues/4", owner) == (
        "/acme/tools/issues/4", "tools")
    assert repo_short_urls.rewrite("/tools/blob/README.md", owner) == (
        "/acme/tools/blob/README.md", "tools")
    assert repo_short_urls.rewrite("/tools.git/info/refs", owner) == (
        "/acme/tools.git/info/refs", "")
    assert repo_short_urls.rewrite("/tools/git-upload-pack", owner) == (
        "/acme/tools/git-upload-pack", "")
    assert repo_short_urls.rewrite("/tools/info", owner) == (
        "/acme/tools/tree/info", "tools")
    for path in ("/", "/api/x", "/dashboard", "/login", "/acme/tools",
                 "/robots.txt", "/wp-login.php", "/.env", "/tools.git",
                 "/@ada", "/a%2Fb"):
        assert repo_short_urls.rewrite(path, owner) is None, path
    assert repo_short_urls.rewrite("/tools", "") is None
    assert repo_short_urls.first_name("/Tools.git/info/refs") == "tools"


def test_short_address_setting_is_validated_and_cached():
    assert repo_short_urls.parse("") == {"short": False, "owner": "",
                                         "canonical": False}
    assert repo_short_urls.parse('{"short":true,"owner":"Acme"}') == {
        "short": True, "owner": "acme", "canonical": False}
    assert repo_short_urls.parse('{"short":true,"owner":"bad name"}') == {
        "short": False, "owner": "", "canonical": False}
    assert repo_short_urls.parse("[1]") == {"short": False, "owner": "",
                                            "canonical": False}
    assert repo_short_urls.clean(True, "@Acme") == {
        "short": True, "owner": "acme", "canonical": False}
    with pytest.raises(repo_short_urls.SettingError):
        repo_short_urls.clean(True, "")
    with pytest.raises(repo_short_urls.SettingError):
        repo_short_urls.clean(False, "no/slash")
    repo_short_urls.forget()
    assert repo_short_urls.cached(5) is None
    repo_short_urls.remember({"short": True, "owner": "acme"}, 5)
    assert repo_short_urls.cached(5 + repo_short_urls.CACHE_MS - 1)["owner"] == "acme"
    assert repo_short_urls.cached(5 + repo_short_urls.CACHE_MS) is None
    repo_short_urls.forget()


def test_short_addresses_are_routed_after_every_other_route():
    runtime = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
    route = runtime.split("    async def _route(self, request, url):", 1)[1]
    assert route.index("short_route = await self._repo_short_route(url)") < \
        route.index("if looks_like_repo_route(url.path):")
    assert route.index("REFERRAL_LINK_RE.match(url.path)") < \
        route.index("short_route = await self._repo_short_route(url)")
    assert "return await self._route(request, short_url)" in route
    helper = runtime.split("    async def _repo_short_route(self, url):", 1)[1]
    helper = helper.split("\n    async def ", 1)[0]
    assert "_account_row(self.env, name)" in helper
    assert "_org_row(self.env, name)" in helper
    page = runtime.split("    async def _serve_repo_page(", 1)[1]
    assert "if social_version and not short_name:" in page
    assert 'meta name="forkmesh-repo-short"' in page
    helpers = (ROOT / "public" / "dashboard" / "js" / "02-helpers.js").read_text(
        encoding="utf-8")
    assert 'meta[name="forkmesh-repo-short"]' in helpers
    assert '...(routed ? rest : ["tree", ...rest])' in helpers


def test_the_running_build_and_the_update_status_are_read_safely(tmp_path):
    info = tmp_path / "build-info.json"
    info.write_text(json.dumps({
        "version": "1.2.8", "commit": "a" * 40, "commitTime": 1_700_000_000,
        "subject": "Notes:  same\nfeatures", "builtAt": 1_700_000_100}))
    env = SimpleNamespace(BUILD_INFO_FILE=str(info), BUILD_REV="b" * 12,
                          APP_VERSION="0.0.1")
    assert server_update.running(env) == {
        "commit": "a" * 40, "time": 1_700_000_000,
        "subject": "Notes: same features", "version": "1.2.8",
        "builtAt": 1_700_000_100}
    env = SimpleNamespace(BUILD_INFO_FILE="relative.json", BUILD_REV="b" * 12,
                          APP_VERSION="0.0.1")
    assert server_update.running(env)["commit"] == "b" * 12
    assert server_update.running(env)["version"] == "0.0.1"
    assert server_update.mode(SimpleNamespace()) == ""
    assert server_update.mode(SimpleNamespace(CORE_ADMIN_UPDATES="1")) == "core"
    bad = SimpleNamespace(
        INSTANCE_UPDATE_REQUEST="/var/lib/forkmesh-instance/../../etc/x",
        INSTANCE_UPDATE_STATUS="/var/lib/forkmesh-instance-update/status.json")
    assert server_update.mode(bad) == ""
    good = SimpleNamespace(
        INSTANCE_UPDATE_REQUEST="/var/lib/forkmesh-instance/update/request",
        INSTANCE_UPDATE_STATUS="/var/lib/forkmesh-instance-update/status.json")
    assert server_update.mode(good) == "instance"
    current = {"commit": "abcdef123456"}
    assert server_update.freshness(current, {"commit": "abcdef123456" + "0" * 28}) == "current"
    assert server_update.freshness(current, {"commit": "f" * 40}) == "behind"
    assert server_update.freshness(current, {}) == "unknown"
    upstream = server_update.core_upstream(json.dumps({
        "commit": "c" * 40, "time": 5, "subject": "x", "version": "2.0.0",
        "checkedAt": NOW}))
    assert upstream == {"commit": "c" * 40, "time": 5, "subject": "x",
                        "version": "2.0.0", "checkedAt": NOW}


def test_instance_status_and_requests(tmp_path, monkeypatch):
    status = tmp_path / "status.json"
    request = tmp_path / "request"
    monkeypatch.setattr(server_update, "INSTANCE_REQUEST_ROOT", str(tmp_path) + "/")
    monkeypatch.setattr(server_update, "INSTANCE_STATUS_ROOT", str(tmp_path) + "/")
    env = SimpleNamespace(INSTANCE_UPDATE_REQUEST=str(request),
                          INSTANCE_UPDATE_STATUS=str(status))
    status.write_text(json.dumps({
        "state": "updating", "action": "update", "step": "build",
        "startedAt": 10, "lastError": "<b>", "fromCommit": "a" * 40,
        "upstreamCommit": "d" * 40, "upstreamSubject": "New", "checkedAt": 9,
        "upstreamVersion": "1.3.0; rm"}))
    read = server_update.instance_status(env)
    assert read["state"] == "updating" and read["step"] == "build"
    assert read["upstream"]["commit"] == "d" * 40
    assert read["upstream"]["version"] == ""
    assert server_update.busy(read)
    status.write_text(json.dumps({"state": "weird", "step": "Build!"}))
    read = server_update.instance_status(env)
    assert read["state"] == "idle" and read["step"] == "" and not server_update.busy(read)
    assert not server_update.request_instance(env, "reboot")
    assert server_update.request_instance(env, "check")
    assert request.read_text() == "check\n"
    assert server_update.instance_status(env)["requested"] == "check"
    assert server_update.busy(server_update.instance_status(env))


class FakeResponse:
    def __init__(self, body, status=200, headers=None):
        self.body, self.status, self.headers = body, status, headers or {}


class FakeRequest:
    def __init__(self, url, method="GET", form=None):
        self.url, self.method = url, method
        self.form = urlencode(form or {})

    async def text(self):
        return self.form


def bind(monkeypatch, tmp_path, runtime):
    monkeypatch.setattr(server_update, "INSTANCE_REQUEST_ROOT", str(tmp_path) + "/")
    monkeypatch.setattr(server_update, "INSTANCE_STATUS_ROOT", str(tmp_path) + "/")
    values = {
        "_admin_shell": admin_shell, "_html_escape": html.escape,
        "_admin_href": lambda _query, **params: "?" + "&amp;".join(
            "%s=%s" % item for item in params.items()),
        "Date": SimpleNamespace(now=lambda: NOW),
        "Response": FakeResponse,
        "method_name": lambda request: request.method,
        "_admin_csrf_ok": lambda env, form: form.get("csrf") == ["ok"],
    }
    values.update(runtime)
    for name, value in values.items():
        monkeypatch.setattr(admin, name, value, raising=False)


def test_the_update_page_shows_both_builds_and_starts_an_instance_update(
        tmp_path, monkeypatch):
    audits = []
    info = tmp_path / "build-info.json"
    info.write_text(json.dumps({"version": "1.2.8", "commit": "a" * 40,
                                "commitTime": 1_700_000_000,
                                "subject": "Fix <script>"}))
    (tmp_path / "status.json").write_text(json.dumps({
        "state": "idle", "upstreamCommit": "b" * 40, "upstreamSubject": "Newer",
        "upstreamVersion": "1.2.9", "checkedAt": 1_799_999_000}))

    async def audit(env, account, action, kind, target, outcome, details):
        audits.append((account, action, outcome, details["notice"]))

    async def nav_badges(env):
        return {}

    monkeypatch.setattr(admin_shell, "nav_badges", nav_badges)
    bind(monkeypatch, tmp_path, {"_audit_sensitive_action": audit})
    env = SimpleNamespace(
        ADMIN_PATH="secret", BUILD_INFO_FILE=str(info),
        INSTANCE_UPDATE_REQUEST=str(tmp_path / "request"),
        INSTANCE_UPDATE_STATUS=str(tmp_path / "status.json"))
    page = asyncio.run(admin.admin_handler(
        env, FakeRequest("https://code.example.com/secret?view=update"),
        {"view": ["update"]}, "<csrf>", "", "ada")).body
    assert "Update available" in page and "1.2.9" in page and "Newer" in page
    assert "Fix &lt;script&gt;" in page and "aaaaaaaaaaaa" in page
    assert 'value="start_update"' in page and 'value="check_update"' in page
    assert 'http-equiv="refresh"' not in page
    post = FakeRequest("https://code.example.com/secret", "POST",
                       {"csrf": "ok", "action": "start_update"})
    answer = asyncio.run(admin.admin_handler(env, post, {"view": ["update"]},
                                             "", "", "ada"))
    assert answer.status == 303 and "notice=unconfirmed" in answer.headers["location"]
    assert not (tmp_path / "request").exists()
    post = FakeRequest("https://code.example.com/secret", "POST",
                       {"csrf": "ok", "action": "start_update", "confirm": "yes"})
    answer = asyncio.run(admin.admin_handler(env, post, {"view": ["update"]},
                                             "", "", "ada"))
    assert answer.headers["location"] == "/secret?view=update&notice=update_started"
    assert (tmp_path / "request").read_text() == "update\n"
    assert audits[-1] == ("ada", "admin.console_start_update", "success",
                          "update_started")
    answer = asyncio.run(admin.admin_handler(env, post, {"view": ["update"]},
                                             "", "", "ada"))
    assert "notice=busy" in answer.headers["location"]
    page = asyncio.run(admin.admin_handler(
        env, FakeRequest("https://code.example.com/secret?view=update"),
        {"view": ["update"]}, "<csrf>", "", "ada")).body
    assert 'http-equiv="refresh"' in page and 'value="start_update"' not in page
    forged = FakeRequest("https://code.example.com/secret", "POST",
                         {"csrf": "bad", "action": "check_update"})
    answer = asyncio.run(admin.admin_handler(env, forged, {"view": ["update"]},
                                             "", "", "ada"))
    assert "notice=csrf" in answer.headers["location"]


def test_a_core_update_is_queued_once_for_self_deploy(tmp_path, monkeypatch):
    rows = []

    async def d1_first(env, sql, *args):
        if "FROM deploy_approvals" in sql:
            return {"id": 1} if any(r[-1] == "pending" for r in rows) else None
        return None

    async def d1_run(env, sql, *args):
        assert "INSERT INTO deploy_approvals" in sql
        rows.append(args + ("pending",))

    async def audit(*args):
        pass

    bind(monkeypatch, tmp_path, {"d1_first": d1_first, "d1_run": d1_run,
                                 "_audit_sensitive_action": audit})
    env = SimpleNamespace(CORE_ADMIN_UPDATES="1")
    post = FakeRequest("https://app.forkmesh.com/x", "POST",
                       {"csrf": "ok", "action": "start_update", "confirm": "yes"})
    answer = asyncio.run(admin.admin_handler(env, post, {"view": ["update"]},
                                             "", "", "ada"))
    assert "notice=update_started" in answer.headers["location"]
    assert rows[0][1:5] == ("admin", "forkmesh", "forkmesh", "main")
    assert rows[0][6] == "ada"
    answer = asyncio.run(admin.admin_handler(env, post, {"view": ["update"]},
                                             "", "", "ada"))
    assert "notice=busy" in answer.headers["location"] and len(rows) == 1
    check = FakeRequest("https://app.forkmesh.com/x", "POST",
                        {"csrf": "ok", "action": "check_update"})
    answer = asyncio.run(admin.admin_handler(env, check, {"view": ["update"]},
                                             "", "", "ada"))
    assert "notice=unavailable" in answer.headers["location"]


def test_repository_addresses_are_saved_for_an_existing_owner(tmp_path, monkeypatch):
    saved = []

    async def d1_run(env, sql, *args):
        saved.append(args)

    async def account_row(env, name):
        return "bi", ({"name": name} if name == "ada" else None)

    async def org_row(env, name):
        return "bi", ({"name": name} if name == "acme" else None)

    async def audit(*args):
        pass

    bind(monkeypatch, tmp_path, {"d1_run": d1_run, "_account_row": account_row,
                                 "_org_row": org_row,
                                 "_audit_sensitive_action": audit})
    env = SimpleNamespace()
    repo_short_urls.remember({"short": False, "owner": ""}, NOW)

    def post(**form):
        request = FakeRequest("https://code.example.com/x", "POST",
                              dict(form, csrf="ok", action="save_repo_paths"))
        return asyncio.run(admin.admin_handler(
            env, request, {"view": ["repo-addresses"]}, "", "", "ada"))

    assert "notice=owner_unknown" in post(short="1", owner="nobody").headers["location"]
    assert "notice=owner_invalid" in post(short="1", owner="").headers["location"]
    answer = post(short="1", owner="Acme")
    assert answer.headers["location"].endswith("view=repo-addresses&notice=addresses_saved")
    assert saved[-1][:2] == (
        "repo_paths", '{"canonical":false,"owner":"acme","short":true}')
    assert repo_short_urls.cached(NOW) is None
    post(short="1", owner="acme", canonical="1")
    assert saved[-1][1] == '{"canonical":true,"owner":"acme","short":true}'
    post(owner="acme", canonical="1")
    assert saved[-1][1] == '{"canonical":false,"owner":"acme","short":false}'


def test_the_console_links_both_pages():
    keys = [item[0] for _group, items in admin_shell.NAV for item in items]
    assert "update" in keys and "repo-addresses" in keys
    runtime = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
    assert "in _server_update_admin.VIEWS:" in runtime
    entry = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    assert '_server_update_admin = _LazyModule("server_update_admin")' in entry


def git(cwd, env, *args):
    return subprocess.run(["git", *args], cwd=cwd, env=env, check=True,
                          capture_output=True, text=True).stdout.strip()


INSTALLER = """#!/usr/bin/env bash
printf 'FMI {"step":"source","state":"running"}\\n'
git -C "$FORKMESH_INSTANCE_SRC" checkout -q --detach FETCH_HEAD
printf 'FMI {"step":"services","state":"running"}\\n'
[ "$1" = "--update" ] || exit 9
%s
"""


def publish(remote, env, subject, version, tail="exit 0"):
    (remote / "desktop").mkdir(exist_ok=True)
    (remote / "desktop" / "CMakeLists.txt").write_text(
        "project(ForkMesh VERSION %s LANGUAGES CXX)\n" % version)
    (remote / "www" / "public").mkdir(parents=True, exist_ok=True)
    (remote / "www" / "public" / "install-instance.sh").write_text(INSTALLER % tail)
    git(remote, env, "add", "-A")
    git(remote, env, "commit", "-qm", subject)
    return git(remote, env, "rev-parse", "HEAD")


@pytest.fixture
def instance(tmp_path):
    env = sandbox_env(tmp_path)
    remote = tmp_path / "remote"
    remote.mkdir()
    git(remote, env, "init", "-q", "-b", "main")
    first = publish(remote, env, "First release", "1.0.0")
    src = tmp_path / "src"
    git(tmp_path, env, "clone", "-q", "--depth", "1", "file://%s" % remote, str(src))
    state = tmp_path / "state"
    state.mkdir()
    (tmp_path / "instance.json").write_text(json.dumps(
        {"sourceUrl": "file://%s" % remote, "sourceRef": "main"}))
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    (bin_dir / "systemctl").write_text("#!/bin/sh\nexit 3\n")
    (bin_dir / "systemctl").chmod(0o755)
    env.update(PATH="%s:%s" % (bin_dir, env["PATH"]),
               FORKMESH_UPDATE_REQUEST=str(tmp_path / "request"),
               FORKMESH_UPDATE_STATE=str(state),
               FORKMESH_INSTANCE_SRC=str(src),
               FORKMESH_INSTANCE_JSON=str(tmp_path / "instance.json"))
    return SimpleNamespace(env=env, remote=remote, src=src, state=state,
                           first=first, request=tmp_path / "request")


def helper(instance, action):
    instance.request.write_text(action + "\n")
    result = subprocess.run(["bash", str(HELPER)], env=instance.env,
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    assert not instance.request.exists()
    return json.loads((instance.state / "status.json").read_text())


def test_the_helper_checks_then_updates_from_the_source(instance):
    latest = publish(instance.remote, instance.env, "Second release", "1.1.0")
    status = helper(instance, "check")
    assert status["state"] == "idle" and status["action"] == "check"
    assert status["currentCommit"] == instance.first
    assert status["currentVersion"] == "1.0.0"
    assert status["upstreamCommit"] == latest
    assert status["upstreamSubject"] == "Second release"
    assert status["upstreamVersion"] == "1.1.0"
    status = helper(instance, "update")
    assert status["state"] == "done" and status["exitCode"] == 0
    assert status["fromCommit"] == instance.first and status["toCommit"] == latest
    assert status["step"] == "services" and status["currentSubject"] == "Second release"
    log = instance.state / "update.log"
    assert 'FMI {"step":"source","state":"running"}' in log.read_text()
    assert oct(log.stat().st_mode & 0o777) == "0o600"
    assert oct((instance.state / "status.json").stat().st_mode & 0o777) == "0o640"


def test_a_failed_update_reports_the_installer_error(instance):
    publish(instance.remote, instance.env, "Broken", "1.1.0",
            "printf 'Error: The app did not start.\\n'; exit 4")
    status = helper(instance, "update")
    assert status["state"] == "failed" and status["exitCode"] == 4
    assert status["lastError"] == "The app did not start."


def test_the_helper_ignores_links_and_unknown_requests(instance, tmp_path):
    secret = tmp_path / "secret"
    secret.write_text("update\n")
    instance.request.symlink_to(secret)
    result = subprocess.run(["bash", str(HELPER)], env=instance.env,
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0
    assert not instance.request.is_symlink() and secret.exists()
    assert not (instance.state / "status.json").exists()
    instance.request.write_text("reboot now\n")
    subprocess.run(["bash", str(HELPER)], env=instance.env, timeout=60)
    assert not (instance.state / "status.json").exists()


def test_a_running_setup_blocks_the_update(instance):
    (Path(instance.env["PATH"].split(":")[0]) / "systemctl").write_text(
        "#!/bin/sh\nexit 0\n")
    status = helper(instance, "update")
    assert status["state"] == "failed"
    assert "still running" in status["lastError"]


def test_a_merge_request_updates_only_when_the_source_moved(instance):
    status = helper(instance, "auto")
    assert status["state"] == "done" and status["trigger"] == "merge"
    assert status["toCommit"] == instance.first
    assert not (instance.state / "update.log").exists()
    latest = publish(instance.remote, instance.env, "Merged change", "1.1.0")
    status = helper(instance, "auto")
    assert status["state"] == "done" and status["trigger"] == "merge"
    assert status["fromCommit"] == instance.first and status["toCommit"] == latest
    status = helper(instance, "update")
    assert status["trigger"] == "admin"


def test_the_helper_fetches_from_the_configured_source_not_origin(instance, tmp_path):
    other = tmp_path / "own"
    git(tmp_path, instance.env, "clone", "-q", str(instance.remote), str(other))
    moved = publish(other, instance.env, "Own platform change", "1.0.1")
    (tmp_path / "instance.json").write_text(json.dumps(
        {"sourceUrl": "file://%s" % other, "sourceRef": "main"}))
    status = helper(instance, "check")
    assert status["upstreamCommit"] == moved
    assert status["sourceUrl"] == "file://%s" % other
    (tmp_path / "instance.json").write_text(json.dumps(
        {"sourceUrl": "https://bad host/x", "sourceRef": "main"}))
    status = helper(instance, "check")
    assert status["state"] == "failed" and "not valid" in status["lastError"]


def test_an_own_domain_source_is_fetched_through_the_local_web_server():
    text = HELPER.read_text(encoding="utf-8")
    assert 'resolve=(-c "http.curloptResolve=$domain:443:127.0.0.1")' in text
    assert 'auto) run_update merge ;;' in text


def test_the_self_source_is_parsed_from_the_build_source():
    import self_update
    origin = "https://code.example.com"
    assert self_update.target(origin + "/acme/platform", origin) == (
        "acme", "platform")
    assert self_update.target(origin + "/acme/platform.git", origin) == (
        "acme", "platform")
    assert self_update.target(origin + "/platform.git", origin, "acme") == (
        "acme", "platform")
    for source in (origin + "/platform", "https://app.forkmesh.com/forkmesh/"
                   "forkmesh", origin + "/a/b/c", origin + "/acme/.hidden",
                   "https://code.example.com.evil.test/acme/platform", ""):
        assert self_update.target(source, origin) is None, source
    assert self_update.parse('{"auto": true}') == {"auto": True}
    assert self_update.parse('{"auto": "yes"}') == {"auto": False}
    assert self_update.parse("[") == {"auto": False}


class SelfUpdateWorld:
    def __init__(self, tmp_path, monkeypatch):
        import self_update
        self.module = self_update
        self.settings = {}
        self.audits = []
        self.base = "main"
        self.protected = True
        monkeypatch.setattr(server_update, "INSTANCE_REQUEST_ROOT",
                            str(tmp_path) + "/")
        monkeypatch.setattr(server_update, "INSTANCE_STATUS_ROOT",
                            str(tmp_path) + "/")
        info = tmp_path / "build-info.json"
        info.write_text(json.dumps({
            "commit": "a" * 40, "source": "https://code.example.com/acme/"
            "platform.git", "ref": "main",
            "daemon": {"state": "stale", "builtCommit": "b" * 40}}))
        self.request = tmp_path / "request"
        self.env = SimpleNamespace(
            PUBLIC_BASE_URL="https://code.example.com",
            BUILD_INFO_FILE=str(info),
            INSTANCE_UPDATE_REQUEST=str(self.request),
            INSTANCE_UPDATE_STATUS=str(tmp_path / "status.json"))

        async def d1_first(env, sql, *args):
            doc = self.settings.get(args[0])
            return {"doc": doc} if doc is not None else None

        async def d1_run(env, sql, *args):
            self.settings[args[0]] = args[1]

        async def storage_owner(env, owner, repo):
            return "alice" if owner in ("acme", "alice") else owner

        async def pull(env, owner, repo, number):
            return {"base": self.base, "number": str(number)}

        async def protected_refs(env, owner, repo):
            if self.protected is None:
                raise RuntimeError("settings unavailable")
            return ["refs/heads/main"] if self.protected else []

        async def audit(env, actor, action, kind, target, outcome, details):
            self.audits.append((actor, action, target, outcome, details))

        for name, value in {
                "d1_first": d1_first, "d1_run": d1_run,
                "_branch_protection": SimpleNamespace(
                    storage_owner=storage_owner, protected_refs=protected_refs,
                    branch_is_protected=lambda branch, refs: (
                        "refs/heads/" + branch) in refs),
                "_pull_lifecycle_record": pull,
                "_audit_sensitive_action": audit,
                "Date": SimpleNamespace(now=lambda: NOW)}.items():
            monkeypatch.setattr(self_update, name, value, raising=False)

    def merge(self, owner="alice", repo="platform", number=7):
        return asyncio.run(self.module.on_merge(
            self.env, owner, repo, number, "bob", {"baseAfter": "c" * 40}))


def test_a_merge_into_the_source_branch_files_one_update(tmp_path, monkeypatch):
    world = SelfUpdateWorld(tmp_path, monkeypatch)
    assert world.merge() == "" and not world.request.exists()
    asyncio.run(world.module.save(world.env, True))
    assert world.merge(repo="other") == ""
    assert world.merge(owner="mallory") == ""
    world.base = "topic"
    assert world.merge() == "" and not world.request.exists()
    world.base = "main"
    assert world.merge() == "requested"
    assert world.request.read_text() == "auto\n"
    assert world.merge(owner="acme", number=8) == "queued"
    assert world.request.read_text() == "auto\n"
    last = asyncio.run(world.module.last(world.env))
    assert last["pull"] == 8 and last["outcome"] == "queued"
    assert last["commit"] == "c" * 40 and last["repo"] == "acme/platform"
    assert [a[1] for a in world.audits] == ["admin.auto_update"] * 2
    assert world.audits[0][4]["outcome"] == "requested"
    world.request.unlink()
    asyncio.run(world.module.save(world.env, False))
    assert world.merge() == "" and not world.request.exists()
    core = SimpleNamespace(CORE_ADMIN_UPDATES="1")
    assert asyncio.run(world.module.on_merge(core, "alice", "platform", 9,
                                             "bob", {})) == ""


def test_a_merge_never_deploys_from_an_unprotected_source_branch(tmp_path, monkeypatch):
    world = SelfUpdateWorld(tmp_path, monkeypatch)
    asyncio.run(world.module.save(world.env, True))
    for protected in (False, None):
        world.protected = protected
        assert world.merge() == "unprotected" and not world.request.exists()
    assert world.audits[-1][3] == "refused"
    world.protected = True
    world.request.write_text("check\n")
    assert world.merge() == "requested"
    assert world.request.read_text() == "auto\n"
    world.request.write_text("update\n")
    assert world.merge() == "queued"
    assert world.request.read_text() == "update\n"


def test_the_console_turns_automatic_updates_on_and_shows_the_last_merge(
        tmp_path, monkeypatch):
    world = SelfUpdateWorld(tmp_path, monkeypatch)
    audits = []

    async def audit(env, account, action, kind, target, outcome, details):
        audits.append((action, outcome, details.get("notice")))

    async def nav_badges(env):
        return {}

    monkeypatch.setattr(admin_shell, "nav_badges", nav_badges)
    bind(monkeypatch, tmp_path, {"_audit_sensitive_action": audit,
                                 "_self_update": world.module})
    env = SimpleNamespace(ADMIN_PATH="secret", **vars(world.env))

    def post(**form):
        request = FakeRequest("https://code.example.com/secret", "POST",
                              dict(form, csrf="ok", action="save_auto_update"))
        return asyncio.run(admin.admin_handler(
            env, request, {"view": ["update"]}, "", "", "ada"))

    assert "notice=auto_on" in post(auto="1").headers["location"]
    assert world.settings["self_update"] == '{"auto":true}'
    world.merge()
    page = asyncio.run(admin.admin_handler(
        env, FakeRequest("https://code.example.com/secret?view=update"),
        {"view": ["update"]}, "<csrf>", "", "ada")).body
    assert "Update when a pull request is merged" in page
    assert "acme/platform" in page and "pull request #7 by bob" in page
    assert "Update requested" in page and "Turn off" in page
    assert "The git node program was built from older server code" in page
    assert "bbbbbbbbbbbb" in page
    assert "notice=auto_off" in post(auto="0").headers["location"]
    world.protected = False
    assert "notice=auto_unprotected" in post(auto="1").headers["location"]
    assert world.settings["self_update"] == '{"auto":false}'
    world.protected = True
    world.env.BUILD_INFO_FILE = str(tmp_path / "missing.json")
    env.BUILD_INFO_FILE = world.env.BUILD_INFO_FILE
    assert "notice=auto_no_target" in post(auto="1").headers["location"]
    assert [a[2] for a in audits] == ["auto_on", "auto_off", "auto_unprotected",
                                      "auto_no_target"]


def test_the_merge_handler_hands_merges_to_the_self_update():
    handlers = (ROOT / "src" / "route_handlers.py").read_text(encoding="utf-8")
    merge = handlers.split("async def repository_pull_merge_handler(", 1)[1]
    merge = merge.split("\nasync def ", 1)[0]
    block = merge.split('if mutation == "merge":\n            badge_fact', 1)[1]
    assert "_self_update.on_merge(" in block
    status = server_update.instance_status(SimpleNamespace())
    assert status["trigger"] == ""
