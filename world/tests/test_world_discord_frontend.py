"""Static safety checks for the on-demand World Discord bridge."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"


def test_world_discord_bridge_is_manual_and_never_handles_provider_secrets():
    source = (WORLD / "world-discord-panel.js").read_text(encoding="utf-8")
    loader = (WORLD / "world-discord.js").read_text(encoding="utf-8")
    index = (WORLD / "index.html").read_text(encoding="utf-8")
    styles = (WORLD / "world.css").read_text(encoding="utf-8")
    assert 'loadExtra("/world/world-discord.js", false)' in index
    assert 'window.addEventListener("forkmesh:world-interactive"' in index
    assert "requestIdleCallback" in index
    assert "setInterval" not in loader
    assert "innerHTML" not in loader
    assert "/api/orgs/${encodeURIComponent(state.organization)}/discord" in source
    assert "/discord/oauth/start" in source
    assert "textContent" in source
    assert "innerHTML" not in source
    assert "setInterval" not in source
    assert "DISCORD_BOT_TOKEN" not in source
    assert "oauth/start" in source
    assert "state.reading" in source
    assert "state.writing" in source
    assert "window.location.assign" in source
    assert "window.open" not in source
    assert "consumeOAuthOutcome" in source
    assert "Discord returned an invalid or expired authorization" in source
    assert "stage: state lookup" in source
    assert "stage: record decryption" in source
    assert "stage: record state binding" in source
    assert "stage: session context" in source
    assert "stage: authorization code" in source
    assert "Disconnect Discord" in source
    assert "connector.state === \"configured\"" in source
    assert "world-discord-panel" in styles


def test_world_discord_panel_loads_only_when_the_bridge_is_asked_for():
    """The eager entry point must stay a loader, not the bridge itself."""
    loader = (WORLD / "world-discord.js").read_text(encoding="utf-8")
    panel = (WORLD / "world-discord-panel.js").read_text(encoding="utf-8")
    assert 'import("./world-discord-panel.js")' in loader
    assert 'from "./world-discord-panel.js"' not in loader
    # Both entry points the panel used to bind for itself.
    assert "[data-world-discord-open]" in loader
    assert "forkmesh:open-discord" in loader
    # The panel no longer boots itself; the loader owns that call.
    assert "export function bootWorldDiscord()" in panel
    assert "export function showWorldDiscordPanel()" in panel
    assert "DOMContentLoaded" not in panel
    # An authorization return still opens the panel without a second click.
    assert 'searchParams.has("discord")' in loader
    # One binding per entry point: the loader releases its stand-ins as soon as
    # the panel binds its own, so a single click cannot toggle the panel twice.
    assert "releaseLoaderBindings()" in loader
    assert "removeEventListener" in loader


def test_world_discord_bridge_renders_inside_the_settings_panel():
    """The bridge is a settings section, not a floating HUD card."""
    app = (WORLD / "world.js").read_text(encoding="utf-8")
    loader = (WORLD / "world-discord.js").read_text(encoding="utf-8")
    panel = (WORLD / "world-discord-panel.js").read_text(encoding="utf-8")
    styles = (WORLD / "world.css").read_text(encoding="utf-8")

    # The mount point ships with the settings markup, inside the same panel as
    # the trigger that opens it.
    settings = app.split('<section class="world-settings"', 1)[1].split(
        'class="world-settings-tabs"', 1
    )[0]
    assert "data-world-discord-open" in settings
    assert "data-world-discord-root" in settings
    assert 'aria-controls="world-discord-root"' in settings

    # No body-level overlay floats over the HUD.
    assert "document.body.append" not in panel
    assert "position: fixed" not in styles.split(
        "[data-world-discord-root] {", 1
    )[1].split("}", 1)[0]

    # Opening the bridge opens settings instead of closing it, so the 3D
    # Discord board and an OAuth return both land somewhere visible.
    assert "toggleSettings?.(true)" in panel
    assert "toggleSettings?.(false)" not in panel
    assert "toggleSettings?.(false)" not in loader
