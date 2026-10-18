import json
from pathlib import Path


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
IOS = REPO / "native" / "ios"
CORE = IOS / "ForkMeshCore" / "Sources"


def _manifest():
    return json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))


def _read(path):
    return (IOS / path).read_text(encoding="utf-8")


def test_is_an_iphone_section_off_by_default():
    manifest = _manifest()
    assert manifest["id"] == "eye-control"
    assert manifest["entrypoints"] == {"ios": "builtin:eye-control", "settings": "builtin:package"}
    assert manifest["install"] == {"bundled": True, "default": False, "sync": "account", "places": ["mobile"]}
    assert manifest["dataLifecycle"] == {"scope": "none", "localData": False}
    assert {p["id"] for p in manifest["permissions"]} == {"device.camera.read", "app.input.control"}
    assert not (PACKAGE / "content").exists()
    assert (PACKAGE / manifest["screenshots"][0]["path"]).is_file()


def test_listed_in_the_catalog():
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    entry = next(p for p in catalog["packages"] if p["id"] == "eye-control")
    assert entry == {"id": "eye-control", "version": _manifest()["version"], "manifest": "packages/eye-control/manifest.json"}


def test_the_phone_routes_the_package_to_its_screen():
    assert '"eye-control": "eyeControl"' in _read("ForkMeshCore/Sources/FMPackages/MarketModels.swift")
    assert '.eyeControl: "eye-control"' in _read("ForkMeshCore/Sources/FMApi/Navigation/PackageGate.swift")
    assert 'case "eye-control": return [.eyeControl]' in _read("ForkMeshCore/Sources/FMApi/Navigation/RouteParser.swift")
    assert "fallback(routes, .eyeControl) { _ in AnyView(EyeControlScreen()) }" in _read("ForkMesh/Navigation/RouteFallbacks.swift")


def test_uses_arkit_face_tracking_and_says_why_it_needs_the_camera():
    tracker = _read("ForkMesh/Platform/EyeTracker.swift")
    assert "ARFaceTrackingConfiguration" in tracker
    assert ".eyeBlinkLeft" in tracker and ".eyeBlinkRight" in tracker
    assert "follow where you look when you turn on Eye Control" in _read("Config/ForkMesh-Info.plist")


def test_core_math_has_no_apple_only_imports():
    source = (CORE / "FMTools" / "EyeControlModels.swift").read_text(encoding="utf-8")
    imports = {line.split()[1] for line in source.splitlines() if line.startswith("import ")}
    assert imports == {"Foundation"}
