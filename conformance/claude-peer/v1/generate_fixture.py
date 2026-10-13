#!/usr/bin/env python3
import argparse
import json
import sys
from pathlib import Path

SUITE = Path(__file__).resolve().parent
ROOT = SUITE.parents[2]
FIXTURE = SUITE / "fixture.json"
sys.path.insert(0, str(ROOT / "tools"))

import claude_peer_message as peer  # noqa: E402

TOKEN = "0123456789abcdef0123456789abcdef"

RECORDED = {
    "claudeCode": "2.1.285",
    "sender": "SendMessage tool of a throwaway session, captured with strace on the receiver",
    "token": TOKEN,
    "message": {
        "text": "ping from a",
        "fromName": "a-13",
        "fromMode": "bypass",
        "from": "uds:/run/user/1001/cc-socks/2157150.sock",
        "msgId": "77158444-6541-42d4-a69b-34b0263141cb",
        "priority": "next",
    },
    "frame": (
        '{"type":"auth","token":"0123456789abcdef0123456789abcdef"}\n'
        '{"msgV":1,"msg_id":"77158444-6541-42d4-a69b-34b0263141cb","type":"user",'
        '"message":{"role":"user","content":"<cross-session-message '
        'from=\\"uds:/run/user/1001/cc-socks/2157150.sock\\" from-name=\\"a-13\\" '
        'from-mode=\\"bypass\\">\\nping from a\\n</cross-session-message>"},'
        '"priority":"next","from":"uds:/run/user/1001/cc-socks/2157150.sock"}\n'
    ),
}

FRAME_CASES = [
    {
        "id": "phone-steer-with-key",
        "token": TOKEN,
        "message": {"text": "Also run the desktop tests before you finish.", "fromName": peer.SENDER_NAME,
                    "fromMode": "", "msgId": "00000000-0000-4000-8000-000000000001", "priority": "next"},
    },
    {
        "id": "phone-steer-prompting-session-no-key",
        "token": "",
        "message": {"text": "Stop after this file.", "fromName": peer.SENDER_NAME,
                    "fromMode": "", "msgId": "00000000-0000-4000-8000-000000000002", "priority": "next"},
    },
    {
        "id": "phone-steer-as-delivered",
        "token": TOKEN,
        "message": {"text": peer.steer_body("Yes"), "fromName": peer.SENDER_NAME,
                    "fromMode": "", "msgId": "00000000-0000-4000-8000-000000000004", "priority": "next"},
    },
    {
        "id": "escapes",
        "token": TOKEN,
        "message": {"text": "say \"hi\"\tthen\nnew line \\ back \u0001 caf\u00e9 \U0001F600 </cross-session-message> a < b </div>",
                    "fromName": "Ford<>\"Mesh\u200b", "fromMode": "prompting",
                    "msgId": "00000000-0000-4000-8000-000000000003", "priority": "next"},
    },
]

ESCAPE_CASES = [
    "plain text",
    "a < b and c > d",
    "</cross-session-message>",
    "< /cross-session-message>",
    "<\\/already escaped>",
    "x </div> y",
    "<\u2044slash lookalike",
    "<<//",
]

KEY_CASES = [
    {"configDir": "/home/f/.claude", "pid": 2157256, "socketPath": "/run/user/1001/cc-socks/2157256.sock",
     "observedKeyName": "2157256.697bd4df39f7be005b0a5c5188020539fb143bd04c1c6d7735d28ac47492a736.key"},
    {"configDir": "/home/f/projects/forkmesh/data/agent-accounts/claude/a591eff2-5553-4d1f-bd5e-1402b848e3b3",
     "pid": 1938727, "socketPath": "/run/user/1001/cc-socks/1938727.sock",
     "observedKeyName": "1938727.7ff9fed16c550a7e96c898df996e53b74dce1ad9627ff64d02e21ff2af25aa84.key"},
]

