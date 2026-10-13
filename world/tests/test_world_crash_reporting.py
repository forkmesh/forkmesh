"""World crash detection, crash-report, and safe-mode reboot contracts."""

import ast
import re
from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


def _client_error_fields():
    tree = ast.parse(ENTRY)
    names = {
        "CLIENT_ERROR_SURFACES",
        "CLIENT_ERROR_KINDS",
    }
    body = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in names
            for target in node.targets
        ):
            body.append(node)
        elif (
            isinstance(node, ast.FunctionDef)
            and node.name in {
                "_sanitize_client_error_text",
                "_client_error_fields",
            }
        ):
            body.append(node)
    namespace = {"re": re}
    exec(compile(ast.Module(body=body, type_ignores=[]), "<entry>", "exec"),
         namespace)
    return namespace["_client_error_fields"]


def test_crash_and_discard_kinds_are_accepted_by_the_client_error_collector():
    assert (
        'CLIENT_ERROR_KINDS = frozenset(\n'
        '    {"error", "unhandledrejection", "crash", "discard"})'
    ) in ENTRY


def test_world_arms_a_pagehide_cleared_crash_guard_with_heartbeat():
    assert 'const CRASH_GUARD_KEY = "forkmesh.world.crash-guard.v1"' in WORLD
    assert 'const CRASH_COUNT_KEY = "forkmesh.world.crash-count.v1"' in WORLD
    # Armed on boot and on bfcache resume, refreshed alongside the
    # once-per-second diagnostics tick, cleared on every orderly pagehide.
    assert "this.armCrashGuard();" in WORLD
    assert "this.beatCrashGuard();" in WORLD
    assert (
        "this.disarmCrashGuard();\n"
        "    this.pauseWorldActivity();"
    ) in WORLD
    # The heartbeat carries the diagnostics that make the report debuggable.
    assert "heapUsedMb" in WORLD
    assert "contextLosses" in WORLD
    assert "UNMASKED_RENDERER_WEBGL" in WORLD
    assert "document.visibilityState" in WORLD


def test_world_reports_the_previous_crashed_session_on_reload():
    assert "reportPreviousWorldCrash()" in WORLD
    assert "document.wasDiscarded === true" in WORLD
    assert "World crashed and reloaded" in WORLD
    assert "World reloaded after the browser discarded the tab" in WORLD
    assert '"/api/client-errors"' in WORLD
    assert 'reportWorldClientError(message, kind = "crash") {' in WORLD
    assert 'surface: "world"' in WORLD
    assert "keepalive: true" in WORLD


def test_a_session_that_ended_hidden_is_reported_but_not_counted_as_a_crash():
    report = WORLD[WORLD.index("  reportPreviousWorldCrash() {"):]
    report = report[: report.index("  reportWorldClientError(")]
    assert 'String(record.visibility || "") === "hidden"' in report
    assert "const discarded = browserDiscarded || closedHidden;" in report
    assert "if (!discarded) this.recordWorldCrash();" in report
    assert "World reloaded after the browser closed it in the background" in report
    assert "World crashed and reloaded" in report
    assert 'this.reportWorldClientError(parts.join("; "), discarded ? "discard" : "crash");' in report
    visibility = WORLD[WORLD.index("  handleVisibility = () => {"):]
    visibility = visibility[: visibility.index("if (document.hidden) {")]
    assert "this.beatCrashGuard();" in visibility


def test_webgl_context_losses_are_reported_with_diagnostics():
    assert "World renderer crashed; WebGL context lost after" in WORLD
    assert "this.reportedRendererContextLoss = true;" in WORLD
    assert "this.rendererContextLosses" in WORLD
    # A restored context still proves the current allocation profile failed;
    # the next ordinary reload must come back in compact mode.
    lost = WORLD[WORLD.index('    if (state !== "lost") return;'):]
    lost = lost[:lost.index("    this.beatCrashGuard();")]
    assert "this.recordWorldCrash();" in lost
    # The first full-renderer loss is restarted in compact mode after the
    # browser's restoration window; the compact retry stays manual to avoid a
    # reload loop on a persistently unhealthy driver.
    assert "const RENDERER_RECOVERY_RELOAD_DELAY_MS = 10_000;" in WORLD
    assert "if (!this.rendererSafeMode)" in WORLD
    assert "this.reloadForRendererRecovery();" in WORLD
    assert "rendererRecoveryReloadTimer" in WORLD
    # Driver status and high-water marks make a loss after a long session
    # distinguishable from a scene that was simply too heavy at the end.
    assert "context reason ${statusMessage}" in WORLD
    assert "peak textures ${coarseCrashLabel(highWater.textures)}" in WORLD
    assert "noteRendererDiagnosticsHighWater(snapshot);" in WORLD
    assert "renderer.setAnimationLoop(null);" in SCENE
    assert "statusMessage: String(event.statusMessage || \"\")" in SCENE


