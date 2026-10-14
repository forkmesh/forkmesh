#!/usr/bin/env python3

import hashlib
import json
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
DASHBOARD_CHAT = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
WELCOME = (PUBLIC / "member-welcome.js").read_text(encoding="utf-8")


def _everyone_id(name):
    return hashlib.sha256(
        f"forkmesh-everyone-channel-v1:{name}".encode()).hexdigest()[:32]


GENERAL_ID = _everyone_id("general")
RANDOM_ID = _everyone_id("random")
WELCOME_ID = _everyone_id("welcome")
CUSTOM_ID = "c" * 32


def _function_source(name):
    match = re.search(
        r"^(?:async )?function " + re.escape(name) + r"\(.*?^}\n", CHAT,
        re.S | re.M)
    assert match, name
    return match.group(0)


def _const_source(name):
    match = re.search(r"^const " + re.escape(name) + r" = .*?;\n", CHAT, re.M)
    assert match, name
    return match.group(0)


def _run(script):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
        cwd=PUBLIC,
    )
    return json.loads(result.stdout)


def _harness(steps, signed_in=True):
    helpers = "\n".join(_function_source(name) for name in (
        "isPrivateChannelKey",
        "privateChannelKey",
        "privateChannelForKey",
        "isDirectMessageKey",
        "directMessageKey",
        "directMessageForKey",
        "channelDisplayLabel",
        "channelWireLabel",
        "isEveryoneChannel",
        "defaultChannelKey",
        "channelListRank",
        "frameMatchesScope",
        "reconcilePrivateChannels",
        "refreshEveryoneChannelMembers",
        "activeRoomMembers",
        "noteRoster",
    ))
    consts = "\n".join(_const_source(name) for name in (
        "PRIVATE_CHANNELS_ENDPOINT",
        "EVERYONE_CHANNEL_ORDER",
        "EVERYONE_MEMBERS_REFRESH_MS",
    ))
    session = '({ nodeName: "jett" })' if signed_in else "null"
    return _run(f"""
      {consts}
      const MAX_NAME = 32;
      const PEER_STALE_MS = 180000;
      const FORKBOT_SENDER_ID = "forkbot";
      const userSession = () => {session};
      const privateChannels = new Map();
      const directMessages = new Map();
      const everyoneChannelMembers = new Map();
      const channelMeta = new Map();
      const roster = new Map();
      const requests = [];
      const selections = [];
      let activeChannel = "#general";
      let defaultChannelChosen = false;
      let savedPrivateChannelId = "";
      const localStorage = {{ removeItem() {{}} }};
      const ensureChannel = (key) => {{ channelMeta.set(key, {{ unread: 0 }}); return key; }};
      const releaseChannelMessages = (key) => channelMeta.delete(key);
      const setActiveChannel = (key) => {{
        selections.push(key);
        activeChannel = key;
        defaultChannelChosen = true;
      }};
      const switchChatRoom = () => {{}};
      const updateAdminChannelControls = () => {{}};
      const renderRooms = () => {{}};
      const schedulePeopleRender = () => {{}};
      const privateChannelRequest = async (path) => {{
        requests.push(path);
        return {{ everyone: true, members: [
          {{ username: "Ana", joinedAt: 1 }},
          {{ username: "jett", joinedAt: 2 }},
          {{ username: "ana", joinedAt: 3 }},
          {{ username: "", joinedAt: 4 }},
        ] }};
      }};
      const everyone = (id, name, memberCount) => ({{
        id, name, kind: "everyone", visibility: "public", canManage: true,
        memberCount, topic: "", keyVersion: 1, updatedAt: 0,
      }});
      const list = [
        everyone({json.dumps(GENERAL_ID)}, "general", 42),
        everyone({json.dumps(RANDOM_ID)}, "random", 42),
        everyone({json.dumps(WELCOME_ID)}, "welcome", 42),
        {{ id: {json.dumps(CUSTOM_ID)}, name: "alpha", visibility: "public",
          canManage: true, keyVersion: 1 }},
      ];
      {helpers}
      {steps}
    """)


def test_relay_room_is_labelled_world_but_keeps_its_wire_name():
    result = _harness("""
      reconcilePrivateChannels(list, { selectSaved: false });
      const general = privateChannelKey(list[0].id);
      const welcome = privateChannelKey(list[2].id);
      console.log(JSON.stringify({
        worldLabel: channelDisplayLabel("#general"),
        worldWire: channelWireLabel("#general"),
        generalLabel: channelDisplayLabel(general),
        generalWire: channelWireLabel(general),
        welcomeWire: channelWireLabel(welcome),
      }));
    """)
    assert result == {
        "worldLabel": "World",
        "worldWire": "#general",
        "generalLabel": "#general",
        "generalWire": "#general",
        "welcomeWire": "#welcome",
    }


