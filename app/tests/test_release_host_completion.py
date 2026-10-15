#!/usr/bin/env python3
"""Host-side completion of held-back releases (tools/forkmesh-release-host.py).

The release sandboxes build neither the Android APK nor the Windows
installer, so the gate holds a candidate until this tool supplies them.
"""

import importlib.util
import plistlib
import re
import struct
import sys
import zlib
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "forkmesh-release-host.py"
REQUIRED = frozenset({
    "forkmesh-linux-x86_64",
    "forkmesh-linux-x86_64.AppImage",
    "forkmesh-mirror-node-linux-x86_64",
})
APK = "forkmesh-android-universal.apk"
WIN = "forkmesh-windows-x86_64-setup.exe"
DMG = "forkmesh-macos-arm64.dmg"
INTEL_DMG = "forkmesh-macos-x86_64.dmg"
LINUX = frozenset({"forkmesh-linux-x86_64", "forkmesh-linux-x86_64.AppImage"})
MIRROR = "forkmesh-mirror-node-linux-x86_64"


def _load_tool():
    spec = importlib.util.spec_from_file_location("forkmesh_release_host", TOOL)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


tool = _load_tool()
REAL_COMPLETE_IOS = tool.complete_ios
REAL_FIND_BUILD_MAC = tool.find_build_mac
REAL_CHECK_HOMEPAGE = tool.check_homepage


@pytest.fixture(autouse=True)
def no_office_mac_or_network(monkeypatch):
    monkeypatch.setattr(tool, "find_build_mac", lambda *a, **k: None)
    monkeypatch.setattr(tool, "complete_ios", lambda args: 0)
    monkeypatch.setattr(tool, "check_homepage", lambda args: 0)


def _manifest(tag, names):
    return {"tag": tag, "assets": [{"name": name} for name in sorted(names)]}


def test_version_code_increases_with_every_release():
    assert tool.android_version_code("v0.8.7") == 807
    assert tool.android_version_code("0.8.10") == 810
    codes = [tool.android_version_code(t) for t in ("v0.8.7", "v0.8.8", "v0.9.0", "v1.0.0")]
    assert codes == sorted(codes) and len(set(codes)) == len(codes)
    # v0.8.6's hand-built APK carried versionCode 4; the next one must exceed it.
    assert tool.android_version_code("v0.8.7") > 4


@pytest.mark.parametrize("tag", ["v0.8", "release-1", "v0.100.0", "v0.8.100", ""])
def test_version_code_rejects_tags_outside_the_scheme(tag):
    with pytest.raises(tool.CompletionError):
        tool.android_version_code(tag)


def test_candidate_lacking_only_latest_apk_is_pending():
    manifests = {
        "latest": _manifest("v0.8.6", REQUIRED | {APK}),
        "candidate-0.8.6": _manifest("v0.8.6", REQUIRED | {APK}),
        "candidate-0.8.7": _manifest("v0.8.7", REQUIRED | {"forkmesh-linux-x86_64.AppImage.zsync"}),
    }
    assert tool.pending_android_completion(manifests, REQUIRED) == (
        "candidate-0.8.7", manifests["candidate-0.8.7"])


def test_newest_held_back_candidate_wins():
    manifests = {
        "latest": _manifest("v0.8.6", REQUIRED | {APK}),
        "candidate-0.8.7": _manifest("v0.8.7", REQUIRED),
        "candidate-0.8.10": _manifest("v0.8.10", REQUIRED),
        "candidate-0.8.9": _manifest("v0.8.9", REQUIRED),
    }
    assert tool.pending_android_completion(manifests, REQUIRED)[0] == "candidate-0.8.10"


@pytest.mark.parametrize(
    "manifests",
    [
        # Already promoted: latest is the candidate.
        {"latest": _manifest("v0.8.7", REQUIRED | {APK}),
         "candidate-0.8.7": _manifest("v0.8.7", REQUIRED | {APK})},
        # Candidate already carries the APK (the gate decides the rest).
        {"latest": _manifest("v0.8.6", REQUIRED | {APK}),
         "candidate-0.8.7": _manifest("v0.8.7", REQUIRED | {APK})},
        # A partial desktop matrix is not an Android problem.
        {"latest": _manifest("v0.8.6", REQUIRED | {APK}),
         "candidate-0.8.7": _manifest("v0.8.7", {"forkmesh-linux-x86_64"})},
        # latest never offered an APK: nothing would be dropped.
        {"latest": _manifest("v0.8.6", REQUIRED),
         "candidate-0.8.7": _manifest("v0.8.7", REQUIRED)},
        # An older candidate left behind is not re-promoted.
        {"latest": _manifest("v0.8.7", REQUIRED | {APK}),
         "candidate-0.8.5": _manifest("v0.8.5", REQUIRED)},
        # No latest manifest at all.
        {"candidate-0.8.7": _manifest("v0.8.7", REQUIRED)},
    ],
)
def test_nothing_to_complete(manifests):
    assert tool.pending_android_completion(manifests, REQUIRED) is None


def test_installer_key_is_read_from_install_sh():
    key = "MCowBQYDK2VwAyEAHBYOCW4qnyZkYAnEoqUrYxPiDRszjfJa+xJUeyQPUS0="
    assert tool.installer_public_key(f'KEY="{key}"\n# again {key}\n') == key
    with pytest.raises(tool.ConfigError):
        tool.installer_public_key("no key here")


def test_install_sh_carries_exactly_one_release_key():
    text = (ROOT / "www" / "public" / "install.sh").read_text()
    assert tool.INSTALLER_KEY_RE.fullmatch(tool.installer_public_key(text))


def test_hosts_file_parsing_matches_the_fanout_format():
    text = "# ForkMesh mirror fan-out targets\n\nmirror35 198.51.100.35\nmirror36 198.51.100.36\n"
    assert tool.parse_hosts(text) == [("mirror35", "198.51.100.35"), ("mirror36", "198.51.100.36")]
    with pytest.raises(tool.ConfigError):
        tool.parse_hosts("mirror35 $(reboot)\n")


def test_auto_mode_exits_quietly_without_a_pending_release(tmp_path, monkeypatch):
    monkeypatch.setattr(tool, "manifests_at", lambda repo, ref: {})
    code = tool.main(["--auto", "--repo", str(ROOT), "--work-dir", str(tmp_path)])
    assert code == 0


# --- host assets beyond the APK ---------------------------------------------


def test_host_assets_are_wanted_when_required_or_already_offered():
    latest = _manifest("v0.8.7", REQUIRED | {APK})

    # latest offers the APK, so withdrawing it is not allowed; Windows is not
    # required and nothing offers it yet, so it is not wanted.
    assert tool.wanted_host_assets(latest, REQUIRED) == LINUX | {APK}
    # Making Windows required is what bootstraps a platform nothing offers.
    assert tool.wanted_host_assets(latest, REQUIRED | {WIN}) == LINUX | {APK, WIN}
    assert tool.wanted_host_assets(None, REQUIRED) == LINUX


def test_an_incomplete_sandbox_matrix_is_not_a_host_problem():
    wanted = frozenset({APK, WIN})
    partial = _manifest("v0.8.8", {"forkmesh-linux-x86_64"})
    assert tool.missing_host_assets(partial, REQUIRED, wanted) == frozenset()

    complete = _manifest("v0.8.8", REQUIRED)
    assert tool.missing_host_assets(complete, REQUIRED, wanted) == wanted


