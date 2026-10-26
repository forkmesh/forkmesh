import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
PUBLIC = REPO / "app" / "public"
WEB = PACKAGE / "content" / "web"
DESKTOP = PACKAGE / "content" / "desktop"
MANIFEST = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
CATALOG = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
PAGE = (WEB / "page.js").read_text(encoding="utf-8")
VIEW = (WEB / "view.html").read_text(encoding="utf-8")
STUDIO = (PUBLIC / "call-studio.js").read_text(encoding="utf-8")
FEATURE = (DESKTOP / "FaceChangerFeature.cpp").read_text(encoding="utf-8")
FEATURE_HEADER = (DESKTOP / "FaceChangerFeature.h").read_text(encoding="utf-8")
CAMERA = (DESKTOP / "FaceCamera.cpp").read_text(encoding="utf-8")
CAMERA_HEADER = (DESKTOP / "FaceCamera.h").read_text(encoding="utf-8")
ENGINE_PAGE = (DESKTOP / "FaceStudioPage.cpp").read_text(encoding="utf-8")
CAMERA_TEST = (DESKTOP / "tests" / "test_face_camera.cpp").read_text(encoding="utf-8")
DESKTOP_SOURCES = (FEATURE, FEATURE_HEADER, CAMERA, CAMERA_HEADER, ENGINE_PAGE,
                   (DESKTOP / "FaceStudioPage.h").read_text(encoding="utf-8"))
CXX = shutil.which("g++")
BUILD = json.loads((DESKTOP / "build.json").read_text(encoding="utf-8"))
NODE = shutil.which("node")
BRANDS = (
    "zoom", "teams", "google meet", "skype", "facetime", "discord", "slack",
    "webex", "whatsapp", "obs", "snapchat", "instagram", "tiktok",
)


def test_listing_is_free_and_matches_the_catalog():
    assert MANIFEST["id"] == "face-changer"
    assert MANIFEST["price"] == {"amount": 0, "currency": "USD"}
    assert MANIFEST["license"]["model"] == "free-open-source"
    assert MANIFEST["license"]["spdx"] == "MIT"
    assert (PACKAGE / "LICENSE").read_text(encoding="utf-8").startswith("MIT License")
    row = next(entry for entry in CATALOG["packages"] if entry["id"] == "face-changer")
    assert row["version"] == MANIFEST["version"]
    assert row["manifest"] == "packages/face-changer/manifest.json"
    assert MANIFEST["install"] == {
        "bundled": True, "default": False, "sync": "account",
        "places": ["mobile", "desktop", "web"],
    }
    assert MANIFEST["dataLifecycle"]["scope"] == "none"
    assert "worker" not in MANIFEST
    assert not (PACKAGE / "content" / "worker").exists()
    assert len(MANIFEST["summary"]) <= 240
    market = (REPO / "www" / "public" / "market.html").read_text(encoding="utf-8")
    assert 'data-market-package="face-changer"' in market


def test_preview_is_an_svg_with_a_webp_for_phones():
    [shot] = MANIFEST["screenshots"]
    assert shot["path"] == "preview/market.svg" and shot["alt"].strip()
    assert (PACKAGE / "preview" / "market.svg").is_file()
    assert (PACKAGE / "preview" / "market.webp").stat().st_size > 1000


def test_permissions_say_camera_and_microphone_stay_on_the_device():
    labels = {item["id"]: item["label"] for item in MANIFEST["permissions"]}
    assert "device.camera.read" in labels and "device.microphone.read" in labels
    assert "stays on this device" in labels["device.camera.read"]
    assert "stays on this device" in labels["device.microphone.read"]
    assert all(len(label) <= 120 for label in labels.values())


def test_copy_names_no_other_apps():
    copy = " ".join([
        MANIFEST["summary"], MANIFEST["screenshots"][0]["alt"],
        json.dumps(MANIFEST["contributions"]), json.dumps(MANIFEST["permissions"]),
        VIEW, (PACKAGE / "README.md").read_text(encoding="utf-8"),
        (PACKAGE / "preview" / "market.svg").read_text(encoding="utf-8"),
    ]).lower()
    for brand in BRANDS:
        assert not re.search(r"\b%s\b" % re.escape(brand), copy), brand
    assert "any call or meeting app" in copy


