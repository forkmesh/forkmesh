#!/usr/bin/env python3
"""Executable contracts for shared encrypted chat browser modules."""

import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
CRYPTO = PUBLIC / "chat-crypto.js"
ATTACHMENTS = PUBLIC / "chat-attachments.js"
TRANSPORT = PUBLIC / "chat-room-transport.js"
SEAL = PUBLIC / "chat-seal.js"
RICH_TEXT = PUBLIC / "chat-rich-text.js"
THREAD_MODEL = PUBLIC / "chat-thread-model.js"
CHAT = PUBLIC / "chat.js"
HTML = PUBLIC / "chat.html"
FULL_CHAT = CHAT.read_text(encoding="utf-8")


def _run_module(script):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_shared_chat_modules_exist_and_full_chat_imports_them():
    assert CRYPTO.exists()
    assert ATTACHMENTS.exists()
    assert TRANSPORT.exists()
    chat = CHAT.read_text(encoding="utf-8")
    html = HTML.read_text(encoding="utf-8")
    assert 'from "./chat-crypto.js"' in chat
    assert 'from "./chat-attachments.js"' in chat
    assert 'from "./chat-room-transport.js"' in chat
    assert 'from "./chat-rich-text.js"' in chat
    assert 'from "./chat-thread-model.js"' in chat
    assert '<script type="module" src="/chat.js"></script>' in html
    assert '<link rel="stylesheet" href="/chat.css?v=${version}" />' in html
    assert '<link rel="stylesheet" href="/site-header.css" />' in html
    assert '<script src="/site-header.js?v=${version}" defer></script>' in html
    assert '<div data-forkmesh-header="simple"></div>' in html


def test_rich_text_renderer_rejects_active_links_and_never_parses_html():
    script = f"""
      class FakeNode {{
        constructor(tag = "#text", text = "") {{
          this.tagName = tag;
          this.text = text;
          this.childNodes = [];
          this.href = "";
        }}
        append(...children) {{ this.childNodes.push(...children); }}
        replaceChildren(...children) {{ this.childNodes = [...children]; }}
      }}
      globalThis.Node = FakeNode;
      globalThis.window = {{ location: {{ href: "https://forkmesh.com/chat" }} }};
      globalThis.document = {{
        createElement: (tag) => new FakeNode(tag),
        createTextNode: (text) => new FakeNode("#text", String(text)),
      }};
      const {{ normalizeRichText, renderRichText }} =
        await import({json.dumps(RICH_TEXT.as_uri())});
      const root = new FakeNode("root");
      renderRichText(root, {{
        richText: {{
          v: 1,
          source: "[bad](javascript:alert(1)) [ok](https://example.com/x) " +
            "<img src=x onerror=alert(1)>",
        }},
      }});
      const flat = [];
      const walk = (node) => {{
        flat.push({{ tag: node.tagName, text: node.text, href: node.href }});
        for (const child of node.childNodes) walk(child);
      }};
      walk(root);
      process.stdout.write(JSON.stringify({{
        flat,
        bounded: normalizeRichText("x".repeat(20000)).source.length,
      }}));
    """
    result = _run_module(script)
    links = [entry["href"] for entry in result["flat"] if entry["tag"] == "a"]
    text = "".join(
        entry["text"] for entry in result["flat"] if entry["tag"] == "#text"
    )
    assert links == ["https://example.com/x"]
    assert "<img src=x onerror=alert(1)>" in text
    assert "javascript:" not in links
    assert result["bounded"] == 16000


def test_thread_model_deduplicates_replies_and_tombstones_roots_with_replies():
    script = f"""
      import {{ createThreadStore }} from {json.dumps(THREAD_MODEL.as_uri())};
      const store = createThreadStore();
      store.registerRoot({{ id: "root", text: "hello", channel: "#general" }});
      store.addReply({{ id: "reply", rootId: "root", sender: "alice", ts: 2 }});
      store.addReply({{ id: "reply", rootId: "root", sender: "mallory", ts: 1 }});
      const deleted = store.deleteTarget("root");
      process.stdout.write(JSON.stringify({{
        deleted,
        root: store.root("root"),
        replies: store.replies("root"),
        summary: store.summary("root"),
      }}));
    """
    result = _run_module(script)
    assert result["deleted"] is True
    assert result["root"]["deleted"] is True
    assert result["root"]["text"] == ""
    assert len(result["replies"]) == 1
    assert result["replies"][0]["sender"] == "alice"
    assert result["summary"]["count"] == 1


