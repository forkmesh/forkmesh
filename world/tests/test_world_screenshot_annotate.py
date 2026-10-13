#!/usr/bin/env python3
"""Static contracts for the World screenshot capture and annotation flow."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
CSS = (WORLD / "world.css").read_text(encoding="utf-8")
QT_MARKUP = (
    ROOT.parent
    / "marketplace"
    / "packages"
    / "capture"
    / "content"
    / "desktop"
    / "ScreenshotMarkupWindow.cpp"
).read_text(encoding="utf-8")


def test_topbar_offers_a_camera_capture_button():
    # The camera button lives in the top nav next to the view and sound
    # toggles and routes through the shared delegated click handler.
    assert 'data-world-screenshot' in APP
    assert 'title="Capture and annotate a screenshot"' in APP
    assert '📷' in APP
    assert 'closest("[data-world-screenshot]")' in APP
    assert "this.startScreenshotCapture()" in APP


def test_capture_is_drag_selected_and_cancelable():
    # Capture starts as a full-viewport crosshair overlay with a drag
    # marquee; Escape cancels and tiny drags never produce a screenshot.
    assert "world-shot-overlay" in APP
    assert "world-shot-marquee" in APP
    assert "Drag to select the area to capture" in APP
    assert "rect.width < 8 || rect.height < 8" in APP
    assert "closeScreenshotUI" in APP
    assert ".world-shot-overlay" in CSS
    assert "cursor: crosshair" in CSS


def test_capture_rerenders_the_webgl_frame_before_readback():
    # The renderer runs without preserveDrawingBuffer, so the crop must
    # paint a fresh frame and read it back synchronously.
    assert "world.renderer.render(world.scene, world.camera)" in APP
    assert "drawImage" in APP


def test_capture_composites_the_hud_over_the_rendered_frame():
    # The HUD is DOM painted over the canvas, so a capture rasterizes the
    # world root into the crop instead of shipping a bare 3D frame.
    assert "renderHudImage" in APP
    assert "await this.captureWorldRegion(rect)" in APP
    assert 'this.$("[data-world-root]")' in APP
    assert "foreignObject" in APP
    assert "hudStylesheetText" in APP
    # Capture chrome and the live canvas are excluded from the clone.
    assert "[data-world-canvas-wrap], .world-shot-overlay" in APP
    # Foreign-object rendering never fetches subresources.
    assert "inlineHudImages" in APP
    assert "readAsDataURL" in APP


def test_hud_raster_stays_transparent_over_the_rendered_frame():
    # .fm-world paints an opaque backdrop behind the canvas. The HUD layer is
    # drawn on top of the WebGL readback, so the clone must drop that fill or
    # the capture returns chrome on a flat colour with no game world.
    assert "background: #f5f6f8" in CSS
    assert 'clone.style.background = "transparent"' in APP
    assert 'clone.style.backgroundColor = "transparent"' in APP


def test_annotator_matches_the_desktop_markup_toolset():
    # Same tools, palette, and affordances as the desktop
    # Screenshot & Markup window.
    assert "Annotate Screenshot" in APP
    for tool in ("Pencil", "Line", "Arrow", "Rectangle", "Ellipse", "Text"):
        assert tool in APP, tool
        assert tool in QT_MARKUP, tool
    for color in (
        "#ff3232",
        "#ffa500",
        "#ffe600",
        "#32c850",
        "#3282ff",
        "#c832ff",
        "#000000",
        "#ffffff",
    ):
        assert color in APP, color
    assert "data-shot-undo" in APP
    assert "Discard" in APP
    assert "data-shot-download" in APP


def test_annotated_screenshot_can_stage_in_the_hud_composer():
    chat = (ROOT / "public" / "dashboard-chat.js").read_text(
        encoding="utf-8"
    )
    assert "data-shot-compose" in APP
    assert "Add to chat / prompt" in APP
    assert "data-shot-alt" in APP
    assert "this.compactQaFailureScreenshot(" in APP
    assert "this.openChatTerminal(altText" in APP
    assert "attachment:" in APP
    assert "pendingWorldComposerPrefill" in chat
    assert "fileFromWorldComposerAttachment" in chat
    assert "stageDashboardAttachments(control, [file])" in chat
    assert "async function taskAttachmentMetadata(file)" in chat
    assert 'canvas.toDataURL("image/webp", 0.72)' in chat
    assert "attachment.thumbnail = thumbnail" in chat
    assert "Choose a destination, then send." in chat
    assert "void sendDashboardDraft(control)" not in chat[
        chat.index("function applyWorldComposerPrefill()"):
        chat.index("// The World's ForkBot avatar")
    ]


def test_annotator_is_an_accessible_dialog_with_pressed_states():
    assert 'setAttribute("role", "dialog")' in APP
    assert 'setAttribute("aria-modal", "true")' in APP
    assert "data-shot-tool" in APP
    assert "data-shot-color" in APP
    assert "aria-pressed" in APP
    assert ".world-shot-annotator" in CSS
    assert ".world-shot-canvas" in CSS


def test_screenshot_ui_is_torn_down_with_the_component():
    assert "this.screenshotUI = null;" in APP
    assert "this.closeScreenshotUI();" in APP
    assert (
        'window.removeEventListener("keydown", active.onKeyDown, true)' in APP
    )