def test_crashed_sessions_reboot_into_the_compact_renderer():
    # The safe-mode reboot is the mitigation: after a crash (or a context
    # loss that forced the recovery reload) the next boot skips multisample,
    # stencil, and shadow-map allocations.
    assert "this.rendererSafeMode = this.worldCrashCount() > 0;" in WORLD
    assert "forceCompactRenderer: this.rendererSafeMode === true," in WORLD
    assert "forceCompactRenderer = false," in SCENE
    assert (
        "forceCompactRenderer || win?.matchMedia?."
        '("(pointer: coarse)")?.matches,'
    ) in SCENE
    # The renderer-recovery reload counts like a crash for the next boot.
    assert (
        "this.recordWorldCrash();\n"
        "    this.preserveWorldPositionForRefresh();"
    ) in WORLD


def test_memory_constrained_renderer_reduces_large_plate_resolution():
    # The memory crash applies to phones and oversized desktop surfaces: ~96
    # full-resolution plates retain both 2D and GPU backing stores.
    assert "const CANVAS_TEXTURE_SCALE_MIN_PIXELS = 512 * 512;" in SCENE
    assert "let canvasTextureScale = 1;" in SCENE
    assert "setCanvasTextureScale(\n    compactRenderer" in SCENE
    assert "? 0.25" in SCENE
    assert ": memoryConstrainedRenderer\n        ? 0.5" in SCENE
    assert "canvasTextureScale," in SCENE
    # The scale must be chosen before the first plate paints, or the phone
    # allocates the full-size buffers this is meant to avoid.
    build = SCENE[SCENE.index("export async function createWorldScene("):]
    assert build.index(
        "setCanvasTextureScale(\n    compactRenderer"
    ) < build.index("createWorldSky({")
    # Draw code keeps its own coordinate system: only the buffer shrinks.
    assert "canvas.width = Math.max(1, Math.round(width * scale));" in SCENE
    assert "context.setTransform(scale, 0, 0, scale, 0, 0);" in SCENE
    # Painters that read canvas.width/height keep full resolution.
    assert "if (draw.length >= 2) return 1;" in SCENE
    # A caller can ask for a lighter plate on top of the renderer-wide cut.
    assert "canvasTextureScale * plate" in SCENE
    # A crowd of directory figures is the avatar-shaped version of this same
    # plate budget: a hundred-plus chest cards and name boards at full
    # resolution outweigh the whole static World, so the Members Circle asks
    # for half-size plates and the renderer-wide cut still applies on top.
    assert "plateScale: 0.5," in SCENE
    assert (
        "const plate = plateScale > 0 && plateScale < 1 ? plateScale : 1;"
    ) in SCENE
    assert "canvasTextureScale * plate" in SCENE
    # Both of an avatar's big plates honour it, first paint and repaint alike.
    # The chest card also carries a legibility override, which only ever
    # applies to the handful of cards a visitor is close enough to read; see
    # test_world_chest_card_legibility.
    assert SCENE.count("  }, plateScale") == 2
    assert "  }, plateScale, badgeTextureScale(detail));" in SCENE
    assert SCENE.count("avatar.userData.plateScale,") == 2
    # An in-place repaint lands in the same coordinate system.
    repaint = SCENE[SCENE.index("function repaintCanvasTexture("):]
    repaint = repaint[:repaint.index("\nfunction ")]
    assert "Number(canvas.dataset?.textureScale) || 1" in repaint
    assert (
        "context.clearRect(0, 0, canvas.width / scale, canvas.height / scale)"
    ) in repaint


