#!/usr/bin/env python3

import json
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
TRANSPORT = PUBLIC / "chat-room-transport.js"
CRYPTO = PUBLIC / "chat-crypto.js"
SEAL = PUBLIC / "chat-seal.js"
CHAT = PUBLIC / "chat.js"
CHAT_CSS = PUBLIC / "chat.css"
CHAT_TEXT = CHAT.read_text(encoding="utf-8")


def _run_module(script):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
        cwd=PUBLIC,
    )
    return json.loads(result.stdout)


FAKE_WEBSOCKET = """
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
    close() { this.readyState = FakeWebSocket.CLOSED; }
  }
"""


def _transport_run(steps):
    return _run_module(f"""
      import {{ createChatRoomTransport }} from {json.dumps(TRANSPORT.as_uri())};
      import {{ derivePassphraseKey, encryptObject }}
        from {json.dumps(CRYPTO.as_uri())};
      import {{ createRoomSealStore }} from {json.dumps(SEAL.as_uri())};
      {FAKE_WEBSOCKET}
      const items = new Map();
      const sealStore = createRoomSealStore({{
        storage: {{
          getItem: (k) => (items.has(k) ? items.get(k) : null),
          setItem: (k, v) => items.set(k, String(v)),
          removeItem: (k) => items.delete(k),
        }},
        acct: "me",
        wrappingKey: null,
      }});
      const received = [];
      const counts = [];
      const marks = {{}};
      const transport = createChatRoomTransport({{
        authorize: async (room) => ({{
          scope: room.scope,
          passphrase: "room secret",
          roomName: "channel-alpha",
          webSocketUrl: "/api/chat/channels/alpha/ws",
        }}),
        allowsControllerAccess: () => true,
        resolveSeal: (room, name) => sealStore.resolve(room.scope, name),
        onPlain: (plain) => received.push(plain),
        onHistory: (plains) => received.push(...plains),
        onRecipients: (id, count, _room, names) =>
          counts.push(names.length ? [id, count, names] : [id, count]),
        WebSocketImpl: FakeWebSocket,
        locationLike: {{ protocol: "https:", host: "relay.example" }},
        setTimeoutImpl: () => 0,
        clearTimeoutImpl: () => {{}},
      }});
      await transport.connect({{ scope: "channel-alpha" }});
      const socket = FakeWebSocket.instances[0];
      socket.open();
      const key = await derivePassphraseKey("room secret", "channel-alpha");
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
        received, counts, marks, sent: socket.sent }}));
    """)


def test_outer_relay_count_reaches_the_plain_and_inner_values_are_dropped():
    result = _transport_run("""
      const stamped = await encryptObject(
        { type: "chat", id: "m1", text: "hi", relayRecipients: 99 }, key);
      stamped.persist = true;
      stamped.srv = true;
      stamped.recipients = 3;
      stamped.recipientNames = ["bob", "Carol", "dave", 7];
      socket.emit("message", { data: JSON.stringify(stamped) });
      const forged = await encryptObject(
        { type: "chat", id: "m2", text: "hey", relayRecipients: 42,
          relayRecipientNames: ["mallory"] }, key);
      forged.srv = true;
      socket.emit("message", { data: JSON.stringify(forged) });
      const invalid = await encryptObject(
        { type: "chat", id: "m3", text: "yo" }, key);
      invalid.recipients = "7";
      invalid.srv = true;
      socket.emit("message", { data: JSON.stringify(invalid) });
      await settle();
    """)
    by_id = {plain["id"]: plain for plain in result["received"]}
    assert by_id["m1"]["relayRecipients"] == 3
    assert by_id["m1"]["relayRecipientNames"] == ["bob", "dave"]
    assert "relayRecipients" not in by_id["m2"]
    assert "relayRecipientNames" not in by_id["m2"]
    assert "relayRecipients" not in by_id["m3"]


def test_sender_ack_maps_the_frame_hash_back_to_the_message_id():
    result = _transport_run("""
      marks.own = socket.sent.length;
      await transport.send(
        { type: "chat", id: "own-1", text: "hello" }, { persist: true });
      await transport.send({ type: "typing", id: "typing-1" });
      const { createHash } = await import("node:crypto");
      const ref = createHash("sha256").update(socket.sent[marks.own]).digest("hex")
        .slice(0, 40);
      socket.emit("message", { data: JSON.stringify({
        kind: "forkmesh-recipients", v: 1, ref, recipients: 4,
        recipientNames: ["alice", "bob"] }) });
      socket.emit("message", { data: JSON.stringify({
        kind: "forkmesh-recipients", v: 1, ref: "0".repeat(40),
        recipients: 9 }) });
      await settle();
    """)
    first = result["sent"][result["marks"]["own"]]
    assert result["counts"] == [["own-1", 4, ["alice", "bob"]]]
    assert json.loads(first)["persist"] is True
    assert "recipients" not in json.loads(first)


