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


def _function_source(name):
    match = re.search(
        r"^(?:async )?function " + re.escape(name) + r"\(.*?^}\n", CHAT_TEXT,
        re.S | re.M)
    assert match, name
    return match.group(0)


def _const_source(name):
    match = re.search(
        r"^const " + re.escape(name) + r" =.*?;\n", CHAT_TEXT, re.S | re.M)
    assert match, name
    return match.group(0)


def _transport_run(steps, direct=True):
    return _run_module(f"""
      import {{ createHash }} from "node:crypto";
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
      const delivered = [];
      const reads = [];
      const transport = createChatRoomTransport({{
        authorize: async (room) => ({{
          scope: room.scope,
          passphrase: "room secret",
          roomName: "direct-alpha",
          webSocketUrl: "/api/chat/direct-messages/alpha/ws",
        }}),
        allowsControllerAccess: () => true,
        isDirectRoom: () => {json.dumps(direct)},
        onDelivered: (ids, room) => delivered.push([ids, room.scope]),
        onRead: (room) => reads.push(room.scope),
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
      const refOf = (text) =>
        createHash("sha256").update(text).digest("hex").slice(0, 40);
      socket.emit("message", {{ data: JSON.stringify({{
        kind: "forkmesh-history-end", v: 1 }}) }});
      await settle();
      {steps}
      process.stdout.write(JSON.stringify({{ delivered, reads }}));
    """)


def test_delivered_refs_resolve_to_sent_and_replayed_message_ids():
    result = _transport_run("""
      await transport.send({ type: "chat", id: "mine", text: "hi" },
        { persist: true });
      const sentRef = refOf(socket.sent[socket.sent.length - 1]);
      const other = await encryptObject(
        { type: "chat", id: "other-device", text: "yo" }, key);
      other.persist = true;
      other.ref = "a".repeat(40);
      socket.emit("message", { data: JSON.stringify(other) });
      await settle();
      socket.emit("message", { data: JSON.stringify({
        kind: "forkmesh-delivered", v: 1,
        refs: [sentRef, "a".repeat(40), "f".repeat(40), "bad", 3] }) });
      socket.emit("message", { data: JSON.stringify({
        kind: "forkmesh-delivered", v: 1, refs: ["e".repeat(40)] }) });
      socket.emit("message", { data: JSON.stringify({
        kind: "forkmesh-read", v: 1 }) });
      await settle();
    """)
    assert result["delivered"] == [
        [["mine", "other-device"], "direct:alpha"]]
    assert result["reads"] == ["direct:alpha"]


def test_receipt_frames_are_ignored_outside_direct_messages():
    result = _transport_run("""
      await transport.send({ type: "chat", id: "mine", text: "hi" },
        { persist: true });
      const sentRef = refOf(socket.sent[socket.sent.length - 1]);
      socket.emit("message", { data: JSON.stringify({
        kind: "forkmesh-delivered", v: 1, refs: [sentRef] }) });
      socket.emit("message", { data: JSON.stringify({
        kind: "forkmesh-read", v: 1 }) });
      await settle();
    """, direct=False)
    assert result == {"delivered": [], "reads": []}


