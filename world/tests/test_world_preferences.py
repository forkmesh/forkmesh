"""Account-scoped World preference and saved-view synchronization contracts."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2] / "app"
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY = (
    (ROOT / "src/entry.py").read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
)
WORLD = (ROOT.parent / "world/public/world/world.js").read_text(encoding="utf-8")


def test_world_preferences_require_a_revocable_session_and_same_origin_write():
    assert "async def world_preferences_handler" in ENTRY
    assert "_account_session_record(env, request, data)" in ENTRY
    assert '"invalid_session"' in ENTRY
    assert "_request_same_origin(request)" in ENTRY
    assert '"origin_not_allowed"' in ENTRY
    assert "WORLD_PREFERENCES_MAX_BYTES = 320 * 1024" in ENTRY


def test_preferences_stay_in_the_encrypted_account_record_and_are_bounded():
    assert 'rec["world_preferences"] = stored' in ENTRY
    assert "await _save_account(env, account_bi, rec)" in ENTRY
    assert '"storage": "account-encrypted"' in ENTRY
    assert "WORLD_PREFERENCES_MAX_VIEWS = 5" in ENTRY
    assert "data:image/webp;base64" in ENTRY
    assert "abs(x) > 1900" in ENTRY
    assert "settingsUpdatedAt" in ENTRY


def test_stale_devices_merge_instead_of_overwriting_newer_preferences():
    assert 'incoming["settingsUpdatedAt"] >= settings_updated_at' in ENTRY
    assert 'view["updatedAt"] >= previous["updatedAt"]' in ENTRY
    assert "mergeWorldPreferenceSnapshot(payload)" in WORLD
    assert "view.updatedAt > previous.updatedAt" in WORLD
    assert "WORLD_PREFERENCES_SYNC_DELAY_MS = 700" in WORLD


def test_signed_in_world_syncs_views_and_movement_with_offline_fallback():
    assert 'const WORLD_PREFERENCES_ENDPOINT = "/api/world/preferences"' in WORLD
    assert "void this.loadWorldPreferences()" in WORLD
    assert "queueWorldPreferencesSync()" in WORLD
    assert "this.settings.moveSpeed" in WORLD
    assert "this.settings.moveAccel" not in WORLD
    assert "this.settings.daylightMode" in WORLD
    assert "this.savedViews.slice(0, SAVED_VIEWS_MAX)" in WORLD
    assert "Local storage remains the offline source" in WORLD
    assert "synced to your account" in WORLD


def test_daylight_lock_is_bounded_before_account_storage():
    assert '"daylightMode": 8' in ENTRY
    assert '(None, "auto", "day", "night")' in ENTRY


def test_avatar_face_preference_survives_account_sync():
    assert '"faceImagePreference": 12' in ENTRY
    assert 'None, "photo", "generated"' in ENTRY
    assert 'faceImagePreference: "photo"' in WORLD


def test_debug_fresh_start_resets_account_settings_and_viewer_storage():
    assert "data-world-start-fresh" in WORLD
    assert "Restore everything and start fresh" in WORLD
    fresh = WORLD.split("  async startWorldFresh() {", 1)[1].split("\n  }\n", 1)[0]
    assert "window.confirm(" in fresh
    assert fresh.index("window.confirm(") < fresh.index("clearWorldViewerStorage();")
    assert "{ settings: {}, settingsUpdatedAt: Date.now(), savedViews: [] }" in fresh
    assert 'this.positionKey = "";' in fresh
    assert "location.reload();" in fresh
    assert 'incoming["settingsUpdatedAt"] >= settings_updated_at' in ENTRY
    clear = WORLD.split("function clearWorldViewerStorage() {", 1)[1].split(
        "\n}\n", 1)[0]
    assert "globalThis.localStorage, globalThis.sessionStorage" in clear
    for prefix in ('"forkmesh.world."', '"forkmesh.worldComposer."', '"forkmesh-world-"'):
        assert prefix in WORLD.split("const WORLD_FRESH_START_PREFIXES", 1)[1][:200]
    keep = WORLD.split("const WORLD_FRESH_START_KEEP", 1)[1][:200]
    assert "SAVED_VIEWS_KEY_PREFIX" in keep