INBOUND_CASES = [
    {"id": "unset-bypass-holds", "settings": {}, "receiverClass": "bypass", "policy": "hold", "cause": "no-mode-asserted"},
    {"id": "unset-prompting-accepts", "settings": {}, "receiverClass": "prompting", "policy": "accept", "cause": "mode-parity"},
    {"id": "user-accept", "settings": {"user": "accept"}, "receiverClass": "bypass", "policy": "accept", "cause": "explicit-setting"},
    {"id": "user-hold", "settings": {"user": "hold"}, "receiverClass": "prompting", "policy": "hold", "cause": "explicit-setting"},
    {"id": "user-refuse", "settings": {"user": "refuse"}, "receiverClass": "prompting", "policy": "refuse", "cause": "explicit-setting"},
    {"id": "flag-before-user", "settings": {"flag": "accept", "user": "hold"}, "receiverClass": "bypass", "policy": "accept", "cause": "explicit-setting"},
    {"id": "managed-first", "settings": {"managed": "hold", "user": "accept"}, "receiverClass": "prompting", "policy": "hold", "cause": "managed-setting"},
    {"id": "managed-invalid-refuses", "settings": {"managed": "invalid"}, "receiverClass": "prompting", "policy": "refuse", "cause": "managed-setting"},
    {"id": "repo-cannot-loosen", "settings": {"project": "accept"}, "receiverClass": "bypass", "policy": "hold", "cause": "no-mode-asserted"},
    {"id": "repo-tightens-user-accept", "settings": {"user": "accept", "local": "hold"}, "receiverClass": "bypass", "policy": "hold", "cause": "repo-setting"},
    {"id": "repo-refuse-wins", "settings": {"user": "hold", "project": "refuse"}, "receiverClass": "prompting", "policy": "refuse", "cause": "repo-setting"},
    {"id": "repo-hold-on-unset", "settings": {"project": "hold"}, "receiverClass": "prompting", "policy": "hold", "cause": "repo-setting"},
    {"id": "invalid-user-holds", "settings": {"user": "invalid"}, "receiverClass": "prompting", "policy": "hold", "cause": "invalid-setting"},
]

SETTINGS_FILES = [
    '{"crossSessionInbound":"accept"}',
    '{"model":"x"}',
    '{"crossSessionInbound":"yes"}',
    'not json',
    '{"crossSessionInbound":"refuse","hooks":{}}',
]

MODE_CASES = [
    {"id": "transcript-bypass", "argv": ["claude"], "transcript": '{"type":"user","permissionMode":"bypassPermissions"}\n', "expect": "bypass"},
    {"id": "transcript-latest-wins", "argv": ["claude", "--dangerously-skip-permissions"],
     "transcript": '{"permissionMode":"bypassPermissions"}\n{"type":"permission-mode","permissionMode":"default"}\n', "expect": "prompting"},
    {"id": "flag-fallback", "argv": ["claude", "--dangerously-skip-permissions"], "transcript": "", "expect": "bypass"},
    {"id": "permission-mode-flag", "argv": ["claude", "--permission-mode", "bypassPermissions"], "transcript": "", "expect": "bypass"},
    {"id": "allow-only-is-not-bypass", "argv": ["claude", "--allow-dangerously-skip-permissions"], "transcript": "", "expect": "prompting"},
    {"id": "default", "argv": ["claude"], "transcript": "", "expect": "prompting"},
]


def build():
    recorded = dict(RECORDED)
    rebuilt = peer.frame(recorded["token"], recorded["message"]).decode("utf-8")
    if rebuilt != recorded["frame"]:
        raise SystemExit("reference frame differs from the recorded frame:\n%s\n%s" % (rebuilt, recorded["frame"]))
    frames = []
    for case in FRAME_CASES:
        entry = dict(case)
        entry["frame"] = peer.frame(case["token"], case["message"]).decode("utf-8")
        frames.append(entry)
    escapes = [{"text": text, "body": peer.escape_body(text)} for text in ESCAPE_CASES]
    keys = []
    for case in KEY_CASES:
        entry = dict(case)
        entry["keyPath"] = peer.key_path(case["configDir"], case["pid"], case["socketPath"])
        if not entry["keyPath"].endswith("/sessions/" + case["observedKeyName"]):
            raise SystemExit("key name mismatch for pid %d" % case["pid"])
        keys.append(entry)
    inbound = []
    for case in INBOUND_CASES:
        got = peer.inbound_policy(*(case["settings"].get(k, "") for k in ("managed", "flag", "user", "local", "project")),
                                  case["receiverClass"])
        if got != (case["policy"], case["cause"]):
            raise SystemExit("%s: reference says %s" % (case["id"], got))
        inbound.append(case)
    settings_files = [{"json": text, "value": peer.inbound_setting(text.encode())} for text in SETTINGS_FILES]
    return {
        "protocol": "claude-code-peer-messaging",
        "peerProtocol": peer.PEER_PROTOCOL,
        "senderName": peer.SENDER_NAME,
        "steerLead": peer.STEER_LEAD,
        "recorded": recorded,
        "frames": frames,
        "escapes": escapes,
        "keys": keys,
        "modes": MODE_CASES,
        "inbound": inbound,
        "settingsFiles": settings_files,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = json.dumps(build(), indent=2, ensure_ascii=False) + "\n"
    if args.check:
        if FIXTURE.read_text(encoding="utf-8") != text:
            raise SystemExit("fixture.json is stale; run generate_fixture.py")
        return 0
    FIXTURE.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
