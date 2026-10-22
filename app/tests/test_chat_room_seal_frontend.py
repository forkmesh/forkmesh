#!/usr/bin/env python3
"""Executable contracts for automatic device-to-device chat encryption."""

import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
SEAL = PUBLIC / "chat-seal.js"
TRANSPORT = PUBLIC / "chat-room-transport.js"
CHAT = PUBLIC / "chat.js"
DASHBOARD_CHAT = PUBLIC / "dashboard-chat.js"
HTML = PUBLIC / "chat.html"

VECTOR_ROOM = "world-general"
SENDER_JWK = {
    "kty": "OKP",
    "crv": "X25519",
    "d": "UE5ZIyTdbSCtuDSqkI3g5TnXBMw1nx4Oz1BfmvE6CGc",
    "x": "SJXRaOihp8_2nTX69yDoRqkDhapmBc30VI8lARlbQTw",
}
RECEIVER_JWK = {
    "kty": "OKP",
    "crv": "X25519",
    "d": "cJMcO0LKgE1B6DRDn8z2mCcgt-cKyuWEtafYL2hFjHk",
    "x": "_aR7FyhOKbbBVToq3l-eYAT3_k5Hc3yNyZc-qf9hCXM",
}
SENDER_KEY_ID = "SQAm1SbhWWBxNiSt"
RECEIVER_KEY_ID = "PA22MdT_F-KVxa5L"
VECTOR_FRAME = {
    "type": "sealed",
    "v": 2,
    "kid": SENDER_KEY_ID,
    "pub": SENDER_JWK["x"],
    "nonce": "43+zI9tUg9t5SdVG",
    "tag": "D9qgU0oVQpoOY05RaI3xDg==",
    "body": (
        "Jn79ws7IdrSi5kFmmLHAB7yUUuARxKFeLZKdzMQBoHbghdPyQgEq8OZanyL0hTh/"
        "TYbavVieT2DhVLLZq2F0kH1yeAP7JdwPDqPysTZCETMtZYDTKBLr"
    ),
    "keys": [
        {
            "kid": RECEIVER_KEY_ID,
            "nonce": "dIYd3AXET2IQhRjH",
            "tag": "wga2/Tmek1r/0u9oXq4ckw==",
            "key": "pNMivvZuaR6ZEu3V/GkZOVYDlSP4Lr4MtsG0mIwtOMs=",
        },
        {
            "kid": SENDER_KEY_ID,
            "nonce": "5vZE4C5wbgSbNENA",
            "tag": "eiRSoFEo1m5rBwQ0jSgiEw==",
            "key": "K7BObyf/R3vByKsOWMJQWiRDBb02WFDP3fy4cGaJ5O8=",
        },
    ],
}


def _run_module(script):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
        cwd=PUBLIC,
    )
    return json.loads(result.stdout)


def _storage(name, jwk=None):
    seeded = ""
    if jwk is not None:
        seeded = (
            f'items.set("forkmesh.chat.e2ee.x25519.v2", '
            f"JSON.stringify({json.dumps(jwk)}));"
        )
    return f"""
      const {name}Items = new Map();
      const {name} = {{
        getItem: (key) => {name}Items.has(key) ? {name}Items.get(key) : null,
        setItem: (key, value) => {name}Items.set(key, String(value)),
        removeItem: (key) => {name}Items.delete(key),
      }};
      (() => {{ const items = {name}Items; {seeded} }})();
    """


def _fake_websocket():
    return """
      class FakeWebSocket {
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;
        static instances = [];
        constructor(url) {
          this.url = url;
          this.readyState = FakeWebSocket.CONNECTING;
          this.sent = [];
          this.listeners = new Map();
          FakeWebSocket.instances.push(this);
        }
        addEventListener(type, callback) {
          const entries = this.listeners.get(type) || [];
          entries.push(callback);
          this.listeners.set(type, entries);
        }
        emit(type, event = {}) {
          for (const callback of this.listeners.get(type) || []) callback(event);
        }
        open() {
          this.readyState = FakeWebSocket.OPEN;
          this.emit("open");
        }
        send(value) { this.sent.push(value); }
        close(code = 1000, reason = "") {
          if (this.readyState === FakeWebSocket.CLOSED) return;
          this.readyState = FakeWebSocket.CLOSED;
          this.emit("close", { code, reason });
        }
      }
    """