def _receipt_run(steps):
    helpers = "\n".join(
        [_const_source(name) for name in (
            "DIRECT_RECEIPT_LABELS",
            "DIRECT_RECEIPT_CHECK",
            "DIRECT_RECEIPT_BACK_RING",
            "DIRECT_RECEIPT_BACK_DOT",
        )] + [_function_source(name) for name in (
            "knownRecipientCount",
            "isDirectMessageKey",
            "directReceiptState",
            "directReceiptIcon",
            "paintDirectReceipt",
            "makeDirectReceipt",
            "applyDirectDelivered",
            "applyDirectRead",
        )])
    return _run_module(f"""
      class FakeElement {{
        constructor(tag) {{
          this.tag = tag; this.attributes = {{}}; this.children = [];
          this.dataset = {{}}; this.className = ""; this.title = "";
        }}
        setAttribute(name, value) {{ this.attributes[name] = String(value); }}
        append(child) {{ this.children.push(child); }}
        replaceChildren(...children) {{ this.children = children; }}
      }}
      const document = {{
        createElement: (tag) => new FakeElement(tag),
        createElementNS: (_ns, tag) => new FakeElement(tag),
      }};
      const rows = new Map();
      const channelMessages = new Map();
      {helpers}
      const shape = (record) => {{
        const mark = record.receiptEl;
        const svg = mark.children[0];
        return {{
          state: mark.dataset.state, cls: mark.className, title: mark.title,
          aria: mark.attributes["aria-label"], role: mark.attributes.role,
          shapes: svg.children.map((child) => child.tag +
            (child.attributes.fill === "currentColor" ? ":filled" : "") +
            (child.attributes["stroke-dasharray"] ? ":dashed" : "")),
        }};
      }};
      const record = (id, fields = {{}}) => {{
        const value = {{ id, channel: "direct:" + "a".repeat(32), self: true,
          recipients: null, ...fields }};
        rows.set(id, value);
        const list = channelMessages.get(value.channel) || [];
        list.push(value);
        channelMessages.set(value.channel, list);
        return value;
      }};
      {steps}
    """)


def test_own_direct_messages_step_through_sending_sent_delivered_read():
    result = _receipt_run("""
      const sending = record("m1");
      const acked = record("m2", { recipients: 1 });
      const peer = record("m3", { self: false, recipients: 1 });
      const channel = record("m4", { channel: "private:" + "b".repeat(32),
        recipients: 3 });
      const states = {
        sending: directReceiptState(sending),
        acked: directReceiptState(acked),
        peer: directReceiptState(peer),
        channel: directReceiptState(channel),
      };
      makeDirectReceipt(sending);
      makeDirectReceipt(acked);
      const before = [shape(sending), shape(acked)];
      applyDirectDelivered(["m2", "m3", "missing"]);
      const delivered = shape(acked);
      applyDirectRead("direct:" + "a".repeat(32));
      const read = shape(acked);
      const stillSending = shape(sending);
      applyDirectDelivered(["m2"]);
      process.stdout.write(JSON.stringify({
        states, before, delivered, read, stillSending,
        afterLateDelivery: acked.receipt, peerReceipt: peer.receipt ?? null,
      }));
    """)
    assert result["states"] == {
        "sending": "pending", "acked": "sent", "peer": "", "channel": ""}
    assert result["before"] == [
        {"state": "pending", "cls": "chat-receipt is-pending",
         "title": "Sending", "aria": "Sending", "role": "img",
         "shapes": ["circle:dashed"]},
        {"state": "sent", "cls": "chat-receipt is-sent", "title": "Sent",
         "aria": "Sent", "role": "img", "shapes": ["circle", "path"]},
    ]
    assert result["delivered"]["title"] == "Delivered"
    assert result["delivered"]["shapes"] == ["path", "circle", "path"]
    assert result["read"]["title"] == "Read"
    assert result["read"]["shapes"] == [
        "path:filled", "circle:filled", "path"]
    assert result["stillSending"]["state"] == "pending"
    assert result["afterLateDelivery"] == "read"
    assert result["peerReceipt"] is None


def test_direct_rows_show_the_receipt_instead_of_a_recipient_count():
    build_row = _function_source("buildRow")
    assert "if (directReceiptState(record)) {" in build_row
    assert "row.append(makeDirectReceipt(record));" in build_row
    assert "paintDirectReceipt(rec);" in _function_source(
        "applyRecipientCount")
    assert "onDelivered: (ids) => applyDirectDelivered(ids)," in CHAT_TEXT
    assert "onRead: (room) => applyDirectRead(room.scope)," in CHAT_TEXT
    assert ".chat-receipt-check { stroke: var(--bg); }" in CHAT_CSS


def test_a_hidden_tab_does_not_mark_a_direct_message_read():
    mark = _function_source("markDirectMessageRead")
    assert "if (document.hidden || channelKey !== activeChannel) {" in mark
    assert "directReadDeferred.add(channelKey);" in mark
    assert (
        'document.addEventListener("visibilitychange", '
        "markActiveDirectMessageReadIfDeferred);"
    ) in CHAT_TEXT