def test_dashboard_page_asks_for_the_camera_and_microphone():
    [page] = MANIFEST["contributions"]
    assert page["socket"] == "dashboard.page"
    assert page["entrypoint"] == "dashboard-view"
    assert page["config"]["route"] == "/dashboard/face-changer"
    assert page["config"]["devices"] == ["camera", "microphone"]
    assert MANIFEST["entrypoints"]["dashboard-script"] == "content/web/page.js"
    assert 'registerPage("face-changer"' in PAGE
    assert 'data-view="face-changer"' in VIEW


def test_desktop_page_is_a_native_rail_page():
    assert BUILD["sources"] == [
        "FaceCamera.h", "FaceCamera.cpp", "FaceStudioPage.h", "FaceStudioPage.cpp",
        "FaceChangerFeature.h", "FaceChangerFeature.cpp",
    ]
    assert BUILD["mocHeaders"] == []
    assert BUILD["testSources"] == ["tests/test_face_camera.cpp", "FaceCamera.h", "FaceCamera.cpp"]
    assert "Q_OBJECT" not in "".join(DESKTOP_SOURCES)
    assert not (DESKTOP / "FaceChangerLink.cpp").exists()
    assert "NativePackageRegistration faceChangerRegistration" in FEATURE
    assert 'QStringLiteral("builtin:face-changer")' in FEATURE
    assert "new forkmesh::facechanger::FaceChangerFeature(host, parent)" in FEATURE
    assert MANIFEST["entrypoints"]["desktop"] == "builtin:face-changer"
    assert 'm_openWebPage(QStringLiteral("%s"))' % MANIFEST["contributions"][0]["config"]["route"] in FEATURE
    icons = (REPO / "desktop" / "resources" / "icons.qrc").read_text(encoding="utf-8")
    assert 'alias="person.svg"' in icons


def test_desktop_page_runs_the_app_engine_on_the_active_host():
    host = (REPO / "desktop" / "src" / "NativePackageExtension.h").read_text(encoding="utf-8")
    assert "std::function<QString(const QString &)> webPageUrl;" in host
    market = (REPO / "desktop" / "src" / "MainWindowMarket.cpp").read_text(encoding="utf-8")
    assert "host.webPageUrl = [this]" in market
    assert "host.webPageUrl(QStringLiteral(\"/\"))" in FEATURE
    assert "page->setHtml(faceStudioPage(), m_origin)" in FEATURE
    for module in ("/chat-call-faces.js", "/call-studio.js", "/chat-call-backgrounds.js",
                   "/chat-call-outfits.js", "/chat-call-face-custom.js"):
        assert 'import("%s")' % module in ENGINE_PAGE
    assert "faces.createFaceStage({" in ENGINE_PAGE and "manual: true" in ENGINE_PAGE
    assert "stage.processFrame(cam)" in ENGINE_PAGE
    assert "audio: false" in ENGINE_PAGE


def test_desktop_page_only_grants_the_camera_to_its_own_host():
    assert "PermissionType::MediaVideoCapture" in FEATURE
    assert "QWebEnginePage::MediaVideoCapture" in FEATURE
    assert FEATURE.count("permission.deny()") == 1
    assert "PermissionDeniedByUser" in FEATURE
    assert "createWindow(WebWindowType) override { return nullptr; }" in FEATURE
    assert "NavigationTypeLinkClicked" in FEATURE


def test_desktop_page_never_feeds_its_own_virtual_camera_back_in():
    assert "const SKIP = /forkmesh|dummy video|loopback|virtual/i;" in ENGINE_PAGE
    assert "OnlyVirtual" in ENGINE_PAGE


def test_virtual_camera_is_a_v4l2loopback_device_on_linux():
    assert "V4L2_BUF_TYPE_VIDEO_OUTPUT" in CAMERA and "V4L2_PIX_FMT_YUYV" in CAMERA
    assert "#ifdef Q_OS_LINUX" in CAMERA
    assert '"exclusive_caps=1"' in CAMERA and '"pkexec"' in CAMERA
    assert 'QStringLiteral("ForkMesh Camera")' in CAMERA
    labels = {item["id"]: item["label"] for item in MANIFEST["permissions"]}
    assert "ForkMesh Camera" in labels["device.video.virtual"]
    assert "v4l2loopback" in labels["device.process.video"]


