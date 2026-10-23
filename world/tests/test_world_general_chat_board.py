"""Contracts for the in-world recent #general message board."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
PUBLIC = ROOT / "public"
WORLD_PUBLIC = ROOT.parent / "world" / "public" / "world"
CHAT = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
WORLD = (WORLD_PUBLIC / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


def test_chat_embed_forwards_bounded_history_and_thumbnail_metadata():
    for contract in (
        "function worldAttachmentPreview(attachment)",
        'canvas.toDataURL("image/webp", 0.7)',
        "preview.length <= 120_000",
        'type: "forkmesh:world-chat"',
        "attachmentName:",
        "attachmentMime:",
        "reactionCount:",
        "attachmentPreview",
        "emitNewestWorldChatHistory()",
    ):
        assert contract in CHAT
    assert "if (ts < newestHistoryTs) return" not in CHAT
    assert "if (meta?.attachment && !history)" in CHAT


def test_world_accepts_chat_only_from_its_native_same_window_controller():
    for contract in (
        'this.$("[data-world-native-chat]") ? [window] : []',
        "if (!chatSources.includes(event.source)) return",
        '"forkmesh:world-chat-native"',
        "this.recentWorldChatMessages.slice(-40)",
        "this.world?.updateWorldGeneralChat?.(",
        "attachmentPreview.length <= 120_000",
    ):
        assert contract in WORLD


def test_native_world_chat_never_posts_to_the_cross_origin_page_parent():
    dispatch = CHAT[
        CHAT.index("function dispatchWorldChatPayload"):
        CHAT.index("dispatchWorldChatPayload({ type: \"forkmesh:chat-ready\" })")
    ]
    assert dispatch.index('document.querySelector("[data-world-native-chat]")') < (
        dispatch.index("window.parent !== window")
    )
    assert 'new CustomEvent("forkmesh:world-chat-native"' in dispatch
    assert "window.parent.postMessage(payload, location.origin)" in dispatch
    assert "dispatchWorldChatPayload({ ...payload, attachmentPreview })" in CHAT
    assert 'type: "forkmesh:world-activity"' not in CHAT
    assert "dispatchWorldEmote(emote)" in CHAT


def test_physical_chat_board_is_beside_events_and_opens_full_chat():
    for contract in (
        "function worldGeneralChatTexture(THREE, messages = [])",
        '"#GENERAL · RECENT CHAT"',
        '"forkmesh-world-general-chat-board"',
        '"forkmesh-world-general-chat-board-face"',
        '"world-general-chat-board"',
        'placeBillboardOnIsland(worldGeneralChatBoard, { circle: "hub" });',
        "function updateWorldGeneralChat(messages = [])",
        "updateWorldGeneralChat,",
        "onWorldGeneralChatSelect()",
    ):
        assert contract in SCENE
    assert 'onWorldGeneralChatSelect: () => {' in WORLD
    assert 'this.openWorldChat("/chat")' in WORLD
