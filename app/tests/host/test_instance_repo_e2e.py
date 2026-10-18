from __future__ import annotations

import json
import os
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import time

import httpx
import pytest

from forkmesh_host import paths

NODE = "forkmesh"
NODE_KEY = "N" * 43


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _git(cwd, *args, env=None):
    return subprocess.run(
        ["git", *args], cwd=str(cwd), check=True, capture_output=True,
        env=env, text=True).stdout.strip()


@pytest.fixture(scope="module")
def instance(staged_src, tmp_path_factory, sandbox_env):
    del staged_src
    tmp = tmp_path_factory.mktemp("instance-repo")
    assets = tmp / "assets"
    shutil.copytree(paths.APP_DIR / "public", assets)
    (assets / "index.html").write_text("<!doctype html><title>home</title>")
    (assets / "404.html").write_text("<!doctype html><title>404</title>")
    port = _free_port()
    origin = "http://127.0.0.1:%d" % port
    env = sandbox_env(tmp)
    env.update({
        "FORKMESH_HOST_DATA_DIR": str(tmp / "data"),
        "FORKMESH_HOST_ASSETS_DIR": str(assets),
        "FORKMESH_HOST_OFFLINE": "1",
        "FORKMESH_VAR_DATA_KEY": "repo-e2e-data-key-" + os.urandom(16).hex(),
        "FORKMESH_VAR_ADMIN_PASS": "repo-e2e-admin-" + os.urandom(8).hex(),
        "FORKMESH_VAR_APP_ORIGIN": origin,
        "FORKMESH_VAR_API_ORIGIN": origin,
        "FORKMESH_VAR_WORLD_ORIGIN": origin,
        "FORKMESH_VAR_PUBLIC_BASE_URL": origin,
        "FORKMESH_VAR_CORS_ALLOWED_ORIGINS": origin,
        "FORKMESH_VAR_TURNSTILE_SITE_KEY": "1x00000000000000000000AA",
        "FORKMESH_VAR_TURNSTILE_SECRET_KEY":
            "1x0000000000000000000000000000000AA",
        "FORKMESH_VAR_TURNSTILE_TEST_MODE": "1",
        "FORKMESH_VAR_HOSTED_GIT_CORE_NODES": NODE,
        "FORKMESH_VAR_HOSTED_REPO_MAX_BYTES": str(64 * 1024 * 1024),
        "GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "t@example.test",
        "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "t@example.test",
    })
    log = open(tmp / "server.log", "w+")
    process = subprocess.Popen(
        [sys.executable, "-m", "forkmesh_host", "serve", "--port", str(port),
         "--migrate", "--no-build", "--no-cron", "--log-level", "warning"],
        cwd=str(paths.HOST_DIR), env=env, stdout=log,
        stderr=subprocess.STDOUT)
    deadline = time.time() + 60
    while time.time() < deadline:
        if process.poll() is not None:
            log.seek(0)
            pytest.fail("server exited: " + log.read()[-4000:])
        try:
            if httpx.get(origin + "/health", timeout=1).status_code == 200:
                break
        except httpx.HTTPError:
            time.sleep(0.2)
    else:
        process.kill()
        pytest.fail("server did not start")
    db = tmp / "data" / "d1.sqlite"
    with sqlite3.connect(db) as conn:
        conn.execute(
            "INSERT INTO mirror_https_endpoints (node_bi, node_name, base_url,"
            " public_key, registration_sig, issued_at, checked_at, healthy,"
            " integrity, updated_at) VALUES (?,?,?,?,?,?,?,?,?,?)",
            ("bi-" + NODE, NODE, "https://git.instance.test", NODE_KEY, "sig",
             int(time.time() * 1000), int(time.time() * 1000), 1, "ok",
             int(time.time() * 1000)))
    hosted = tmp / "hosted"
    hosted.mkdir(mode=0o700)
    yield {"origin": origin, "db": db, "env": env, "tmp": tmp,
           "hosted": hosted, "log": log}
    process.terminate()
    try:
        process.wait(15)
    except subprocess.TimeoutExpired:
        process.kill()
    log.close()


def _cli(instance, *args):
    completed = subprocess.run(
        [sys.executable, "-m", "forkmesh_host", *args,
         "--data-dir", str(instance["tmp"] / "data")],
        cwd=str(paths.HOST_DIR), env=instance["env"], capture_output=True,
        text=True, timeout=300)
    lines = completed.stdout.strip().splitlines()
    assert lines, completed.stderr[-3000:]
    return completed.returncode, json.loads(lines[-1])