def test_room_transport_authorizes_encrypts_replays_and_suspends_cleanly():
    script = f"""
      import {{ createChatRoomTransport }} from {json.dumps(TRANSPORT.as_uri())};
      import {{ derivePassphraseKey, encryptObject }}
        from {json.dumps(CRYPTO.as_uri())};
      import {{ createRoomSealStore, sealFrame }}
        from {json.dumps(SEAL.as_uri())};

      class FakeWebSocket {{
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;
        static instances = [];
        constructor(url) {{
          this.url = url;
          this.readyState = FakeWebSocket.CONNECTING;
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
        close(code = 1000, reason = "") {{
          if (this.readyState === FakeWebSocket.CLOSED) return;
          this.readyState = FakeWebSocket.CLOSED;
          this.emit("close", {{ code, reason }});
        }}
      }}

      const states = [];
      const received = [];
      const timers = [];
      let authorizations = 0;
      const sealStore = createRoomSealStore();
      let activeSeal = null;
      const transport = createChatRoomTransport({{
        authorize: async (room) => {{
          authorizations += 1;
          return {{
            scope: room.scope,
            passphrase: "room secret",
            roomName: "channel-alpha",
            webSocketUrl: "/api/chat/channel-alpha/ws?ticket=one",
          }};
        }},
        resolveSeal: async (room, roomName) =>
          activeSeal ||= await sealStore.resolve(room.scope, roomName),
        onPlain: (plain, room) => received.push({{ plain, scope: room.scope }}),
        onState: (state) => states.push(state),
        WebSocketImpl: FakeWebSocket,
        locationLike: {{ protocol: "https:", host: "relay.example" }},
        setTimeoutImpl: (callback, delay) => {{
          timers.push({{ callback, delay }});
          return timers.length;
        }},
        clearTimeoutImpl: () => {{}},
      }});

      await transport.connect({{ scope: "channel-alpha" }});
      const socket = FakeWebSocket.instances[0];
      socket.open();
      await transport.send({{ type: "chat", text: "hello" }}, {{ persist: true }});

      const key = await derivePassphraseKey("room secret", "channel-alpha");
      const incoming = await encryptObject(
        await sealFrame({{ type: "chat", text: "replayed" }}, activeSeal), key);
      socket.emit("message", {{ data: JSON.stringify(incoming) }});
      for (let attempt = 0; attempt < 20 && !received.length; attempt += 1) {{
        await new Promise((resolve) => setTimeout(resolve, 5));
      }}

      transport.suspend();
      const timerCountAfterSuspend = timers.length;
      socket.close(1011, "relay restarted");

      process.stdout.write(JSON.stringify({{
        authorizations,
        states,
        socketUrl: socket.url,
        sentEnvelope: JSON.parse(socket.sent[0]),
        received,
        connected: transport.connected,
        state: transport.state,
        timerCountAfterSuspend,
        timerCountFinal: timers.length,
      }}));
    """
    result = _run_module(script)
    assert result["authorizations"] == 1
    assert result["states"][:3] == ["authorizing", "connecting", "connected"]
    assert result["states"][-1] == "suspended"
    assert result["socketUrl"] == (
        "wss://relay.example/api/chat/channel-alpha/ws?ticket=one"
    )
    assert result["sentEnvelope"]["persist"] is True
    assert result["received"] == [
        {
            "plain": {"type": "chat", "text": "replayed"},
            "scope": "channel-alpha",
        }
    ]
    assert result["connected"] is False
    assert result["state"] == "suspended"
    assert result["timerCountFinal"] == result["timerCountAfterSuspend"]


