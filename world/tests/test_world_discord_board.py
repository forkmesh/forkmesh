"""Contracts for the physical Discord connector board in the Boards Circle."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SCENE = world_scene_source()
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")


def test_discord_board_is_aligned_with_the_existing_billboard_circle():
    for contract in (
        "function worldDiscordBoardTexture(THREE)",
        'worldDiscordBoard.name = "forkmesh-world-discord-board"',
        'worldDiscordFrame.userData.interactive = "world-discord-board"',
        'worldDiscordFace.userData.interactive = "world-discord-board"',
        'placeBillboardOnIsland(worldDiscordBoard, { circle: "social" })',
    ):
        assert contract in SCENE


def test_clicking_discord_board_opens_the_connector_ui():
    assert "onWorldDiscordBoardSelect = () => {}" in SCENE
    assert "onWorldDiscordBoardSelect();" in SCENE
    assert 'new CustomEvent("forkmesh:open-discord")' in WORLD