def test_pending_completion_reports_every_missing_host_asset():
    manifests = {
        "latest": _manifest("v0.8.7", REQUIRED | {APK}),
        "candidate-0.8.8": _manifest("v0.8.8", REQUIRED),
    }
    channel, _, missing = tool.pending_completion(manifests, REQUIRED | {WIN})
    assert channel == "candidate-0.8.8"
    assert missing == frozenset({APK, WIN})


def test_the_published_release_is_completed_in_place_for_a_new_host_asset():
    # Windows becomes required after v0.8.7 is already public. The candidate it
    # was promoted from is revisited so the live release gains the installer,
    # rather than every user waiting for the next version.
    manifests = {
        "latest": _manifest("v0.8.7", REQUIRED | {APK}),
        "candidate-0.8.7": _manifest("v0.8.7", REQUIRED | {APK}),
    }
    channel, _, missing = tool.pending_completion(manifests, REQUIRED | {WIN})
    assert channel == "candidate-0.8.7"
    assert missing == frozenset({WIN})

    # ...and once it carries the installer there is nothing left to do.
    manifests["candidate-0.8.7"] = _manifest("v0.8.7", REQUIRED | {APK, WIN})
    assert tool.pending_completion(manifests, REQUIRED | {WIN}) is None


def test_completing_in_place_never_reopens_an_older_release():
    manifests = {
        "latest": _manifest("v0.8.7", REQUIRED | {APK}),
        "candidate-0.8.5": _manifest("v0.8.5", REQUIRED),
    }
    assert tool.pending_completion(manifests, REQUIRED | {WIN}) is None


def test_apk_selection_still_ignores_the_already_promoted_release():
    # pending_android_completion keeps its pre-Windows contract: it only ever
    # reports a candidate strictly newer than latest.
    manifests = {
        "latest": _manifest("v0.8.7", REQUIRED | {APK}),
        "candidate-0.8.7": _manifest("v0.8.7", REQUIRED | {APK}),
    }
    assert tool.pending_android_completion(manifests, REQUIRED) is None


def test_a_failed_build_is_not_retried_immediately(tmp_path):
    marker = tmp_path / "failed"
    assert not tool.failure_backoff_active(marker, 1000.0)
    marker.write_text("1000")
    assert tool.failure_backoff_active(marker, 1000.0 + 60)
    assert not tool.failure_backoff_active(
        marker, 1000.0 + tool.FAILURE_BACKOFF_SECONDS + 1)


def test_the_windows_signer_is_pinned_to_a_single_certificate():
    assert re.fullmatch(r"[0-9a-f]{64}", tool.DEFAULT_WINDOWS_SIGNER_SHA256)
    assert tool.HOST_ASSETS == ("forkmesh-linux-x86_64", "forkmesh-linux-x86_64.AppImage",
                                APK, WIN, DMG, INTEL_DMG)


def test_a_mirror_node_only_candidate_waits_for_the_baseline_linux_build():
    manifests = {
        "latest": _manifest("v1.2.4", REQUIRED | {APK, WIN, DMG,
                                                  "forkmesh-linux-x86_64.AppImage.zsync"}),
        "candidate-1.2.5": _manifest("v1.2.5", {MIRROR}),
    }
    channel, _, missing = tool.pending_completion(manifests, REQUIRED)
    assert channel == "candidate-1.2.5"
    assert missing == LINUX | {APK, WIN, DMG}


def test_a_candidate_without_the_mirror_node_is_not_completed():
    manifests = {
        "latest": _manifest("v1.2.4", REQUIRED | {APK}),
        "candidate-1.2.5": _manifest("v1.2.5", set()),
    }
    assert tool.pending_completion(manifests, REQUIRED) is None


def test_the_linux_baseline_build_publishes_the_zsync_with_the_appimage():
    source = TOOL.read_text()
    assert "if APPIMAGE_ASSET in missing:\n                    built[ZSYNC_ASSET]" in source
    assert tool.ZSYNC_ASSET == "forkmesh-linux-x86_64.AppImage.zsync"
    assert tool.LINUX_ASSETS == ("forkmesh-linux-x86_64", "forkmesh-linux-x86_64.AppImage")


SIGNER = "6e4a2120fa222535b0a1500324f1fe77cb22b7d9132c4f8a48d73567882adf6c"


@pytest.mark.parametrize("line", [
    # build-tools 36 and older
    f"Signer #1 certificate SHA-256 digest: {SIGNER}",
    # build-tools 37 (what held v1.2.0 and v1.2.1 back)
    f"V2 Signer: certificate SHA-256 digest: {SIGNER}",
])
def test_the_apk_signer_is_read_from_either_apksigner_format(line):
    output = "\n".join(
        "FORKMESH_APK_CERT " + text for text in (
            "V2 Signer: certificate DN: CN=ForkMesh Direct Distribution",
            line,
            "V2 Signer: certificate SHA-1 digest: " + "0" * 40,
        ))
    assert tool.apk_signer_sha256(output) == SIGNER


def test_an_apk_with_no_or_two_signers_names_no_signer():
    assert tool.apk_signer_sha256("FORKMESH_APK_BADGING package: x") == ""
    two = "\n".join((
        f"FORKMESH_APK_CERT Signer #1 certificate SHA-256 digest: {SIGNER}",
        "FORKMESH_APK_CERT Signer #2 certificate SHA-256 digest: " + "a" * 64,
    ))
    assert tool.apk_signer_sha256(two) == ""


def test_the_apk_is_the_native_android_app():
    script = tool.BUILD_APK_SCRIPT
    assert "cd native/android" in script
    assert ":app:assembleRelease" in script
    assert '-Pforkmesh.appId="$FORKMESH_ANDROID_APPLICATION_ID"' in script
    assert '-Pforkmesh.versionCode="$FORKMESH_ANDROID_VERSION_CODE"' in script
    assert '-Pforkmesh.versionName="$FORKMESH_ANDROID_VERSION_NAME"' in script
    assert "--no-daemon" in script and "loopback-netns.sh ./gradlew" in script
    assert ". tools/android-toolchain.sh" in script
    assert tool.APK_APPLICATION_ID == "com.forkmesh.forkmesh"
    assert tool.APK_OUTPUT == "native/android/app/build/outputs/apk/release/app-release.apk"
    assert "FORKMESH_APK_CERT" in script and "FORKMESH_APK_BADGING" in script


def test_the_apk_badging_names_the_package_and_version_code():
    output = ("FORKMESH_APK_BADGING package: name='com.forkmesh.forkmesh' "
              "versionCode='10205' versionName='1.2.5' platformBuildVersionName='17'")
    assert tool.apk_badging(output) == ("com.forkmesh.forkmesh", 10205)
    other = output.replace("com.forkmesh.forkmesh", "com.forkmesh.nativeapp")
    assert tool.apk_badging(other) == ("com.forkmesh.nativeapp", 10205)
    assert tool.apk_badging("FORKMESH_APK_BADGING nothing") is None