def test_devices_exchange_public_keys_and_encrypt_without_setup():
    result = _run_module(f"""
      import {{
        acceptSealKey, createRoomSealStore, isSealKeyFrame, isSealedFrame,
        sealFrame, sealKeyFrame, unsealFrame,
      }} from "./chat-seal.js";
      {_storage("aliceStorage")}
      {_storage("bobStorage", RECEIVER_JWK)}
      bobStorageItems.set("forkmesh.chat.seal.scope", "obsolete phrase");
      const aliceStore = createRoomSealStore({{ storage: aliceStorage, acct: "alice" }});
      const bobStore = createRoomSealStore({{ storage: bobStorage, acct: "bob" }});
      const alice = await aliceStore.resolve("scope", {json.dumps(VECTOR_ROOM)});
      const bob = await bobStore.resolve("scope", {json.dumps(VECTOR_ROOM)});
      const aliceKey = await sealKeyFrame(alice);
      const bobKey = await sealKeyFrame(bob);
      await acceptSealKey(bobKey, alice);
      await acceptSealKey(aliceKey, bob);
      const frame = await sealFrame(
        {{ type: "chat", id: "m1", senderId: aliceStore.node, text: "no setup required" }}, alice);
      const relabelled = {{ ...frame, kid: bob.keyId }};
      const revived = createRoomSealStore({{ storage: bobStorage, acct: "bob" }});
      const revivedBob = await revived.resolve("scope", {json.dumps(VECTOR_ROOM)});
      const rotatedBob = await revived.resolve("scope", "chat-channel:42:v2");
      const otherControllerBob = await revived.resolve(
        "scope", {json.dumps(VECTOR_ROOM)}, "relay-two.example");
      process.stdout.write(JSON.stringify({{
        aliceId: alice.keyId,
        bobId: bob.keyId,
        keyFrames: isSealKeyFrame(aliceKey) && isSealKeyFrame(bobKey),
        bundleAcct: aliceKey.acct,
        bundleNode: aliceKey.node === aliceStore.node && aliceKey.nsig.length === 86,
        sealed: isSealedFrame(frame),
        version: frame.v,
        leaked: JSON.stringify(frame).includes("no setup required"),
        recipientIds: frame.keys.map((entry) => entry.kid),
        opened: await unsealFrame(frame, bob),
        relabelled: await unsealFrame(relabelled, bob),
        persistedIdentity: bobStorageItems.get("forkmesh.chat/e2ee/ed25519IdentityV3"),
        legacyIdentityRetained: bobStorageItems.has("forkmesh.chat.e2ee.x25519.v2"),
        legacyPhraseRetained: bobStorageItems.has("forkmesh.chat.seal.scope"),
        revivedId: revivedBob.keyId,
        revivedPeers: revivedBob.peerCount,
        rotatedPeers: rotatedBob.peerCount,
        otherControllerPeers: otherControllerBob.peerCount,
        firstAnnouncement: bob.shouldPersistAdvertisement(100000000),
        duplicateAnnouncement: bob.shouldPersistAdvertisement(100000001),
        storeApi: Object.keys(bobStore).sort(),
      }}));
    """)
    assert result["aliceId"] != result["bobId"]
    assert result["keyFrames"] is True
    assert result["bundleAcct"] == "alice"
    assert result["bundleNode"] is True
    assert result["sealed"] is True
    assert result["version"] == 3
    assert result["leaked"] is False
    assert result["recipientIds"] == sorted([result["aliceId"], result["bobId"]])
    assert result["opened"]["status"] == "opened"
    assert result["opened"]["plain"]["text"] == "no setup required"
    assert result["opened"]["acct"] == "alice"
    assert result["relabelled"]["status"] == "invalid"
    assert len(result["persistedIdentity"]) == 44
    assert result["legacyIdentityRetained"] is False
    assert result["legacyPhraseRetained"] is False
    assert result["revivedId"] == result["bobId"]
    assert result["revivedPeers"] == 1
    assert result["rotatedPeers"] == 0
    assert result["otherControllerPeers"] == 0
    assert result["firstAnnouncement"] is True
    assert result["duplicateAnnouncement"] is False
    assert result["storeApi"] == [
        "account", "has", "identity", "node", "nodeBindProof", "profile", "ready",
        "resolve", "signCall", "subscribe", "wipeExpired",
    ]


