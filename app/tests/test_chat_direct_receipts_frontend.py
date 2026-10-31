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
      import {{ decryptObject, derivePassphraseKey, encryptObject }}
        from {json.dumps(CRYPTO.as_uri())};
      import {{ acceptSealKey, createRoomSealStore, isSealKeyFrame, sealFrame, sealKeyFrame }}
        from {json.dumps(SEAL.as_uri())};
      const memory = () => {{
        const items = new Map();
        return {{
          getItem: (k) => (items.has(k) ? items.get(k) : null),
          setItem: (k, v) => items.set(k, String(v)),
          removeItem: (k) => items.delete(k),
        }};
      }};
      const dm = {{ participants: ["me", "peer"] }};
      const sealStore = createRoomSealStore({{ storage: memory(), acct: "me", wrappingKey: null }});
      const peerSeal = await createRoomSealStore({{ storage: memory(), acct: "peer", wrappingKey: null }})
        .resolve("direct:alpha", "direct-alpha", "", dm);
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
        resolveSeal: (room, name) => sealStore.resolve(room.scope, name, "", dm),
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
      const myBundle = (await Promise.all(socket.sent.map((raw) =>
        decryptObject(JSON.parse(raw), key)))).find(isSealKeyFrame);
      await acceptSealKey(myBundle, peerSeal);
      socket.emit("message", {{ data: JSON.stringify(
        await encryptObject(await sealKeyFrame(peerSeal), key)) }});
      await new Promise((resolve) => setTimeout(resolve, 80));
      const fromPeer = async (plain) =>
        encryptObject(await sealFrame(plain, peerSeal, "direct-alpha"), key);
      {steps}
      process.stdout.write(JSON.stringify({{ delivered, reads }}));
    """)


def test_delivered_refs_resolve_to_sent_and_replayed_message_ids():
    result = _transport_run("""
      await transport.send({ type: "chat", id: "mine", text: "hi" },
        { persist: true });
      const sentRef = refOf(socket.sent[socket.sent.length - 1]);
      const other = await fromPeer(
        { type: "chat", id: "other-device", text: "yo" });
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
            "RECEIPT_LABELS",
            "RECEIPT_FRONT_TICK",
            "RECEIPT_BACK_TICK",
            "RECEIPT_BACK_TICK_READ",
            "RECEIPT_BACK_RING",
            "RECEIPT_BACK_DISC",
        )] + [_function_source(name) for name in (
            "knownRecipientCount",
            "isDirectMessageKey",
            "receiptState",
            "receiptIcon",
            "paintReceipt",
            "makeReceipt",
            "updateReceipt",
            "applyReceiptSent",
            "applyDirectDelivered",
            "applyDirectRead",
        )])
    return _run_module(f"""
      import {{ createThreadStore }}
        from {json.dumps((PUBLIC / "chat-thread-model.js").as_uri())};
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
      const threads = createThreadStore();
      let activeThreadRootId = "";
      let threadRenders = 0;
      const renderThread = () => {{ threadRenders += 1; }};
      {helpers}
      const shape = (record) => {{
        const mark = record.receiptEl;
        const svg = mark.children[0];
        return {{
          state: mark.dataset.state, cls: mark.className, title: mark.title,
          aria: mark.attributes["aria-label"], role: mark.attributes.role,
          viewBox: svg.attributes.viewBox,
          shapes: svg.children.map((child) => child.tag +
            (child.attributes.fill === "currentColor" ? ":filled" : "") +
            (child.attributes["stroke-dasharray"] ? ":dashed" : "") +
            (child.attributes.class === "chat-receipt-check" ? ":cut" : "")),
        }};
      }};
      const record = (id, fields = {{}}) => {{
        const value = {{ id, channel: "direct:" + "a".repeat(32), self: true,
          recipients: null, ...fields }};
        rows.set(id, value);
        const list = channelMessages.get(value.channel) || [];
        list.push(value);
        channelMessages.set(value.channel, list);
        threads.registerRoot({{ ...value }});
        return value;
      }};
      {steps}
    """)


def test_own_direct_messages_step_through_sending_sent_delivered_read():
    result = _receipt_run("""
      const sending = record("m1");
      const acked = record("m2", { recipients: 1 });
      const peer = record("m3", { self: false, recipients: 1 });
      const relayed = record("m5", { relayed: true });
      const states = {
        sending: receiptState(sending),
        acked: receiptState(acked),
        peer: receiptState(peer),
        relayed: receiptState(relayed),
      };
      makeReceipt(sending);
      makeReceipt(acked);
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
        relayedRead: relayed.receipt,
      }));
    """)
    assert result["states"] == {
        "sending": "pending", "acked": "sent", "peer": "",
        "relayed": "sent"}
    assert result["before"] == [
        {"state": "pending", "cls": "chat-receipt is-pending",
         "title": "Sending", "aria": "Sending", "role": "img",
         "viewBox": "0 0 22 14", "shapes": ["circle:dashed"]},
        {"state": "sent", "cls": "chat-receipt is-sent", "title": "Sent",
         "aria": "Sent", "role": "img", "viewBox": "0 0 22 14",
         "shapes": ["circle", "path"]},
    ]
    assert result["delivered"]["title"] == "Delivered"
    assert result["delivered"]["shapes"] == ["path", "path", "circle", "path"]
    assert result["read"]["title"] == "Read"
    assert result["read"]["shapes"] == [
        "path:filled", "circle:filled", "path:cut", "path:cut"]
    assert result["stillSending"]["state"] == "pending"
    assert result["afterLateDelivery"] == "read"
    assert result["peerReceipt"] is None
    assert result["relayedRead"] == "read"