def _repo_cli(instance, source, *extra, name="cli-made"):
    return _cli(instance, "instance-repo", "--owner", "alice",
                "--organization", "acme", "--name", name,
                "--description", "Imported on the server",
                "--from", str(source), "--hosted-root",
                str(instance["hosted"]), *extra)


def _session(client, name):
    page = client.get("/signup")
    field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
    created = client.post("/api/accounts/signup", json={
        "nodeName": name, field: name + "@repo-e2e.dev",
        "password": "correct-horse-battery",
        "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"})
    assert created.status_code == 201, created.text
    return client.post("/api/accounts/login", json={
        "nodeName": name, "password": "correct-horse-battery"}
    ).json()["sessionToken"]


@pytest.fixture(scope="module")
def source(instance):
    env = instance["env"]
    work = instance["tmp"] / "source"
    work.mkdir()
    _git(work, "init", "-q", "-b", "main", env=env)
    (work / "README.md").write_text("# source\n")
    _git(work, "add", ".", env=env)
    _git(work, "commit", "-q", "-m", "first", env=env)
    _git(work, "tag", "-a", "v1", "-m", "v1", env=env)
    _git(work, "checkout", "-q", "-b", "feature", env=env)
    (work / "f.txt").write_text("f\n")
    _git(work, "add", ".", env=env)
    _git(work, "commit", "-q", "-m", "feature", env=env)
    _git(work, "checkout", "-q", "-b", "zzz-default", env=env)
    return work


@pytest.fixture(scope="module")
def web(instance):
    with httpx.Client(base_url=instance["origin"], timeout=30,
                      headers={"origin": instance["origin"]}) as client:
        token = _session(client, "alice")
        code, verified = _cli(instance, "ops", "verify-email", "alice")
        assert code == 0, verified
        headers = {"authorization": "Bearer " + token}
        org = client.post("/api/orgs", json={"name": "acme"},
                          headers=headers)
        assert org.status_code == 201, org.text
        made = client.post("/api/hosted-repositories", headers=headers, json={
            "name": "web-made", "organization": "acme", "readme": False,
            "description": "Imported on the server"})
        assert made.status_code == 201, made.text
        yield {"client": client, "headers": headers, "made": made.json()}


def _rows(instance, sql, *args):
    with sqlite3.connect(instance["db"]) as conn:
        conn.row_factory = sqlite3.Row
        return [dict(row) for row in conn.execute(sql, args).fetchall()]


def _hosted_rows(instance):
    return {row["id"]: row for row in _rows(
        instance, "SELECT * FROM hosted_repos WHERE deleted_at=0")}


def test_cli_repository_matches_the_web_one_and_lists_on_the_dashboard(
        instance, source, web):
    code, result = _repo_cli(instance, source)
    assert code == 0, result
    assert result["ok"] and result["created"] is True
    assert result["owner"] == "alice" and result["organization"] == "acme"
    assert result["branches"] == ["feature", "main", "zzz-default"]
    assert result["defaultBranch"] == "main"
    assert result["commit"] == _git(source, "rev-parse", "main")
    assert result["tags"] == 1 and result["refsUpdated"] == 4
    git_dir = instance["hosted"] / (result["id"] + ".git")
    assert result["gitDir"] == str(git_dir)
    assert _git(git_dir, "symbolic-ref", "HEAD") == "refs/heads/main"
    assert _git(git_dir, "rev-parse", "feature") == _git(
        source, "rev-parse", "feature")
    assert not _git(git_dir, "for-each-ref", "refs/forkmesh")
    sys.path.insert(0, str(paths.REPO_DIR / "tools"))
    import mirror_gateway
    assert result["stateHash"] == mirror_gateway.routing_refs_sha256(git_dir)

    rows = _hosted_rows(instance)
    web_id = web["made"]["hosting"]["id"]
    cli_row, web_row = rows[result["id"]], rows[web_id]
    for column in ("owner_bi", "creator_bi", "visibility"):
        assert cli_row[column] == web_row[column]
    assert cli_row["pushed_by"] == NODE and cli_row["pushed_at"] > 0
    links = {row["repo"]: row for row in _rows(
        instance, "SELECT * FROM org_repos")}
    assert links["cli-made"]["node_owner"] == links["web-made"]["node_owner"]
    assert links["cli-made"]["org_bi"] == links["web-made"]["org_bi"]
    audits = _rows(instance, "SELECT actor_bi, action, outcome "
                   "FROM sensitive_audit_log WHERE action IN "
                   "('repository.create_hosted', 'git.push')")
    created = [a for a in audits if a["action"] == "repository.create_hosted"]
    assert len(created) == 2 and created[0]["actor_bi"] == created[1][
        "actor_bi"]
    assert len([a for a in audits if a["action"] == "git.push"]) == 4

    client, headers = web["client"], web["headers"]
    listed = client.get("/api/repositories", headers=headers)
    assert listed.status_code == 200, listed.text
    repos = {r["name"]: r for r in listed.json()["repositories"]
             if r.get("owner") == "alice"}
    assert set(repos) >= {"cli-made", "web-made"}
    cli, made = repos["cli-made"], repos["web-made"]
    for field in ("owner", "visibility", "source", "maintainer",
                  "description"):
        assert cli[field] == made[field], field
    assert cli["cloneUrl"] == instance["origin"] + "/acme/cli-made"
    assert made["cloneUrl"] == instance["origin"] + "/acme/web-made"
    assert cli["branch"] == "main" and cli["commit"] == result["commit"]
    org_repos = client.get("/api/orgs/acme/repos", headers=headers)
    assert org_repos.status_code == 200, org_repos.text
    named = {item["repo"]: item for item in org_repos.json()["repos"]}
    assert named["cli-made"]["node"] == named["web-made"]["node"] == "alice"