def test_room_transport_delivers_a_replayed_backlog_as_one_batch():
    # The relay replays a room's retained history to every joining socket, one
    # cipher envelope per message, closed by a "forkmesh-history-end" marker.
    # Handed over frame by frame that backlog repaints the log once per
    # message, so the transport buffers it into a single onHistory call and
    # keeps live frames strictly after it.
    script = f"""
      import {{ createChatRoomTransport }} from {json.dumps(TRANSPORT.as_uri())};
      import {{ derivePassphraseKey, encryptObject }}
        from {json.dumps(CRYPTO.as_uri())};
      import {{ createRoomSealStore, sealFrame }}
        from {json.dumps(SEAL.as_uri())};

      class FakeWebSocket {{
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;
        static instances = [];
        constructor(url) {{
          this.url = url;
          this.readyState = FakeWebSocket.CONNECTING;
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
        close() {{ this.readyState = FakeWebSocket.CLOSED; }}
      }}

      const key = await derivePassphraseKey("room secret", "channel-alpha");
      const seal = await createRoomSealStore().resolve(
        "channel-alpha", "channel-alpha");
      const replay = async (text) => {{
        const envelope = await encryptObject(
          await sealFrame({{ type: "chat", text }}, seal), key);
        envelope.historyReplay = true;
        return JSON.stringify(envelope);
      }};

      const batches = [];
      const live = [];
      const transport = createChatRoomTransport({{
        authorize: async (room) => ({{
          scope: room.scope,
          passphrase: "room secret",
          roomName: "channel-alpha",
          webSocketUrl: "/api/chat/channel-alpha/ws?ticket=one",
        }}),
        resolveSeal: async () => seal,
        onPlain: (plain) => live.push(plain.text),
        onHistory: (plains) => batches.push(plains.map((plain) => plain.text)),
        onState: () => {{}},
        WebSocketImpl: FakeWebSocket,
        locationLike: {{ protocol: "https:", host: "relay.example" }},
        setTimeoutImpl: () => 0,
        clearTimeoutImpl: () => {{}},
      }});

      await transport.connect({{ scope: "channel-alpha" }});
      const socket = FakeWebSocket.instances[0];
      socket.open();
      const backlog = [
        await replay("one"), await replay("two"), await replay("three"),
      ];
      const settle = async () => {{
        for (let attempt = 0; attempt < 40; attempt += 1) {{
          await new Promise((resolve) => setTimeout(resolve, 5));
        }}
      }};
      for (const frame of backlog) socket.emit("message", {{ data: frame }});
      const batchesBeforeMarker = batches.length;
      socket.emit("message", {{
        data: JSON.stringify({{ kind: "forkmesh-history-end", v: 1 }}),
      }});
      await settle();
      const afterMarker = batches.map((batch) => batch.slice());
      const fresh = await encryptObject(
        await sealFrame({{ type: "chat", text: "live" }}, seal), key);
      socket.emit("message", {{ data: JSON.stringify(fresh) }});
      await settle();

      process.stdout.write(JSON.stringify({{
        batchesBeforeMarker, afterMarker, batches, live,
      }}));
    """
    result = _run_module(script)
    # Nothing is handed over until the marker says the backlog is complete.
    assert result["batchesBeforeMarker"] == 0
    # Then all of it lands at once, in the order the relay replayed it.
    assert result["afterMarker"] == [["one", "two", "three"]]
    # A live frame arriving afterwards is a normal message, not more backlog.
    assert result["batches"] == [["one", "two", "three"]]
    assert result["live"] == ["live"]


