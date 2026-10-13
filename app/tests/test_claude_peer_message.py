import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SUITE = ROOT / "conformance" / "claude-peer" / "v1"
sys.path.insert(0, str(ROOT / "tools"))

import claude_peer_message as peer  # noqa: E402

FIXTURE = json.loads((SUITE / "fixture.json").read_text(encoding="utf-8"))


def test_fixture_is_current():
    subprocess.run([sys.executable, str(SUITE / "generate_fixture.py"), "--check"], check=True)


def test_rebuilds_the_recorded_claude_code_frame():
    recorded = FIXTURE["recorded"]
    assert peer.frame(recorded["token"], recorded["message"]).decode("utf-8") == recorded["frame"]
    token, frames = peer.parse_frame(recorded["frame"].encode("utf-8"))
    assert token == recorded["token"]
    assert frames[0]["type"] == "user"
    assert frames[0]["message"]["content"].startswith("<cross-session-message from=")


def test_frames_escapes_keys_and_modes_match_the_fixture():
    for case in FIXTURE["frames"]:
        assert peer.frame(case["token"], case["message"]).decode("utf-8") == case["frame"], case["id"]
    for case in FIXTURE["escapes"]:
        assert peer.escape_body(case["text"]) == case["body"]
    for case in FIXTURE["keys"]:
        assert peer.key_path(case["configDir"], case["pid"], case["socketPath"]) == case["keyPath"]
    with tempfile.TemporaryDirectory() as tmp:
        for case in FIXTURE["modes"]:
            path = ""
            if case["transcript"]:
                path = os.path.join(tmp, case["id"] + ".jsonl")
                Path(path).write_text(case["transcript"], encoding="utf-8")
            assert peer.permission_class(case["argv"], path) == case["expect"], case["id"]


def test_a_steer_opens_with_the_lead_and_displays_without_it():
    assert FIXTURE["steerLead"] == peer.STEER_LEAD
    assert peer.steer_body("Yes") == peer.STEER_LEAD + "\n\nYes"
    assert peer.steer_text(peer.steer_body("Yes")) == "Yes"
    assert peer.steer_text("ping from a") == "ping from a"


def test_inbound_policy_matches_the_fixture():
    for case in FIXTURE["inbound"]:
        settings = case["settings"]
        got = peer.inbound_policy(*(settings.get(k, "") for k in ("managed", "flag", "user", "local", "project")),
                                  case["receiverClass"])
        assert got == (case["policy"], case["cause"]), case["id"]
    for case in FIXTURE["settingsFiles"]:
        assert peer.inbound_setting(case["json"].encode()) == case["value"]


def test_predicts_the_hold_from_the_settings_files():
    with tempfile.TemporaryDirectory() as tmp:
        config = os.path.join(tmp, "cfg")
        cwd = os.path.join(tmp, "repo", "sub")
        os.makedirs(config)
        os.makedirs(os.path.join(cwd, ".claude"))
        os.makedirs(os.path.join(tmp, "repo", ".git"))
        session = {"configDir": config, "cwd": cwd}
        managed = os.path.join(tmp, "managed-settings.json")
        assert peer.predict_inbound(session, ["claude"], "bypass", managed) == ("hold", "no-mode-asserted")
        Path(config, "settings.json").write_text('{"crossSessionInbound": "accept"}')
        assert peer.predict_inbound(session, ["claude"], "bypass", managed) == ("accept", "explicit-setting")
        Path(tmp, "repo", ".claude").mkdir()
        Path(tmp, "repo", ".claude", "settings.local.json").write_text('{"crossSessionInbound": "hold"}')
        assert peer.predict_inbound(session, ["claude"], "bypass", managed) == ("hold", "repo-setting")
        assert peer.flag_inbound_setting(["claude", "--settings", '{"crossSessionInbound":"accept"}']) == "accept"


def _record(pid, proc_start, sock, protocol=1, session_id="sid"):
    data = {"pid": pid, "sessionId": session_id, "cwd": "/tmp/work", "name": "work", "status": "idle",
            "entrypoint": "cli", "messagingSocketPath": sock}
    if proc_start:
        data["procStart"] = proc_start
    if protocol is not None:
        data["peerProtocol"] = protocol
    return json.dumps(data)


def test_discovers_live_sessions_and_skips_stale_ones():
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        config = os.path.join(tmp, "claude")
        sessions = os.path.join(config, "sessions")
        os.makedirs(sessions)
        live = os.path.join(tmp, "live.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(live)
        server.listen(2)
        me, parent = os.getpid(), os.getppid()
        Path(sessions, "%d.json" % me).write_text(_record(me, peer.proc_start(me), live, session_id="sid-live"))
        Path(sessions, "%d.json" % parent).write_text(_record(parent, peer.proc_start(parent), os.path.join(tmp, "gone.sock")))
        Path(sessions, "4194305.json").write_text(_record(4194305, "1", live))
        Path(sessions, "1.json").write_text(_record(1, "", live, protocol=None))
        Path(sessions, "junk.json").write_text("{}")
        token = "0123456789abcdef0123456789abcdef"
        Path(peer.key_path(config, me, live)).write_text(json.dumps({"peerToken": token}))
        found = peer.read_sessions(config)
        assert [s["pid"] for s in found] == [me]
        recycled = os.path.join(tmp, "recycled")
        os.makedirs(os.path.join(recycled, "sessions"))
        Path(recycled, "sessions", "%d.json" % me).write_text(_record(me, "1", live))
        assert peer.read_sessions(recycled) == []

        received = []

        def serve():
            conn, _ = server.accept()
            chunks = []
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
            conn.close()
            received.append(b"".join(chunks))

        reader = threading.Thread(target=serve)
        reader.start()
        peer.deliver(found[0], "Also run the tests")
        reader.join(5)
        server.close()
        got_token, frames = peer.parse_frame(received[0])
        assert got_token == token
        assert frames[0]["priority"] == "next"
        assert "from" not in frames[0]
        assert frames[0]["message"]["content"] == (
            '<cross-session-message from-name="ForkMesh (phone)">\n'
            + peer.STEER_LEAD + "\n\nAlso run the tests\n</cross-session-message>")