def test_browser_ignores_v2_frames():
    result = _run_module(f"""
      import {{ createRoomSealStore, isSealedFrame, isSealKeyFrame, unsealFrame }}
        from "./chat-seal.js";
      {_storage("receiverStorage", RECEIVER_JWK)}
      const receiver = await createRoomSealStore({{ storage: receiverStorage, acct: "bob", node: "bob" }})
        .resolve("scope", {json.dumps(VECTOR_ROOM)});
      const frame = {json.dumps(VECTOR_FRAME)};
      process.stdout.write(JSON.stringify({{
        sealed: isSealedFrame(frame),
        key: isSealKeyFrame({{ type: "e2ee-key", v: 2, kid: frame.kid, pub: frame.pub }}),
        receiver: await unsealFrame(frame, receiver),
      }}));
    """)
    assert result == {"sealed": False, "key": False, "receiver": {"status": "invalid"}}


def test_transport_exposes_only_recipient_ciphertext_to_the_controller():
    result = _run_module(f"""
      import {{ createChatRoomTransport }} from "./chat-room-transport.js";
      import {{ decryptObject, derivePassphraseKey, encryptObject }}
        from "./chat-crypto.js";
      import {{
        acceptSealKey, createRoomSealStore, isSealKeyFrame, isSealedFrame,
        sealFrame, sealKeyFrame, unsealFrame,
      }} from "./chat-seal.js";
      {_fake_websocket()}
      {_storage("aliceStorage")}
      {_storage("bobStorage")}
      const aliceStore = createRoomSealStore({{ storage: aliceStorage, acct: "alice" }});
      const bobStore = createRoomSealStore({{ storage: bobStorage, acct: "bob" }});
      const bob = await bobStore.resolve("scope", {json.dumps(VECTOR_ROOM)});
      const received = [];
      const refused = [];
      const transport = createChatRoomTransport({{
        authorize: async () => ({{
          scope: "scope",
          passphrase: "controller-derived",
          roomName: {json.dumps(VECTOR_ROOM)},
          webSocketUrl: "/api/chat/world-general/ws",
        }}),
        resolveSeal: (room, roomName) => aliceStore.resolve(room.scope, roomName),
        onPlain: (plain) => received.push(plain),
        onSealed: (keyId) => refused.push(keyId),
        WebSocketImpl: FakeWebSocket,
        locationLike: {{ protocol: "https:", host: "relay.example" }},
      }});
      await transport.connect({{ scope: "scope" }});
      const socket = FakeWebSocket.instances[0];
      socket.open();
      const roomKey = await derivePassphraseKey(
        "controller-derived", {json.dumps(VECTOR_ROOM)});
      const settle = () => new Promise((resolve) => setTimeout(resolve, 80));
      await settle();

      const initial = await Promise.all(socket.sent.map(async (raw) =>
        decryptObject(JSON.parse(raw), roomKey)));
      const aliceKey = initial.find(isSealKeyFrame);
      await acceptSealKey(aliceKey, bob);
      socket.emit("message", {{ data: JSON.stringify(
        await encryptObject(await sealKeyFrame(bob), roomKey)) }});
      await settle();

      const sent = await transport.send(
        {{ type: "chat", id: "m1", senderId: aliceStore.node, text: "beyond the controller" }},
        {{ persist: true }},
      );
      await settle();
      const controllerViews = await Promise.all(socket.sent.map(async (raw) => {{
        const envelope = JSON.parse(raw);
        return {{ envelope, frame: await decryptObject(envelope, roomKey) }};
      }}));
      const message = controllerViews.find((entry) => isSealedFrame(entry.frame));

      socket.emit("message", {{ data: JSON.stringify(await encryptObject({{
        type: "chat", id: "forged", text: "operator plaintext",
      }}, roomKey)) }});
      socket.emit("message", {{ data: JSON.stringify(await encryptObject(
        await sealFrame({{
          type: "chat", id: "peer", senderId: bobStore.node, text: "peer ciphertext",
        }}, bob), roomKey)) }});
      await settle();
      process.stdout.write(JSON.stringify({{
        sent,
        sealed: transport.sealed,
        controllerFrame: message.frame,
        controllerText: JSON.stringify(message.frame),
        persist: message.envelope.persist,
        participant: await unsealFrame(message.frame, bob),
        received,
        refused,
      }}));
    """)
    assert result["sent"] is True
    assert result["sealed"] is True
    assert result["persist"] is True
    assert result["controllerFrame"]["v"] == 3
    assert "beyond the controller" not in result["controllerText"]
    assert result["participant"]["plain"]["text"] == "beyond the controller"
    assert [frame["id"] for frame in result["received"]] == ["peer"]
    assert result["refused"] == [""]


