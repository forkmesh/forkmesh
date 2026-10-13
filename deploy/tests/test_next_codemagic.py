"""tools/next_codemagic.py: Codemagic app and builds, no network."""

import json
import subprocess
from pathlib import Path

import pytest

import next_codemagic as tool
import next_common as common
import render
from fakes import FakeHttp, FakeSsh

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"


def fleet(tmp_path):
    return render.load_fleet(FLEET, state_path=tmp_path / "none.json")


def test_desktop_token_reads_the_qsettings_variables_line(tmp_path):
    payload = json.dumps({"OTHER": "x", "CODEMAGIC_API_TOKEN": "tok-123"})
    conf = tmp_path / "ForkMesh.conf"
    conf.write_text('[actions]\nvariables="@ByteArray(%s)"\n'
                    % payload.replace('"', '\\"'))
    assert tool.desktop_token(conf) == "tok-123"
    assert tool.desktop_token(tmp_path / "missing.conf") == ""


def test_repository_url_embeds_the_quoted_password(tmp_path):
    url = tool.repository_url(fleet(tmp_path), "a/b+c")
    assert url == "https://fmnext-ci:a%2Fb%2Bc@app.next.forkmesh.com/git/forkmesh-next.git"


def test_create_app_records_the_id_and_never_prints_the_password(tmp_path, sandbox, capsys):
    (sandbox / "secrets.env").write_text("GIT_CI_PASSWORD=pw-secret\n")
    api = FakeHttp({("POST", "/apps"): {"application": {"_id": "app1", "appName": "x"}}})
    assert tool.create_app(fleet(tmp_path), api, common.Runner(execute=False)) == 0
    assert api.calls == []
    tool.create_app(fleet(tmp_path), api, common.Runner(execute=True), team="t1")
    body = api.calls[0][2]
    assert body["repositoryUrl"].startswith("https://fmnext-ci:pw-secret@")
    assert body["teamId"] == "t1"
    state = json.loads((sandbox / "codemagic-next.json").read_text())
    assert state["appId"] == "app1" and "pw-secret" not in json.dumps(state)
    assert (sandbox / "codemagic-next.json").stat().st_mode & 0o777 == 0o600
    assert "pw-secret" not in capsys.readouterr().out
    with pytest.raises(common.ToolError):
        tool.create_app(fleet(tmp_path), api, common.Runner(execute=True))


def test_build_pushes_first_then_triggers(tmp_path, sandbox):
    (sandbox / "codemagic-next.json").write_text(json.dumps({"appId": "app1"}))
    order = []
    api = FakeHttp({("POST", "/builds"): lambda body: order.append(("build", body))
                    or {"buildId": "b1"}})
    main = "c" * 40
    ssh = FakeSsh(answers=[(lambda h, s: True, (main + "\n").encode())])
    ssh.key, ssh.known_hosts = Path("/k"), Path("/kh")

    def run(argv, **kwargs):
        if "push" in argv:
            order.append(("push", argv[-3]))
        return subprocess.CompletedProcess(argv, 0, main + "\n", "")
    build_id = tool.start_build(fleet(tmp_path), api, common.Runner(execute=True),
                                "ios-native-build", ssh=ssh, run=run)
    assert build_id == "b1"
    assert [o[0] for o in order] == ["push", "push", "push", "build"]
    assert order[-1][1] == {"appId": "app1", "workflowId": "ios-native-build",
                            "branch": "main",
                            "environment": {"variables": {"CM_CLONE_UNSHALLOW": "true"}}}
    with pytest.raises(common.ToolError):
        tool.start_build(fleet(tmp_path), api, common.Runner(execute=True), "ios-build")


def test_show_lists_steps_and_artefacts():
    api = FakeHttp({("GET", "/builds/b1"): {"build": {
        "status": "finished", "fileWorkflowId": "ios-native-adhoc", "branch": "main",
        "buildActions": [{"_id": "s1", "status": "success", "name": "Build ipa"}],
        "artefacts": [{"name": "ForkMesh-native-1.0-7-adhoc.ipa", "url": "https://x/y"}]}}})
    lines = []
    build = tool.show(api, "b1", logs=True, out=lines.append)
    assert build["status"] == "finished"
    assert lines == ["build b1: finished (ios-native-adhoc, main)",
                     "  success    Build ipa",
                     "  artefact   ForkMesh-native-1.0-7-adhoc.ipa"]


def test_codemagic_yaml_has_the_native_workflows():
    import yaml
    data = yaml.safe_load((REPO / "codemagic.yaml").read_text())
    assert set(tool.WORKFLOWS) <= set(data["workflows"])
    for name in tool.WORKFLOWS:
        assert data["workflows"][name]["working_directory"] == "native/ios"
    for name in ("ios-native-adhoc", "ios-native-release"):
        assert data["workflows"][name]["environment"]["groups"] == ["app_store_connect"]


