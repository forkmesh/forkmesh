import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DESKTOP_STATS = ROOT / "desktop/src/MainWindowStats.cpp"
DESKTOP_CONTROL = ROOT / "desktop/src/MainWindowLocalControlStats.cpp"
KOTLIN = ROOT / "native/android/core/src/main/kotlin/com/forkmesh/nativeapp/insights"
SWIFT = ROOT / "native/ios/ForkMeshCore/Sources/FMInsights"
ANDROID_CODEPOINTS = ROOT / "native/android/app/src/main/assets/fonts/MaterialIcons.codepoints"
HOMEPAGE = ROOT / "www/public/homepage-downloads.js"

LITERAL = r'"((?:[^"\\]|\\.)*)"'


def desktop_catalog():
    text = DESKTOP_STATS.read_text(encoding="utf-8")
    body = text[text.index("MainWindow::statsTileCatalog() const"):text.index("MainWindow::defaultStatsLayout() const")]
    groups = dict(re.findall(r'const QString (\w+) = QStringLiteral\("([^"]+)"\);', body))
    entries = []
    pattern = r'\{QStringLiteral\("([^"]+)"\),\s*QStringLiteral\("([^"]+)"\),\s*(\w+),\s*QStringLiteral\(((?:\s*"(?:[^"\\]|\\.)*")+)\s*\)\}'
    for match in re.finditer(pattern, body):
        hint = "".join(re.findall(LITERAL, match.group(4)))
        entries.append((match.group(1), match.group(2), groups[match.group(3)], hint))
    return entries


def kotlin_catalog():
    text = (KOTLIN / "DesktopTiles.kt").read_text(encoding="utf-8")
    return [tuple(row) for row in re.findall(r'DesktopTileEntry\(%s, %s, %s, %s\)' % ((LITERAL,) * 4), text)]


def swift_catalog():
    text = (SWIFT / "DesktopTiles.swift").read_text(encoding="utf-8")
    pattern = r'DesktopTileEntry\(id: %s, title: %s, group: %s,\s*hint: %s\)' % ((LITERAL,) * 4)
    return [tuple(row) for row in re.findall(pattern, text)]


def test_desktop_catalog_is_read():
    entries = desktop_catalog()
    assert len(entries) >= 100
    assert len({entry[0] for entry in entries}) == len(entries)
    body = DESKTOP_STATS.read_text(encoding="utf-8")
    body = body[body.index("MainWindow::statsTileCatalog() const"):body.index("MainWindow::defaultStatsLayout() const")]
    assert len(re.findall(r'\{QStringLiteral\("[a-z0-9-]+"\),', body)) == len(entries)


def test_android_offers_every_desktop_stat():
    assert kotlin_catalog() == desktop_catalog()


def test_ios_offers_every_desktop_stat():
    assert swift_catalog() == desktop_catalog()


def test_the_desktop_answers_the_phones_tile_request():
    assert 'method == QLatin1String("stats.tiles")' in DESKTOP_CONTROL.read_text(encoding="utf-8")
    assert 'TILES = "stats.tiles"' in (KOTLIN / "DesktopStats.kt").read_text(encoding="utf-8")
    assert 'tiles = "stats.tiles"' in (SWIFT / "DesktopStats.swift").read_text(encoding="utf-8")


def test_every_android_stats_icon_is_in_the_font():
    names = {line.split()[0].removesuffix("_baseline") for line in ANDROID_CODEPOINTS.read_text(encoding="utf-8").splitlines() if line.strip()}
    desktop = (KOTLIN / "DesktopTiles.kt").read_text(encoding="utf-8")
    icons = dict(re.findall(r'^\s+"([^"]+)" to "([^"]+)",$', desktop, re.M))
    assert set(icons) == {entry[2] for entry in desktop_catalog()}
    releases = (KOTLIN / "LiveReleases.kt").read_text(encoding="utf-8")
    release_icons = re.findall(r'StatsTileCatalogEntry\([^)]*, "Releases", "[^"]*", "([^"]+)"\)', releases)
    assert len(release_icons) == 6
    assert sorted(icon for icon in list(icons.values()) + release_icons if icon not in names) == []


def test_release_tiles_read_the_same_builds_and_key_as_the_homepage():
    homepage = HOMEPAGE.read_text(encoding="utf-8")
    key = re.search(r'const publicKey =\s*"([^"]+)"', homepage).group(1)
    assets = set(re.findall(r'\["(forkmesh-[^"]+)", \["', homepage))
    for source in (KOTLIN / "LiveReleases.kt", SWIFT / "LiveReleases.swift"):
        text = source.read_text(encoding="utf-8")
        assert key in text, source
        assert set(re.findall(r'"(forkmesh-[a-z0-9_.-]+\.(?:apk|dmg|exe|AppImage))"', text)) == assets, source
