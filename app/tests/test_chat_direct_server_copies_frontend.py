#!/usr/bin/env python3

import json
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
TRANSPORT = PUBLIC / "chat-room-transport.js"
CRYPTO = PUBLIC / "chat-crypto.js"
CHAT_TEXT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
CHAT_HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
CHAT_CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")


def _run_module(script):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
        cwd=PUBLIC,
    )
    return json.loads(result.stdout)


def _transport_run(steps):
    return _run_module(f"""
      import {{ createChatRoomTransport }} from {json.dumps(TRANSPORT.as_uri())};
      import {{ derivePassphraseKey, encryptObject }}
        from {json.dumps(CRYPTO.as_uri())};
      class FakeWebSocket {{
        static OPEN = 1;
        static instances = [];
        constructor(url) {{
          this.url = url;
          this.readyState = 0;
          this.sent = [];
          this.listeners = new Map();
          FakeWebSocket.instances.push(this);
        }}
        addEventListener(type, callback) {{
          const entries = this.listeners.get(type) || [];
          entries.push(callback);
          this.listeners.set(type, entries);
        }}
        emit(type, event = {{}}) {{
          for (const callback of this.listeners.get(type) || []) callback(event);
        }}
        open() {{
          this.readyState = FakeWebSocket.OPEN;
          this.emit("open");
        }}
        send(value) {{ this.sent.push(value); }}
        close() {{ this.readyState = 3; }}
      }}
      const received = [];
      const copies = [];
      const transport = createChatRoomTransport({{
        authorize: async (room) => ({{
          scope: room.scope,
          passphrase: "room secret",
          roomName: "direct-alpha",
          webSocketUrl: "/api/chat/direct-messages/alpha/ws",
        }}),
        allowsControllerAccess: () => true,
        onPlain: (plain) => received.push(plain),
        onHistory: (plains) => received.push(...plains),
        onServerCopies: (stored, room) => copies.push([stored, room.scope]),
        WebSocketImpl: FakeWebSocket,
        locationLike: {{ protocol: "https:", host: "relay.example" }},
        setTimeoutImpl: (callback, ms) => setTimeout(callback, ms),
        clearTimeoutImpl: (handle) => clearTimeout(handle),
      }});
      await transport.connect({{ scope: "direct:alpha" }});
      const socket = FakeWebSocket.instances[0];
      socket.open();
      const key = await derivePassphraseKey("room secret", "direct-alpha");
      const settle = async () => {{
        for (let i = 0; i < 10; i += 1) {{
          await new Promise((resolve) => setTimeout(resolve, 2));
        }}
      }};
      socket.emit("message", {{ data: JSON.stringify({{
        kind: "forkmesh-history-end", v: 1 }}) }});
      await settle();
      {steps}
      process.stdout.write(JSON.stringify({{
        received, copies, sent: socket.sent }}));
    """)


def _receipts(result):
    return [
        json.loads(frame)["refs"] for frame in result["sent"]
        if json.loads(frame).get("kind") == "forkmesh-received"
    ]


def test_received_frames_are_acked_once_in_one_batch():
    result = _transport_run("""
      const first = await encryptObject({ type: "chat", id: "m1", text: "a" }, key);
      first.persist = true;
      first.ref = "a".repeat(40);
      const second = await encryptObject({ type: "chat", id: "m2", text: "b" }, key);
      second.persist = true;
      second.ref = "b".repeat(40);
      const plain = await encryptObject({ type: "chat", id: "m3", text: "c" }, key);
      const forged = await encryptObject({ type: "chat", id: "m4", text: "d" }, key);
      forged.ref = "not-a-ref";
      const person = await encryptObject({ type: "chat", id: "m5", text: "e" }, key);
      person.persist = true;
      person.ref = "d".repeat(40);
      for (const frame of [first, second, plain, forged]) frame.srv = true;
      socket.emit("message", { data: JSON.stringify(person) });
      socket.emit("message", { data: JSON.stringify(first) });
      socket.emit("message", { data: JSON.stringify(second) });
      socket.emit("message", { data: JSON.stringify(first) });
      socket.emit("message", { data: JSON.stringify(plain) });
      socket.emit("message", { data: JSON.stringify(forged) });
      socket.emit("message", { data: JSON.stringify({
        kind: "cipher", v: 1, iv: "AAAA", data: "AAAA", ref: "c".repeat(40) }) });
      await settle();
      await new Promise((resolve) => setTimeout(resolve, 250));
    """)
    assert [plain["id"] for plain in result["received"]] == [
        "m1", "m2", "m1", "m3", "m4"]
    assert _receipts(result) == [["a" * 40, "b" * 40]]