def test_room_transport_flushes_a_backlog_a_relay_never_terminates():
    # An older relay replays retained frames without the end marker, and a room
    # with no history sends neither. Both still owe the consumer exactly one
    # onHistory call, or a chat that holds its loading state waits forever.
    script = f"""
      import {{ createChatRoomTransport }} from {json.dumps(TRANSPORT.as_uri())};
      import {{ derivePassphraseKey, encryptObject }}
        from {json.dumps(CRYPTO.as_uri())};
      import {{ createRoomSealStore, sealFrame }}
        from {json.dumps(SEAL.as_uri())};

      class FakeWebSocket {{
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;
        static instances = [];
        constructor(url) {{
          this.url = url;
          this.readyState = FakeWebSocket.CONNECTING;
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
        send() {{}}
        close() {{ this.readyState = FakeWebSocket.CLOSED; }}
      }}

      // Fire pending timers on demand so the fallback deadlines are testable
      // without waiting them out.
      const timers = new Map();
      let nextTimer = 1;
      const runTimers = () => {{
        const pending = [...timers.entries()];
        timers.clear();
        for (const [, entry] of pending) entry.callback();
        return pending.map(([, entry]) => entry.delay);
      }};

      const key = await derivePassphraseKey("room secret", "channel-alpha");
      const seal = await createRoomSealStore().resolve(
        "channel-alpha", "channel-alpha");
      const settle = async () => {{
        for (let attempt = 0; attempt < 40; attempt += 1) {{
          await new Promise((resolve) => setTimeout(resolve, 5));
        }}
      }};
      const build = (label) => {{
        const batches = [];
        const transport = createChatRoomTransport({{
          authorize: async (room) => ({{
            scope: room.scope,
            passphrase: "room secret",
            roomName: "channel-alpha",
            webSocketUrl: "/api/chat/channel-alpha/ws?ticket=" + label,
          }}),
          resolveSeal: async () => seal,
          onPlain: () => {{}},
          onHistory: (plains) => batches.push(plains.map((plain) => plain.text)),
          onState: () => {{}},
          WebSocketImpl: FakeWebSocket,
          locationLike: {{ protocol: "https:", host: "relay.example" }},
          setTimeoutImpl: (callback, delay) => {{
            const id = nextTimer;
            nextTimer += 1;
            timers.set(id, {{ callback, delay }});
            return id;
          }},
          clearTimeoutImpl: (id) => {{ timers.delete(id); }},
        }});
        return {{ transport, batches }};
      }};

      const unterminated = build("unterminated");
      await unterminated.transport.connect({{ scope: "channel-alpha" }});
      const first = FakeWebSocket.instances.at(-1);
      first.open();
      const envelope = await encryptObject(
        await sealFrame({{ type: "chat", text: "kept" }}, seal), key);
      envelope.historyReplay = true;
      first.emit("message", {{ data: JSON.stringify(envelope) }});
      // Let the frame reach the buffer before reading which deadline is armed:
      // holding a frame should shorten the wait to the inter-frame gap.
      await settle();
      const replayDelays = runTimers();
      await settle();

      const quiet = build("quiet");
      await quiet.transport.connect({{ scope: "channel-alpha" }});
      FakeWebSocket.instances.at(-1).open();
      const idleDelays = runTimers();
      await settle();

      process.stdout.write(JSON.stringify({{
        unterminated: unterminated.batches,
        quiet: quiet.batches,
        replayDelays,
        idleDelays,
      }}));
    """
    result = _run_module(script)
    assert result["unterminated"] == [["kept"]]
    # A silent room still resolves, so "Loading messages…" always gives way.
    assert result["quiet"] == [[]]
    # A frame in hand only waits out the short inter-frame gap; a room that has
    # sent nothing waits the longer post-open deadline.
    assert max(result["replayDelays"]) < min(result["idleDelays"])


def test_thread_summary_names_the_latest_replier_and_their_fellow_repliers():
    # The summary row under a message shows who replied, so the model owes it
    # the repliers most recent first (one entry each) and the last one by name.
    script = f"""
      import {{ createThreadStore }} from {json.dumps(THREAD_MODEL.as_uri())};
      const store = createThreadStore();
      store.registerRoot({{ id: "root", text: "hello", channel: "#general" }});
      const replies = [
        {{ id: "a", sender: "carol", senderId: "peer-carol", ts: 1 }},
        {{ id: "b", sender: "dave", senderId: "peer-dave", ts: 2 }},
        {{ id: "c", sender: "carol", senderId: "peer-carol", ts: 3 }},
        {{ id: "d", sender: "erin", senderId: "peer-erin", ts: 4 }},
      ];
      for (const reply of replies) store.addReply({{ ...reply, rootId: "root" }});
      process.stdout.write(JSON.stringify({{
        summary: store.summary("root"),
        empty: store.summary("missing"),
      }}));
    """
    result = _run_module(script)
    summary = result["summary"]
    assert summary["count"] == 4
    assert summary["latestSender"] == "erin"
    assert summary["latestSenderId"] == "peer-erin"
    # Most recent first, and carol appears once despite replying twice.
    assert [replier["sender"] for replier in summary["repliers"]] == [
        "erin", "carol", "dave",
    ]
    assert result["empty"]["count"] == 0
    assert result["empty"]["repliers"] == []
    assert result["empty"]["latestSender"] == ""


def test_public_chat_thread_summary_shows_who_replied():
    css = (PUBLIC / "chat.css").read_text(encoding="utf-8")
    summary = FULL_CHAT[
        FULL_CHAT.index("function renderThreadSummary("):
        FULL_CHAT.index("function threadMessageHeader(")
    ]
    assert "summary.repliers.slice(0, THREAD_SUMMARY_FACES)" in summary
    assert "makeAvatar(" in summary
    assert "name.textContent = summary.latestSender" in summary
    assert "threadCountLabel(summary.count)" in summary
    assert 'last reply from ${summary.latestSender}' in summary
    assert ".chat-thread-summary-faces" in css
    assert ".chat-thread-summary-name" in css
    assert ".chat-thread-summary-count" in css


