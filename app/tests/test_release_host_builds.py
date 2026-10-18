import importlib.util
import json
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "forkmesh-release-host.py"
COMMIT = "a" * 40
MAIN = "c" * 40
APK = "forkmesh-android-universal.apk"
WIN = "forkmesh-windows-x86_64-setup.exe"
LINUX = ("forkmesh-linux-x86_64", "forkmesh-linux-x86_64.AppImage")
DMGS = ("forkmesh-macos-arm64.dmg", "forkmesh-macos-x86_64.dmg")
MIRROR = "forkmesh-mirror-node-linux-x86_64"
EVERYTHING = (APK, WIN, *LINUX, "forkmesh-linux-x86_64.AppImage.zsync", *DMGS, MIRROR)


def _load_tool():
    spec = importlib.util.spec_from_file_location("forkmesh_release_host_builds", TOOL)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


tool = _load_tool()


def _manifest(tag, names, builds=None, checksums="sums"):
    assets = []
    for name in sorted(names):
        asset = {"name": name}
        if builds and name in builds:
            asset.update(build=builds[name], commit=MAIN)
        assets.append(asset)
    return {"tag": tag, "tag_commit": COMMIT, "build_commit": COMMIT,
            "checksums_sha256": checksums, "assets": assets}


def _released(tag="v1.2.8", builds=None):
    candidate = _manifest(tag, EVERYTHING, builds)
    return {"latest": dict(candidate), f"candidate-{tag[1:]}": candidate}


def test_every_build_gets_a_larger_android_version_code():
    codes = [tool.android_version_code(tag, build)
             for tag, build in (("v1.2.8", 1), ("v1.2.8", 2), ("v1.2.8", 99), ("v1.2.9", 1),
                                ("v1.3.0", 1), ("v2.0.0", 1))]
    assert codes == sorted(codes) and len(set(codes)) == len(codes)
    assert tool.android_version_code("v1.2.9") == 1020901
    assert tool.android_version_code("v1.2.9") > 10208
    for build in (0, 100):
        with pytest.raises(tool.CompletionError):
            tool.android_version_code("v1.2.9", build)


def test_build_numbers_file_round_trips_and_ignores_garbage():
    text = tool.render_builds("v1.2.8", {"android": 3, "linux": 2})
    assert json.loads(text) == {"tag": "v1.2.8", "builds": {
        "android": 3, "linux": 2, "macos": 1, "windows": 1}}
    assert tool.parse_builds(text)["builds"]["android"] == 3
    junk = tool.parse_builds('{"tag": "latest", "builds": {"android": true, "linux": 0, '
                             '"macos": "4", "windows": 100}}')
    assert junk == {"tag": "", "builds": {p: 1 for p in tool.PLATFORM_ASSETS}}
    assert tool.parse_builds("not json")["tag"] == ""
    assert tool.recorded_builds(text, "v1.2.9") == {p: 1 for p in tool.PLATFORM_ASSETS}


def test_the_first_rebuild_of_a_release_is_build_two_from_main():
    plan = tool.plan_deploy(_released(), "android", "", MAIN)
    assert (plan["tag"], plan["build"], plan["commit"], plan["rebuild"]) == (
        "v1.2.8", 2, MAIN, True)
    assert plan["channel"] == "candidate-1.2.8"


def test_the_next_build_follows_the_file_and_the_signed_manifest():
    builds = tool.render_builds("v1.2.8", {"windows": 4})
    assert tool.plan_deploy(_released(), "windows", builds, MAIN)["build"] == 5
    manifests = _released(builds={APK: 6})
    assert tool.plan_deploy(manifests, "android", builds, MAIN)["build"] == 7
    stale = tool.render_builds("v1.2.7", {"android": 9})
    assert tool.plan_deploy(_released(), "android", stale, MAIN)["build"] == 2


def test_a_reset_never_reuses_a_published_build_number():
    reset = tool.render_builds("v1.2.8", {})
    manifests = _released(builds={DMGS[0]: 3, DMGS[1]: 3})
    assert tool.plan_deploy(manifests, "macos", reset, MAIN)["build"] == 4