def test_the_native_apk_takes_the_release_version():
    assert tool.android_version_name("v1.2.5") == "1.2.5"
    assert tool.android_version_code("v1.2.5") == 10205
    assert tool.android_version_code("v1.2.5") > tool.android_version_code("v1.2.4")
    with pytest.raises(tool.CompletionError):
        tool.android_version_name("latest")


def _configured_build_tree(build_dir, source_dir, toolchain):
    (build_dir / "CMakeFiles" / "3.31.6").mkdir(parents=True)
    (build_dir / "CMakeCache.txt").write_text(
        f"CMAKE_HOME_DIRECTORY:INTERNAL={source_dir}\n", encoding="utf-8")
    (build_dir / "CMakeFiles" / "3.31.6" / "CMakeSystem.cmake").write_text(
        f'set(CMAKE_HOST_SYSTEM_NAME "Linux")\n\ninclude("{toolchain}")\n',
        encoding="utf-8")


def test_a_build_tree_configured_before_the_checkout_moved_starts_over(tmp_path):
    # The projects layout moved the checkout into src/; the interrupted v1.2.2
    # Windows tree kept including the toolchain file at its old path and failed
    # at configure on every attempt.
    source = tmp_path / "release" / "win-v1" / "desktop"
    source.mkdir(parents=True)
    toolchain = tmp_path / "src" / "desktop" / "cmake" / "mingw.cmake"
    toolchain.parent.mkdir(parents=True)
    toolchain.write_text("", encoding="utf-8")

    current = tmp_path / "current"
    _configured_build_tree(current, source, toolchain)
    assert tool.discard_relocated_build_tree(current, source) is False
    assert (current / "CMakeCache.txt").is_file()

    moved_toolchain = tmp_path / "moved-toolchain"
    _configured_build_tree(moved_toolchain, source, tmp_path / "desktop" / "cmake" / "mingw.cmake")
    assert tool.discard_relocated_build_tree(moved_toolchain, source) is True
    assert not moved_toolchain.exists()

    moved_source = tmp_path / "moved-source"
    _configured_build_tree(moved_source, tmp_path / "old-cache" / "win-v1" / "desktop", toolchain)
    assert tool.discard_relocated_build_tree(moved_source, source) is True
    assert not moved_source.exists()

    assert tool.discard_relocated_build_tree(tmp_path / "never-configured", source) is False


BUILD = "3b50dc56f6121d240b9f1640c382e97c27815d6d"


def _dmg_bytes(size=tool.DMG_MIN_BYTES + 4096):
    body = bytearray(size)
    body[-512:-508] = b"koly"
    return bytes(body)


class FakeCodemagic:
    def __init__(self, builds=(), states=(), artefact_bytes=None, artefacts=None):
        self.listed = list(builds)
        self.states = list(states)
        self.started = []
        self.downloads = []
        self.payload = _dmg_bytes() if artefact_bytes is None else artefact_bytes
        self.artefacts = artefacts

    def builds(self, workflow=tool.CODEMAGIC_WORKFLOW):
        return self.listed

    def start_build(self, tag, workflow=tool.CODEMAGIC_WORKFLOW, variables=None):
        self.started.append((tag, workflow, variables) if variables else tag)
        return "new-build"

    def build(self, build_id):
        status = self.states.pop(0) if len(self.states) > 1 else self.states[0]
        build = {"_id": build_id, "status": status, "commit": {"hash": BUILD}}
        if status == "finished":
            build["artefacts"] = self.artefacts if self.artefacts is not None else [
                {"name": "ForkMesh-1.2.4-macos-universal.dmg",
                 "url": "https://api.codemagic.io/artifacts/a/b/ForkMesh-1.2.4-macos-universal.dmg",
                 "size": len(self.payload)},
                {"name": ".ninja_log", "url": "https://api.codemagic.io/artifacts/a/b/.ninja_log"},
            ]
        return build

    def download(self, url, dest):
        self.downloads.append(url)
        Path(dest).write_bytes(self.payload)


def test_the_macos_image_is_a_host_asset_wanted_once_latest_offers_it():
    latest = _manifest("v1.2.4", REQUIRED | {APK, WIN, DMG})
    candidate = _manifest("v1.2.5", REQUIRED)
    pending = tool.pending_completion(
        {"latest": latest, "candidate-1.2.5": candidate}, REQUIRED)
    assert pending == ("candidate-1.2.5", candidate, frozenset({APK, WIN, DMG}))


def test_the_codemagic_token_is_read_from_the_actions_variables():
    conf = (
        "[General]\nx=1\n\n[actions]\n"
        'variables="@ByteArray({\\"ADMIN_PASS\\":\\"a\\\\nb\\",'
        '\\"CODEMAGIC_API_TOKEN\\":\\"tok-123\\"})"\n\n[agents]\nvariables=nope\n'
    )
    assert tool.actions_variable(conf, "CODEMAGIC_API_TOKEN") == "tok-123"
    assert tool.actions_variable(conf, "ADMIN_PASS") == "a\nb"
    assert tool.actions_variable(conf, "CODEMAGIC_APP_ID") == ""
    assert tool.actions_variable("[agents]\nvariables=x\n", "CODEMAGIC_API_TOKEN") == ""


def test_a_missing_codemagic_token_is_a_configuration_error(tmp_path, monkeypatch):
    monkeypatch.delenv("CODEMAGIC_API_TOKEN", raising=False)
    conf = tmp_path / "ForkMesh.conf"
    conf.write_text("[actions]\nrunAlertMode=none\n")
    monkeypatch.setattr(type(tool.LAYOUT), "settings_file", property(lambda self: conf))
    with pytest.raises(tool.ConfigError) as error:
        tool.codemagic_client()
    assert "CODEMAGIC_API_TOKEN" in str(error.value) and DMG in str(error.value)


def test_the_public_tag_must_peel_to_the_build_commit_before_codemagic_starts():
    listing = f"aaaa\trefs/tags/v1.2.4\n{BUILD}\trefs/tags/v1.2.4^{{}}\n"
    assert tool.peeled_tag_commit(listing, "v1.2.4") == BUILD
    assert tool.peeled_tag_commit(f"{BUILD}\trefs/tags/v1.2.4\n", "v1.2.4") == BUILD
    assert tool.peeled_tag_commit("", "v1.2.4") == ""

    client = FakeCodemagic()
    with pytest.raises(tool.CompletionError):
        tool.start_macos_build(client, "v1.2.4", BUILD, "https://x",
                               tag_lookup=lambda repo, tag: "b" * 40)
    assert client.started == []
    assert tool.start_macos_build(client, "v1.2.4", BUILD, "https://x",
                                  tag_lookup=lambda repo, tag: BUILD) == "new-build"
    assert client.started == ["v1.2.4"]


def test_a_live_or_finished_build_of_the_same_tag_is_reused():
    builds = [
        {"_id": "failed", "workflowId": "macos-desktop", "tag": "v1.2.4", "status": "failed",
         "createdAt": "2026-09-24T09:00:00Z"},
        {"_id": "other-tag", "workflowId": "macos-desktop", "tag": "v1.2.3", "status": "finished"},
        {"_id": "wrong-commit", "workflowId": "macos-desktop", "tag": "v1.2.4",
         "status": "finished", "commit": {"hash": "c" * 40}},
        {"_id": "running", "workflowId": "macos-desktop", "tag": "v1.2.4", "status": "building",
         "createdAt": "2026-09-24T09:10:00Z"},
    ]
    client = FakeCodemagic(builds=builds)
    assert tool.start_macos_build(client, "v1.2.4", BUILD, "https://x",
                                  tag_lookup=lambda repo, tag: pytest.fail("no lookup")) == "running"
    assert client.started == []


