import json
from pathlib import Path
import re


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
DOC = REPO / "docs" / "marketplace" / "phone-security.md"
IOS = REPO / "native" / "ios"
IOS_TOOLS = IOS / "ForkMeshCore" / "Sources" / "FMTools"
ANDROID = REPO / "native" / "android"
ANDROID_CORE = ANDROID / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "phonesecurity"
ROW = re.compile(r"^\| `((?:ios|android)\.[a-z0-9.\-]+)` \| ([^|]+?) \| ([^|]+?) \| [^|]+ \|$", re.M)


def _manifest():
    return json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))


def _spec(platform):
    return {match[0]: (match[1].replace("`", ""), match[2]) for match in ROW.findall(DOC.read_text(encoding="utf-8"))
            if match[0].startswith(platform + ".")}


def _read(paths):
    return "\n".join(path.read_text(encoding="utf-8") for path in paths)


def _ios_sources():
    return _read(sorted(IOS_TOOLS.glob("PhoneSecurity*.swift")))


def _android_sources():
    return _read(sorted(ANDROID_CORE.glob("*.kt")))


def _ios_ids():
    source = _ios_sources()
    ids = set(re.findall(r'id: "(ios\.[a-z0-9.\-]+)"', source))
    enum = re.search(r"public enum PhonePermission: String, CaseIterable, Sendable \{(.*?)\n\}", source, re.S)
    ids |= {"ios.permission." + case for case in re.findall(r"case (\w+)", enum.group(1))}
    return ids


def test_is_a_phone_package_with_no_worker_or_web_page():
    manifest = _manifest()
    assert manifest["id"] == "phone-security"
    assert manifest["name"] == "Phone Security"
    assert manifest["entrypoints"] == {
        "ios": "builtin:phone-security", "android": "builtin:phone-security", "settings": "builtin:package"}
    assert manifest["install"] == {"bundled": True, "default": False, "sync": "account", "places": ["mobile"]}
    assert manifest["dataLifecycle"] == {"scope": "none", "localData": False}
    assert not (PACKAGE / "content").exists()
    assert (PACKAGE / manifest["screenshots"][0]["path"]).is_file()


def test_asks_only_to_read():
    manifest = _manifest()
    ids = {permission["id"] for permission in manifest["permissions"]}
    assert ids == {"device.security.read", "device.integrity.read", "device.apps.read"}
    assert not any(permission["id"].endswith(".fix") or permission["risk"] == "high" for permission in manifest["permissions"])


def test_is_in_the_catalog_at_its_version():
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    rows = [row for row in catalog["packages"] if row["id"] == "phone-security"]
    assert rows == [{"id": "phone-security", "version": _manifest()["version"],
                     "manifest": "packages/phone-security/manifest.json"}]


def test_the_desktop_package_stays_desktop_only():
    desktop = json.loads((REPO / "marketplace" / "packages" / "computer-security" / "manifest.json").read_text(encoding="utf-8"))
    assert set(desktop["entrypoints"]) == {"desktop", "settings"}


def test_the_spec_lists_both_platforms():
    assert len(_spec("ios")) >= 25
    assert len(_spec("android")) >= 35


def test_the_iphone_implements_exactly_the_spec():
    assert _ios_ids() == set(_spec("ios"))


def test_every_iphone_title_is_the_specs():
    source = _ios_sources()
    for check_id, (title, _category) in _spec("ios").items():
        assert f'"{title}"' in source, check_id


def test_every_iphone_check_is_explained():
    source = (IOS_TOOLS / "PhoneSecurityExplanations.swift").read_text(encoding="utf-8")
    for check_id in _spec("ios"):
        assert f'"{check_id}":' in source, check_id


def test_the_iphone_categories_are_the_specs():
    models = (IOS_TOOLS / "PhoneSecurityModels.swift").read_text(encoding="utf-8")
    for check_id, (_title, category) in _spec("ios").items():
        assert f'return "{category}"' in models, check_id


def test_the_android_app_implements_exactly_the_spec():
    literal = set(re.findall(r'"(android\.[a-z][a-z0-9.\-]*)"', _android_sources()))
    assert literal == set(_spec("android"))


def test_every_android_check_is_explained():
    source = (ANDROID_CORE / "PhoneSecurityExplanations.kt").read_text(encoding="utf-8")
    for check_id in _spec("android"):
        assert f'"{check_id}"' in source, check_id


def test_every_android_title_is_the_specs():
    source = _android_sources()
    for check_id, (title, _category) in _spec("android").items():
        assert f'"{title}"' in source, check_id


def test_both_phones_route_the_package_to_its_screen():
    models = (IOS / "ForkMeshCore" / "Sources" / "FMPackages" / "MarketModels.swift").read_text(encoding="utf-8")
    gate = (IOS / "ForkMeshCore" / "Sources" / "FMApi" / "Navigation" / "PackageGate.swift").read_text(encoding="utf-8")
    fallbacks = (IOS / "ForkMesh" / "Navigation" / "RouteFallbacks.swift").read_text(encoding="utf-8")
    parser = (IOS / "ForkMeshCore" / "Sources" / "FMApi" / "Navigation" / "RouteParser.swift").read_text(encoding="utf-8")
    assert '"phone-security": "phoneSecurity"' in models
    assert '.phoneSecurity: "phone-security"' in gate
    assert "fallback(routes, .phoneSecurity) { _ in AnyView(PhoneSecurityScreen()) }" in fallbacks
    assert 'case "phone-security": return [.phoneSecurity]' in parser
    kotlin = ANDROID / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp"
    android_gate = (kotlin / "market" / "PackageGate.kt").read_text(encoding="utf-8")
    android_parser = (kotlin / "navigation" / "RouteParser.kt").read_text(encoding="utf-8")
    assert '"phone-security"' in android_gate
    assert '"phone-security" -> listOf(Route.PhoneSecurity)' in android_parser


def test_the_probes_never_delete_or_upload():
    probes = [IOS / "ForkMesh" / "Platform" / "PhoneSecurityProbe.swift"]
    probes += sorted((ANDROID / "app" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "feature" / "phonesecurity").glob("*.kt"))
    text = _read(probes)
    for forbidden in ("removeItem", "deleteRecursively", "URLSession", "HttpURLConnection", "OkHttp", "Runtime.getRuntime().exec",
                      "ProcessBuilder"):
        assert forbidden not in text, forbidden