def test_public_transport_accepts_only_server_marked_plaintext():
    result = _run_module(f"""
      import {{ createChatRoomTransport }} from "./chat-room-transport.js";
      import {{ decryptObject, derivePassphraseKey, encryptObject }}
        from "./chat-crypto.js";
      {_fake_websocket()}
      const received = [];
      const refused = [];
      const transport = createChatRoomTransport({{
        authorize: async () => ({{
          scope: "public-scope",
          passphrase: "controller-derived",
          roomName: "public-channel",
          webSocketUrl: "/ws",
        }}),
        allowsControllerAccess: () => true,
        // Public access must not depend on local device-key storage working.
        resolveSeal: async () => null,
        onPlain: (plain) => received.push(plain),
        onSealed: (keyId) => refused.push(keyId),
        WebSocketImpl: FakeWebSocket,
        locationLike: {{ protocol: "https:", host: "relay.example" }},
        setTimeoutImpl: () => 1,
        clearTimeoutImpl: () => {{}},
      }});
      const connected = await transport.connect({{ scope: "public-scope" }});
      const socket = FakeWebSocket.instances[0];
      socket.open();
      const sent = await transport.send(
        {{ type: "chat", id: "m1", channel: "#public", text: "for MCP" }},
        {{ persist: true }},
      );
      const roomKey = await derivePassphraseKey(
        "controller-derived", "public-channel");
      const sentFrames = socket.sent.length;
      socket.emit("message", {{ data: JSON.stringify(await encryptObject({{
        type: "chat", id: "person", channel: "#public", text: "unsealed person",
      }}, roomKey)) }});
      socket.emit("message", {{ data: JSON.stringify({{ ...(await encryptObject({{
        type: "chat", id: "bot", channel: "#public", text: "from a webhook",
      }}, roomKey)), srv: true }}) }});
      for (let attempt = 0; attempt < 20 && !received.length; attempt += 1) {{
        await new Promise((resolve) => setTimeout(resolve, 5));
      }}
      process.stdout.write(JSON.stringify({{
        connected, sent, sentFrames, sealed: transport.sealed,
        controllerReadable: transport.controllerReadable,
        received, refused,
      }}));
    """)
    assert result["connected"] is True
    assert result["sent"] is False
    assert result["sentFrames"] == 0
    assert result["sealed"] is False
    assert result["controllerReadable"] is True
    assert [frame["id"] for frame in result["received"]] == ["bot"]
    assert result["received"][0]["frameAuth"] == {"srv": True}
    assert result["refused"] == [""]


