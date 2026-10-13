import json
from pathlib import Path
import re


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
IOS = REPO / "native" / "ios"


def _manifest():
    return json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))


def test_is_an_iphone_section_not_an_agent():
    manifest = _manifest()
    assert manifest["entrypoints"] == {"ios": "builtin:food-guide", "settings": "builtin:package"}
    assert "contributions" not in manifest
    assert not (PACKAGE / "content").exists()
    assert manifest["dataLifecycle"]["scope"] == "none"
    assert {p["id"] for p in manifest["permissions"]} == {"health.nutrition.read", "account.calories.read"}


def test_the_phone_routes_the_package_to_its_screen():
    models = (IOS / "ForkMeshCore" / "Sources" / "FMPackages" / "MarketModels.swift").read_text(encoding="utf-8")
    gate = (IOS / "ForkMeshCore" / "Sources" / "FMApi" / "Navigation" / "PackageGate.swift").read_text(encoding="utf-8")
    fallbacks = (IOS / "ForkMesh" / "Navigation" / "RouteFallbacks.swift").read_text(encoding="utf-8")
    assert '"food-guide": "foodGuide"' in models
    assert '.foodGuide: "food-guide"' in gate
    assert "fallback(routes, .foodGuide) { _ in AnyView(FoodGuideScreen()) }" in fallbacks


def test_apple_health_reading_is_declared():
    plist = (IOS / "Config" / "ForkMesh-Info.plist").read_text(encoding="utf-8")
    guide = (IOS / "ForkMeshCore" / "Sources" / "FMTools" / "AppleHealthGuide.swift").read_text(encoding="utf-8")
    assert "so Food Guide can suggest meals" in plist
    assert 'tool: .foodGuide, name: "Food Guide"' in guide


def test_recipes_link_to_the_web_and_use_known_groceries():
    catalog = (IOS / "ForkMeshCore" / "Sources" / "FMTools" / "FoodCatalog.swift").read_text(encoding="utf-8")
    products = set(re.findall(r'FoodProduct\("([A-Za-z]+)"', catalog))
    used = set(re.findall(r'FoodPortion\("([A-Za-z]+)"', catalog))
    urls = re.findall(r'url: "([^"]+)"', catalog)
    assert used and used <= products
    assert len(urls) >= 18 and len(set(urls)) == len(urls)
    assert all(url.startswith("https://") for url in urls)


def test_listed_in_the_catalog_and_off_by_default():
    catalog = json.loads((REPO / "marketplace" / "catalog.json")
                         .read_text(encoding="utf-8"))
    entry = next(row for row in catalog["packages"] if row["id"] == "food-guide")
    manifest = _manifest()
    assert entry["version"] == manifest["version"]
    assert manifest["install"]["default"] is False
    assert (PACKAGE / "preview" / "market.webp").is_file()
