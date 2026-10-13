import hashlib
from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
WORLD = PUBLIC / "world"
BRAND_MARK = ROOT.parent / "www" / "public" / "assets" / "logo.svg"
INDEX = (WORLD / "index.html").read_text(encoding="utf-8")
APP = (WORLD / "world.js").read_text(encoding="utf-8")
LOADER = (WORLD / "world-loader.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
CSS = (WORLD / "world.css").read_text(encoding="utf-8")
THREE_VENDOR = WORLD / "vendor" / "three-0.184.0"


def method(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    return source[start : source.index(next_signature, start)]


def test_first_paint_does_not_wait_for_full_css_or_feature_modules():
    assert "data-world-boot-shell" not in INDEX
    assert INDEX.count('class="world-loading-curtain"') == 1
    assert "world-loading-curtain" not in LOADER
    assert 'import("./world-boot.js")' in LOADER
    assert LOADER.index('import("./world-boot.js")') < LOADER.index(
        'await import("./world-runtime.js")'
    )
    assert 'await import("./world-runtime.js")' in LOADER
    assert "window.requestAnimationFrame(resolve)" in LOADER
    preload = LOADER.index('link.rel = "modulepreload"')
    assert LOADER.index("window.requestAnimationFrame(resolve)") < preload
    assert preload < LOADER.index('await import("./world-runtime.js")')
    for href in ("/world/vendor/three-0.184.0/three.module.min.js",
                 "/world/vendor/three-0.184.0/three.core.min.js",
                 "/world/world-scene.js"):
        assert '"%s"' % href in LOADER
    assert '<link rel="preload" as="style" href="/world/world.css" />' in INDEX
    assert 'media="print"' in INDEX
    assert 'onload="this.media=\'all\'"' in INDEX
    assert '<script defer src="/api-client.js?v=${version}"></script>' in INDEX
    assert '<script defer src="/analytics.js"></script>' not in INDEX
    assert '<script defer src="/membership-prompt.js' not in INDEX
    assert (
        '<script type="text/plain" data-world-after-interactive src="/analytics.js"></script>'
        in INDEX
    )
    assert (
        '<script type="text/plain" data-world-after-interactive '
        'src="/membership-prompt.js?v=${version}"></script>'
    ) in INDEX
    deferred = INDEX[INDEX.index("window.forkmeshWorldLoadDeferredScripts = function"):]
    deferred = deferred[: deferred.index("</script>")]
    assert 'querySelectorAll("script[data-world-after-interactive]")' in deferred
    assert '"forkmesh:world-interactive"' in deferred
    assert "requestIdleCallback" in deferred
    watchdog = INDEX[INDEX.index("setTimeout(function () {\n        var world"):]
    assert "window.forkmeshWorldLoadDeferredScripts()" in watchdog[: watchdog.index("}, 10000);")]
    assert 'loadExtra("/world/world-discord.js", false)' in INDEX
    assert 'loadExtra("/world/world-notes.js", false)' in INDEX
    assert 'window.addEventListener("forkmesh:world-interactive"' in INDEX
    assert 'requestIdleCallback' in INDEX
    assert 'rel="preload" as="image"' not in INDEX


def test_mobile_renderer_runtime_is_pinned_and_same_origin():
    module_path = THREE_VENDOR / "three.module.min.js"
    core_path = THREE_VENDOR / "three.core.min.js"
    license_path = THREE_VENDOR / "LICENSE"

    assert "cdn.jsdelivr.net" not in INDEX
    assert "cdn.jsdelivr.net" not in APP
    head = INDEX[: INDEX.index('<script type="module" src="/world/world.js"></script>')]
    for href in (
        "/world/world-boot.js",
        "/world/world-debug-log.js",
        "/world/vendor/three-0.184.0/three.core.min.js",
        "/world/vendor/three-0.184.0/three.module.min.js",
        "/world/world-scene.js",
    ):
        assert f'<link rel="modulepreload" href="{href}" />' in head
    assert "?v=" not in head.split('rel="modulepreload"', 1)[1].split("<script", 1)[0]
    assert (
        '"./vendor/three-0.184.0/three.module.min.js"'
        in APP
    )
    assert module_path.is_file()
    assert core_path.is_file()
    assert license_path.is_file()
    assert 256_000 < module_path.stat().st_size < 512_000
    assert 256_000 < core_path.stat().st_size < 512_000
    assert hashlib.sha256(module_path.read_bytes()).hexdigest() == (
        "36a60b0120335f89a80a0dab70292292b0ec414b3d05e83cd09a3ea428c6712a"
    )
    assert hashlib.sha256(core_path.read_bytes()).hexdigest() == (
        "6486aa0d719cfa87ec88dc47223b59b1fb8417a1a407fc0e52467c943e2f8cc9"
    )
    assert 'from"./three.core.min.js"' in module_path.read_text(
        encoding="utf-8"
    )
    assert 'const t="184"' in core_path.read_text(encoding="utf-8")
    assert "MIT License" in license_path.read_text(encoding="utf-8")


def test_startup_uses_small_brand_and_font_assets():
    assert "/assets/logo.png" not in INDEX
    assert "/assets/logo.png" not in APP
    assert "/assets/logo.svg" in INDEX
    assert "/assets/logo.svg" in APP
    assert BRAND_MARK.read_bytes().startswith(b"<svg")
    assert BRAND_MARK.stat().st_size < 4_000
    assert "/assets/fonts/Geist-Variable.woff2" in CSS
    assert "/assets/fonts/GeistMono-Variable.woff2" in CSS
    assert "Lato-" not in CSS
    assert "DejaVuSansMono" not in CSS


def test_bootstrap_opens_scene_without_waiting_for_cross_district_data():
    bootstrap = method(
        APP,
        "  async bootstrap() {",
        "\n  setLoadingProgress(percent, label) {",
    )
    assert "this.world = await scene.createWorldScene({" in bootstrap
    assert "this.loadWorldData()" not in bootstrap
    assert "await Promise.allSettled" not in bootstrap
    assert "void this.loadNearbyMemberData().then(" in bootstrap
    for eager_statement in (
        "\n      void this.loadLobbyLinkBoard();",
        "\n      void this.refreshOrgAgentBots();",
        "\n      void this.refreshDesktopAgentBots();",
        "\n      void this.refreshStoreLibrary();",
        "\n      this.startMirrorPolling();",
        "\n      this.startMirrorActionsPolling();",
        "\n      this.startInstanceDirectoryPolling();",
        "\n      this.startEventPolling();",
        "\n      this.startNotificationChannel();",
    ):
        assert eager_statement not in bootstrap
    assert "window.setTimeout(() => {" in bootstrap
    assert "window.requestIdleCallback(startDeferredGlobalWork" in bootstrap


def test_initial_spawn_fetches_only_visible_member_circle_snapshots():
    nearby = method(
        APP,
        "  loadNearbyMemberData() {",
        "\n  loadFountainData(",
    )
    assert nearby.count("this.fetchJSON(") == 3
    assert "this.fetchJSON(WORLD_MEMBER_DIRECTORY_PATH" in nearby
    for endpoint in (
        "/api/world/inactive",
        "/api/world/visitors",
    ):
        assert endpoint in nearby
    for distant_endpoint in (
        "/api/network/overview",
        "/api/world/instances",
        "/api/world/organizations",
        "/api/world/fediverse",
        "/api/world/events",
        "/api/status/world",
    ):
        assert distant_endpoint not in nearby


def test_district_endpoints_and_polling_are_proximity_scoped():
    assert 'if (id === "fountain") {\n      this.startFountainWatch();' in APP
    assert 'previousLandmarkId === "fountain"' in APP
    assert "this.stopFountainWatch();" in APP
    assert 'state === "nearby" ? "nearby" : "distant"' in APP
    assert "this.startOfficeDataWatch();" in APP
    assert "this.stopOfficeDataWatch();" in APP
    assert "void this.refreshInstanceDirectory();" in APP
    assert "this.startInstanceDirectoryPolling(5 * 60 * 1000);" in APP
    assert "window.clearInterval(this.instanceDirectoryTimer);" in APP
    assert "void this.loadSocialWorldData();" in APP
    assert 'if (landmark.id === "events") {' in APP
    assert "this.notificationChannel?.dispose();" in APP
    assert 'if (landmark.id === "broadcast" && hydrate)' in APP
    assert 'if (landmark.id === "security" && hydrate)' in APP
    for landmark in (
        "fountain",
        "repositories",
        "organizations",
        "fediverse",
        "security",
        "events",
    ):
        assert f'  "{landmark}",' in method(
            APP,
            "const DEFERRED_LANDMARKS = new Set([",
            "\n\nconst LANDMARK_CONSTRUCTION_REASONS",
        )


def test_mirror_node_cabinets_are_hydrated_once_after_boot():
    bootstrap = method(
        APP,
        "  async bootstrap() {",
        "\n  setLoadingProgress(percent, label) {",
    )
    deferred = bootstrap[bootstrap.index("const startDeferredGlobalWork = () => {"):]
    deferred = deferred[: deferred.index("\n      };")]
    assert "void this.loadFountainData();" in deferred
    assert "startFountainWatch" not in deferred
    assert "startMirrorPolling" not in deferred


def test_office_inlay_is_small_and_loaded_only_on_approach():
    webp = PUBLIC / "assets" / "world" / "marketing-table-cube-inlay.webp"
    png = PUBLIC / "assets" / "world" / "marketing-table-cube-inlay.png"
    assert webp.exists()
    assert webp.stat().st_size < 100_000
    assert not png.exists()
    logo_loader = method(
        SCENE,
        "  function loadOfficeTableLogoTexture() {",
        "\n  const officeTableLogo =",
    )
    assert '"/assets/world/marketing-table-cube-inlay.webp"' in logo_loader
    assert "new THREE.TextureLoader().load(" in logo_loader
    assert SCENE.count("loadOfficeTableLogoTexture();") == 1
    assert 'officeZoneState === "nearby"' in SCENE
    assert "marketing-table-cube-inlay.png" not in SCENE


def test_cookie_restore_has_a_hard_startup_budget():
    connected = method(
        APP,
        "  async connectedCallback() {",
        "\n  disconnectedCallback() {",
    )
    assert "const WORLD_SESSION_RESTORE_BUDGET_MS = 250;" in APP
    assert "const sessionRestore = restoreWorldSessionFromCookie();" in connected
    assert "await waitForStartupBudget(" in connected
    assert connected.index("await waitForStartupBudget(") < connected.index(
        "this.innerHTML = worldTemplate("
    )


def test_authenticated_secondary_reads_wait_for_interactive_or_user_intent():
    context = method(APP, "  async loadContext() {", "\n  async loadSatelliteSky() {")
    identity = method(
        APP,
        "  applyWorldTicketIdentity(ticket) {",
        "\n  clearWorldTicketIdentity() {",
    )
    assert "syncWorldClientProfile" not in context
    assert '"forkmesh:world-interactive"' in identity
    assert "this.loadWorldPreferences().finally(" in identity
    assert "this.isEventsPanelOpen()" in identity
    assert "this.refreshPersonalNotifications(true)" in identity
    assert identity.index("this.isEventsPanelOpen()") < identity.index(
        "this.refreshPersonalNotifications(true)"
    )


def test_presence_waits_for_background_ticket_without_blocking_the_renderer():
    bootstrap = method(APP, "  async bootstrap() {", "\n  setLoadingProgress(")
    assert "let contextPromise = null;" in bootstrap
    assert "contextPromise = this.trackBootStep(" in bootstrap
    assert "await contextPromise" not in bootstrap
    assert bootstrap.index("this.world = await scene.createWorldScene({") < bootstrap.index(
        "void contextPromise.then(() => this.connectPresence());"
    )
    assert "contextPromise ||= this.loadContext().catch(() => {});" in bootstrap


def test_versioned_runtime_is_immutable_and_everything_else_stays_no_store():
    headers = (ROOT.parent / "www" / "public" / "_headers").read_text(encoding="utf-8")
    world_rule = headers.index("\n/world/*\n")
    runtime_rule = headers.index("\n/world/world-runtime.js\n")
    assert runtime_rule > world_rule
    runtime_block = headers[runtime_rule:].split("\n\n", 1)[0]
    assert "Cache-Control: public, max-age=31536000, immutable" in runtime_block
    assert "no-store" in headers[world_rule:].split("\n\n", 1)[0]
    build = (ROOT.parent / "app" / "tools" / "build_site_assets.py").read_text(encoding="utf-8")
    assert 'f"./world-runtime.js?v={runtime_version}" not in loader' in build
    assert 'raise RuntimeError("World loader is missing its runtime import")' in build
    assert '/world/world-runtime.js?v={runtime_version}' in build