def test_server_copy_count_reaches_the_consumer():
    result = _transport_run("""
      socket.emit("message", { data: JSON.stringify({
        kind: "forkmesh-server-copies", v: 1, stored: 2 }) });
      socket.emit("message", { data: JSON.stringify({
        kind: "forkmesh-server-copies", v: 1, stored: -1 }) });
      socket.emit("message", { data: JSON.stringify({
        kind: "forkmesh-server-copies", v: 1, stored: 0 }) });
      await settle();
    """)
    assert result["copies"] == [[2, "direct:alpha"], [0, "direct:alpha"]]
    assert _receipts(result) == []


def test_chat_header_shows_whether_messages_are_on_the_server():
    assert 'id="chat-direct-server-copies"' in CHAT_HTML
    assert "#chat-direct-server-copies" in CHAT_CSS
    assert (
        "onServerCopies: (stored, room) => "
        "applyDirectServerCopies(stored, room.scope)"
    ) in CHAT_TEXT
    render = re.search(
        r"^function directServerCopiesText\(.*?^}\n", CHAT_TEXT, re.S | re.M)
    assert render
    assert '"Not stored on the server"' in render.group(0)
    assert "messages on the server" in render.group(0)
    assert "renderDirectServerCopies();" in CHAT_TEXT


SEAL = PUBLIC / "chat-seal.js"


