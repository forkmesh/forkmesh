import json
from pathlib import Path


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
MANIFEST = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))


def test_the_package_is_local_only():
    assert MANIFEST["id"] == "rss-reader"
    assert "worker" not in MANIFEST
    assert set(MANIFEST["entrypoints"]) == {"desktop", "settings"}
    assert MANIFEST["entrypoints"]["desktop"] == "builtin:rss-reader"
    assert not MANIFEST.get("contributions")
    assert MANIFEST["dataLifecycle"] == {"scope": "none", "localData": True}
    assert MANIFEST["install"]["places"] == ["mobile", "desktop"]
    assert not (PACKAGE / "content" / "web").exists()
    assert not (PACKAGE / "content" / "worker").exists()
    assert "public_web_text" not in (REPO / "app" / "src" / "entry.py").read_text(encoding="utf-8")


def test_the_catalog_row_matches():
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    row = next(item for item in catalog["packages"] if item["id"] == "rss-reader")
    assert row == {"id": "rss-reader", "version": MANIFEST["version"],
                   "manifest": "packages/rss-reader/manifest.json"}


def test_netnewswire_is_credited_everywhere_it_is_described():
    for path in (PACKAGE / "README.md", REPO / "docs" / "marketplace" / "rss-reader.md"):
        text = " ".join(path.read_text(encoding="utf-8").split())
        assert "NetNewsWire" in text and "Ranchero" in text and "Brent Simmons" in text, path
    assert "NetNewsWire" in MANIFEST["summary"] and "NetNewsWire" in MANIFEST["license"]["terms"]
    about = [path for path in (REPO / "native").rglob("RSSReader*") if path.is_file()]
    about += [path for path in (REPO / "native").rglob("*") if path.is_file() and "rssreader" in str(path).lower()]
    about += list((PACKAGE / "content" / "desktop").rglob("*.cpp"))
    assert any("NetNewsWire" in path.read_text(encoding="utf-8", errors="ignore") for path in about)