def test_the_dmg_is_polled_downloaded_and_verified(tmp_path):
    client = FakeCodemagic(states=["queued", "building", "finished"])
    sleeps = []
    dmg = tool.await_macos_dmg(client, "b1", "v1.2.4", BUILD, tmp_path,
                               poll=5, sleep=sleeps.append, clock=lambda: 0)
    assert dmg == tmp_path / DMG and dmg.read_bytes() == client.payload
    assert sleeps == [5, 5]
    assert client.downloads == [
        "https://api.codemagic.io/artifacts/a/b/ForkMesh-1.2.4-macos-universal.dmg"]


def test_a_zipped_artefact_is_unpacked(tmp_path):
    import io
    import zipfile
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        archive.writestr("dist/ForkMesh-1.2.4-macos-universal.dmg", _dmg_bytes())
    client = FakeCodemagic(states=["finished"], artefact_bytes=buffer.getvalue())
    dmg = tool.await_macos_dmg(client, "b1", "v1.2.4", BUILD, tmp_path, sleep=lambda s: None)
    assert dmg.read_bytes() == _dmg_bytes()


@pytest.mark.parametrize("state", ["failed", "canceled", "timeout"])
def test_a_failed_codemagic_build_fails_the_completion(tmp_path, state):
    client = FakeCodemagic(states=[state])
    with pytest.raises(tool.CompletionError):
        tool.await_macos_dmg(client, "b1", "v1.2.4", BUILD, tmp_path, sleep=lambda s: None)


def test_a_build_that_never_finishes_times_out(tmp_path):
    ticks = iter(range(0, 10_000, 100))
    client = FakeCodemagic(states=["building"])
    with pytest.raises(tool.CompletionError, match="did not finish"):
        tool.await_macos_dmg(client, "b1", "v1.2.4", BUILD, tmp_path, timeout=250,
                             sleep=lambda s: None, clock=lambda: next(ticks))


def test_a_build_of_another_commit_is_refused():
    with pytest.raises(tool.CompletionError, match="not the release build commit"):
        tool.codemagic_build_finished(
            {"status": "finished", "commit": {"hash": "d" * 40}}, BUILD)
    assert tool.codemagic_build_finished({"status": "building"}, BUILD) is False


def test_the_artefact_must_carry_the_release_version(tmp_path):
    client = FakeCodemagic(states=["finished"], artefacts=[
        {"name": "ForkMesh-1.2.3-macos-universal.dmg", "url": "https://api.codemagic.io/a"}])
    with pytest.raises(tool.CompletionError, match="ForkMesh-1.2.4-macos-universal.dmg"):
        tool.await_macos_dmg(client, "b1", "v1.2.4", BUILD, tmp_path, sleep=lambda s: None)


@pytest.mark.parametrize("payload, match", [
    (b"\0" * (tool.DMG_MIN_BYTES + 4096), "koly"),
    (_dmg_bytes(4096), "only"),
])
def test_a_file_that_is_not_a_disk_image_is_refused(tmp_path, payload, match):
    path = tmp_path / DMG
    path.write_bytes(payload)
    with pytest.raises(tool.CompletionError, match=match):
        tool.verify_dmg(path)


def test_a_size_or_md5_mismatch_is_refused(tmp_path):
    import hashlib
    path = tmp_path / DMG
    path.write_bytes(_dmg_bytes())
    tool.verify_dmg(path, len(_dmg_bytes()), hashlib.md5(_dmg_bytes()).hexdigest())
    with pytest.raises(tool.CompletionError):
        tool.verify_dmg(path, len(_dmg_bytes()) + 1)
    with pytest.raises(tool.CompletionError):
        tool.verify_dmg(path, None, "0" * 32)


@pytest.mark.parametrize("url, allowed", [
    ("https://api.codemagic.io/artifacts/x", True),
    ("https://codemagic.io/x", True),
    ("http://api.codemagic.io/x", False),
    ("https://evil.example/codemagic.io", False),
    ("https://api.codemagic.io.evil.example/x", False),
])
def test_the_token_is_only_sent_to_codemagic(url, allowed):
    assert tool.codemagic_url_allowed(url) is allowed



def test_a_codemagic_refusal_defers_only_the_macos_image(tmp_path, monkeypatch):
    commit = "a" * 40
    candidate = dict(_manifest("v9.9.9", ["forkmesh-mirror-node-linux-x86_64"]),
                     tag_commit=commit, build_commit=commit)
    missing = frozenset({tool.MACOS_ASSET, tool.WINDOWS_ASSET})
    monkeypatch.setattr(tool, "manifests_at", lambda repo, ref: {})
    monkeypatch.setattr(tool, "pending_completion",
                        lambda manifests, required: ("candidate-9.9.9", candidate, missing))
    monkeypatch.setattr(tool, "git", lambda *a, **k: commit if "rev-parse" in a else "")
    monkeypatch.setattr(tool, "installer_public_key", lambda text: "key")
    monkeypatch.setattr(tool, "seed_targets", lambda hosts, *a: ["host"])
    monkeypatch.setattr(tool, "codemagic_client", lambda: object())

    def refuse(*a, **k):
        raise tool.CompletionError("Codemagic POST /builds returned HTTP 402")
    monkeypatch.setattr(tool, "start_macos_build", refuse)
    installer = tmp_path / "setup.exe"
    installer.write_bytes(b"MZ")
    monkeypatch.setattr(tool, "build_windows_installer", lambda *a, **k: installer)
    calls = {}

    def publish(*a, promote=True, **k):
        calls["promote"] = promote
        calls["built"] = sorted(a[9])
        return "b" * 40, {}
    monkeypatch.setattr(tool, "publish_and_promote", publish)
    monkeypatch.setattr(tool, "seed_mirrors", lambda *a, **k: None)
    monkeypatch.setattr(tool, "land_on_main", lambda repo, branch, message: calls.setdefault(
        "landed", message))
    monkeypatch.setattr(tool, "move_tag", lambda *a: calls.setdefault("moved", a))
    monkeypatch.setattr(tool, "remove_worktree", lambda *a: None)
    code = tool.main(["--auto", "--repo", str(ROOT), "--work-dir", str(tmp_path),
                      "--hosts-file", str(tmp_path / "hosts")])
    assert code == 0
    assert calls["promote"] is False
    assert calls["built"] == [tool.WINDOWS_ASSET]
    assert "macOS image deferred" in calls["landed"]
    assert "moved" not in calls
    assert (tmp_path / "failed-candidate-9.9.9").is_file()


ARM64 = 0x0100000C
X86_64 = 0x01000007


def _thin(cpu):
    return b"\xcf\xfa\xed\xfe" + struct.pack("<iII", cpu, 0, 6) + bytes(48)


