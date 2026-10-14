import json
import shutil
import subprocess
from pathlib import Path

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
FIXTURE = PACKAGE / "tests" / "fixtures" / "rings.json"
PAGE = PACKAGE / "content" / "web" / "page.js"
IOS = REPO / "native" / "ios"
ANDROID = REPO / "native" / "android"

RUN_PAGE = r"""
const fs = require("fs");
const vm = require("vm");
const [page, fixturePath] = process.argv.slice(1);
const window = {};
vm.runInNewContext(fs.readFileSync(page, "utf8"), { window });
const R = window.ForkMeshPeopleRings;
const fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
const cases = fixture.cases.map((item) => {
  const member = R.parseMember(item.user);
  return member ? { score: R.score(member), ring: R.ring(member, fixture.now), contributions: member.contributions } : null;
});
const members = fixture.cases.map((item) => R.parseMember(item.user)).filter(Boolean);
const order = R.sections(members, fixture.now).map((s) => ({ ring: s.ring, names: s.people.map((p) => p.member.name) }));
const layout = fixture.layout.map((l) => ({
  radii: [0, 1, 2, 3, 4].map((i) => +R.radius(i, l.size).toFixed(6)),
  capacities: [0, 1, 2, 3, 4].map((i) => R.capacity(i, l.size, l.seat, l.label)),
}));
process.stdout.write(JSON.stringify({ rings: R.RINGS.map((r) => r.id), cases, order, layout }));
"""


def _manifest():
    return json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))


def test_ships_on_every_surface():
    manifest = _manifest()
    entry = manifest["entrypoints"]
    assert entry["desktop"] == entry["ios"] == entry["android"] == "builtin:people"
    assert entry["dashboard-script"] == "content/web/page.js"
    page = manifest["contributions"][0]["config"]
    assert page["route"] == "/dashboard/people"
    assert manifest["install"] == {"bundled": True, "default": False, "sync": "account"}
    assert manifest["dataLifecycle"]["scope"] == "none"
    assert len(manifest["summary"]) <= 240


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_web_rings_match_the_shared_fixture():
    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    result = json.loads(subprocess.run(
        ["node", "-e", RUN_PAGE, str(PAGE), str(FIXTURE)],
        check=True, capture_output=True, text=True, timeout=60).stdout)
    assert result["rings"] == fixture["rings"] == ["blazing", "glowing", "kindling", "warm", "embers"]
    for got, item in zip(result["cases"], fixture["cases"]):
        if item["member"] is None:
            assert got is None
            continue
        assert got == {"score": item["score"], "ring": item["ring"], "contributions": item["member"]["contributions"]}
    assert result["order"] == fixture["order"]
    for got, want in zip(result["layout"], fixture["layout"]):
        assert got["radii"] == want["radii"]
        assert got["capacities"] == want["capacities"]


def test_every_platform_reads_the_same_fixture():
    relative = "marketplace/packages/people/tests/fixtures/rings.json"
    swift = (IOS / "ForkMeshCore" / "Tests" / "FMAccountTests" / "PeopleTests.swift").read_text(encoding="utf-8")
    kotlin = (ANDROID / "core" / "src" / "test" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "settings" / "PeopleTest.kt").read_text(encoding="utf-8")
    cpp = (PACKAGE / "content" / "desktop" / "tests" / "test_people_model.cpp").read_text(encoding="utf-8")
    for source in (swift, kotlin, cpp):
        assert relative in source


def test_the_phones_route_the_package_to_its_screen():
    models = (IOS / "ForkMeshCore" / "Sources" / "FMPackages" / "MarketModels.swift").read_text(encoding="utf-8")
    gate = (IOS / "ForkMeshCore" / "Sources" / "FMApi" / "Navigation" / "PackageGate.swift").read_text(encoding="utf-8")
    fallbacks = (IOS / "ForkMesh" / "Navigation" / "RouteFallbacks.swift").read_text(encoding="utf-8")
    assert '"people": "people"' in models
    assert '.people: "people"' in gate
    assert "fallback(routes, .people) { _ in AnyView(PeopleScreen()) }" in fallbacks
    kotlin_gate = (ANDROID / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "market" / "PackageGate.kt").read_text(encoding="utf-8")
    features = (ANDROID / "app" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "feature" / "Features.kt").read_text(encoding="utf-8")
    assert 'put("people", Route.Kind.People)' in kotlin_gate
    assert "feature.people.PeopleFeature.install()" in features


def test_the_desktop_page_stays_inside_its_api_boundary():
    feature = (PACKAGE / "content" / "desktop" / "PeopleFeature.cpp").read_text(encoding="utf-8")
    model = (PACKAGE / "content" / "desktop" / "PeopleModel.cpp").read_text(encoding="utf-8")
    assert 'QStringLiteral("builtin:people")' in feature
    assert 'QStringLiteral("/api/accounts")' in feature
    paths = {line.strip() for line in model.splitlines() if 'QStringLiteral("/api/' in line}
    assert all("/api/accounts" in line for line in paths)