def test_full_clone_and_redacted_logs():
    import yaml
    data = yaml.safe_load((REPO / "codemagic.yaml").read_text())
    for name in tool.WORKFLOWS:
        assert data["workflows"][name]["environment"]["vars"]["CM_CLONE_UNSHALLOW"] == "true"
    assert tool.CLONE_VARIABLES == {"CM_CLONE_UNSHALLOW": "true"}
    line = "git clone --depth 50 https://fmnext-ci:s3cr-et_X@app.next.forkmesh.com/git/x.git d"
    assert tool.redact(line) == \
        "git clone --depth 50 https://fmnext-ci:***@app.next.forkmesh.com/git/x.git d"


def test_mirror_build_waits_for_the_mirror_and_pushes_nothing(tmp_path, sandbox):
    main = "a" * 40
    served = iter(["b" * 40, "b" * 40, main])
    calls = []

    def run(argv, **kwargs):
        calls.append(argv)
        if "ls-remote" in argv:
            return subprocess.CompletedProcess(argv, 0, next(served) + "\trefs/heads/main\n", "")
        if "merge-base" in argv:
            return subprocess.CompletedProcess(argv, 1, "", "")
        return subprocess.CompletedProcess(argv, 0, main + "\n", "")
    api = FakeHttp({("POST", "/builds"): {"buildId": "b9"}})
    lines = []
    runner = common.Runner(execute=True)
    runner.say = lines.append
    build_id = tool.start_build(fleet(tmp_path), api, runner, "ios-native-adhoc", run=run,
                                mirror=True, sleep=lambda s: None)
    assert build_id == "b9"
    assert not any("push" in argv for argv in calls)
    assert api.calls[0][2]["appId"] == tool.MIRROR_APP_ID
    assert sum("ls-remote" in argv for argv in calls) == 3
    assert any("serves main aaaaaaaaaaaa" in line for line in lines)


def test_mirror_wait_accepts_a_newer_main_and_times_out(tmp_path):
    main, newer = "a" * 40, "c" * 40

    def run(argv, **kwargs):
        if "ls-remote" in argv:
            return subprocess.CompletedProcess(argv, 0, newer + "\trefs/heads/main\n", "")
        if "merge-base" in argv:
            return subprocess.CompletedProcess(argv, 0, "", "")
        return subprocess.CompletedProcess(argv, 0, main + "\n", "")
    runner = common.Runner(execute=True)
    assert tool.wait_for_mirror(runner, repo=tmp_path, run=run) == newer

    def stale(argv, **kwargs):
        if "ls-remote" in argv:
            raise subprocess.TimeoutExpired(argv, 60)
        return subprocess.CompletedProcess(argv, 0, main + "\n", "")
    ticks = iter(range(0, 10000, 600))
    with pytest.raises(common.ToolError):
        tool.wait_for_mirror(runner, repo=tmp_path, run=stale, timeout=1200,
                             sleep=lambda s: None, clock=lambda: next(ticks))


def test_wait_follows_steps_and_new_log_lines():
    polls = [
        {"status": "building", "buildActions": [
            {"_id": "s1", "name": "Archive", "status": "building",
             "startedAt": "2026-10-01T09:00:00+00:00"}]},
        {"status": "building", "buildActions": [
            {"_id": "s1", "name": "Archive", "status": "success",
             "startedAt": "2026-10-01T09:00:00+00:00",
             "finishedAt": "2026-10-01T09:00:42+00:00"},
            {"_id": "s2", "name": "Export", "status": "failed",
             "startedAt": "2026-10-01T09:00:42+00:00",
             "finishedAt": "2026-10-01T09:00:43+00:00"}]},
        {"status": "failed", "buildActions": []},
    ]
    logs = {"s1": iter(["one\ntwo &amp; three\n", "one\ntwo &amp; three\nfour\n"]),
            "s2": iter(["boom\n"])}
    builds = iter(polls)

    class Api:
        def request(self, method, path, body=None, expect=None):
            if "/step/" in path:
                return {"text": next(logs[path.rsplit("/", 1)[1]])}
            return {"build": next(builds)}
    lines = []
    assert tool.wait(Api(), "b1", sleep=lambda s: None, out=lines.append) == "failed"
    text = "\n".join(line[11:] if line[2:3].isdigit() else line for line in lines)
    assert "> Archive" in text
    assert "    two & three" in text
    assert "    four" in text
    assert "success Archive (42s)" in text
    assert "failed Export (1s)" in text
    assert "boom" not in text
    assert text.count("    one") == 1