def test_everyone_channels_list_first_then_world_then_other_channels():
    result = _harness("""
      reconcilePrivateChannels([...list].reverse(), { selectSaved: false });
      const order = ["#general", ...channelMeta.keys()]
        .sort((a, b) => channelListRank(a) - channelListRank(b) ||
          channelDisplayLabel(a).localeCompare(channelDisplayLabel(b)))
        .map((key) => channelDisplayLabel(key));
      const records = list.map((value) => privateChannels.get(value.id));
      console.log(JSON.stringify({
        order,
        manage: records.map((record) => record.canManage),
        kinds: records.map((record) => record.kind),
        counts: records.map((record) => record.memberCount),
      }));
    """)
    assert result["order"] == [
        "#general", "#random", "#welcome", "World", "#alpha"]
    assert result["manage"] == [False, False, False, True]
    assert result["kinds"] == ["everyone", "everyone", "everyone", "channel"]
    assert result["counts"] == [42, 42, 42, 0]


def test_signed_in_user_lands_on_server_general_once():
    result = _harness(f"""
      reconcilePrivateChannels(list, {{ connect: false }});
      const first = [...selections];
      activeChannel = "#general";
      reconcilePrivateChannels(list, {{}});
      console.log(JSON.stringify({{
        first,
        after: selections.length,
        fallback: defaultChannelKey(),
      }}));
    """)
    assert result["first"] == ["private:" + GENERAL_ID]
    assert result["after"] == 1
    assert result["fallback"] == "private:" + GENERAL_ID


def test_background_refresh_does_not_move_the_reader():
    result = _harness("""
      reconcilePrivateChannels(list, { selectSaved: false });
      console.log(JSON.stringify({ selections }));
    """)
    assert result["selections"] == []


def test_guests_stay_on_world():
    result = _harness("""
      reconcilePrivateChannels(list, {});
      console.log(JSON.stringify({ selections, fallback: defaultChannelKey() }));
    """, signed_in=False)
    assert result == {"selections": [], "fallback": "#general"}


def test_everyone_members_come_from_the_members_endpoint():
    result = _harness("""
      reconcilePrivateChannels(list, { selectSaved: false });
      const general = privateChannels.get(list[0].id);
      activeChannel = privateChannelKey(general.id);
      const before = activeRoomMembers();
      await refreshEveryoneChannelMembers(general);
      await refreshEveryoneChannelMembers(general);
      await refreshEveryoneChannelMembers(privateChannels.get(list[3].id));
      console.log(JSON.stringify({
        before,
        members: activeRoomMembers(),
        requests,
      }));
    """)
    assert result["before"] is None
    assert result["members"] == ["ana", "jett"]
    assert result["requests"] == [f"/api/chat/channels/{GENERAL_ID}/members"]


def test_server_welcome_line_reaches_welcome_and_is_not_a_person():
    result = _harness("""
      reconcilePrivateChannels(list, { selectSaved: false });
      const welcome = privateChannelKey(list[2].id);
      const frame = { type: "chat", id: "w1", senderId: "system:welcome",
        sender: "ForkMesh", accountKind: "user", channel: "#welcome",
        text: "Welcome", webhook: true, ts: 1 };
      noteRoster(frame);
      console.log(JSON.stringify({
        welcome: frameMatchesScope(frame, welcome),
        general: frameMatchesScope(frame, privateChannelKey(list[0].id)),
        roster: roster.size,
      }));
    """)
    assert result == {"welcome": True, "general": False, "roster": 0}


def test_frontend_wiring_for_everyone_channels():
    select = _function_source("setActiveChannel")
    assert "defaultChannelChosen = true;" in select
    assert "refreshEveryoneChannelMembers(privateChannelForKey(channel));" in select
    assert 'setActiveChannel(defaultChannelKey());' in _function_source(
        "reconcileDirectMessages")
    people = _function_source("renderPeople")
    assert "everyone.memberCount" in people
    assert "Every ForkMesh member is in this channel." in people
    rooms = _function_source("renderRooms")
    assert '"globe"' in rooms
    assert "World chat, open to visitors" in rooms
    assert "Every ForkMesh member is in this channel" in rooms
    assert "renderChatEntry" in CHAT
    entry = _function_source("renderChatEntry")
    assert "entry.channel !== channelWireLabel(scope)" in entry
    assert "Say hello in #welcome." in WELCOME
    assert "Say hello in #general." not in WELCOME
    chrome = DASHBOARD_CHAT[DASHBOARD_CHAT.index("function syncDiscordChrome()"):]
    chrome = chrome[:chrome.index("\n  }\n")]
    assert 'PUBLIC_WORLD_GENERAL\n      ? "World"' in chrome