def test_settings_live_under_the_package_data_prefix_the_host_removes():
    assert 'QStringLiteral("market/packages/%1/data/face.%2")' in FEATURE


@pytest.mark.skipif(CXX is None or not shutil.which("pkg-config"), reason="g++ or pkg-config missing")
def test_virtual_camera_frames_and_discovery(tmp_path):
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "Qt6Gui"], capture_output=True, text=True)
    if flags.returncode != 0:
        pytest.skip("Qt6Gui is not installed")
    binary = tmp_path / "test_face_camera"
    build = subprocess.run(
        [CXX, "-std=c++17", "-fPIC", "-O2", "-I", str(DESKTOP), "-o", str(binary),
         str(DESKTOP / "tests" / "test_face_camera.cpp"), str(DESKTOP / "FaceCamera.cpp"),
         *flags.stdout.split()],
        capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    env = {"PATH": "/usr/bin:/bin", "HOME": str(tmp_path), "QT_QPA_PLATFORM": "offscreen",
           "XDG_DATA_HOME": str(tmp_path), "XDG_CONFIG_HOME": str(tmp_path),
           "XDG_CACHE_HOME": str(tmp_path), "TMPDIR": str(tmp_path)}
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60, env=env)
    assert result.returncode == 0, result.stderr + result.stdout
    assert "all checks passed" in result.stdout


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_engine_page_script_parses(tmp_path):
    script = ENGINE_PAGE.split('<script type="module">', 1)[1].split("</script>", 1)[0]
    module = tmp_path / "engine.mjs"
    module.write_text(script, encoding="utf-8")
    result = subprocess.run([NODE, "--check", str(module)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_page_uses_the_call_engine_from_the_app_and_copies_none_of_it():
    for module in ("/chat-call-faces.js", "/chat-call-voice.js", "/chat-call-backgrounds.js",
                   "/chat-call-face-custom.js", "/call-studio.js", "/chat-call-outfits.js"):
        assert 'import("%s")' % module in PAGE
    assert "faces.createFaceStage({" in PAGE
    assert "mountVoicePreviewControls(" in PAGE
    assert "startStudioVoice(" in PAGE and "startStudioClip(" in PAGE
    assert not list(WEB.glob("chat-call-*.js"))
    assert "registerProcessor" not in PAGE and "getContext(\"webgl" not in PAGE


def test_page_offers_outfits_and_remembers_the_choice():
    assert 'data-fc="outfits"' in VIEW and 'aria-labelledby="fc-outfit-label"' in VIEW
    assert "outfitId: state.support.backgrounds ? state.outfitId : \"\"," in PAGE
    assert "state.stage?.setOutfit(state.outfitId);" in PAGE
    assert "outfits.rememberCallOutfit(outfit.id);" in PAGE
    assert "outfits.savedCallOutfit()" in PAGE
    assert "outfits.callOutfitThumbnail(entry[0], document)" in PAGE


def test_stage_takes_the_shape_of_the_picture():
    assert ".fc-stage.is-portrait {" in VIEW
    assert 'video.addEventListener("resize", fitStage);' in PAGE
    assert 'stageBox.classList.toggle("is-portrait", Boolean(width && height && height > width));' in PAGE


def test_raw_camera_is_never_shown():
    assert PAGE.count(".srcObject =") == 2
    assert "video.srcObject = new MediaStream([state.stage.track]);" in PAGE
    assert "video.srcObject = null;" in PAGE
    assert "srcObject = stream" not in PAGE and "srcObject = state.stream" not in PAGE
    assert "popOutStudioTrack(state.stage.track" in PAGE
    assert "startStudioClip([state.stage.track, audio]" in PAGE
    assert "studioSnapshot(state.stage.canvas)" in PAGE


def test_page_stops_everything_and_uploads_nothing():
    assert "fetch(" not in PAGE and "XMLHttpRequest" not in PAGE and "sendBeacon" not in PAGE
    assert 'addEventListener("pagehide"' in PAGE
    stop = PAGE.split("function stopCamera()", 1)[1].split("\n  }\n", 1)[0]
    for step in ("closePopout()", "state.stage?.stop()", "stopStudioStream(state.stream)",
                 "state.voice?.stop({ keepStream: true })"):
        assert step in stop


def test_sources_are_code_only():
    for text in (PAGE, STUDIO, *DESKTOP_SOURCES, CAMERA_TEST):
        assert not re.search(r"^\s*//", text, re.M)
        assert "/*" not in text


STUDIO_HARNESS = r"""
const base = process.argv[1];
const studio = await import(base + "call-studio.js");
const store = new Map();
const storage = {
  getItem: (key) => (store.has(key) ? store.get(key) : null),
  setItem: (key, value) => store.set(key, String(value)),
  removeItem: (key) => store.delete(key),
};
const out = {};
out.choices = studio.studioFaceChoices().map(([id]) => id);
out.unknown = studio.rememberStudioFace("nobody", storage);
out.maya = studio.rememberStudioFace("maya", storage);
out.saved = studio.savedStudioFace(storage);
out.customWithoutPhoto = studio.knownStudioFace("custom", storage);
store.set("forkmesh.chat.callFaceImage", "data:image/jpeg;base64,AAAA");
out.customWithPhoto = studio.knownStudioFace("custom", storage);
out.image = studio.studioFaceImage("custom", storage);
out.extensions = ["video/mp4;codecs=avc1", "video/webm;codecs=vp8,opus", "audio/mp4", "audio/ogg;codecs=opus", "audio/webm"].map(studio.studioClipExtension);
out.name = studio.studioFileName("Face Changer!", "png", new Date(2026, 0, 2, 3, 4, 5));
out.time = [0, 999, 61000, 300000].map(studio.formatStudioTime);
const stats = [];
let stageOptions = null;
const preview = await studio.startStudioVoice({
  micStream: "mic", faceId: "maya", storage,
  onStats: (value) => stats.push(["page", value.userPitchHz]),
  createVoicePreviewImpl: async (options) => {
    const stage = await options.createVoiceStageImpl({ onStats: (value) => stats.push(["preview", value.userPitchHz]) });
    return { options, stage };
  },
  createVoiceStageImpl: async (options) => {
    stageOptions = options;
    options.onStats({ userPitchHz: 131 });
    return { track: "processed" };
  },
});
out.moduleUrl = preview.options.moduleUrl.endsWith("/chat-call-voice-worklet.js");
out.faceId = preview.options.faceId;
out.stats = stats;
const recorded = [];
class FakeStream { constructor(tracks) { this.tracks = tracks; } }
studio.startStudioClip([{ readyState: "live", id: "v" }, null, { readyState: "ended" }, { readyState: "live", id: "a" }], "video", {
  MediaStreamImpl: FakeStream,
  recordImpl: (stream, kind, options) => recorded.push([stream.tracks.map((t) => t.id), kind, options.maxMs]),
});
studio.startStudioClip([{ readyState: "live", id: "a" }], "audio", {
  MediaStreamImpl: FakeStream,
  recordImpl: (stream, kind) => recorded.push([stream.tracks.map((t) => t.id), kind]),
});
out.recorded = recorded;
try {
  studio.startStudioClip([null], "video", { MediaStreamImpl: FakeStream, recordImpl: () => {} });
  out.empty = "recorded";
} catch (error) {
  out.empty = error.message;
}
out.denied = studio.studioMediaError({ name: "NotAllowedError" }, { video: true });
const bytes = new Uint8Array(studio.STUDIO_SHARE_CHUNK + 10).map((_, index) => index % 251);
const clip = new Blob([bytes], { type: "video/mp4" });
const posted = [];
const ios = { webkit: { messageHandlers: { fmShare: { postMessage: (text) => posted.push(JSON.parse(text)) } } } };
out.iosBridge = Boolean(studio.studioShareBridge(ios)) && studio.canShareStudioFiles("video/mp4", ios);
out.iosOutcome = await studio.shareStudioBlob(clip, "face-changer.mp4", { title: "Clip", globalRef: ios });
out.iosEvents = posted.map((message) => message.event);
out.iosBegin = { name: posted[0].name, mime: posted[0].mime, size: posted[0].size, title: posted[0].title };
out.iosSameId = posted.every((message) => message.id === posted[0].id);
const joined = Buffer.concat(posted.filter((m) => m.event === "chunk").map((m) => Buffer.from(m.data, "base64")));
out.iosBytes = joined.length === bytes.length && joined.equals(Buffer.from(bytes));
const androidPosted = [];
const android = { FmShareBridge: { post: (text) => androidPosted.push(JSON.parse(text).event) } };
out.androidOutcome = await studio.shareStudioBlob(new Blob(["hi"], { type: "image/png" }), "snap.png", { globalRef: android });
out.androidEvents = androidPosted;
const shared = [];
const browser = { File, navigator: { canShare: ({ files }) => files[0].type === "video/mp4", share: async (data) => shared.push([data.files[0].name, data.title]) } };
out.browserCan = [studio.canShareStudioFiles("video/mp4", browser), studio.canShareStudioFiles("video/webm", browser)];
out.browserOutcome = await studio.shareStudioBlob(clip, "face-changer.mp4", { title: "Clip", globalRef: browser });
out.browserShared = shared;
out.browserWebm = await studio.shareStudioBlob(new Blob(["x"], { type: "video/webm" }), "c.webm", { globalRef: browser });
const cancelled = { File, navigator: { share: async () => { const error = new Error("no"); error.name = "AbortError"; throw error; } } };
out.cancelled = await studio.shareStudioBlob(clip, "c.mp4", { globalRef: cancelled });
out.nothing = [studio.canShareStudioFiles("video/mp4", {}), await studio.shareStudioBlob(clip, "c.mp4", { globalRef: {} })];
console.log(JSON.stringify(out));
"""


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_shared_studio_glue_behaves():
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", STUDIO_HARNESS, PUBLIC.as_uri() + "/"],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    out = json.loads(result.stdout.strip().splitlines()[-1])
    assert out["choices"][0] == "" and out["choices"][-1] == "custom" and "maya" in out["choices"]
    assert out["unknown"] == "" and out["maya"] == "maya" and out["saved"] == "maya"
    assert out["customWithoutPhoto"] == "" and out["customWithPhoto"] == "custom"
    assert out["image"].startswith("data:image/")
    assert out["extensions"] == ["mp4", "webm", "m4a", "ogg", "webm"]
    assert out["name"] == "face-changer-2026-01-02-030405.png"
    assert out["time"] == ["0:00", "0:00", "1:01", "5:00"]
    assert out["moduleUrl"] is True and out["faceId"] == "maya"
    assert out["stats"] == [["preview", 131], ["page", 131]]
    assert out["recorded"] == [[["v", "a"], "video", 300000], [["a"], "voice"]]
    assert out["empty"] == "nothing_to_record"
    assert out["iosBridge"] is True and out["iosOutcome"] == "native"
    assert out["iosEvents"] == ["begin", "chunk", "chunk", "end"] and out["iosSameId"] is True
    assert out["iosBegin"] == {"name": "face-changer.mp4", "mime": "video/mp4", "size": 384 * 1024 + 10, "title": "Clip"}
    assert out["iosBytes"] is True
    assert out["androidOutcome"] == "native" and out["androidEvents"] == ["begin", "chunk", "end"]
    assert out["browserCan"] == [True, False]
    assert out["browserOutcome"] == "shared" and out["browserShared"] == [["face-changer.mp4", "Clip"]]
    assert out["browserWebm"] == "unsupported"
    assert out["cancelled"] == "cancelled"
    assert out["nothing"] == [False, "unsupported"]
    assert "Allow the camera and microphone" in out["denied"]


def test_a_clip_can_go_to_the_share_sheet():
    assert 'data-fc="clip-share"' in VIEW
    assert "shareStudioBlob" in PAGE and "canShareStudioFiles" in PAGE
    assert "studioShareBridge" in PAGE
    assert "fmShare" in STUDIO and "FmShareBridge" in STUDIO