def test_a_new_version_builds_each_platform_once_at_its_build_commit():
    manifests = _released()
    manifests["candidate-1.2.9"] = _manifest("v1.2.9", (MIRROR,))
    plan = tool.plan_deploy(manifests, "linux", "", MAIN)
    assert (plan["tag"], plan["build"], plan["commit"], plan["rebuild"]) == (
        "v1.2.9", 1, COMMIT, False)
    assert tool.plan_deploy(manifests, "linux", "", MAIN, first_build=True)["skip"] is False
    manifests["candidate-1.2.9"] = _manifest("v1.2.9", (MIRROR, *LINUX))
    skipped = tool.plan_deploy(manifests, "linux", "", MAIN, first_build=True)
    assert skipped["skip"] is True and skipped["build"] == 1
    assert tool.plan_deploy(manifests, "linux", "", MAIN)["build"] == 2


def test_the_target_is_the_newest_candidate_not_older_than_latest():
    manifests = _released("v1.2.8")
    manifests["candidate-1.2.7"] = _manifest("v1.2.7", EVERYTHING)
    assert tool.deploy_target(manifests)[0] == "candidate-1.2.8"
    manifests["candidate-1.3.0"] = _manifest("v1.3.0", (MIRROR,))
    assert tool.deploy_target(manifests)[0] == "candidate-1.3.0"
    assert tool.deploy_target(manifests, "1.2.8")[0] == "candidate-1.2.8"
    with pytest.raises(tool.CompletionError):
        tool.deploy_target(manifests, "v9.9.9")
    with pytest.raises(tool.CompletionError):
        tool.deploy_target({"latest": _manifest("v1.2.8", EVERYTHING)})


def test_build_ninety_nine_is_the_last_one():
    builds = tool.render_builds("v1.2.8", {"android": 99})
    with pytest.raises(tool.CompletionError, match="cut a new version"):
        tool.plan_deploy(_released(), "android", builds, MAIN)


def test_promotion_waits_for_everything_latest_offers():
    latest = _manifest("v1.2.8", EVERYTHING)
    have = frozenset({MIRROR, *LINUX})
    blockers = tool.promotion_blockers(latest, have, frozenset({MIRROR}))
    assert APK in blockers and WIN in blockers
    assert not tool.promotion_blockers(latest, frozenset(EVERYTHING), frozenset({MIRROR}))


def _args(tmp_path, *extra):
    return ["--repo", str(ROOT), "--work-dir", str(tmp_path), "--hosts-file",
            str(tmp_path / "hosts"), *extra]


def _fake_host(monkeypatch, tmp_path, manifests, built):
    monkeypatch.setattr(tool, "manifests_at", lambda repo, ref: manifests)
    monkeypatch.setattr(tool, "git", lambda *a, **k: MAIN if "rev-parse" in a else "")
    monkeypatch.setattr(tool, "parked_tag_target", lambda *a: None)
    monkeypatch.setattr(tool, "installer_public_key", lambda text: "key")
    monkeypatch.setattr(tool, "seed_targets", lambda hosts, *a: ["host"])
    monkeypatch.setattr(tool, "seed_mirrors", lambda *a, **k: None)
    monkeypatch.setattr(tool, "remove_worktree", lambda *a: None)
    calls = {}

    def build_apk(repo, work, tag, commit, code, signer):
        calls["apk"] = (tag, commit, code)
        apk = tmp_path / APK
        apk.write_bytes(b"apk")
        return apk
    monkeypatch.setattr(tool, "build_apk", build_apk)

    def publish(*a, promote=True, publish_args=(), extra_files=None, built_at="", **k):
        calls.update(promote=promote, publish_args=publish_args, extra_files=extra_files,
                     built=sorted(a[9]), built_at=built_at)
        return "b" * 40, {}
    monkeypatch.setattr(tool, "publish_and_promote", publish)
    monkeypatch.setattr(tool, "land_on_main",
                        lambda repo, branch, message: calls.setdefault("landed", message))
    monkeypatch.setattr(tool, "move_tag", lambda *a: calls.setdefault("moved", a))
    return calls


def test_an_android_rebuild_publishes_build_two_and_promotes(tmp_path, monkeypatch):
    calls = _fake_host(monkeypatch, tmp_path, _released(), None)
    assert tool.main(_args(tmp_path, "--deploy-build", "android")) == 0
    assert calls["apk"] == ("v1.2.8", MAIN, 1020802)
    assert calls["promote"] is True
    assert calls["publish_args"] == ("--build", "2", "--asset-commit", MAIN)
    assert json.loads(calls["extra_files"][tool.BUILDS_FILE])["builds"]["android"] == 2
    assert calls["built_at"] == MAIN
    assert "Android v1.2.8 build 2" in calls["landed"]
    assert calls["moved"][1:] == ("v1.2.8", "b" * 40)