def _function_source(name):
    match = re.search(
        r"^function " + re.escape(name) + r"\(.*?^}\n", CHAT_TEXT,
        re.S | re.M)
    assert match, name
    return match.group(0)


def test_label_renders_to_n_with_delivered_tooltip_and_hides_when_unknown():
    helpers = "\n".join(_function_source(name) for name in (
        "knownRecipientCount",
        "knownRecipientNames",
        "recipientCountTitle",
        "paintRecipientLabel",
        "makeRecipientLabel",
    ))
    result = _run_module(f"""
      class FakeElement {{
        constructor() {{ this.attributes = {{}}; this.hidden = false;
          this.textContent = ""; this.className = ""; this.title = ""; }}
        setAttribute(name, value) {{ this.attributes[name] = String(value); }}
        removeAttribute(name) {{ delete this.attributes[name];
          if (name === "title") this.title = ""; }}
      }}
      const document = {{ createElement: () => new FakeElement() }};
      {helpers}
      const shape = (el) => ({{
        text: el.textContent, hidden: el.hidden, title: el.title,
        aria: el.attributes["aria-label"] || null, cls: el.className }});
      const many = makeRecipientLabel(123);
      const one = makeRecipientLabel(1);
      const zero = makeRecipientLabel(0);
      const unknown = makeRecipientLabel(null);
      const later = makeRecipientLabel(undefined);
      paintRecipientLabel(later, 5);
      const forged = makeRecipientLabel("12");
      const two = makeRecipientLabel(2, "chat-recipients", ["alice", "bob"]);
      const crowd = makeRecipientLabel(
        30, "chat-recipients", ["alice", "bob", "<b>x</b>"]);
      process.stdout.write(JSON.stringify({{
        many: shape(many), one: shape(one), zero: shape(zero),
        unknown: shape(unknown), later: shape(later), forged: shape(forged),
        two: shape(two), crowd: shape(crowd),
      }}));
    """)
    assert result["many"] == {
        "text": "to 123", "hidden": False, "title": "Delivered to 123 people",
        "aria": "Delivered to 123 people", "cls": "chat-recipients",
    }
    assert result["one"]["title"] == "Delivered to 1 person"
    assert result["zero"]["text"] == "to 0"
    assert result["unknown"]["hidden"] is True
    assert result["unknown"]["text"] == ""
    assert result["unknown"]["aria"] is None
    assert result["later"]["text"] == "to 5"
    assert result["forged"]["hidden"] is True
    assert result["two"]["text"] == "to 2"
    assert result["two"]["title"] == "Delivered to 2 people: @alice, @bob"
    assert result["two"]["aria"] == "Delivered to 2 people: @alice, @bob"
    assert result["crowd"]["title"] == (
        "Delivered to 30 people: @alice, @bob and 28 more")


def test_chat_rows_and_threads_show_the_relay_count_beside_the_receipt():
    build_row = _function_source("buildRow")
    assert build_row.index("row.append(record.recipientsEl);") < (
        build_row.index("if (receipt) row.append(makeReceipt(record));"))
    assert "if (receipt && isDirectMessageKey(record.channel)) {" in build_row
    header = _function_source("threadMessageHeader")
    assert "!(receipt && isDirectMessageKey(record.channel))" in header
    assert header.index('"chat-thread-recipients"') < header.index(
        "if (receipt) meta.append(makeReceipt(record));")
    assert "record.recipientsEl = makeRecipientLabel(\n" \
        "      record.recipients, \"chat-recipients\", record.recipientNames);" \
        in build_row
    assert "row.append(record.recipientsEl);" in build_row
    assert "record.recipients, \"chat-thread-recipients\", " \
        "record.recipientNames" in _function_source("threadMessageHeader")
    handle = _function_source("handlePlain")
    assert (
        'plain, "peer", scope, plain.relayRecipients ?? null,\n'
        "      plain.relayRecipientNames ?? []);" in handle)
    assert (
        "plain, scope, plain.relayRecipients ?? null,\n"
        "      plain.relayRecipientNames ?? []);" in handle)
    assert 'renderChatEntry(entry, "peer", scope);' in handle
    assert "renderThreadEntry(entry, scope);" in handle
    assert "onRecipients: (id, count, _room, names) =>\n" \
        "      applyRecipientCount(id, count, names)," in CHAT_TEXT
    css = CHAT_CSS.read_text(encoding="utf-8")
    block = css[css.index(".chat-recipients {"):]
    block = block[:block.index("}")]
    assert "color: var(--quiet);" in block
    assert "font-size: 11.5px;" in block