def test_rerun_is_idempotent_and_only_moves_refs(instance, source, web):
    del web
    code, first = _repo_cli(instance, source)
    assert code == 0, first
    assert first["created"] is False and first["refsUpdated"] == 0
    env = instance["env"]
    _git(source, "checkout", "-q", "main", env=env)
    (source / "more.txt").write_text("more\n")
    _git(source, "add", ".", env=env)
    _git(source, "commit", "-q", "-m", "more", env=env)
    code, moved = _repo_cli(instance, source)
    assert code == 0, moved
    assert moved["id"] == first["id"] and moved["refsUpdated"] == 1
    assert moved["commit"] == _git(source, "rev-parse", "main")
    assert moved["stateHash"] != first["stateHash"]
    assert len(_rows(instance, "SELECT 1 FROM hosted_repos WHERE "
                     "deleted_at=0")) == 2

    _git(source, "reset", "-q", "--hard", "HEAD~1", env=env)
    (source / "other.txt").write_text("other\n")
    _git(source, "add", ".", env=env)
    _git(source, "commit", "-q", "-m", "diverged", env=env)
    code, refused = _repo_cli(instance, source)
    assert code == 1 and "--force" in refused["error"]
    git_dir = instance["hosted"] / (first["id"] + ".git")
    assert _git(git_dir, "rev-parse", "main") == moved["commit"]
    assert not _git(git_dir, "for-each-ref", "refs/forkmesh")
    code, forced = _repo_cli(instance, source, "--force")
    assert code == 0, forced
    assert forced["commit"] == _git(source, "rev-parse", "main")


def test_a_bundle_fills_a_new_repository_and_bad_input_is_refused(
        instance, source, web):
    del web
    bundle = instance["tmp"] / "source.bundle"
    _git(source, "bundle", "create", str(bundle), "--branches", "--tags")
    code, result = _repo_cli(instance, bundle, name="from-bundle")
    assert code == 0, result
    assert result["created"] and result["defaultBranch"] == "main"
    assert result["branches"] == ["feature", "main", "zzz-default"]
    code, missing = _cli(
        instance, "instance-repo", "--owner", "nobody", "--organization",
        "acme", "--name", "x", "--from", str(source), "--hosted-root",
        str(instance["hosted"]))
    assert code == 1 and "nobody" in missing["error"]
    code, other_org = _cli(
        instance, "instance-repo", "--owner", "alice", "--organization",
        "elsewhere", "--name", "cli-made", "--from", str(source),
        "--hosted-root", str(instance["hosted"]))
    assert code == 1 and not other_org["ok"]