def test_direct_messages_are_resealed_once_the_peer_key_arrives():
    result = _run_module(f"""
      import {{ createChatRoomTransport, frameRef }}
        from {json.dumps(TRANSPORT.as_uri())};
      import {{ decryptObject, derivePassphraseKey, encryptObject }}
        from {json.dumps(CRYPTO.as_uri())};
      import {{ acceptSealKey, createRoomSealStore, sealFrame, sealKeyFrame, unsealFrame }}
        from {json.dumps(SEAL.as_uri())};
      const memory = () => {{
        const values = new Map();
        return {{
          getItem: (key) => values.has(key) ? values.get(key) : null,
          setItem: (key, value) => values.set(key, String(value)),
          removeItem: (key) => values.delete(key),
        }};
      }};
      class FakeWebSocket {{
        static OPEN = 1;
        static instances = [];
        constructor() {{
          this.readyState = 0;
          this.sent = [];
          this.listeners = new Map();
          FakeWebSocket.instances.push(this);
        }}
        addEventListener(type, callback) {{
          const entries = this.listeners.get(type) || [];
          entries.push(callback);
          this.listeners.set(type, entries);
        }}
        emit(type, event = {{}}) {{
          for (const callback of this.listeners.get(type) || []) callback(event);
        }}
        open() {{
          this.readyState = FakeWebSocket.OPEN;
          this.emit("open");
        }}
        send(value) {{ this.sent.push(value); }}
        close() {{ this.readyState = 3; }}
      }}
      const roomName = "direct-alpha";
      const aliceStore = createRoomSealStore({{ storage: memory(), acct: "alice", node: "alice" }});
      const bob = await createRoomSealStore({{ storage: memory(), acct: "bob", node: "bob" }})
        .resolve("direct:alpha", roomName);
      const stranger = await createRoomSealStore({{ storage: memory(), acct: "stranger", node: "stranger" }})
        .resolve("direct:alpha", roomName);
      const transport = createChatRoomTransport({{
        authorize: async (room) => ({{
          scope: room.scope,
          passphrase: "room secret",
          roomName,
          webSocketUrl: "/api/chat/direct-messages/alpha/ws",
        }}),
        isDirectRoom: (room) => room.scope.startsWith("direct:"),
        resolveSeal: (room, name) => aliceStore.resolve(room.scope, name),
        WebSocketImpl: FakeWebSocket,
        locationLike: {{ protocol: "https:", host: "relay.example" }},
        setTimeoutImpl: (callback, ms) => setTimeout(callback, ms),
        clearTimeoutImpl: (handle) => clearTimeout(handle),
      }});
      await transport.connect({{ scope: "direct:alpha" }});
      const socket = FakeWebSocket.instances[0];
      socket.open();
      const key = await derivePassphraseKey("room secret", roomName);
      const settle = async () => {{
        for (let i = 0; i < 20; i += 1) {{
          await new Promise((resolve) => setTimeout(resolve, 2));
        }}
      }};
      const deliver = async (envelope) => {{
        socket.emit("message", {{ data: JSON.stringify(envelope) }});
        await settle();
      }};
      const sentEnvelopes = () => socket.sent.map((text) => JSON.parse(text));
      const keyAdverts = async (persistedOnly) => {{
        let count = 0;
        for (const envelope of sentEnvelopes()) {{
          if (envelope.kind !== "cipher") continue;
          const inner = await decryptObject(envelope, key);
          if (inner?.type === "e2ee-key" && (!persistedOnly || envelope.persist)) {{
            count += 1;
          }}
        }}
        return count;
      }};
      await deliver({{ kind: "forkmesh-history-end", v: 1 }});
      await deliver(await encryptObject(await sealKeyFrame(stranger), key));

      await transport.send(
        {{ type: "chat", id: "m1", senderId: "alice", text: "are you there", ts: Date.now() }},
        {{ persist: true }});
      const firstText = socket.sent.at(-1);
      const firstRef = await frameRef(firstText);
      const firstInner = await decryptObject(JSON.parse(firstText), key);

      const persistedBefore = await keyAdverts(true);
      const unreadable = await encryptObject(
        await sealFrame({{ type: "chat", id: "x1", senderId: "stranger", text: "hidden" }},
                        stranger, roomName), key);
      unreadable.persist = true;
      unreadable.ref = "a".repeat(40);
      await deliver(unreadable);
      await deliver(unreadable);
      const persistedAfter = await keyAdverts(true);

      const bobKey = await encryptObject(await sealKeyFrame(bob), key);
      bobKey.persist = true;
      bobKey.ref = "b".repeat(40);
      const sentBefore = socket.sent.length;
      await deliver(bobKey);
      await new Promise((resolve) => setTimeout(resolve, 250));
      const after = sentEnvelopes().slice(sentBefore);
      const resent = after.find((envelope) => envelope.resend === true);
      const superseded = after.find(
        (envelope) => envelope.kind === "forkmesh-superseded");
      for (const envelope of sentEnvelopes()) {{
        if (envelope.kind !== "cipher") continue;
        const inner = await decryptObject(envelope, key);
        if (inner?.type === "e2ee-key") await acceptSealKey(inner, bob);
      }}
      const reopened = resent
        ? await unsealFrame(await decryptObject(resent, key), bob, roomName)
        : null;
      const keyAcked = sentEnvelopes().some(
        (envelope) => envelope.kind === "forkmesh-received" &&
          envelope.refs.includes("b".repeat(40)));

      await deliver({{ kind: "forkmesh-server-copies", v: 1, stored: 0 }});
      const phone = await createRoomSealStore({{ storage: memory(), acct: "phone", node: "phone" }})
        .resolve("direct:alpha", roomName);
      const beforePhone = socket.sent.length;
      await deliver(await encryptObject(await sealKeyFrame(phone), key));
      const resentAgain = sentEnvelopes().slice(beforePhone)
        .some((envelope) => envelope.resend === true);

      process.stdout.write(JSON.stringify({{
        firstKeys: firstInner.keys.length,
        persistedBefore, persistedAfter,
        resent: Boolean(resent),
        resentPersist: resent?.persist, resentNotify: resent?.notify,
        reopened, supersededRefs: superseded?.refs,
        firstRef, keyAcked, resentAgain,
      }}));
    """)
    assert result["firstKeys"] == 1
    assert result["persistedAfter"] == result["persistedBefore"] + 1
    assert result["resent"] is True
    assert result["resentPersist"] is True and result["resentNotify"] is True
    assert result["reopened"]["status"] == "opened"
    assert result["reopened"]["plain"]["id"] == "m1"
    assert result["reopened"]["plain"]["text"] == "are you there"
    assert result["supersededRefs"] == [result["firstRef"]]
    assert result["keyAcked"] is False
    assert result["resentAgain"] is False