def test_public_chat_avatars_are_round_on_every_surface():
    # One avatar element backs message rows, the people pane, mention
    # suggestions, direct-message rows and thread headers. chat.css loads after
    # chat.html's inline block and therefore decides the shape for all of them.
    css = (PUBLIC / "chat.css").read_text(encoding="utf-8")
    for source in (css, HTML.read_text(encoding="utf-8")):
        # Anchored on the line start so this reads the base rule, not one of the
        # per-surface overrides (".chat-direct-row .chat-avatar {").
        rule = source[source.index("\n      .chat-avatar {"):]
        rule = rule[:rule.index("}")]
        assert "border-radius: 50%" in rule
    sized = css[css.index(".chat-thread-message-head .chat-avatar {"):]
    assert "border-radius" not in sized[:sized.index("}")]


def test_shared_aes_envelope_round_trips_with_existing_wire_shape():
    script = f"""
      import {{ derivePassphraseKey, encryptObject, decryptObject }}
        from {json.dumps(CRYPTO.as_uri())};
      const key = await derivePassphraseKey("test-passphrase", "world-general");
      const envelope = await encryptObject({{ type: "chat", text: "hello" }}, key);
      const plain = await decryptObject(envelope, key);
      process.stdout.write(JSON.stringify({{
        keys: Object.keys(envelope).sort(),
        kind: envelope.kind,
        version: envelope.v,
        plain,
      }}));
    """
    result = _run_module(script)
    assert result == {
        "keys": ["body", "kind", "nonce", "tag", "v"],
        "kind": "cipher",
        "version": 1,
        "plain": {"type": "chat", "text": "hello"},
    }


def test_meeting_proof_binds_avatar_to_exact_text_and_attachment():
    script = f"""
      import {{ createMeetingBinding, signMeetingProof, verifyMeetingProof }}
        from {json.dumps(CRYPTO.as_uri())};
      const binding = await createMeetingBinding();
      const message = {{
        participantId: "participant-1",
        messageId: "message-1",
        senderId: "sender-1",
        ts: 1700000000000,
        text: "hello",
        attachment: {{
          fileName: "notes.txt",
          fileMime: "text/plain",
          file: btoa("private notes"),
          size: 13,
        }},
      }};
      const proof = await signMeetingProof(binding.privateKey, message);
      const valid = await verifyMeetingProof(binding.publicJwk, proof, message);
      const altered = await verifyMeetingProof(binding.publicJwk, proof, {{
        ...message,
        text: "altered",
      }});
      const alteredFile = await verifyMeetingProof(binding.publicJwk, proof, {{
        ...message,
        attachment: {{ ...message.attachment, file: btoa("changed notes") }},
      }});
      process.stdout.write(JSON.stringify({{
        valid,
        altered,
        alteredFile,
        extractable: binding.privateKey.extractable,
        publicKeys: Object.keys(binding.publicJwk).sort(),
        proofKeys: Object.keys(proof).sort(),
      }}));
    """
    result = _run_module(script)
    assert result == {
        "valid": True,
        "altered": False,
        "alteredFile": False,
        "extractable": False,
        "publicKeys": ["crv", "kty", "x", "y"],
        "proofKeys": ["participantId", "signature", "ts", "v"],
    }


def test_attachment_module_sanitizes_and_digests_bounded_entries():
    script = f"""
      import {{ attachmentFromEntry, normalizeAttachmentForProof }}
        from {json.dumps(ATTACHMENTS.as_uri())};
      const entry = {{
        fileName: "../release-notes.txt",
        fileMime: "TEXT/PLAIN",
        file: btoa("release notes"),
      }};
      const attachment = attachmentFromEntry(entry);
      const normalized = await normalizeAttachmentForProof(attachment);
      process.stdout.write(JSON.stringify({{
        attachment,
        normalized,
        oversized: attachmentFromEntry({{
          ...entry,
          file: "A".repeat(Math.ceil(1024 * 1024 * 4 / 3) + 8),
        }}),
      }}));
    """
    result = _run_module(script)
    assert result["attachment"] == {
        "fileName": "release-notes.txt",
        "fileMime": "text/plain",
        "file": "cmVsZWFzZSBub3Rlcw==",
        "size": 13,
    }
    assert result["normalized"]["fileName"] == "release-notes.txt"
    assert result["normalized"]["fileMime"] == "text/plain"
    assert result["normalized"]["size"] == 13
    assert len(result["normalized"]["digest"]) == 43
    assert result["oversized"] is None