def _fat(cpus, commit=BUILD):
    header = b"\xca\xfe\xba\xbe" + struct.pack(">I", len(cpus))
    body = bytearray(0x4000 * (len(cpus) + 1))
    for index, cpu in enumerate(cpus):
        offset = 0x4000 * (index + 1)
        header += struct.pack(">iIIII", cpu, 0, offset, 0x4000, 14)
        body[offset:offset + 64] = _thin(cpu)
        body[offset + 512:offset + 512 + len(commit)] = commit.encode()
    body[:len(header)] = header
    return bytes(body)


def _udif(payload, chunk=4096, kind=0x80000005):
    payload = payload + bytes(-len(payload) % 512)
    data, entries = b"", []
    for start in range(0, len(payload), chunk):
        piece = payload[start:start + chunk]
        packed = zlib.compress(piece) if kind == 0x80000005 else piece
        entries.append(struct.pack(">IIQQQQ", kind, 0, start // 512, len(piece) // 512,
                                   len(data), len(packed)))
        data += packed
    entries.append(struct.pack(">IIQQQQ", 0xFFFFFFFF, 0, len(payload) // 512, 0, len(data), 0))
    mish = (b"mish" + struct.pack(">IQQQII", 1, 0, len(payload) // 512, 0, 0, 0) + bytes(24)
            + bytes(136) + struct.pack(">I", len(entries)) + b"".join(entries))
    xml = plistlib.dumps({"resource-fork": {"blkx": [{"Data": mish, "Name": "disk"}]}})
    body = data + bytes(tool.DMG_MIN_BYTES)
    koly = bytearray(512)
    koly[:4] = b"koly"
    koly[216:232] = struct.pack(">QQ", len(body), len(xml))
    return body + xml + bytes(koly)


def _write(tmp_path, payload, **kwargs):
    path = tmp_path / "ForkMesh-1.2.5-macos-universal.dmg"
    path.write_bytes(_udif(payload, **kwargs))
    return path


def test_a_universal_local_dmg_is_published_under_both_macos_names(tmp_path):
    payload = bytes(4000) + _fat([ARM64, X86_64]) + _fat([X86_64, ARM64])
    path, names = tool.local_macos_dmg(_write(tmp_path, payload), BUILD)
    assert names == frozenset({DMG, INTEL_DMG})
    report = tool.inspect_macos_dmg(path, BUILD)
    assert report == {"universal": True, "universal_binaries": 2,
                      "single_arch_binaries": 0, "carries_commit": True}


def test_an_arm64_only_local_dmg_is_published_as_the_arm64_image_only(tmp_path):
    payload = _thin(ARM64) + bytes(8192) + BUILD.encode()
    _, names = tool.local_macos_dmg(_write(tmp_path, payload), BUILD)
    assert names == frozenset({DMG})


def test_one_single_arch_binary_makes_the_local_dmg_arm64_only(tmp_path):
    payload = _fat([ARM64, X86_64]) + bytes(100) + _thin(ARM64)
    report = tool.inspect_macos_dmg(_write(tmp_path, payload), BUILD)
    assert report["universal"] is False and report["single_arch_binaries"] == 1


def test_a_fragmented_universal_binary_is_still_universal(tmp_path):
    fat = bytearray(_fat([X86_64, ARM64]))
    displaced = fat[0x8000:0xC000]
    fat[0x8000:0xC000] = bytes(0x4000)
    payload = bytes(fat) + bytes(100) + bytes(displaced)
    report = tool.inspect_macos_dmg(_write(tmp_path, payload), BUILD)
    assert report["universal"] is True and report["single_arch_binaries"] == 0


def test_a_displaced_slice_does_not_excuse_a_thin_binary_of_the_other_arch(tmp_path):
    fat = bytearray(_fat([X86_64, ARM64]))
    fat[0x8000:0xC000] = bytes(0x4000)
    payload = bytes(fat) + bytes(100) + _thin(X86_64)
    report = tool.inspect_macos_dmg(_write(tmp_path, payload), BUILD)
    assert report["universal"] is False and report["single_arch_binaries"] == 1


def test_a_local_dmg_from_other_source_is_refused(tmp_path):
    payload = _fat([ARM64, X86_64], commit="c" * 40)
    with pytest.raises(tool.CompletionError, match="build commit"):
        tool.local_macos_dmg(_write(tmp_path, payload), BUILD)


def test_a_local_dmg_with_unreadable_compression_is_refused(tmp_path):
    with pytest.raises(tool.CompletionError, match="UDZO"):
        tool.inspect_macos_dmg(_write(tmp_path, _fat([ARM64, X86_64]), kind=0x80000007), BUILD)


def test_a_missing_local_dmg_is_a_configuration_error(tmp_path):
    with pytest.raises(tool.ConfigError):
        tool.local_macos_dmg(tmp_path / "absent.dmg", BUILD)


def _completion(monkeypatch, tmp_path, manifests, calls):
    monkeypatch.setattr(tool, "manifests_at", lambda repo, ref: manifests)
    monkeypatch.setattr(tool, "git", lambda *a, **k: BUILD if "rev-parse" in a else "")
    monkeypatch.setattr(tool, "installer_public_key", lambda text: "key")
    monkeypatch.setattr(tool, "seed_targets", lambda hosts, *a: ["host"])
    monkeypatch.setattr(tool, "codemagic_client",
                        lambda: calls.setdefault("codemagic", True) and object())

    def publish(*a, promote=True, **k):
        calls["promote"] = promote
        calls["built"] = dict(a[9])
        return "b" * 40, {}
    monkeypatch.setattr(tool, "publish_and_promote", publish)
    monkeypatch.setattr(tool, "seed_mirrors", lambda *a, **k: None)
    monkeypatch.setattr(tool, "land_on_main", lambda repo, branch, message: calls.setdefault(
        "landed", message))
    monkeypatch.setattr(tool, "move_tag", lambda *a: calls.setdefault("moved", a))
    monkeypatch.setattr(tool, "remove_worktree", lambda *a: None)


def _release(latest_names):
    candidate = dict(_manifest("v1.2.5", REQUIRED | {APK, WIN, MIRROR}),
                     tag_commit=BUILD, build_commit=BUILD)
    return {"latest": _manifest("v1.2.4", REQUIRED | {APK, WIN} | latest_names),
            "candidate-1.2.5": candidate}


def test_the_tag_run_publishes_a_universal_local_dmg_without_codemagic(tmp_path, monkeypatch):
    calls = {}
    _completion(monkeypatch, tmp_path, _release({DMG}), calls)
    dmg = _write(tmp_path, _fat([ARM64, X86_64]))
    code = tool.main(["--tag", "v1.2.5", "--macos-dmg", str(dmg), "--repo", str(ROOT),
                      "--work-dir", str(tmp_path / "work"),
                      "--hosts-file", str(tmp_path / "hosts")])
    assert code == 0
    assert "codemagic" not in calls
    assert calls["promote"] is True
    assert calls["built"] == {DMG: dmg, INTEL_DMG: dmg}
    assert "moved" in calls
    assert dmg.is_file()


def test_the_dry_run_names_the_local_dmg(tmp_path, monkeypatch, capsys):
    calls = {}
    _completion(monkeypatch, tmp_path, _release({DMG}), calls)
    dmg = _write(tmp_path, _fat([ARM64, X86_64]))
    code = tool.main(["--tag", "v1.2.5", "--macos-dmg", str(dmg), "--dry-run",
                      "--repo", str(ROOT), "--work-dir", str(tmp_path / "work")])
    out = capsys.readouterr().out
    assert code == 0 and calls == {}
    assert f"would publish {dmg} as {DMG}, {INTEL_DMG}" in out
    assert "would build nothing" in out


def test_the_tag_run_without_a_local_dmg_builds_both_images_from_one_universal_build(
        tmp_path, monkeypatch, capsys):
    calls = {}
    _completion(monkeypatch, tmp_path, _release({DMG}), calls)
    code = tool.main(["--tag", "v1.2.5", "--dry-run", "--repo", str(ROOT),
                      "--work-dir", str(tmp_path / "work")])
    out = capsys.readouterr().out
    assert code == 0
    assert f"would build {DMG}, {INTEL_DMG} from one universal image on the office Mac" in out
    assert "codemagic" not in calls


def test_an_offered_intel_image_holds_the_release_until_a_universal_dmg(tmp_path, monkeypatch):
    calls = {}
    _completion(monkeypatch, tmp_path, _release({DMG, INTEL_DMG}), calls)
    arm = _write(tmp_path, _thin(ARM64) + bytes(4096) + BUILD.encode())
    monkeypatch.setattr(tool, "start_macos_build", lambda *a, **k: "build-1")
    monkeypatch.setattr(tool, "await_macos_dmg", lambda *a, **k: arm)
    code = tool.main(["--auto", "--repo", str(ROOT), "--work-dir", str(tmp_path / "work"),
                      "--hosts-file", str(tmp_path / "hosts")])
    assert code == 0
    assert calls["promote"] is False
    assert calls["built"] == {DMG: arm}
    assert "moved" not in calls


def test_an_arm64_only_local_dmg_is_not_published_as_the_intel_image(tmp_path, monkeypatch):
    calls = {}
    _completion(monkeypatch, tmp_path, _release({DMG}), calls)
    dmg = _write(tmp_path, _thin(ARM64) + bytes(4096) + BUILD.encode())
    code = tool.main(["--tag", "v1.2.5", "--macos-dmg", str(dmg), "--repo", str(ROOT),
                      "--work-dir", str(tmp_path / "work"),
                      "--hosts-file", str(tmp_path / "hosts")])
    assert code == 0
    assert calls["built"] == {DMG: dmg}
    assert calls["promote"] is True


def test_the_office_mac_builds_both_macos_images_without_codemagic(tmp_path, monkeypatch):
    calls = {}
    _completion(monkeypatch, tmp_path, _release({DMG, INTEL_DMG}), calls)
    dmg = _write(tmp_path, _fat([ARM64, X86_64]))
    monkeypatch.setattr(tool, "find_build_mac", lambda *a, **k: "mac")
    monkeypatch.setattr(tool, "build_macos_on_mac",
                        lambda mac, repo, work, tag, commit: calls.setdefault("mac", mac) and dmg)
    code = tool.main(["--auto", "--repo", str(ROOT), "--work-dir", str(tmp_path / "work"),
                      "--hosts-file", str(tmp_path / "hosts")])
    assert code == 0
    assert calls["mac"] == "mac" and "codemagic" not in calls
    assert calls["built"] == {DMG: dmg, INTEL_DMG: dmg}
    assert calls["promote"] is True and "moved" in calls


def test_codemagic_builds_the_macos_image_when_the_mac_drops_off(tmp_path, monkeypatch):
    calls = {}
    _completion(monkeypatch, tmp_path, _release({DMG, INTEL_DMG}), calls)
    dmg = _write(tmp_path, _fat([ARM64, X86_64]))
    monkeypatch.setattr(tool, "find_build_mac", lambda *a, **k: "mac")

    def unreachable(*a):
        raise tool.MacUnreachable("lost the office Mac")
    monkeypatch.setattr(tool, "build_macos_on_mac", unreachable)
    monkeypatch.setattr(tool, "start_macos_build", lambda *a, **k: "build-1")
    monkeypatch.setattr(tool, "await_macos_dmg", lambda *a, **k: dmg)
    code = tool.main(["--auto", "--repo", str(ROOT), "--work-dir", str(tmp_path / "work"),
                      "--hosts-file", str(tmp_path / "hosts")])
    assert code == 0
    assert calls["codemagic"] is True
    assert calls["built"] == {DMG: dmg, INTEL_DMG: dmg}
    assert calls["promote"] is True


def test_a_failed_mac_build_is_retried_later_not_sent_to_codemagic(tmp_path, monkeypatch):
    calls = {}
    _completion(monkeypatch, tmp_path, _release({DMG, INTEL_DMG}), calls)
    monkeypatch.setattr(tool, "find_build_mac", lambda *a, **k: "mac")

    def broken(*a):
        raise tool.CompletionError("the office Mac step failed (1): ninja: build stopped")
    monkeypatch.setattr(tool, "build_macos_on_mac", broken)
    code = tool.main(["--auto", "--repo", str(ROOT), "--work-dir", str(tmp_path / "work"),
                      "--hosts-file", str(tmp_path / "hosts")])
    assert code == 1
    assert "codemagic" not in calls and "built" not in calls
    assert (tmp_path / "work" / "failed-candidate-1.2.5").is_file()


def _mac_lan(tmp_path, monkeypatch, mac_at, open_ssh):
    key_dir = tmp_path / "mac-build"
    key_dir.mkdir()
    (key_dir / "id_ed25519").write_text("key")
    (key_dir / "known_hosts").write_text("forkmesh-mac ssh-ed25519 AAAA")
    address_file = tmp_path / "state" / "mac-build" / "address"
    monkeypatch.setattr(tool, "MAC_ADDRESS_FILE", address_file)
    monkeypatch.setattr(tool, "mac_setting", lambda name, default: default)
    asked = []

    def reachable(self):
        asked.append(self.address)
        return self.address == mac_at

    monkeypatch.setattr(tool.MacHost, "reachable", reachable)
    monkeypatch.setattr(tool, "lan_ssh_hosts",
                        lambda exclude: [a for a in open_ssh if a not in exclude])
    return key_dir, address_file, asked


def test_the_office_mac_found_by_a_lan_scan_is_tried_first_next_time(tmp_path, monkeypatch):
    key_dir, address_file, asked = _mac_lan(tmp_path, monkeypatch, "192.168.1.57",
                                            ["192.168.1.3", "192.168.1.57"])
    assert REAL_FIND_BUILD_MAC(key_dir).address == "192.168.1.57"
    assert asked == ["192.168.1.3", "192.168.1.57"]
    assert address_file.read_text() == "192.168.1.57\n"
    asked.clear()
    assert REAL_FIND_BUILD_MAC(key_dir).address == "192.168.1.57"
    assert asked == ["192.168.1.57"]


def test_a_stale_remembered_mac_address_falls_back_to_the_hint(tmp_path, monkeypatch):
    monkeypatch.setattr(tool, "MAC_BUILD_HOST", "build.test")
    key_dir, address_file, asked = _mac_lan(tmp_path, monkeypatch, "build.test", [])
    address_file.parent.mkdir(parents=True)
    address_file.write_text("192.168.1.57\n")
    assert REAL_FIND_BUILD_MAC(key_dir).address == "build.test"
    assert asked == ["192.168.1.57", "build.test"]
    assert address_file.read_text() == "build.test\n"


def test_the_mac_build_log_names_the_image_and_its_digest():
    digest = "96ecd3f1f76cc7dd47d1f3746dc6817dd4ac52c7e28aecb98672be9981396acb"
    path = "/Users/build/desktop-release/dist/ForkMesh-1.2.5-macos-universal.dmg"
    text = f"minos 13.0\nForkMesh 1.2.5\n{digest}  {path}\n{path}\n"
    assert tool.mac_dmg_result(text) == (digest, path)
    with pytest.raises(tool.CompletionError, match="no finished .dmg"):
        tool.mac_dmg_result("ninja: build stopped: subcommand failed.\n")


def test_the_codemagic_macos_artefact_is_the_universal_image():
    assert tool.dmg_artefact_name("v1.2.6") == "ForkMesh-1.2.6-macos-universal.dmg"
    config = yaml_load(ROOT / "codemagic.yaml")["workflows"]["macos-desktop"]
    script = "\n".join(step["script"] for step in config["scripts"])
    assert "tools/package/macos_universal_on_mac.sh" in script
    assert 'FORKMESH_BUILD_COMMIT="$CM_COMMIT"' in script
    assert "ForkMesh-*-macos-universal.dmg" in script


def yaml_load(path):
    import yaml
    return yaml.safe_load(path.read_text())


def _candidates(*tags):
    return {f"candidate-{t.lstrip('v')}": dict(_manifest(t, [MIRROR]), tag_commit=BUILD,
                                                build_commit=BUILD)
            for t in tags}


def test_ios_ships_the_newest_candidate_from_the_first_automated_release():
    assert tool.ios_release_tag(_candidates("v1.2.4", "v1.2.5")) == ""
    assert tool.ios_release_tag(_candidates("v1.2.5", "v1.2.6", "v1.3.0")) == "v1.3.0"
    stray = {"candidate-1.2.7": _manifest("v1.2.9", [MIRROR])}
    assert tool.ios_release_tag(stray) == ""


def test_the_app_store_notes_come_from_the_annotated_tag():
    raw = ("v1.2.6\n\nVersion 1.2.6 adds a thing.\n\n-----BEGIN PGP SIGNATURE-----\nabc\n"
           "-----END PGP SIGNATURE-----\n")
    assert tool.release_notes_text(raw, "v1.2.6") == "Version 1.2.6 adds a thing."
    assert tool.release_notes_text("", "v1.2.6") == "ForkMesh 1.2.6."


def test_codemagic_env_lines_are_read_without_evaluation():
    text = "FORKMESH_IOS_VERSION=1.4.0\nFORKMESH_IOS_BUILD=21\nnot a line\n$(x)=y\n"
    assert tool.cm_env_values(text) == {"FORKMESH_IOS_VERSION": "1.4.0",
                                        "FORKMESH_IOS_BUILD": "21"}


def _ios(monkeypatch, tmp_path, tags=("v1.2.6",)):
    monkeypatch.setattr(tool, "manifests_at", lambda repo, ref: _candidates(*tags))
    monkeypatch.setattr(tool, "release_notes", lambda repo, tag: f"notes for {tag}")
    work = tmp_path / "work"
    work.mkdir(exist_ok=True)
    return ["--repo", str(ROOT), "--work-dir", str(work)], work


def test_a_new_release_is_submitted_to_the_app_store_from_the_office_mac(tmp_path, monkeypatch):
    monkeypatch.setattr(tool, "complete_ios", REAL_COMPLETE_IOS)
    args, work = _ios(monkeypatch, tmp_path)
    seen = []
    monkeypatch.setattr(tool, "find_build_mac", lambda *a, **k: "mac")
    monkeypatch.setattr(tool, "release_ios_on_mac", lambda mac, repo, w, tag, commit, notes: (
        seen.append((mac, tag, commit, notes)) or {"route": "mac", "version": "1.4.0",
                                                   "build": "21"}))
    monkeypatch.setattr(tool, "codemagic_client", lambda *a: pytest.fail("used Codemagic"))
    assert tool.main(["--auto", "--ios-only", *args]) == 0
    assert seen == [("mac", "v1.2.6", BUILD, "notes for v1.2.6")]
    state = __import__("json").loads((work / "ios-v1.2.6.json").read_text())
    assert state["route"] == "mac" and state["build"] == "21" and state["build_commit"] == BUILD
    assert tool.main(["--auto", "--ios-only", *args]) == 0
    assert len(seen) == 1


def test_codemagic_ships_ios_when_the_office_mac_is_offline(tmp_path, monkeypatch):
    monkeypatch.setattr(tool, "complete_ios", REAL_COMPLETE_IOS)
    args, work = _ios(monkeypatch, tmp_path)
    client = FakeCodemagic(states=["finished"])
    monkeypatch.setattr(tool, "codemagic_client", lambda *a: client)
    monkeypatch.setattr(tool, "public_tag_commit", lambda repo, tag: BUILD)
    assert tool.main(["--auto", "--ios-only", *args]) == 0
    tag, workflow, variables = client.started[0]
    assert (tag, workflow) == ("v1.2.6", "ios-native-release")
    assert variables["FORKMESH_IOS_APP_STORE"] == "1"
    assert variables["FORKMESH_IOS_WHATS_NEW"] == "notes for v1.2.6"
    assert (work / "ios-v1.2.6.json").is_file()


def test_codemagic_falls_back_only_when_the_mac_drops_off(tmp_path, monkeypatch):
    monkeypatch.setattr(tool, "complete_ios", REAL_COMPLETE_IOS)
    args, work = _ios(monkeypatch, tmp_path)
    monkeypatch.setattr(tool, "find_build_mac", lambda *a, **k: "mac")

    def failed(*a):
        raise tool.CompletionError("the office Mac step failed (1): archive failed")
    monkeypatch.setattr(tool, "release_ios_on_mac", failed)
    monkeypatch.setattr(tool, "codemagic_client", lambda *a: pytest.fail("used Codemagic"))
    assert tool.main(["--auto", "--ios-only", *args]) == 1
    assert (work / "failed-ios-v1.2.6").is_file()
    assert not (work / "ios-v1.2.6.json").exists()
    assert tool.main(["--auto", "--ios-only", *args]) == 0


def test_releases_before_the_automation_are_not_submitted(tmp_path, monkeypatch):
    monkeypatch.setattr(tool, "complete_ios", REAL_COMPLETE_IOS)
    args, _ = _ios(monkeypatch, tmp_path, tags=("v1.2.5",))
    monkeypatch.setattr(tool, "find_build_mac", lambda *a, **k: pytest.fail("looked for the Mac"))
    assert tool.main(["--auto", "--ios-only", *args]) == 0


def test_the_ios_dry_run_touches_nothing(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(tool, "complete_ios", REAL_COMPLETE_IOS)
    args, work = _ios(monkeypatch, tmp_path)
    monkeypatch.setattr(tool, "find_build_mac", lambda *a, **k: pytest.fail("looked for the Mac"))
    assert tool.main(["--auto", "--ios-only", "--dry-run", *args]) == 0
    assert "submit it for App Store review" in capsys.readouterr().out
    assert sorted(p.name for p in work.iterdir()) == ["lock"]


def test_an_ios_codemagic_build_of_the_same_tag_is_reused(monkeypatch):
    client = FakeCodemagic(builds=[{"_id": "ios-1", "workflowId": "ios-native-release",
                                    "tag": "v1.2.6", "status": "building"}],
                           states=["finished"])
    result = tool.release_ios_on_codemagic(client, "v1.2.6", "notes", "repo",
                                           tag_lookup=lambda *a: "", sleep=lambda s: None)
    assert result == {"route": "codemagic", "codemagic_build": "ios-1"}
    assert client.started == []


def test_ios_waits_for_the_public_tag_before_codemagic(monkeypatch):
    client = FakeCodemagic(states=["finished"])
    with pytest.raises(tool.CompletionError, match="does not serve tag v1.2.6"):
        tool.release_ios_on_codemagic(client, "v1.2.6", "notes", "repo",
                                      tag_lookup=lambda *a: "", sleep=lambda s: None)


def _homepage(monkeypatch, tmp_path):
    latest = dict(_manifest("v1.2.6", [MIRROR]), checksums_sha256="c" * 64)
    monkeypatch.setattr(tool, "manifests_at", lambda repo, ref: {"latest": latest})
    work = tmp_path / "work"
    work.mkdir()
    import argparse
    return argparse.Namespace(repo=str(ROOT), work_dir=str(work), dry_run=False), work, latest


def test_the_homepage_check_passes_once_app_forkmesh_serves_the_release(tmp_path, monkeypatch):
    args, work, latest = _homepage(monkeypatch, tmp_path)
    assert REAL_CHECK_HOMEPAGE(args, fetch=lambda: dict(latest), now=1000) == 0
    assert (work / "homepage-v1.2.6").is_file()
    assert REAL_CHECK_HOMEPAGE(args, fetch=lambda: pytest.fail("fetched again")) == 0


def test_a_homepage_still_on_the_old_release_fails_after_half_an_hour(tmp_path, monkeypatch):
    args, work, _ = _homepage(monkeypatch, tmp_path)
    old = {"tag": "v1.2.5", "checksums_sha256": "d" * 64}
    assert REAL_CHECK_HOMEPAGE(args, fetch=lambda: old, now=1000) == 0
    assert REAL_CHECK_HOMEPAGE(args, fetch=lambda: old, now=1000 + 29 * 60) == 0
    with pytest.raises(tool.CompletionError, match="serves v1.2.5"):
        REAL_CHECK_HOMEPAGE(args, fetch=lambda: old, now=1000 + 31 * 60)

    def offline():
        raise OSError("connection refused")
    with pytest.raises(tool.CompletionError, match="connection refused"):
        REAL_CHECK_HOMEPAGE(args, fetch=offline, now=1000 + 31 * 60)
    assert not (work / "homepage-v1.2.6").exists()


def test_main_runs_every_step_and_reports_the_worst(tmp_path, monkeypatch):
    ran = []

    def step(name, code):
        return lambda args: ran.append(name) or code
    monkeypatch.setattr(tool, "complete_release", step("assets", 0))
    monkeypatch.setattr(tool, "complete_ios", step("ios", 1))
    monkeypatch.setattr(tool, "check_homepage", step("homepage", 0))
    monkeypatch.setattr(tool, "roll_out_release", step("rollout", 0))
    args = ["--auto", "--repo", str(ROOT), "--work-dir", str(tmp_path)]
    assert tool.main(args) == 1 and ran == ["assets", "rollout", "ios", "homepage"]
    ran.clear()
    assert tool.main([*args, "--skip-ios"]) == 0 and ran == ["assets", "rollout", "homepage"]
    ran.clear()
    assert tool.main([*args, "--ios-only"]) == 1 and ran == ["ios"]
    ran.clear()
    assert tool.main([*args, "--skip-rollout", "--skip-ios"]) == 0 and ran == ["assets", "homepage"]
    ran.clear()
    assert tool.main([*args, "--no-land"]) == 1 and "rollout" not in ran
    ran.clear()
    assert tool.main([*args, "--rollout-only"]) == 0 and ran == ["rollout"]


def _mac_shell(home, script, *args):
    import subprocess
    return subprocess.run(["bash", "-c", script, "mac-step", *args], cwd=home,
                          capture_output=True, text=True, timeout=60)


def test_the_mac_lock_is_shared_with_the_ios_builds_and_retaken_by_a_retry(tmp_path):
    lock = tool.MAC_LOCK_SCRIPT
    assert _mac_shell(tmp_path, lock, "desktop-release v1.2.6", "0").returncode == 0
    assert (tmp_path / "ci/lock/run").read_text() == "desktop-release v1.2.6\n"
    other = _mac_shell(tmp_path, lock, "ios-release v1.2.6", "0")
    assert other.returncode == 1 and "desktop-release v1.2.6" in other.stderr
    owned = f"MAC_LOCK_NAME='ios-release v1.2.6'\n{tool.MAC_OWNED}\necho ran"
    refused = _mac_shell(tmp_path, owned)
    assert refused.returncode == 1 and "lost the build lock" in refused.stderr
    retry = _mac_shell(tmp_path, lock, "desktop-release v1.2.6", "0")
    assert retry.returncode == 0 and "ending an earlier attempt" in retry.stdout
    assert _mac_shell(tmp_path, tool.MAC_UNLOCK_SCRIPT, "ios-release v1.2.6").returncode == 0
    assert (tmp_path / "ci/lock").is_dir()
    assert _mac_shell(tmp_path, tool.MAC_UNLOCK_SCRIPT, "desktop-release v1.2.6").returncode == 0
    assert not (tmp_path / "ci/lock").exists()
    assert _mac_shell(tmp_path, lock, "ios-release v1.2.6", "0").returncode == 0
    ran = _mac_shell(tmp_path, owned)
    assert ran.stdout == "ran\n"
    assert (tmp_path / "ci/lock/pid").read_text().strip().isdigit()


def test_vaulted_actions_variable_is_read_from_the_vault(monkeypatch):
    asked = []
    monkeypatch.setattr(tool, "vault_actions_variable", lambda name: asked.append(name) or "from-vault")
    conf = ('[actions]\nvariables="@ByteArray({\\"CODEMAGIC_APP_ID\\":\\"plain\\",'
            '\\"CODEMAGIC_API_TOKEN\\":\\"[stored in the ForkMesh vault]\\"})"\n')
    assert tool.actions_variable(conf, "CODEMAGIC_APP_ID") == "plain"
    assert tool.actions_variable(conf, "CODEMAGIC_API_TOKEN") == "from-vault"
    assert asked == ["CODEMAGIC_API_TOKEN"]


def test_release_password_reads_plain_files_and_reports_an_unreadable_vault(tmp_path, monkeypatch):
    monkeypatch.setattr(tool, "FORKMESH_DATA", tmp_path)
    password = tmp_path / ".apk-keystore-password"
    password.write_text(" secret \n")
    assert tool.release_password(password, "apk-keystore-password") == "secret"
    password.write_text("[stored in the ForkMesh vault]\n")
    with pytest.raises(tool.ConfigError) as raised:
        tool.release_password(password, "apk-keystore-password")
    assert "vault_missing" in str(raised.value)