def test_crash_reports_name_the_device_renderer_and_resident_scene():
    # "Crashing on mobile" is only actionable with the device class, the
    # renderer mode, and what the GPU was holding when the tab died.
    report = WORLD[WORLD.index("  reportPreviousWorldCrash()"):]
    report = report[:report.index("  reportWorldClientError(")]
    for reading in (
        "device ${device.touch",
        "renderer ${rendererModeLabel(record)}",
        "webgl${record.webgl2 === true",
        "cores ${describe(device.cores)}",
        "device memory ",
        "textures resident/live ${coarseCrashLabel(record.textures)}",
        "geometries resident/live ${coarseCrashLabel(record.geometries)}",
        "programs ${coarseCrashLabel(record.programs)}",
        "canvas raster ${Math.round",
        "scene objects ${coarseCrashLabel(record.objects)}",
        "heaviest ${topElements}",
        "draws ${coarseCrashLabel(record.calls)}",
        "buffer ${buffer} at dpr ",
        "space ${String(record.space",
        "avatars ${describe(record.avatars)}",
        "districts resident ${describe(record.residentDistricts)}",
        "GPU limits texture ${describe(record.maxTextureSize)}",
    ):
        assert reading in report, reading
    # Admin notifications retain only a bounded prefix. Allocation totals and
    # scene cardinality must arrive before volatile timing/frame counters.
    assert report.index(
        "estimated GPU resources textures"
    ) < report.index("uptime ${coarseCrashLabel")
    assert report.index(
        "scene objects ${coarseCrashLabel"
    ) < report.index("fps ${coarseCrashLabel")
    # The device facts are read once and reused, like the GPU label.
    assert "deviceProfile()" in WORLD
    assert "navigator.hardwareConcurrency" in WORLD
    assert "navigator.deviceMemory" in WORLD
    # The heartbeat rides the diagnostics snapshot the renderer already
    # publishes rather than duplicating those readings.
    guard = WORLD[WORLD.index("  beatCrashGuard()"):]
    guard = guard[:guard.index("  reportPreviousWorldCrash()")]
    assert "const output = snapshot?.output;" in guard
    assert (
        "bufferWidth: output ? Math.round(output.drawingBufferWidth) : -1,"
    ) in guard
    assert (
        "compact: output ? output.compactRenderer === true : false,"
    ) in guard
    for reading in (
        "liveTextures: complexity",
        "liveGeometries: complexity",
        "textureMb: complexity",
        "geometryMb: complexity",
        "renderTargetMb: complexity",
        "objects: complexity",
        "meshes: complexity",
        "materials: complexity",
        "topElements:",
        "canvasTextureScale: output",
        "residentDistricts: residency",
        "districtEvictions: residency",
        "activeDistricts:",
        "maxTextureSize: output",
        "maxTextures: output",
    ):
        assert reading in guard


def test_context_loss_report_distinguishes_leaks_from_large_live_assets():
    report = WORLD[WORLD.index("  handleRendererStateChange(state"):]
    report = report[:report.index("  renderWebGLFallback()")]
    for reading in (
        "buffer ${buffer} css ",
        "antialias ${output?.antialias",
        "textures resident/live ",
        "geometries resident/live ",
        "estimated GPU resources textures ",
        "heap ${coarseCrashLabel(memory?.heapUsedMB",
        '"next boot compact"',
        "districts resident ${coarseCrashLabel(residency?.resident)}",
        "GPU limits texture ${coarseCrashLabel(output?.maxTextureSize)}",
    ):
        assert reading in report, reading


def test_volatile_crash_readings_are_bucketed_so_equivalent_crashes_group():
    # The admin view groups rows by exact message text. Raw counters would
    # file every crash as its own group of one — no count, no frequency, and
    # one administrator ping per crash.
    assert "function coarseCrashReading(value)" in WORLD
    assert "function coarseCrashLabel(value, unit = \"\")" in WORLD
    assert "const magnitude = 10 ** Math.floor(Math.log10(number));" in WORLD
    assert "return Math.round(number / magnitude) * magnitude;" in WORLD


def test_crash_reports_reach_the_admin_ping_without_losing_the_tail():
    # The reported group was cut at "context los": the message survived the
    # collector but the ping body truncated it mid-reading.
    assert '_sanitize_client_error_text(payload.get("message"), 900)' in ENTRY
    assert 'String(message || "").slice(0, 900)' in WORLD
    notify = ENTRY[ENTRY.index("async def _notify_new_error_group("):]
    notify = notify[:notify.index("async def _write_error_log(")]
    assert 'str(message or "")[:460]' in notify
    # 460 plus the "JS /client-error/world — " prefix stays inside the
    # 500-character body the notification payload keeps.
    assert '"body": clean_string(body, NOTIFICATION_BODY_MAX)' in ENTRY
    body_max = int(re.search(
        r"^NOTIFICATION_BODY_MAX = (\d+)$", ENTRY, re.M).group(1))
    assert body_max >= 500
    # The admin table shows the whole report rather than one clipped line.
    assert "white-space:pre-wrap;overflow-wrap:anywhere" in ENTRY
    assert "text-overflow:ellipsis;white-space:nowrap;vertical-align:middle" \
        not in ENTRY


def test_crash_reports_survive_server_side_validation_and_redaction():
    fields = _client_error_fields()({
        "kind": "crash",
        "surface": "world",
        "message": (
            "World crashed and reloaded; previous session ended without "
            "pagehide; uptime 342s; heartbeat gap 2s; navigation reload; "
            "visibility visible; tab crashes 1; socket online with 3 peers; "
            "fps 11; frame 84ms worst 1204ms; triangles 1204000; dpr 2; "
            "heap 1900MB of 2048MB; context losses 1; safe mode off; "
            "gpu ANGLE (NVIDIA, GeForce RTX 3060 Direct3D11)"),
        "stack": "",
        "source": "world.js",
        "line": 0,
        "column": 0,
    })
    assert fields is not None
    surface, detail = fields
    assert surface == "world"
    assert detail.startswith("client crash [world]")
    assert "uptime 342s" in detail
    assert "heap 1900MB of 2048MB" in detail
    assert "GeForce RTX 3060" in detail
