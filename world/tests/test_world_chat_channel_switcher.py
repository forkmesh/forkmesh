import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORLD_DIR = ROOT / "world" / "public" / "world"
APP = (WORLD_DIR / "world.js").read_text(encoding="utf-8")
CSS = (WORLD_DIR / "world.css").read_text(encoding="utf-8")
CHAT = (ROOT / "app" / "public" / "dashboard-chat.js").read_text(encoding="utf-8")


def _between(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


ISLAND = _between(APP, '<section\n          class="world-hud-island"', "</section>")
HELPERS = _between(
    APP, "const WORLD_CHAT_CHANNEL_KEY", "function orgAgentSavedMessage(",
)


def _run(body):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", HELPERS + "\n" + body],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def _method(name):
    return _between(APP, f"\n  {name}(", "\n  }\n")


def test_switcher_sits_between_the_mode_toggle_and_the_input_as_a_listbox():
    order = [
        "data-world-composer-modes",
        "data-world-chat-channel-picker",
        "data-world-chat-channel ",
        "data-world-chat-channel-menu",
        'id="fullChatInput"',
        'id="fullChatSend"',
    ]
    positions = [ISLAND.index(marker) for marker in order]
    assert positions == sorted(positions)
    button = _between(ISLAND, "<button type=\"button\" class=\"world-hud-channel-button\"", "</button>")
    assert 'aria-haspopup="listbox"' in button
    assert 'aria-expanded="false"' in button
    assert 'aria-controls="world-chat-channel-menu"' in button
    assert "<span data-world-chat-channel-label>#general</span>" in button
    menu = _between(ISLAND, 'id="world-chat-channel-menu"', "</div>")
    assert 'role="listbox"' in menu
    assert 'aria-label="Channel"' in menu
    assert "data-world-chat-channel-menu hidden" in menu
    assert "data-world-island-full-chat" in ISLAND
    assert ISLAND.count("<button id=") == 1


SPACES = """
const rooms = [
  { space: "", channel: "#general" },
  { space: "neighborhood", channel: "#world-neighborhood" },
  { space: "broadcast", channel: "#world-broadcast" },
];
"""


def test_space_choices_match_the_chat_rooms_and_need_sign_in_beyond_general():
    data = _run(
        SPACES + """
        const guest = worldChatSpaceChoices(rooms, false);
        const member = worldChatSpaceChoices(rooms, true);
        const fallback = worldChatSpaceChoices(null, true);
        const live = worldChatSpaceChoices(
          [{ space: "", channel: "#general" },
           { space: "sky-campus", channel: "#world-sky-campus" },
           { space: "../x", channel: "#bad" },
           { space: "code-planet", channel: "not-a-channel" }],
          true,
        );
        console.log(JSON.stringify({ guest, member, fallback, live }));
        """
    )
    assert data["guest"] == [
        {"id": "space:", "kind": "space", "space": "", "label": "#general"}
    ]
    assert [choice["label"] for choice in data["member"]] == [
        "#general",
        "#world-neighborhood",
        "#world-broadcast",
    ]
    assert data["fallback"] == data["guest"]
    assert [choice["id"] for choice in data["live"]] == ["space:", "space:sky-campus"]


def test_private_channels_and_dms_are_bounded_and_validated():
    data = _run(
        """
        const id = (n) => n.toString(16).padStart(32, "0");
        const channels = [
          { id: id(1), name: "design", visibility: "private" },
          { id: id(2), name: "notices", readOnly: true },
          { id: "nope", name: "bad" },
          { id: id(3), name: "" },
          ...Array.from({ length: 20 }, (_, i) => ({ id: id(100 + i), name: `c${i}` })),
        ];
        const directs = [
          { id: id(4), otherUser: "alice", unreadCount: 3 },
          { id: id(5), otherUser: "Bad User" },
          { id: id(6), otherUser: "bob" },
        ];
        console.log(JSON.stringify(worldChatPrivateChoices(channels, directs)));
        """
    )
    assert data["channels"][0] == {
        "id": "channel:" + "1".rjust(32, "0"),
        "kind": "channel",
        "roomId": "1".rjust(32, "0"),
        "label": "#design",
        "visibility": "private",
    }
    assert "#notices" not in [choice["label"] for choice in data["channels"]]
    assert len(data["channels"]) == 12
    assert [choice["label"] for choice in data["directs"]] == ["@alice", "@bob"]
    assert data["directs"][0]["unread"] == 3


def test_saved_channel_falls_back_to_general_when_it_is_gone():
    data = _run(
        SPACES + """
        const member = worldChatSpaceChoices(rooms, true);
        const guest = worldChatSpaceChoices(rooms, false);
        console.log(JSON.stringify([
          worldChatPickSpace(member, "broadcast"),
          worldChatPickSpace(member, "retired-space"),
          worldChatPickSpace(guest, "broadcast"),
          worldChatPickSpace([], "broadcast"),
        ]));
        """
    )
    assert data == ["broadcast", "", "", ""]


def test_switching_a_space_swaps_the_room_in_place_and_remembers_it():
    select = _method("selectChatChannel")
    assert "this.rememberChatChannel(target);" in select
    assert "window.ForkMeshDashboardChat?.switchSpace?.(target);" in select
    assert "this.syncComposerCopy();" in select
    assert 'window.open("/chat", "_blank", "noopener,noreferrer");' in select
    assert "WORLD_CHAT_PRIVATE_DIRECT_KEY" in select
    assert 'const WORLD_CHAT_PRIVATE_CHANNEL_KEY = "forkmesh.chat.channel";' in APP
    assert 'const WORLD_CHAT_PRIVATE_DIRECT_KEY = "forkmesh.chat.direct";' in APP
    remember = _method("rememberChatChannel")
    assert "localStorage.setItem(" in remember and "} catch (_) {}" in remember
    saved = _method("chatChannelSpace")
    assert "localStorage.getItem(WORLD_CHAT_CHANNEL_KEY)" in saved
    assert "catch (_)" in saved
    assert 'const WORLD_CHAT_CHANNEL_KEY = "forkmesh.world.chatChannel.v1";' in APP
    load = _method("loadNativeWorldChat")
    assert "validWorldSession() && WORLD_CHAT_SPACE_RE.test(savedSpace)" in load
    assert load.index("window.FORKMESH_CHAT_SPACE") < load.index("document.head.append(script)")


def test_composer_copy_names_the_current_channel():
    copy = _method("syncComposerCopy")
    assert "const channel = this.activeChatChannelLabel();" in copy
    assert "`Message ${channel}…`" in copy
    assert "`Send to ${channel}`" in copy
    assert "Message #general" not in copy
    assert "fullInput.placeholder = `Message ${CHANNEL}…`;" in CHAT


def test_private_choices_load_lazily_from_the_chat_apis_when_signed_in():
    load = _between(APP, "\n  async loadChatChannelChoices(", "\n  }\n")
    assert 'read("/api/chat/channels")' in load
    assert "read(`/api/chat/direct-messages?limit=${WORLD_CHAT_PRIVATE_LIMIT}`)" in load
    assert "if (!this.chatSignedIn())" in load
    assert "WORLD_CHAT_CHOICES_TTL_MS" in load
    opener = _method("setChatChannelMenuOpen")
    assert "void this.loadChatChannelChoices();" in opener
    assert 'button.setAttribute("aria-expanded", String(active));' in opener


def test_menu_is_keyboard_driven_and_closes_like_the_model_menu():
    bind = _between(APP, 'this.$("[data-world-chat-channel]")?.addEventListener("click"', 'this.$("[data-world-agent-model]")?.addEventListener("click"')
    for key in ('"ArrowDown"', '"ArrowUp"', '"Home"', '"End"', '"Escape"', '"Tab"'):
        assert key in bind
    assert "this.setChatChannelMenuOpen(false, event.key === \"Escape\");" in bind
    escape = _between(APP, "handleQuickChatEscape = (event) => {", "\n  };\n")
    assert "this.setChatChannelMenuOpen(false, true);" in escape
    assert '!event.target.closest("[data-world-chat-channel-picker]")' in APP
    mode = _method("setComposerMode")
    assert "if (agent) this.setChatChannelMenuOpen(false);" in mode


def test_switcher_is_chat_only_and_compact_on_phones():
    assert ".fm-world .world-hud-island[data-composer-mode=\"agent\"] .world-hud-channel {\n  display: none;" in CSS
    assert "grid-template-columns: auto auto minmax(0, 1fr) auto;" in CSS
    phone = _between(CSS, "@media (max-width: 600px) {\n  .fm-world {\n    --world-hud-island-gap", "\n}\n")
    assert ".fm-world .world-hud-channel-button {\n    max-width: 84px;" in phone
    button = _between(CSS, ".fm-world .world-hud-channel-button {", "\n}")
    assert "max-width: 150px;" in button
    assert "text-overflow: ellipsis;" in CSS


def test_dashboard_chat_switches_rooms_without_a_reload():
    switch = _between(CHAT, "  function switchChatSpace(space) {", "\n  }\n")
    assert "if (requestedOrganization) return false;" in switch
    assert "roomGeneration += 1;" in switch
    assert "previous.close();" in switch
    assert "configureChatSpace(next);" in switch
    for reset in ("roomKey = null;", "roomPassphrase = null;", "roomSeal = null;", "seen.clear();", "rows.clear();", "historyRowIds.length = 0;"):
        assert reset in switch
    assert "connect();" in switch
    connect = _between(CHAT, "  async function connect() {", "\n  }\n")
    assert "const generation = roomGeneration;" in connect
    assert "if (generation !== roomGeneration) {" in connect
    assert "if (socket !== opened) return;" in connect
    assert "switchSpace: (space) => Boolean(dashboardChatController?.switchSpace(space))" in CHAT
    assert 'mountForkMeshDashboardChat({ space: String(window.FORKMESH_CHAT_SPACE || "") });' in CHAT
    assert 'requestedParams.get("space") || String(options?.space || "")' in CHAT