def test_transport_fails_closed_without_a_local_identity():
    result = _run_module(f"""
      import {{ createChatRoomTransport }} from "./chat-room-transport.js";
      {_fake_websocket()}
      const states = [];
      const transport = createChatRoomTransport({{
        authorize: async () => ({{
          passphrase: "controller-derived",
          roomName: {json.dumps(VECTOR_ROOM)},
          webSocketUrl: "/ws",
        }}),
        onState: (state) => states.push(state),
        WebSocketImpl: FakeWebSocket,
        locationLike: {{ protocol: "https:", host: "relay.example" }},
        setTimeoutImpl: () => 1,
        clearTimeoutImpl: () => {{}},
      }});
      const connected = await transport.connect({{ scope: "scope" }});
      process.stdout.write(JSON.stringify({{
        connected, state: transport.state, states,
        sockets: FakeWebSocket.instances.length,
      }}));
    """)
    assert result["connected"] is False
    assert result["state"] == "reconnecting"
    assert result["states"][:2] == ["authorizing", "unavailable"]
    assert result["sockets"] == 0


def test_browser_surfaces_apply_public_and_private_policy_automatically():
    seal = SEAL.read_text(encoding="utf-8")
    chat = CHAT.read_text(encoding="utf-8")
    transport = TRANSPORT.read_text(encoding="utf-8")
    html = HTML.read_text(encoding="utf-8")
    dashboard = DASHBOARD_CHAT.read_text(encoding="utf-8")

    assert 'from "./chat-seal.js"' in chat
    assert 'from "./chat-seal.js"' in transport
    assert "(await activateChatIdentity()).resolve(room.scope, roomName, RELAY_HOST, sealOptionsFor(room.scope))" in chat
    assert "End-to-end" in html and "anyone who joins can read them" in html
    assert "allowsControllerAccess" in transport
    assert "controllerReadableScope" in chat
    assert "targetMatchesScope(plain.target, scope)" in chat
    for obsolete in (
        "chat-seal-secret", "normalizeSealSecret", "Remove seal",
        "deriveRoomSeal", "roomSeals.set(", "roomSeals.clear(",
    ):
        assert obsolete not in chat + html + seal
    for network_api in ("fetch(", "XMLHttpRequest", "WebSocket", "sendBeacon"):
        assert network_api not in seal
    assert "sealKeyFrame" in transport and "acceptSealKey" in transport
    assert "if (!isSealedFrame(framed))" in transport
    assert 'import("./chat-seal.js")' in dashboard
    assert "sealModule.sealKeyFrame(roomSeal)" in dashboard
    assert "sealModule.acceptSealKey(framed, roomSeal)" in dashboard
    assert "sealModule.sealFrame(obj, roomSeal, ACTIVE_ROOM)" in dashboard
    assert "sealModule.unsealFrame(framed, roomSeal, ACTIVE_ROOM, { history })" in dashboard
    assert "node: selfId" not in chat + dashboard
    assert "forkmesh.chat.selfId" not in chat + dashboard
    assert "createRoomSealStore({ acct: account, profile })" in chat + dashboard
    assert "if (PUBLIC_WORLD_GENERAL) return encryptRoomObject(obj);" not in dashboard
    assert "envelope.srv === true" in dashboard
    assert "sealOptionsFor(room.scope)" in chat


def test_forkbot_is_guarded_by_controller_readable_channel_policy():
    for source, guard in (
        (CHAT.read_text(encoding="utf-8"),
         "if (!controllerReadableScope()) {"),
        (DASHBOARD_CHAT.read_text(encoding="utf-8"),
         "if (!PUBLIC_WORLD_GENERAL) {"),
    ):
        body = source.split("async function maybeAskForkbot(")[1]
        assert guard in body.split("FORKBOT_ENDPOINT")[0]

    desktop = (ROOT.parent / "desktop" / "src" /
               "MainWindowMessages.cpp").read_text(encoding="utf-8")
    body = desktop.split("void MainWindow::maybeAskForkbot")[1]
    assert "if (!publicChannel) {" in \
        body.split("/api/forkbot/chat")[0]
    assert "isPrivateConversation" in body.split("/api/forkbot/chat")[0]
    assert "sendBotMessage(channel, botMessage)" in body


if __name__ == "__main__":
    for name, test in sorted(list(globals().items())):
        if name.startswith("test_") and callable(test):
            test()
            print("ok", name)