def test_fork_of_records_the_parent_like_the_web_fork(instance, source, web):
    client, headers = web["client"], web["headers"]
    code, parent = _repo_cli(instance, source)
    assert code == 0, parent
    code, result = _repo_cli(instance, source, "--fork-of", "acme/cli-made",
                             name="platform-fork")
    assert code == 0, result
    assert result["forkedFrom"] == "acme/cli-made"
    listed = client.get("/api/repositories?fresh=1", headers=headers).json()
    record = next(r for r in listed["repositories"]
                  if r.get("owner") == "alice" and r["name"] == "platform-fork")
    assert record["forkedFrom"] == "acme/cli-made"
    forks = client.get("/api/repo/acme/cli-made/forks", headers=headers)
    assert forks.status_code == 200, forks.text
    assert [f["name"] for f in forks.json()["forks"]] == ["platform-fork"]
    code, again = _repo_cli(instance, source, "--fork-of", "acme/cli-made",
                            name="platform-fork")
    assert code == 0 and again["forkedFrom"] == "unchanged"
    code, pushed = _repo_cli(instance, source, name="platform-fork")
    assert code == 0, pushed
    listed = client.get("/api/repositories?fresh=1", headers=headers).json()
    record = next(r for r in listed["repositories"]
                  if r.get("owner") == "alice" and r["name"] == "platform-fork")
    assert record["forkedFrom"] == "acme/cli-made"


def test_fork_of_is_validated(instance, source, web):
    del web
    env = instance["env"]
    unrelated = instance["tmp"] / "unrelated"
    unrelated.mkdir()
    _git(unrelated, "init", "-q", "-b", "main", env=env)
    (unrelated / "x.txt").write_text("x\n")
    _git(unrelated, "add", ".", env=env)
    _git(unrelated, "commit", "-q", "-m", "unrelated", env=env)
    code, refused = _repo_cli(instance, unrelated, "--fork-of",
                              "acme/cli-made", name="not-a-fork")
    assert code == 1 and "shares no history" in refused["error"]
    assert not _rows(instance, "SELECT 1 FROM org_repos WHERE repo=?",
                     "not-a-fork")
    code, missing = _repo_cli(instance, source, "--fork-of", "acme/nothing",
                              name="orphan")
    assert code == 1 and "no such repository" in missing["error"]
    code, itself = _repo_cli(instance, source, "--fork-of", "acme/cli-made")
    assert code == 1 and "itself" in itself["error"]


def test_protection_and_actions_switches_are_set_and_enforced(
        instance, source, web):
    client, headers = web["client"], web["headers"]
    code, result = _repo_cli(instance, source, "--protect-default-branch",
                             "--actions", "off", name="guarded")
    assert code == 0, result
    assert result["settings"] == {"protectDefaultBranch": True,
                                  "actions": False}
    about = client.get("/api/repo/acme/guarded/about", headers=headers).json()
    assert about["features"]["protectDefaultBranch"] is True
    assert about["features"]["actions"] is False
    commit = _git(source, "rev-parse", "main")
    refused = client.post("/api/repo/acme/guarded/branches/ops", headers=headers,
                          json={"op": "update", "branch": "main",
                                "expectedOid": commit, "base": "feature"})
    assert refused.status_code == 403, refused.text
    assert refused.json()["error"] == "branch_protected"
    merge = client.post("/api/repo/acme/guarded/branches/ops", headers=headers,
                        json={"op": "merge", "branch": "feature",
                              "base": "main", "expectedOid": commit,
                              "baseOid": commit})
    assert merge.status_code == 403, merge.text
    bot = client.post("/api/repo/acme/guarded/branches/ops", headers=headers,
                      json={"op": "create", "branch": "forkmesh/pulls",
                            "startOid": commit})
    assert bot.status_code in (400, 403), bot.text
    allowed = client.post("/api/repo/acme/guarded/branches/ops",
                          headers=headers,
                          json={"op": "create", "branch": "topic",
                                "startOid": commit, "queue": True})
    assert allowed.status_code != 403, allowed.text
    turned_off = client.post("/api/repo/acme/guarded/about", headers=headers,
                             json={"features": {"protectDefaultBranch": False}})
    assert turned_off.status_code == 200, turned_off.text
    assert turned_off.json()["features"]["protectDefaultBranch"] is False
    audits = _rows(instance, "SELECT action, outcome FROM sensitive_audit_log "
                   "WHERE action='repository.protection_settings'")
    assert len(audits) == 2
    code, again = _repo_cli(instance, source, "--protect-default-branch",
                            name="guarded")
    assert code == 0 and again["settings"] == {"protectDefaultBranch": True}
