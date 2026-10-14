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
LINK = (DESKTOP / "FaceChangerLink.cpp").read_text(encoding="utf-8")
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
        "places": ["desktop", "web"],
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


def test_desktop_rail_link_opens_the_page():
    assert BUILD["sources"] == ["FaceChangerLink.cpp"] and BUILD["mocHeaders"] == []
    assert 'QStringLiteral("builtin:face-changer")' in LINK
    assert 'QStringLiteral("%s")' % MANIFEST["contributions"][0]["config"]["route"] in LINK
    assert MANIFEST["entrypoints"]["desktop"] == "builtin:face-changer"
    icons = (REPO / "desktop" / "resources" / "icons.qrc").read_text(encoding="utf-8")
    assert 'alias="person.svg"' in icons


def test_page_uses_the_call_engine_from_the_app_and_copies_none_of_it():
    for module in ("/chat-call-faces.js", "/chat-call-voice.js", "/chat-call-backgrounds.js",
                   "/chat-call-face-custom.js", "/call-studio.js"):
        assert 'import("%s")' % module in PAGE
    assert "faces.createFaceStage({" in PAGE
    assert "mountVoicePreviewControls(" in PAGE
    assert "startStudioVoice(" in PAGE and "startStudioClip(" in PAGE
    assert not list(WEB.glob("chat-call-*.js"))
    assert "registerProcessor" not in PAGE and "getContext(\"webgl" not in PAGE


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
    for text in (PAGE, STUDIO, LINK):
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
    assert "Allow the camera and microphone" in out["denied"]
