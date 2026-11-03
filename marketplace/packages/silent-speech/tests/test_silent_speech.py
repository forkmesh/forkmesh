import json
from pathlib import Path


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
IOS = REPO / "native" / "ios"
ANDROID = REPO / "native" / "android"
CORE = IOS / "ForkMeshCore" / "Sources"
KOTLIN = ANDROID / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp"
APP = ANDROID / "app" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp"


def _manifest():
    return json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))


def _read(path):
    return Path(path).read_text(encoding="utf-8")


def test_is_a_phone_section_off_by_default():
    manifest = _manifest()
    assert manifest["id"] == "silent-speech"
    assert manifest["entrypoints"] == {"android": "builtin:silent-speech", "ios": "builtin:silent-speech", "settings": "builtin:package"}
    assert manifest["install"] == {"bundled": True, "default": False, "sync": "account", "places": ["mobile"]}
    assert manifest["dataLifecycle"] == {"scope": "none", "localData": False}
    assert {p["id"] for p in manifest["permissions"]} == {"device.motion.read", "device.speech"}
    assert len(manifest["summary"]) <= 240
    assert not (PACKAGE / "content").exists()
    assert (PACKAGE / manifest["screenshots"][0]["path"]).is_file()


def test_listed_in_the_catalog():
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    entry = next(p for p in catalog["packages"] if p["id"] == "silent-speech")
    assert entry == {"id": "silent-speech", "version": _manifest()["version"], "manifest": "packages/silent-speech/manifest.json"}


def test_the_iphone_routes_the_package_to_its_screen():
    assert '"silent-speech": "silentSpeech"' in _read(CORE / "FMPackages" / "MarketModels.swift")
    assert '.silentSpeech: "silent-speech"' in _read(CORE / "FMApi" / "Navigation" / "PackageGate.swift")
    assert 'case "silent-speech": return [.silentSpeech]' in _read(CORE / "FMApi" / "Navigation" / "RouteParser.swift")
    assert "fallback(routes, .silentSpeech) { _ in AnyView(SilentSpeechScreen()) }" in _read(IOS / "ForkMesh" / "Navigation" / "RouteFallbacks.swift")


def test_android_routes_the_package_to_its_screen():
    assert '"silent-speech" to "silentSpeech"' in _read(KOTLIN / "market" / "MarketModels.kt")
    assert 'put("silent-speech", Route.Kind.SilentSpeech)' in _read(KOTLIN / "market" / "PackageGate.kt")
    assert '"silent-speech" -> listOf(Route.SilentSpeech)' in _read(KOTLIN / "navigation" / "RouteParser.kt")
    assert "SilentSpeechFeature.install(app)" in _read(APP / "feature" / "Features.kt")
    assert "NavigationFeature.register(Route.Kind.SilentSpeech" in _read(APP / "feature" / "silentspeech" / "SilentSpeechFeature.kt")


def test_reads_headphone_motion_and_says_why():
    ios = _read(IOS / "ForkMesh" / "Platform" / "SilentSpeechListener.swift")
    assert "CMHeadphoneMotionManager" in ios and "startDeviceMotionUpdates" in ios
    assert "Silent Speech reads how your head moves through your AirPods" in _read(IOS / "Config" / "ForkMesh-Info.plist")
    android = _read(APP / "feature" / "silentspeech" / "SilentSpeechListener.kt")
    assert "Sensor.TYPE_HEAD_TRACKER" in android and "registerDynamicSensorCallback" in android


def test_core_has_no_platform_imports_and_matches_on_both_phones():
    swift = _read(CORE / "FMTools" / "SilentSpeechModels.swift")
    imports = {line.split()[1] for line in swift.splitlines() if line.startswith("import ")}
    assert imports == {"Foundation"}
    kotlin = _read(KOTLIN / "silentspeech" / "SilentSpeech.kt")
    assert "import android" not in kotlin
    for name in ("SilentDTW", "SilentEvaluator", "SilentSegmenter", "SilentNoiseFloor", "SilentDataset", "SilentRandom"):
        assert name in swift and name in kotlin, name
    for constant in ("0.25", "0.12", "500", "0x5EED", "1.4826", "0.08"):
        assert constant in swift and constant in kotlin, constant