def test_channel_messages_reach_sent_but_never_delivered_or_read():
    result = _receipt_run("""
      const channel = "private:" + "b".repeat(32);
      const pending = record("c1", { channel });
      const acked = record("c2", { channel, recipients: 3 });
      const world = record("w1", { channel: "#general" });
      makeReceipt(pending);
      makeReceipt(acked);
      makeReceipt(world);
      applyDirectDelivered(["c1", "c2"]);
      applyDirectRead(channel);
      const afterReceipts = [shape(pending).state, shape(acked).state];
      applyReceiptSent("w1");
      applyReceiptSent("c2");
      process.stdout.write(JSON.stringify({
        afterReceipts, world: shape(world), acked: shape(acked) }));
    """)
    assert result["afterReceipts"] == ["pending", "sent"]
    assert result["world"]["state"] == "sent"
    assert result["acked"]["state"] == "sent"


def test_own_thread_replies_carry_the_receipt_too():
    result = _receipt_run("""
      const root = record("r1", { self: false });
      threads.addReply({ id: "t1", rootId: "r1", channel: root.channel,
        self: true, recipients: null, receipt: "" });
      threads.addReply({ id: "t2", rootId: "r1", channel: root.channel,
        self: false, recipients: 1 });
      activeThreadRootId = "r1";
      const states = [];
      states.push(receiptState(threads.target("t1")));
      applyDirectDelivered(["t1", "t2"]);
      states.push(receiptState(threads.target("t1")));
      applyDirectRead(root.channel);
      states.push(receiptState(threads.target("t1")));
      states.push(receiptState(threads.target("t2")));
      process.stdout.write(JSON.stringify({ states, threadRenders }));
    """)
    assert result["states"] == ["pending", "delivered", "read", ""]
    assert result["threadRenders"] >= 2


def test_every_own_row_and_thread_reply_shows_the_receipt():
    build_row = _function_source("buildRow")
    assert "const receipt = receiptState(record);" in build_row
    assert "if (receipt && isDirectMessageKey(record.channel)) {" in build_row
    assert "if (receipt) row.append(makeReceipt(record));" in build_row
    header = _function_source("threadMessageHeader")
    assert "if (receipt) meta.append(makeReceipt(record));" in header
    assert "paintReceipt(rec);" in _function_source("applyRecipientCount")
    assert "onDelivered: (ids) => applyDirectDelivered(ids)," in CHAT_TEXT
    assert "onRead: (room) => applyDirectRead(room.scope)," in CHAT_TEXT
    assert ".chat-receipt-check { stroke: var(--bg); }" in CHAT_CSS
    assert ".chat-receipt.is-read { color: var(--accent); }" in CHAT_CSS
    assert "      viewOnce,\n      true,\n" in _function_source("renderChatEntry")
    assert "relayed: true," in _function_source("renderThreadEntry")
    send = _function_source("send")
    assert ('if (sent && envelope.persist && scope === "public-world-general")'
            in send)


def test_delete_for_everyone_withdraws_the_server_copy():
    confirm = _function_source("confirmMessageDelete")
    assert "if (sent && isDirectMessageKey(record.channel)) {" in confirm
    assert "roomTransport?.supersede?.([record.id]);" in confirm
    result = _transport_run("""
      await transport.send({ type: "chat", id: "mine", text: "hi" },
        { persist: true });
      const sentRef = refOf(socket.sent[socket.sent.length - 1]);
      const replayed = await fromPeer(
        { type: "chat", id: "older", text: "yo" });
      replayed.persist = true;
      replayed.ref = "b".repeat(40);
      socket.emit("message", { data: JSON.stringify(replayed) });
      await settle();
      const refs = transport.refsFor(["mine", "older", "nobody"]);
      const before = socket.sent.length;
      const withdrawn = transport.supersede(["mine", "older"]);
      const none = transport.supersede(["nobody"]);
      const frames = socket.sent.slice(before).map((text) => JSON.parse(text))
        .filter((frame) => frame.kind === "forkmesh-superseded");
      delivered.push({ refs, sentRef, withdrawn, none, frames });
    """)
    out = result["delivered"][-1]
    assert sorted(out["refs"]) == sorted([out["sentRef"], "b" * 40])
    assert out["withdrawn"] is True
    assert out["none"] is False
    assert out["frames"] == [{
        "kind": "forkmesh-superseded", "v": 1,
        "refs": [out["sentRef"], "b" * 40]}]


def test_supersede_is_a_no_op_outside_direct_messages():
    result = _transport_run("""
      await transport.send({ type: "chat", id: "mine", text: "hi" },
        { persist: true });
      const before = socket.sent.length;
      delivered.push({ sent: transport.supersede(["mine"]),
        extra: socket.sent.length - before });
    """, direct=False)
    assert result["delivered"] == [{"sent": False, "extra": 0}]


def test_a_hidden_tab_does_not_mark_a_direct_message_read():
    mark = _function_source("markDirectMessageRead")
    assert "if (document.hidden || channelKey !== activeChannel) {" in mark
    assert "directReadDeferred.add(channelKey);" in mark
    assert (
        'document.addEventListener("visibilitychange", '
        "markActiveDirectMessageReadIfDeferred);"
    ) in CHAT_TEXT
