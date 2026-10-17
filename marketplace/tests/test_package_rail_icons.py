import re
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
PACKAGES = REPO / "marketplace" / "packages"
QRC = REPO / "desktop" / "resources" / "icons.qrc"
REGISTRATION = re.compile(
    r'NativePackageRegistration\s+\w+\(\{\s*QStringLiteral\("(builtin:[^"]+)"\),\s*'
    r'\{[^}]*\},\s*QStringLiteral\("([^"]+)"\),\s*QStringLiteral\("([^"]+)"\)',
    re.S,
)


def registrations():
    found = {}
    for source in sorted(PACKAGES.glob("*/content/desktop/*.cpp")):
        for entrypoint, label, icon in REGISTRATION.findall(source.read_text(encoding="utf-8")):
            found[entrypoint] = (label, icon)
    return found


def bundled_icons():
    text = QRC.read_text(encoding="utf-8")
    names = set()
    for prefix, body in re.findall(r'<qresource prefix="([^"]+)">(.*?)</qresource>', text, re.S):
        if prefix in ("/icons/octicons", "/icons/codicons"):
            names.update(re.findall(r'alias="([^"]+)\.svg"', body))
    return names


def test_every_native_package_rail_icon_is_bundled():
    found = registrations()
    assert len(found) >= 10
    icons = bundled_icons()
    missing = {entrypoint: icon for entrypoint, (_, icon) in found.items() if icon not in icons}
    assert missing == {}


def test_security_package_label_and_icon():
    assert registrations()["builtin:computer-security"] == ("Security", "shield")
    assert (REPO / "desktop" / "resources" / "icons" / "forkmesh" / "shield.svg").is_file()
