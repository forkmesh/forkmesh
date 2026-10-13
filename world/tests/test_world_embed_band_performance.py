#!/usr/bin/env python3
"""The World band at the foot of every page must not tax the page (#548).

Three costs were charged to every visitor who scrolled to the bottom of a
marketing page: the whole 3D application downloaded alongside the content
above it, the renderer kept drawing at full cadence long after the band left
the screen, and the live canvas cancelled the wheel and touch gestures that
were trying to scroll past it. These contracts pin the shape of the fix on
both sides of the frame.
"""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
FOOTER_JS = (ROOT / "www" / "public" / "site-footer.js").read_text(
    encoding="utf-8"
)
FOOTER_CSS = (ROOT / "www" / "public" / "site-footer.css").read_text(
    encoding="utf-8"
)


def _section(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


# ----------------------------------------------------------------- the host


def test_the_band_ships_without_a_frame_and_mounts_one_near_the_viewport():
    # The band's markup is a stage, not an iframe: nothing is fetched while
    # the visitor is still reading the page above it.
    band = _section(
        FOOTER_JS,
        "  function renderWorldFrame() {",
        "\n  const WORLD_EMBED_PHASES",
    )
    assert "data-world-stage" in band
    assert "<iframe" not in band
    assert "WORLD_EMBED_ORIGINS.has(window.location.origin)" in band

    mount = _section(
        FOOTER_JS,
        "  function mountWorldStage(stage) {",
        "\n  function mountWorldStages(",
    )
    assert "new IntersectionObserver(" in mount
    assert "{ rootMargin: WORLD_EMBED_PRELOAD_MARGIN }" in mount
    assert "const WORLD_EMBED_PRELOAD_PX = 600;" in FOOTER_JS
    assert (
        "const WORLD_EMBED_PRELOAD_MARGIN = `${WORLD_EMBED_PRELOAD_PX}px 0px`;"
        in FOOTER_JS
    )
    # A band the page is already at mounts on the spot. Waiting for the first
    # observer callback costs a cold renderer a few hundred milliseconds,
    # which is the entire visible load of a band that is already on screen.
    assert "} else if (!observable || nearViewport()) {" in mount
    assert "rect.top <= limit + WORLD_EMBED_PRELOAD_PX" in mount
    # The frame's markup has exactly one consumer — the deferred mount — so
    # no render path can put an iframe on the page ahead of it.
    assert 'src="${WORLD_EMBED_URL}"' in FOOTER_JS
    assert FOOTER_JS.count("worldFrameMarkup()") == 2
    build = _section(
        mount,
        "    function mountFrame() {",
        "\n    window.addEventListener",
    )
    assert "template.innerHTML = worldFrameMarkup().trim();" in build
    assert "stage.prepend(frame);" in build

    # Both entry points mount their stages; a band rendered into a page that
    # never runs this would show a permanent loading state.
    assert "mountWorldStages(footer);" in FOOTER_JS
    assert "mountWorldStages(band);" in FOOTER_JS


def test_the_band_states_are_honest_about_what_it_is_doing():
    phases = _section(
        FOOTER_JS,
        "  const WORLD_EMBED_PHASES = {",
        "\n  function connectionSavesData()",
    )
    for phase in ("idle:", "deferred:", "loading:", "ready:", "engaged:"):
        assert phase in phases
    assert "Loading the Town Square…" in phases
    assert "Click to explore the world" in phases
    # A metered connection is asked before a download this size.
    assert "navigator.connection?.saveData === true" in FOOTER_JS

    # The loading state retires on the world's own signal, with the timeout
    # only as a backstop for a frame that never answers.
    mount = _section(
        FOOTER_JS,
        "  function mountWorldStage(stage) {",
        "\n  function mountWorldStages(",
    )
    assert 'event.data.type === "world-embed-ready"' in mount
    assert "event.origin !== WORLD_EMBED_FRAME_ORIGIN" in mount
    assert "window.setTimeout(markReady, WORLD_EMBED_READY_TIMEOUT_MS)" in mount


def test_an_unengaged_band_leaves_the_scroll_gestures_with_the_page():
    # A WebGL canvas cancels the wheel to drive its camera. Keeping the frame
    # out of hit testing is what stops it swallowing the page's scroll.
    frame_rule = _section(
        FOOTER_CSS,
        ".site-footer-world-frame {",
        "\n.site-footer-world-status {",
    )
    assert "pointer-events: none;" in frame_rule
    assert (
        '.site-footer-world-stage[data-world-phase="engaged"] '
        ".site-footer-world-frame {\n  pointer-events: auto;\n}"
    ) in FOOTER_CSS

    mount = _section(
        FOOTER_JS,
        "  function mountWorldStage(stage) {",
        "\n  function mountWorldStages(",
    )
    # Engagement is opt-in, and the pointer leaving the band or the band
    # leaving the screen hands the wheel back rather than trapping the page.
    assert "setEngaged(true)" in mount
    assert 'if (event.pointerType === "mouse") setEngaged(false);' in mount
    assert "if (!onScreen) setEngaged(false);" in mount


def test_the_host_reports_occupancy_and_engagement_to_the_world():
    mount = _section(
        FOOTER_JS,
        "  function mountWorldStage(stage) {",
        "\n  function mountWorldStages(",
    )
    assert '{ type: "world-embed-visibility", visible: onScreen }' in mount
    assert '{ type: "world-embed-engaged", engaged: next }' in mount
    assert "source: WORLD_EMBED_MESSAGE_SOURCE" in mount
    assert 'const WORLD_EMBED_MESSAGE_SOURCE = "forkmesh-site";' in FOOTER_JS
    # Posting into a frame that has not reached its own origin is a console
    # error, so the current state is replayed on load instead.
    assert "if (!frame || !frameLoaded) return;" in mount
    assert (
        mount.index("frameLoaded = true;")
        < mount.index('post({ type: "world-embed-visibility"')
    )


# ---------------------------------------------------------------- the world


def test_the_world_pauses_its_renderer_when_the_band_leaves_the_screen():
    handler = _section(
        APP,
        "  handleEmbedHostMessage = (event) => {",
        "\n  // Inside a frame the implicit intersection root",
    )
    assert "if (event.source !== window.parent) return;" in handler
    assert "data.source !== WORLD_EMBED_MESSAGE_SOURCE" in handler
    assert 'data.type === "world-embed-visibility"' in handler
    assert "this.embedHostVisible = data.visible !== false;" in handler

    # The observer is the second, independent reporter: inside a frame the
    # implicit intersection root is the top-level viewport, so it sees the
    # band leave the host page even with no message from the host at all.
    watcher = _section(
        APP,
        "  watchEmbedViewport() {",
        "\n  embedOnScreen() {",
    )
    assert "new IntersectionObserver(" in watcher
    assert "{ rootMargin: WORLD_EMBED_VIEWPORT_MARGIN }" in watcher
    assert "this.embedViewportObserver.observe(this);" in watcher
    assert "this.embedViewportVisible = entry.isIntersecting === true;" in watcher
    assert 'window.addEventListener("message", this.handleEmbedHostMessage);' in APP
    assert (
        'window.removeEventListener("message", this.handleEmbedHostMessage);' in APP
    )
    assert "this.embedViewportObserver?.disconnect();" in APP

    # Either reporter saying the band is gone is enough; both must agree
    # before it comes back.
    occupancy = _section(APP, "  embedOnScreen() {", "\n  syncEmbedOccupancy() {")
    assert "if (!this.embedded) return true;" in occupancy
    assert "return this.embedHostVisible && this.embedViewportVisible;" in occupancy


def test_one_predicate_owns_every_renderer_resume():
    # A call site that knew only about tab visibility would restart the loop
    # for a band parked far below the page the visitor is reading.
    predicate = _section(APP, "  worldRenderPaused() {", "\n  syncRenderPause() {")
    assert "if (document.hidden) return true;" in predicate
    assert '[data-world-renderer-recovery]' in predicate
    assert "return !this.embedOnScreen();" in predicate

    # setPaused(false) has exactly one caller, and it is the predicate.
    assert "this.world?.setPaused?.(paused);" in APP
    for resumed in (
        "setPaused(document.hidden)",
        "setPaused?.(document.hidden)",
        "setPaused(false)",
        "setPaused?.(false)",
    ):
        assert resumed not in APP, resumed

    visibility = _section(
        APP, "  handleVisibility = () => {", "\n  handleStorage = (event) => {"
    )
    assert "this.syncRenderPause();" in visibility


def test_a_framed_band_previews_at_half_cadence_until_someone_steers_it():
    assert "this.embedded = windowIsFramed();" in APP
    # A world served as its own page is engaged from the first frame; only a
    # framed band starts as a preview.
    assert "this.embedEngaged = !this.embedded;" in APP
    engage = _section(APP, "  setEmbedEngaged(engaged) {", "\n  postToEmbedHost(")
    assert "if (next === this.embedEngaged || this.destroyed) return;" in engage
    assert "this.world?.setPreviewCadence?.(!next);" in engage
    assert "this.world.setPreviewCadence?.(!this.embedEngaged);" in APP
    # Input reaching the framed document is engagement even if the host never
    # said so.
    activity = _section(
        APP,
        "  handlePublicInputActivity = (event) => {",
        "\n  blockWorldPullToRefresh",
    )
    assert "this.setEmbedEngaged(true);" in activity

    cadence = _section(
        SCENE, "  function setPreviewCadence(preview) {", "\n  // GPU name,"
    )
    assert "const PREVIEW_RENDER_FRAME_INTERVAL_MS = 1000 / 30;" in SCENE
    assert "renderFrameIntervalMs = interval;" in cadence
    # Leaving preview must not wait out a deadline set by the slower interval.
    assert "nextRenderFrameAt = 0;" in cadence
    assert "    setPreviewCadence,\n" in SCENE

    # The frame budget animate() enforces is the mutable one.
    animate = _section(SCENE, "  function animate(", "\n  function setPaused(")
    assert "time - nextRenderFrameAt > renderFrameIntervalMs * 2" in animate
    assert "RENDER_FRAME_INTERVAL_MS" not in animate


def test_the_world_tells_its_host_when_it_is_actually_ready():
    # The frame's load event fires while the scene is still being built, so
    # the band's loading state waits for the world's own signal instead.
    assert 'this.postToEmbedHost({ type: "world-embed-ready" });' in APP
    boot = _section(APP, "  async bootstrap() {", "\n  setLoadingProgress(")
    # Both the working renderer and the WebGL fallback report themselves done.
    assert boot.count("this.announceEmbedReady();") == 2
    post = _section(APP, "  postToEmbedHost(message) {", "\n  // The band has been")
    assert 'source: "forkmesh-world"' in post
    assert "if (!this.embedded) return;" in post
