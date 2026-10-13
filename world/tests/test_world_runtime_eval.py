from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
WORLD = REPOSITORY / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")


def block(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin : source.index(end, begin)]


def test_hud_face_never_paints_inside_the_first_template():
    source = block(APP, "function hudAvatarFaceSource(session, identity) {", "\n}\n")
    assert "const stored = storedHudFacePortrait(key);" in source
    assert "scheduleHudFacePortrait(key);" in source
    assert "proceduralAvatarFaceDataURL" not in source
    schedule = block(APP, "function scheduleHudFacePortrait(key) {", "\n}\n")
    assert "window.requestIdleCallback(paint, { timeout: 2500 });" in schedule
    paint = block(APP, "function paintHudFacePortrait(key) {", "\n}\n")
    assert "hudFacePortrait = { key, url: proceduralAvatarFaceDataURL(key) };" in paint
    assert "localStorage.setItem(" in paint
    assert "HUD_FACE_CACHE_KEY" in paint
    assert "element.updateIdentityUI?.();" in paint


def test_cached_hud_face_is_bound_to_identity_build_and_a_png_data_url():
    stored = block(APP, "function storedHudFacePortrait(key) {", "\n}\n")
    assert "stored?.key === key" in stored
    assert "stored.build === hudFaceCacheBuild()" in stored
    assert "HUD_FACE_URL_RE.test(" in stored
    assert "const HUD_FACE_URL_RE = /^data:image\\/png;base64,[A-Za-z0-9+/=]+$/;" in APP
    assert 'String(import.meta.url || "")' in APP


def test_public_identity_reuses_one_local_time_formatter():
    identity = block(APP, "function publicIdentity(identity, settings) {", "\n}\n")
    assert "new Intl.DateTimeFormat" not in identity
    assert "worldLocalTimeLabel()" in identity
    assert APP.count("new Intl.DateTimeFormat(undefined, {\n    hour: \"numeric\",") == 1
    assert "accountBadgeTimeFormat ||= new Intl.DateTimeFormat(" in APP
