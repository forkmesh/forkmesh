import json
import re
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
PACKAGES = REPO / "marketplace" / "packages"
MARKET = REPO / "desktop" / "src" / "MainWindowMarket.cpp"
SERVICES = REPO / "desktop" / "src" / "MainWindowCorePackageServices.cpp"
REGISTRATION = re.compile(
    r'NativePackageRegistration\s+\w+\(\{\s*QStringLiteral\("(builtin:[^"]+)"\),\s*'
    r'\{[^}]*\},\s*QStringLiteral\("[^"]+"\),\s*QStringLiteral\("[^"]+"\),\s*'
    r'(QString\(\)|QStringLiteral\("([^"]*)"\)),\s*(QString\(\)|QStringLiteral\("([^"]*)"\))',
    re.S,
)
HANDLED_IN_CORE = {"status-page", "world"}


def manifests():
    for path in sorted(PACKAGES.glob("*/manifest.json")):
        yield path.parent, json.loads(path.read_text(encoding="utf-8"))


def package_sources(root):
    folder = root / "content" / "desktop"
    return "".join(path.read_text(encoding="utf-8") for path in sorted(folder.glob("*.cpp")))


def core_linked(root):
    build = root / "content" / "desktop" / "build.json"
    return build.exists() and json.loads(build.read_text(encoding="utf-8")).get("coreLinked") is True


def test_every_desktop_package_has_a_desktop_surface():
    gated = set(re.findall(r'on\(QStringLiteral\("([a-z0-9-]+)"\)\)', MARKET.read_text(encoding="utf-8")))
    services = set(re.findall(r'QStringLiteral\("([a-z0-9-]+)"\)', SERVICES.read_text(encoding="utf-8")))
    missing = []
    for root, manifest in manifests():
        entrypoint = manifest.get("entrypoints", {}).get("desktop")
        if not entrypoint or manifest.get("type") == "agent":
            continue
        sources = package_sources(root)
        if (f'"{entrypoint}"' in sources or "DesktopPackageExtensionRegistration" in sources
                or core_linked(root) or manifest["id"] in gated or manifest["id"] in services
                or manifest["id"] in HANDLED_IN_CORE):
            continue
        missing.append(manifest["id"])
    assert missing == [], (
        "these packages list a desktop entrypoint but nothing on the desktop shows or gates them: "
        "register a page in content/desktop or gate the core surface in packageSurfaceShown")


def test_bugs_is_a_desktop_rail_page_and_a_web_page():
    root = PACKAGES / "bug-reports"
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    assert '"builtin:bug-reports"' in package_sources(root)
    routes = [item["config"]["route"] for item in manifest.get("contributions", [])
              if item.get("socket") == "dashboard.page"]
    assert routes == ["/dashboard/bugs"]
    assert {"desktop", "ios", "android", "web"} <= set(manifest["entrypoints"])


def test_package_api_prefixes_declare_a_request_proof():
    unproven = []
    seen = 0
    for source in sorted(PACKAGES.glob("*/content/desktop/*.cpp")):
        for entrypoint, prefix, prefix_value, proof, proof_value in REGISTRATION.findall(
                source.read_text(encoding="utf-8")):
            seen += 1
            if prefix_value and not proof_value:
                unproven.append(entrypoint)
    assert seen >= 10
    assert unproven == [], "an API prefix without a request proof is refused by the desktop"
