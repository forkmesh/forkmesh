import functools
import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world" / "public" / "world"
APP = ROOT / "app" / "public"
WORLD_JS = (WORLD / "world.js").read_text(encoding="utf-8")
VOICE = (WORLD / "world-voice.js").read_text(encoding="utf-8")
PUPPETS = (WORLD / "world-avatar-puppets.js").read_text(encoding="utf-8")
CORE = (WORLD / "scene" / "core.js").read_text(encoding="utf-8")
ANIMATE = (WORLD / "scene" / "runtime" / "animate.js").read_text(encoding="utf-8")
MEETING = (WORLD / "world-office-meeting.js").read_text(encoding="utf-8")
CSS = (WORLD / "world.css").read_text(encoding="utf-8")

HARNESS = r'''
import { pathToFileURL } from "node:url";
const base = process.argv[1];
const rooms = await import(pathToFileURL(base + "/world-voice-rooms.js").href);
const channels = await import(pathToFileURL(base + "/world-channels.js").href);
const players = new Map([
  ["p1", { name: "alice", voice: "world", mic: true, cam: true, words: 12 }],
  ["p2", { name: "bob", voice: "adhoc:0123456789abcdef", voiceName: "Design jam", mic: false }],
  ["p3", { name: "cara", voice: "bubble:aaaaaaaaaaaaaaaa" }],
  ["p4", { name: "dan", voice: "bubble:aaaaaaaaaaaaaaaa" }],
  ["p5", { name: "eve", voice: "" }],
]);
const self = { room: { id: "adhoc:0123456789abcdef", name: "Design jam" }, mic: true, name: "tester", words: 3 };
const out = {};
out.rooms = rooms.voiceRoomsFromPlayers(players, self).map((room) => [room.id, room.name, room.members.map((m) => m.name)]);
out.bubbles = rooms.bubbleGroups(players, self);
out.bubblesWithSelf = rooms.bubbleGroups(players, { room: { id: "bubble:aaaaaaaaaaaaaaaa" } });
out.labels = ["world", "space:sky-campus", "bubble:aaaaaaaaaaaaaaaa", { id: "adhoc:0123456789abcdef", name: "" }].map((room) => rooms.worldVoiceRoomLabel(room, players));
out.names = ["  Late   night ", "<b>x</b>", "Café 2", ""].map(rooms.cleanVoiceRoomName);
out.valid = ["world", "space:sky-campus", "adhoc:0123456789abcdef", "bubble:0123456789abcdef", "adhoc:xyz", "general"].map((id) => rooms.WORLD_VOICE_ROOM_RE.test(id));
out.tag = await rooms.bubbleTag("0".repeat(32));
const sections = channels.unifiedChannelList({
  spaces: [{ id: "space:", space: "", label: "#general" }, { id: "space:sky-campus", space: "sky-campus", label: "#world-sky-campus" }],
  channels: [{ roomId: "a".repeat(32), label: "#design", visibility: "private" }],
  directs: [{ roomId: "b".repeat(32), label: "@alice", unread: 2 }],
  rooms: rooms.voiceRoomsFromPlayers(players, self),
  selfRoom: self.room.id,
});
out.sections = sections.map((section) => [section.id, section.rows.map((row) => [row.kind, row.label, row.voice || "", (row.members || []).length, Boolean(row.joined), Boolean(row.locked)])]);
out.urls = [channels.conversationUrl({ kind: "channel", roomId: "a".repeat(32) }), channels.conversationUrl({ kind: "dm", roomId: "b".repeat(32) }), channels.conversationUrl({ kind: "channel", roomId: "../x" })];
console.log(JSON.stringify(out));
'''


