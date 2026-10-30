import importlib.util
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[1]
IOS_TOOLS = ROOT / "native" / "ios" / "tools"
sys.path.insert(0, str(IOS_TOOLS))

import asc_appstore  # noqa: E402


def _load_host():
    spec = importlib.util.spec_from_file_location(
        "forkmesh_release_host_ios", ROOT / "tools" / "forkmesh-release-host.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def version(name, state, created="2026-09-01"):
    return {"id": f"id-{name}", "attributes": {"versionString": name, "appVersionState": state,
                                                "createdDate": created}}


LIVE = [version("1.0", "READY_FOR_DISTRIBUTION", "2026-08-19"),
        version("1.1.0", "READY_FOR_DISTRIBUTION", "2026-09-14")]


def test_the_xcconfig_version_ships_while_the_store_has_not_released_it():
    assert asc_appstore.target_version("1.4.0", LIVE) == "1.4.0"
    assert asc_appstore.target_version("1.4.0", LIVE + [version("1.2.1", "REJECTED")]) == "1.4.0"


def test_a_released_version_is_never_reused():
    released = LIVE + [version("1.4.0", "READY_FOR_DISTRIBUTION", "2026-10-01")]
    assert asc_appstore.target_version("1.4.0", released) == "1.4.1"
    pending = LIVE + [version("1.4.2", "PENDING_DEVELOPER_RELEASE", "2026-10-02")]
    assert asc_appstore.target_version("1.4.0", pending) == "1.4.3"


def test_the_rejected_version_is_renamed_and_resubmitted():
    versions = LIVE + [version("1.2.1", "REJECTED", "2026-09-16")]
    assert asc_appstore.plan("1.4.0", versions) == [("rename", "1.2.1 -> 1.4.0"),
                                                    ("reuse", "id-1.2.1")]


def test_a_version_in_review_is_pulled_back_for_the_newer_build():
    versions = LIVE + [version("1.4.0", "WAITING_FOR_REVIEW", "2026-10-01")]
    assert asc_appstore.plan("1.4.0", versions) == [("cancel review", "1.4.0"),
                                                    ("reuse", "id-1.4.0")]


def test_a_new_version_is_created_when_none_is_in_flight():
    assert asc_appstore.plan("1.4.0", LIVE) == [("create", "1.4.0")]


def test_submitting_a_released_version_is_refused():
    with pytest.raises(SystemExit, match="already READY_FOR_DISTRIBUTION"):
        asc_appstore.plan("1.1.0", LIVE)


def release_workflow():
    return yaml.safe_load((ROOT / "codemagic.yaml").read_text())["workflows"]["ios-native-release"]


def step(name):
    for entry in release_workflow()["scripts"]:
        if isinstance(entry, dict) and entry.get("name") == name:
            return entry["script"]
    raise AssertionError(f"no step {name!r}")


def test_the_app_store_submission_is_off_unless_the_release_host_asks():
    workflow = release_workflow()
    assert workflow["environment"]["vars"]["FORKMESH_IOS_APP_STORE"] == "0"
    assert workflow["scripts"][-1]["name"] == "Submit to the App Store"
    assert "asc_appstore.py submit" in step("Submit to the App Store")


def stub(bin_dir, name, body):
    path = bin_dir / name
    path.write_text("#!/usr/bin/env bash\n" + body)
    path.chmod(0o755)


def pick(tmp_path, app_store, store_version):
    work = tmp_path / f"pick-{app_store}-{store_version}"
    (work / "tools").mkdir(parents=True)
    (work / "Config").mkdir()
    bin_dir = work / "bin"
    bin_dir.mkdir()
    (work / "tools" / "asc_bundle_ids.py").write_text(
        "import sys\n"
        "if sys.argv[1] == 'next-build':\n"
        "    print('0' if sys.argv[2] == '1.4.1' else '21')\n"
        "else:\n"
        "    print('6803060735')\n")
    (work / "tools" / "asc_appstore.py").write_text(f"print({store_version!r})\n")
    (work / "Config" / "Version.xcconfig").write_text(
        "MARKETING_VERSION = 1.4.0\nCURRENT_PROJECT_VERSION = 0\n")
    cm_env = work / "cm_env"
    env = {"PATH": f"{bin_dir}:/usr/bin:/bin", "CM_ENV": str(cm_env),
           "FORKMESH_IOS_APP_STORE": app_store, "ASC_ISSUER_ID": "i", "ASC_KEY_ID": "k"}
    subprocess.run(["bash", "-c", step("Pick the next build number")], cwd=work, env=env,
                   check=True, capture_output=True, text=True)
    config = (work / "Config" / "Version.xcconfig").read_text()
    return (re.search(r"^MARKETING_VERSION = (\S+)$", config, re.M).group(1),
            cm_env.read_text())


def test_a_release_build_takes_the_version_the_store_still_accepts(tmp_path):
    assert pick(tmp_path, "1", "1.4.1") == (
        "1.4.1", "FORKMESH_IOS_VERSION=1.4.1\nFORKMESH_IOS_BUILD=0\n")
    assert pick(tmp_path, "1", "1.4.0")[0] == "1.4.0"
    assert pick(tmp_path, "0", "9.9.9") == (
        "1.4.0", "FORKMESH_IOS_VERSION=1.4.0\nFORKMESH_IOS_BUILD=21\n")


def test_the_mac_runner_carries_cm_env_between_steps(tmp_path):
    home = tmp_path / "home"
    (home / ".asc").mkdir(parents=True)
    for name in ("key_id", "issuer", "AuthKey.p8"):
        (home / ".asc" / name).write_text("x\n")
    (home / "Library" / "Keychains").mkdir(parents=True)
    (home / "Library" / "Keychains" / "build.keychain-db").write_text("")
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    stub(bin_dir, "security", "exit 0\n")
    stub(bin_dir, "xcrun", 'case "$*" in *show-sdk-version*) echo 26.0 ;; *) echo "iOS 26.0" ;; esac\n')
    tree = tmp_path / "tree"
    (tree / "native" / "ios" / "tools").mkdir(parents=True)
    shutil.copy(IOS_TOOLS / "xcodebuild_live.awk", tree / "native" / "ios" / "tools")
    steps = tmp_path / "steps"
    steps.mkdir()
    (steps / "00.sh").write_text('echo "FORKMESH_IOS_BUILD=21" >> "$CM_ENV"\n'
                                 'echo "FORKMESH_IOS_VERSION=1.4.0" >> "$CM_ENV"\n')
    (steps / "01.sh").write_text('[ "$FORKMESH_IOS_BUILD" = 21 ] && '
                                 'echo "carried $FORKMESH_IOS_VERSION ($FORKMESH_IOS_BUILD)"\n')
    (steps / "index.tsv").write_text("00.sh\tPick\n01.sh\tUse\n")
    (steps / "env.sh").write_text("export FORKMESH_IOS_APP_STORE=1\n")
    env = {"HOME": str(home), "PATH": f"{bin_dir}:/usr/bin:/bin",
           "XDG_DATA_HOME": str(home), "XDG_CONFIG_HOME": str(home), "XDG_CACHE_HOME": str(home)}
    result = subprocess.run(["bash", str(IOS_TOOLS / "mac_adhoc_build.sh"), str(steps), str(tree),
                             str(tmp_path / "out")], env=env, capture_output=True, text=True,
                            timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "carried 1.4.0 (21)" in result.stdout
    assert (steps / "cm_env").read_text() == "FORKMESH_IOS_BUILD=21\nFORKMESH_IOS_VERSION=1.4.0\n"


def test_the_release_host_writes_the_release_steps_with_the_app_store_on(tmp_path):
    host = _load_host()
    repo = tmp_path / "repo"
    (repo / "native" / "ios" / "tools").mkdir(parents=True)
    shutil.copy(ROOT / "codemagic.yaml", repo / "codemagic.yaml")
    shutil.copy(IOS_TOOLS / "codemagic_steps.py", repo / "native" / "ios" / "tools")
    git = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t"]
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    subprocess.run([*git, "add", "."], check=True)
    subprocess.run([*git, "commit", "-q", "-m", "c"], check=True)
    commit = subprocess.run([*git, "rev-parse", "HEAD"], capture_output=True, text=True,
                            check=True).stdout.strip()
    dest = tmp_path / "steps"
    host.ios_steps(repo, commit, dest, {"FORKMESH_IOS_APP_STORE": "1",
                                        "FORKMESH_IOS_WHATS_NEW": "It's new.\nTwo lines."})
    names = [line.split("\t")[1] for line in (dest / "index.tsv").read_text().splitlines()]
    assert names[-1] == "Submit to the App Store"
    assert "watchOS platform" in names
    printed = subprocess.run(
        ["bash", "-c", f'. "{dest}/env.sh"; echo "$FORKMESH_IOS_APP_STORE|$FORKMESH_IOS_WHATS_NEW"'],
        capture_output=True, text=True, check=True, env={"PATH": os.environ["PATH"]}).stdout
    assert printed == "1|It's new.\nTwo lines.\n"


class FakeReviewApi:
    def __init__(self, detail):
        self.detail = detail
        self.calls = []

    def __call__(self, method, path, body=None):
        self.calls.append((method, path, body))
        if method == "GET":
            if self.detail is None:
                raise SystemExit(f"ERROR: App Store Connect GET {path} answered 404: {{}}")
            return {"data": self.detail}
        return {}


def test_the_review_notes_say_the_app_sells_nothing_ahead_of_what_was_there():
    notes = asc_appstore.review_notes("Demo account: see the fields above.")
    assert notes.startswith(asc_appstore.REVIEW_NOTES + "\n\n" + asc_appstore.HEALTH_REVIEW_NOTES)
    assert notes.endswith("Demo account: see the fields above.")
    upgraded = asc_appstore.review_notes(asc_appstore.REVIEW_NOTES + "\n\nDemo account.")
    assert upgraded == asc_appstore.REVIEW_NOTES + "\n\n" + asc_appstore.HEALTH_REVIEW_NOTES + "\n\nDemo account."
    assert "HealthKit" in asc_appstore.HEALTH_REVIEW_NOTES and "Settings > Legal > Apple Health" in asc_appstore.HEALTH_REVIEW_NOTES
    assert asc_appstore.review_notes(notes) == notes
    assert len(asc_appstore.review_notes("x" * 5000)) == asc_appstore.REVIEW_NOTES_LIMIT
    for word in ("Patreon", "Supporter", "$"):
        assert word not in asc_appstore.REVIEW_NOTES


def test_the_review_notes_are_patched_into_the_versions_review_detail():
    api = FakeReviewApi({"id": "rd-1", "attributes": {"notes": "Sign in with the demo account."}})
    asc_appstore.set_review_notes(api, "v-1")
    method, path, body = api.calls[-1]
    assert (method, path) == ("PATCH", "/v1/appStoreReviewDetails/rd-1")
    assert body["data"]["attributes"]["notes"].endswith("Sign in with the demo account.")
    done = FakeReviewApi({"id": "rd-1", "attributes": {"notes": asc_appstore.review_notes("")}})
    asc_appstore.set_review_notes(done, "v-1")
    assert [call[0] for call in done.calls] == ["GET"]


def test_a_version_without_a_review_detail_gets_one_with_the_notes():
    api = FakeReviewApi(None)
    asc_appstore.set_review_notes(api, "v-1")
    method, path, body = api.calls[-1]
    assert (method, path) == ("POST", "/v1/appStoreReviewDetails")
    assert body["data"]["relationships"]["appStoreVersion"]["data"]["id"] == "v-1"
    assert body["data"]["attributes"]["notes"] == asc_appstore.review_notes("")
