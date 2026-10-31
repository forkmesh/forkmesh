from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
CSS = (WORLD / "world.css").read_text(encoding="utf-8")
SPEC = (ROOT / "world" / "browser_tests" / "tests" / "world.spec.js").read_text(
    encoding="utf-8"
)


def method(name):
    return APP.split(f"\n  {name}(", 1)[1].split("\n  }\n", 1)[0]


def test_each_edge_is_a_dock_and_every_hud_button_is_a_tool():
    for dock in ("top", "left", "right", "bottom"):
        assert f'data-world-dock="{dock}"' in APP
    assert 'data-world-dock="top" data-world-top-actions aria-label="World tools"' in APP
    assert 'data-world-dock="bottom" data-world-avatar-actions' in APP
    tools = set(re.findall(r'data-world-tool="([a-z-]+)"', APP))
    assert {
        "upgrade",
        "summon",
        "camera",
        "sound",
        "capture",
        "notes",
        "dashboard",
        "share",
        "jetpack",
        "emote-wave",
        "emote-sparkle",
    } <= tools
    right = APP.split('data-world-dock="right"', 1)[1].split("</div>", 1)[0]
    assert "data-world-share-current" in right
    shirt = APP.split('class="world-shirt-badge"', 1)[1].split(">", 1)[0]
    assert "data-world-tool" not in shirt


def test_moving_keeps_the_element_so_delegated_handlers_still_fire():
    place = method("placeHudTool")
    assert "others[at].before(tool)" in place
    assert "innerHTML" not in place
    assert 'tool.classList.toggle("world-dock-tool", away);' in place
    assert "if (native) tool.classList.toggle(native, !away);" in place
    assert 'event.target.closest("[data-world-screenshot]")' in APP


def test_layout_persists_per_device_and_resets_to_the_markup_default():
    assert 'const WORLD_HUD_LAYOUT_KEY = "forkmesh.world.hudLayout.v1";' in APP
    bind = method("bindHudDocks")
    assert "this.hudDefaultLayout = this.hudDockLayout();" in bind
    assert bind.index("this.hudDefaultLayout") < bind.index("readJSON(localStorage, WORLD_HUD_LAYOUT_KEY")
    assert "writeJSON(localStorage, WORLD_HUD_LAYOUT_KEY, this.hudDockLayout());" in method("saveHudLayout")
    reset = method("resetHudLayout")
    assert "localStorage.removeItem(WORLD_HUD_LAYOUT_KEY);" in reset
    assert "this.applyHudLayout(this.hudDefaultLayout);" in reset
    assert APP.count("data-world-dock-reset") >= 2
    assert "data-world-dock-arrange-open" in APP


def test_arranging_uses_pointer_events_long_press_and_keyboard():
    bind = method("bindHudDocks")
    assert 'hud.addEventListener("pointerdown"' in bind
    assert 'window.addEventListener("pointermove", move);' in bind
    assert "WORLD_HUD_LONG_PRESS_MS" in bind
    assert "dragstart" not in bind
    assert "this.placeHudTool(tool, WORLD_HUD_DOCK_KEYS[event.key]);" in bind
    assert "event.stopPropagation();" in bind
    assert 'event.key === "Escape"' in bind
    assert 'tool.setAttribute("aria-describedby", "world-dock-arrange-hint");' in method("setHudArranging")
    assert "expanded === true || this.hudArranging === true" in method("setWorldRightRailExpanded")
    assert 'id="world-dock-arrange-hint"' in APP
    assert "data-world-dock-status role=\"status\"" in APP


def test_buttons_take_the_size_of_the_rail_they_sit_in():
    assert ".fm-world .world-dock-tool {" in CSS
    bottom = CSS.split(".fm-world .world-hud-island .world-avatar-actions > .world-dock-tool {", 1)[1].split("}", 1)[0]
    assert "width: 30px;" in bottom
    assert '.fm-world .world-hud[data-hud-expanded="false"] .world-top-actions > .world-dock-tool' in CSS
    assert '.fm-world .world-dock-left[data-empty="true"]' in CSS
    assert '[data-world-arranging="true"] [data-world-tool]' in CSS
    assert "touch-action: none;" in CSS.split('[data-world-arranging="true"] [data-world-tool] {', 1)[1].split("}", 1)[0]


def test_browser_contract_drags_between_edges():
    assert 'test("HUD buttons move between the four edges by drag, keyboard and reset"' in SPEC