@functools.cache
def _run():
    done = subprocess.run(["node", "--input-type=module", "-e", HARNESS, str(WORLD)], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr[-2000:]
    return json.loads(done.stdout)


def test_voice_rooms_and_bubbles_come_from_presence():
    data = _run()
    assert data["rooms"] == [
        ["world", "", ["alice"]],
        ["adhoc:0123456789abcdef", "Design jam", ["bob", "tester"]],
        ["bubble:aaaaaaaaaaaaaaaa", "", ["cara", "dan"]],
    ]
    assert data["bubbles"] == [{"tag": "bubble:aaaaaaaaaaaaaaaa", "members": ["p3", "p4"]}]
    assert data["bubblesWithSelf"] == [{"tag": "bubble:aaaaaaaaaaaaaaaa", "members": ["p3", "p4", "self"]}]
    assert data["labels"] == ["#general · whole World", "#world-sky-campus", "Privacy bubble", "Design jam"]
    assert data["names"] == ["Late night", "", "Café 2", ""]
    assert data["valid"] == [True, True, True, True, False, False]


def test_bubble_tags_match_the_server_hash():
    expected = hashlib.sha256(("forkmesh-world-bubble-v1\n" + "0" * 32).encode()).hexdigest()[:16]
    assert _run()["tag"] == expected


def test_one_list_holds_world_voice_office_channels_and_messages():
    sections = dict(_run()["sections"])
    assert list(sections) == ["world", "voice", "office", "channels", "directs"]
    assert sections["world"] == [["space", "#general", "world", 1, False, False], ["space", "#world-sky-campus", "space:sky-campus", 0, False, False]]
    assert sections["voice"] == [
        ["voice", "Design jam", "adhoc:0123456789abcdef", 2, True, False],
        ["bubble", "Privacy bubble", "bubble:aaaaaaaaaaaaaaaa", 2, False, True],
    ]
    assert sections["office"][0][0] == "office"
    assert sections["channels"] == [["channel", "#design", "", 0, False, False]]
    assert sections["directs"] == [["dm", "@alice", "", 0, False, False]]
    assert _run()["urls"] == ["/chat?embed=world#room=" + "a" * 32, "/chat?embed=world#direct=" + "b" * 32, ""]


def _app_closure():
    imports = re.compile(r'''(?:import\s[^"';]*?from\s*|import\s*\(\s*|import\s*)["']([^"']+)["']''')
    loose = re.compile(r'''["'](/[A-Za-z0-9_./-]+\.js)["']''')
    todo = [WORLD / name for name in ("world-voice.js", "world-avatar-puppets.js", "world-channels.js")]
    seen, found = set(), set()
    while todo:
        path = todo.pop()
        if path in seen or not path.exists():
            continue
        seen.add(path)
        text = path.read_text(encoding="utf-8")
        specs = set(imports.findall(text))
        if APP in path.parents:
            specs |= {spec for spec in loose.findall(text) if (APP / spec.lstrip("/")).exists()}
        for spec in specs:
            if spec.startswith("/"):
                target = APP / spec.lstrip("/")
            elif spec.startswith("."):
                target = (path.parent / spec).resolve()
            else:
                continue
            if APP.resolve() in target.resolve().parents:
                found.add("/" + target.resolve().relative_to(APP.resolve()).as_posix())
            todo.append(target)
    return found


def test_every_app_module_the_world_voice_uses_is_routed_to_the_app():
    needed = _app_closure()
    assert {"/chat-group-calls.js", "/call-avatar-rig.js", "/chat-call-hand-tracker.js", "/chat-group-call-crypto.js", "/chat-call-voice-worklet.js"} <= needed
    for path in (ROOT / "world" / "worker.js", ROOT / "world" / "wrangler.toml", ROOT / "deploy" / "core" / "caddy_routes.py"):
        text = path.read_text(encoding="utf-8")
        missing = sorted(module for module in needed if f'"{module}"' not in text)
        assert not missing, f"{path.name} is missing {missing}"


def test_world_shell_routes_voice_frames_and_publishes_voice_presence():
    send = WORLD_JS.split("\n  sendVoiceFrame(frame) {", 1)[1].split("\n  }\n", 1)[0]
    assert '["bubble-invite", "bubble-knock", "bubble-decline", "reaction"]' in send
    assert 'type !== "rig" && type !== "voice-signal"' in send
    assert "JSON.stringify(Object.assign({ type }, frame))" in send
    assert "...this.voicePresence()," in WORLD_JS
    receive = WORLD_JS.split("\n  receivePresence(message) {", 1)[1].split("\n  syncWorldVoicePeers() {", 1)[0]
    for marker in ('message.type === "rig"', 'message.type === "voice-signal"', 'message.kind === "reaction"', '"bubble-invite", "bubble-knock", "bubble-decline"', "this.avatarPuppets?.forget(departed);"):
        assert marker in receive
    for field in ("voice:", "voiceName: cleanVoiceRoomName(peer.voiceName)", "mic: peer.mic === true", "cam: peer.cam === true", "words:"):
        assert field in WORLD_JS
    for tool in ("voice-mic", "voice-camera", "voice-panel"):
        assert f'data-world-tool="{tool}"' in WORLD_JS
    assert "data-world-voice-perf" in WORLD_JS and ".fm-world .world-voice-perf {" in CSS
    assert "this.voice?.destroy();" in WORLD_JS
    assert 'import("./world-voice.js")' in WORLD_JS and 'import("./world-channels.js")' in WORLD_JS


def test_avatars_with_a_live_rig_stay_full_detail_and_animate_every_frame():
    assert "Boolean(avatar.userData.voiceRig)" in CORE
    assert "avatar.userData.wordBadge?.visible === true" in CORE
    assert "fns.updateAvatarVoice?.(delta, time);" in ANIMATE
    for api in ("setAvatarRig,", "setAvatarFaceCanvas,", "setAvatarSpeech,", "setPrivacyBubbles,", "playAvatarReaction,"):
        assert CORE.count(api) == 2


def test_puppets_paint_live_faces_and_reactions_use_one_vocabulary():
    assert "paintProceduralAvatarFace(context," in PUPPETS
    assert "createFacePuppet()" in PUPPETS
    assert 'import { RIG_REACTIONS, RIG_TEMPLATE, decodeRig, rigTemplatePoints } from "/call-avatar-rig-wire.js";' in PUPPETS
    assert "RIG_REACTIONS[kind]" in VOICE


def test_office_lobby_loads_channel_meeting_rooms():
    lobby = MEETING.split("function openLobby() {", 1)[1].split("\n  }\n", 1)[0]
    assert "void fetchRooms();" in lobby
    join = MEETING.split("async function joinRoom(roomId) {", 1)[1][:300]
    assert "await fetchRooms();" in join


def test_diagnostic_reports_go_to_the_bug_inbox_with_measurements_attached():
    report = VOICE.split("async function sendDiagnostics(", 1)[1].split("\n  function setHands(", 1)[0]
    assert '"/api/bug-reports"' in report
    assert 'screen: "world-voice"' in report
    assert 'name: "world-voice-diagnostics.json"' in report


def test_camera_stays_on_for_video_when_face_tracking_cannot_start():
    start = VOICE.index("async function startCapture()")
    body = VOICE[start:VOICE.index("function stopCapture()", start)]
    assert "await capture.ready" in body and "catch (error)" in body
    fallback = body.index("if (state.sendVideo && camStream)")
    assert "return true;" in body[fallback:fallback + 200]
    assert "if (capture || camStream) stopCapture();" in VOICE


def test_camera_refusal_names_the_reason():
    assert 'host.notify(cameraErrorMessage(error));' in VOICE
    for name in ("NotAllowedError", "NotReadableError", "NotFoundError"):
        assert name in VOICE
