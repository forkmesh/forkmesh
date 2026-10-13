import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CENSUS_SWIFT = ROOT / "native/ios/ForkMeshCore/Sources/FMTools/GlassesCensus.swift"
CENSUS_KOTLIN = ROOT / "native/android/core/src/main/kotlin/com/forkmesh/nativeapp/glasses/GlassesCensus.kt"
IOS_TABLE = ROOT / "native/ios/ForkMesh/Theme/MaterialIconCodepoints.swift"
ANDROID_CODEPOINTS = ROOT / "native/android/app/src/main/assets/fonts/MaterialIcons.codepoints"


def swift_kinds():
    text = CENSUS_SWIFT.read_text(encoding="utf-8")
    icons = text[text.index("public var icon: String {"):text.index("public struct NearbyDevice")]
    labels = text[text.index("public var label: String {"):text.index("public var icon: String {")]
    return (
        dict(re.findall(r'case \.(\w+): return "([^"]+)"', labels)),
        dict(re.findall(r'case \.(\w+): return "([^"]+)"', icons)),
    )


def kotlin_kinds():
    text = CENSUS_KOTLIN.read_text(encoding="utf-8")
    block = text[text.index("enum class NearbyKind"):text.index("data class NearbyDevice")]
    return re.findall(r'^\s+(\w+)\("([^"]+)", "([^"]+)"\),$', block, re.M)


def test_every_device_kind_icon_is_drawable_on_ios():
    table = set(re.findall(r'^\s+"([^"]+)": 0x', IOS_TABLE.read_text(encoding="utf-8"), re.M))
    _, icons = swift_kinds()
    assert icons
    assert sorted(icon for icon in icons.values() if icon not in table) == []


def test_every_device_kind_icon_is_in_the_android_font():
    names = {line.split()[0].removesuffix("_baseline") for line in ANDROID_CODEPOINTS.read_text(encoding="utf-8").splitlines() if line.strip()}
    kinds = kotlin_kinds()
    assert kinds
    assert sorted(icon for _, _, icon in kinds if icon not in names) == []


def test_ios_and_android_share_the_device_kinds():
    labels, icons = swift_kinds()
    swift = [(labels[case], icons[case]) for case in icons]
    kotlin = [(label, icon) for _, label, icon in kotlin_kinds()]
    assert swift == kotlin
