"""The repository district loads on arrival and rises as its maps are built.

Nothing about the repository district — the catalog, the import list, the
flagship tree, or a single hosted size map — may be requested at boot. The
first character to stand on the repositories ground circle triggers the read,
and the portals then rise out of the ring and shimmer until their size maps
land.
"""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


def _bootstrap() -> str:
    start = APP.index("  async bootstrap() {")
    return APP[start:APP.index("\n  handleVisibility =", start)]


def _load_world_data() -> str:
    start = APP.index("  async loadWorldData({")
    return APP[start:APP.index("\n  renderCommunityPlacement(", start)]


def _load_repository_catalog() -> str:
    start = APP.index("  async loadRepositoryCatalog({")
    return APP[start:APP.index("\n  refreshOpenRepositoryPanel() {", start)]


def test_boot_requests_nothing_from_the_repository_district():
    data = _load_world_data()
    assert 'fetchJSON("/api/repositories"' not in data
    assert 'fetchJSON("/api/repository-imports"' not in data
    bootstrap = _bootstrap()
    assert "void this.autoLoadFlagshipRepositoryMap();" not in bootstrap
    assert "void this.hydrateHostedRepositorySizeMaps();" not in bootstrap
    # Import polling watches the district, so it starts with the district.
    assert "this.startRepositoryImportPolling();" not in bootstrap
    assert "this.startRepositoryImportPolling();" in _load_repository_catalog()
    # An unvisited district is honestly reported as such rather than as a
    # failed or empty catalog.
    assert 'this.repositoryCatalogState = "deferred";' in APP
    assert "Repository district asleep" in APP


def test_standing_on_the_repositories_circle_is_what_triggers_the_read():
    assert "function updateRepositoryDistrictArrival() {" in SCENE
    arrival = SCENE[
        SCENE.index("  function updateRepositoryDistrictArrival() {"):
        SCENE.index("\n  function beginRepositoryDistrictReveal() {")
    ]
    # The trigger is the district's own ground circle, and it fires once.
    assert "if (repositoryDistrictEntered) return;" in arrival
    assert "player.position.x - REPOSITORY_ISLAND_CENTER_X," in arrival
    assert "if (distance > REPOSITORY_GROUND_RADIUS) return;" in arrival
    assert "repositoryDistrictEntered = true;" in arrival
    assert "onRepositoryDistrictEnter();" in arrival
    # Sampled on the same bounded proximity pass as every other landmark.
    assert "function nearestLandmark() {\n    updateRepositoryDistrictArrival();" in SCENE
    assert "onRepositoryDistrictEnter = () => {}," in SCENE
    assert (
        "onRepositoryDistrictEnter: () =>\n"
        "          void this.loadRepositoryCatalog({ reason: \"arrival\" }),"
        in APP
    )
    # Opening the repositories panel is the same request by another door.
    assert 'void this.loadRepositoryCatalog({ reason: "panel" });' in APP


def test_the_district_read_runs_once_and_shares_its_in_flight_promise():
    district = _load_repository_catalog()
    assert "if (this.repositoryCatalogRequest) return this.repositoryCatalogRequest;" in district
    assert "if (this.repositoryDistrictVisited) return this.repositories.length > 0;" in district
    assert "this.repositoryDistrictVisited = true;" in district
    # The boot path already reconciled an empty catalog, so the cached alias
    # signature has to be dropped for the real records to reach the scene.
    assert "this.repositoryAliasSignature = null;" in district
    assert "this.syncRepositoryScene();" in district
    assert "this.repositoryCatalogRequest = null;" in district


def test_portals_rise_and_sparkle_until_their_size_maps_land():
    assert "function beginRepositoryDistrictReveal() {" in SCENE
    assert "repositoryRevealPending = true;" in SCENE
    assert "beginRepositoryDistrictReveal," in SCENE
    # Every portal in the arriving district rises, not just fresh imports.
    assert "const risesOnArrival =" in SCENE
    assert "(repositoryRevealPending ||" in SCENE
    assert "risingBefore * (repositoryRevealPending ? 90 : 320);" in SCENE
    assert "repositorySparkleUntil.set(" in SCENE
    # The size tree landing is what ends the shimmer.
    assert "if (record.sizeTree) repositorySparkleUntil.delete(record.key);" in SCENE
    assert "!reducedMotion && !record.sizeTree && performance.now() < sparkleUntil" in SCENE
    assert "function makeRepositorySizeMapSparkle(THREE, radius, label) {" in SCENE
    assert "const REPOSITORY_SPARKLE_MS = 11_000;" in SCENE
    assert "sparkle.userData.repositorySparkleSeeds = seeds;" in SCENE
    # A map that never arrives fades out instead of decorating a dead mirror.
    animation = SCENE[SCENE.index("const sparkle = group.userData.repositorySparkle;"):]
    animation = animation[:animation.index("positions.needsUpdate = true;")]
    assert "const remaining = until - time;" in animation
    assert "sparkle.parent?.remove(sparkle);" in animation
    assert "repositorySparkleUntil.delete(key);" in animation
    assert "clamp(remaining / 1000, 0, 1)" in animation
    # The reveal is a one-time event; later refreshes are ordinary updates.
    assert "repositoryRevealPending = false;" in SCENE


def test_an_open_repository_panel_fills_in_when_the_deferred_catalog_lands():
    panel = APP[
        APP.index("  refreshOpenRepositoryPanel() {"):
        APP.index("\n  // Keep working toward the default open portal after entry.")
    ]
    assert 'detail?.dataset.openLandmark !== "repositories"' in panel
    assert "panel.outerHTML = this.repositoryPanelHTML();" in panel
    assert "this.refreshOpenRepositoryPanel();" in _load_repository_catalog()