def test_attachment_blobs_never_carry_an_active_document_type():
    script = f"""
      import {{ attachmentBlobType }}
        from {json.dumps(ATTACHMENTS.as_uri())};
      const types = [
        "text/html", "image/svg+xml", "application/xhtml+xml", "text/xml",
        "application/xml", "text/javascript", "image/PNG", "video/mp4",
        "text/plain", "application/pdf", "",
      ];
      process.stdout.write(JSON.stringify(types.map(attachmentBlobType)));
    """
    assert _run_module(script) == [
        "application/octet-stream", "application/octet-stream",
        "application/octet-stream", "application/octet-stream",
        "application/octet-stream", "application/octet-stream",
        "image/png", "video/mp4", "text/plain", "application/pdf",
        "application/octet-stream",
    ]
    dashboard_chat = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
    for source in (FULL_CHAT, dashboard_chat):
        assert "type: attachment.fileMime" not in source
        assert "attachment.fileMime.startsWith(" not in source
    assert "new Blob([bytes], { type: blobType })" in FULL_CHAT
    assert (
        "new Blob([bytes], { type: attachmentBlobType(attachment.fileMime) })"
        in dashboard_chat
    )
    local = dashboard_chat[
        dashboard_chat.index("  function attachmentBlobType("):
        dashboard_chat.index("  function attachmentFromEntry(")
    ]
    shared = ATTACHMENTS.read_text(encoding="utf-8")
    shared = shared[
        shared.index("export function attachmentBlobType("):
        shared.index("export function attachmentFromEntry(")
    ]
    pattern = re.compile(r"/\^\(\?:image.*\$/")
    assert pattern.search(local).group(0) == pattern.search(shared).group(0)


def test_full_chat_mentions_include_directory_users_and_refresh_open_popup():
    candidates = FULL_CHAT[
        FULL_CHAT.index("function mentionCandidates("):
        FULL_CHAT.index("function renderMentionSuggest(")
    ]
    directory = FULL_CHAT[
        FULL_CHAT.index("async function refreshUsersDirectory("):
        FULL_CHAT.index("async function markChatActivitySeen(")
    ]
    assert "registeredUsers.values()" in candidates
    assert "updateMentionSuggest();" in directory


def test_direct_thread_replies_use_the_canonical_room_wire_label():
    reply = FULL_CHAT[
        FULL_CHAT.index("function sendThreadReply("):
        FULL_CHAT.index("function cancelMessageEdit(")
    ]
    attachment = FULL_CHAT[
        FULL_CHAT.index("async function sendAttachment("):
        FULL_CHAT.index("async function sendAttachments(")
    ]
    assert "channel: channelWireLabel(activeChannel)" in reply
    assert "channel: channelWireLabel(channelAtSend)" in attachment


def test_channel_switching_preserves_cached_messages_and_labels_cache_misses():
    render = FULL_CHAT[
        FULL_CHAT.index("function renderActiveChannel()"):
        FULL_CHAT.index("// Insert a message record", FULL_CHAT.index("function renderActiveChannel()"))
    ]
    transport = FULL_CHAT[
        FULL_CHAT.index("function handleTransportState("):
        FULL_CHAT.index("function newRoomTransport(")
    ]
    history = FULL_CHAT[
        FULL_CHAT.index("function handleHistory("):
        FULL_CHAT.index("function handlePlain(")
    ]
    assert "const channelSyncState = new Map()" in FULL_CHAT
    assert 'syncState === "loading"' in render
    assert "Loading messages" in render
    # An open socket is not a loaded channel: the relay still has the room's
    # retained backlog on the wire. Marking the channel ready here painted the
    # empty "Say hi!" placeholder for the width of one replay, so the loading
    # label holds until handleHistory has taken that backlog in.
    assert "markChannelLoading(connectedChannel)" in transport
    assert 'channelSyncState.set(connectedChannel, "ready")' not in transport
    assert 'channelSyncState.set(channel, "ready")' in history


def test_people_cards_do_not_grow_to_fill_an_empty_sidebar():
    css = (PUBLIC / "chat.css").read_text(encoding="utf-8")
    html = HTML.read_text(encoding="utf-8")
    for source in (html, css):
        start = source.index(".chat-person {")
        rule = source[start:source.index("}", start)]
        assert "flex: 0 0 auto" in rule
        assert "flex: 1" not in rule