def test_a_first_build_of_a_new_version_waits_for_the_others(tmp_path, monkeypatch):
    manifests = _released()
    manifests["candidate-1.2.9"] = _manifest("v1.2.9", (MIRROR, *LINUX))
    calls = _fake_host(monkeypatch, tmp_path, manifests, None)
    assert tool.main(_args(tmp_path, "--deploy-build", "android", "--first-build")) == 0
    assert calls["apk"] == ("v1.2.9", COMMIT, 1020901)
    assert calls["promote"] is False
    assert calls["publish_args"] == ()
    assert "moved" not in calls


def test_a_dry_run_only_plans(tmp_path, monkeypatch, capsys):
    calls = _fake_host(monkeypatch, tmp_path, _released(), None)
    assert tool.main(_args(tmp_path, "--deploy-build", "android", "--dry-run")) == 0
    assert "apk" not in calls and "promote" not in calls
    assert "Android v1.2.8 build 2" in capsys.readouterr().out


def test_check_build_waits_for_the_homepage(monkeypatch):
    manifests = _released(builds={WIN: 3})
    monkeypatch.setattr(tool, "manifests_at", lambda repo, ref: manifests)
    served = iter([{"tag": "v1.2.8", "checksums_sha256": "old"},
                   {"tag": "v1.2.8", "checksums_sha256": "sums"}])
    args = tool.argparse.Namespace(repo=str(ROOT), check_build="windows", tag=None,
                                   dry_run=False)
    assert tool.check_build(args, fetch=lambda: next(served), sleep=lambda s: None) == 0
    never = tool.argparse.Namespace(repo=str(ROOT), check_build="windows", tag=None,
                                    dry_run=False)
    clock = iter(range(0, 10 ** 6, 600))
    with pytest.raises(tool.CompletionError, match="Windows v1.2.8 build 3"):
        tool.check_build(never, fetch=lambda: {"tag": "v1.2.7"}, sleep=lambda s: None,
                         clock=lambda: next(clock))


def test_check_build_passes_while_the_version_is_held_back(monkeypatch):
    manifests = _released()
    manifests["candidate-1.2.9"] = _manifest("v1.2.9", (MIRROR, APK))
    monkeypatch.setattr(tool, "manifests_at", lambda repo, ref: manifests)
    args = tool.argparse.Namespace(repo=str(ROOT), check_build="android", tag=None,
                                   dry_run=False)
    assert tool.check_build(args, fetch=lambda: pytest.fail("no fetch")) == 0


def test_wait_for_candidate_polls_until_main_has_it(monkeypatch):
    states = iter([{}, {}, {"candidate-1.2.9": _manifest("v1.2.9", (MIRROR,))}])
    monkeypatch.setattr(tool, "manifests_at", lambda repo, ref: next(states))
    args = tool.argparse.Namespace(repo=str(ROOT), tag="v1.2.9")
    assert tool.wait_for_candidate(args, sleep=lambda s: None) == 0


@pytest.mark.parametrize("argv", [
    [],
    ["--wait-candidate"],
    ["--first-build", "--tag", "v1.2.8"],
    ["--auto", "--deploy-build", "linux"],
    ["--deploy-build", "ios"],
])
def test_the_command_line_refuses_unclear_requests(argv):
    with pytest.raises(SystemExit):
        tool.main(argv)


def test_build_modes_wait_for_the_lock(tmp_path):
    import fcntl
    path = tmp_path / "lock"
    with path.open("w") as held, path.open("w") as other:
        fcntl.flock(held, fcntl.LOCK_EX)
        ticks = iter(range(0, 10 ** 6, 60))
        assert tool.acquire_lock(other, False, sleep=lambda s: None,
                                 clock=lambda: next(ticks)) is False
        waits = []

        def release(seconds):
            waits.append(seconds)
            fcntl.flock(held, fcntl.LOCK_UN)
        assert tool.acquire_lock(other, True, sleep=release) is True
        assert waits == [tool.LOCK_POLL_SECONDS]
