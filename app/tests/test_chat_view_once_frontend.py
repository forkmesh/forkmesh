#!/usr/bin/env python3

import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
VIEW_ONCE = PUBLIC / "chat-view-once.js"
TRANSPORT = PUBLIC / "chat-room-transport.js"
CRYPTO = PUBLIC / "chat-crypto.js"
CHAT_TEXT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
CHAT_HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
RECORDER_TEXT = (PUBLIC / "chat-view-once-recorder.js").read_text(
    encoding="utf-8")
MEDIA = "c" * 32


def _run(script):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True, cwd=PUBLIC,
    )
    return json.loads(result.stdout)


def _fake_server():
    return """
      const server = { media: new Map(), calls: [] };
      const request = async (path, options = {}) => {
        const method = options.method || "GET";
        const body = options.body ? JSON.parse(options.body) : null;
        server.calls.push(method + " " + path.replace(/[0-9a-f]{32}/g, "ID"));
        const fail = (status, error) => {
          const err = new Error(error); err.status = status; err.code = error;
          throw err;
        };
        const base = "/api/chat/direct-messages/" + "a".repeat(32) + "/media";
        if (method === "POST" && path === base) {
          server.media.set(MEDIA, { ...body, state: "uploading", parts: new Map() });
          return { media: { id: MEDIA, state: "uploading" } };
        }
        const item = server.media.get(MEDIA);
        if (path.startsWith(base + "?ids=")) {
          return { media: [{ id: MEDIA, state: item.state }] };
        }
        const part = path.match(/\\/parts\\/(\\d+)$/);
        if (part && method === "PUT") {
          item.parts.set(Number(part[1]), body.data); return { ok: true };
        }
        if (part && method === "GET") {
          if (item.state !== "ready") fail(410, "media_gone");
          return { data: item.parts.get(Number(part[1])), parts: item.parts.size };
        }
        if (path.endsWith("/finish")) { item.state = "ready"; return { ok: true }; }
        if (path.endsWith("/consume")) {
          item.parts.clear(); item.state = "opened"; return { ok: true };
        }
        if (method === "DELETE") { item.parts.clear(); item.state = "withdrawn"; return {}; }
        fail(404, "not_found");
      };
    """


def test_upload_download_consume_round_trip_and_frame_codec():
    result = _run(f"""
      import {{ webcrypto }} from "node:crypto";
      import * as vo from {json.dumps(VIEW_ONCE.as_uri())};
      const MEDIA = {json.dumps(MEDIA)};
      {_fake_server()}
      const client = vo.createViewOnceClient({{ request, cryptoImpl: webcrypto }});
      const plain = new Uint8Array(vo.VIEW_ONCE_PART_BYTES + 5000);
      for (let i = 0; i < plain.length; i += 1) plain[i] = (i * 13) % 251;
      const copy = plain.slice();
      const blob = new Blob([plain], {{ type: "audio/webm;codecs=opus" }});
      const conversation = "a".repeat(32);
      const viewOnce = await client.upload(conversation, "voice", blob, 4200);
      const stored = [...server.media.get(MEDIA).parts.values()]
        .map((part) => Buffer.from(part, "base64"));
      const storedBytes = Buffer.concat(stored);
      const frame = {{ type: "chat", text: "🎤 Voice message", viewOnce }};
      const parsed = vo.viewOnceFromEntry(JSON.parse(JSON.stringify(frame)));
      const bytes = await client.download(conversation, parsed);
      const same = Buffer.compare(Buffer.from(bytes), Buffer.from(copy)) === 0;
      await client.consume(conversation, MEDIA);
      let gone = 0;
      try {{ await client.download(conversation, parsed); }}
      catch (error) {{ gone = error.status; }}
      const states = await client.status(conversation, [MEDIA, "nope"]);
      const tampered = {{ ...viewOnce, key: viewOnce.key.slice(0, 20) }};
      process.stdout.write(JSON.stringify({{
        same,
        parts: viewOnce.parts,
        size: viewOnce.size,
        storedSize: storedBytes.length,
        storedIsPlain: storedBytes.includes(Buffer.from(copy.subarray(0, 64))),
        mime: viewOnce.mime,
        durationMs: viewOnce.durationMs,
        calls: server.calls,
        gone,
        status: states.get(MEDIA).state,
        tampered: vo.viewOnceFromEntry({{ viewOnce: tampered }}),
        wrongKind: vo.viewOnceFromEntry({{ viewOnce: {{ ...viewOnce, kind: "video" }} }}),
        badParts: vo.viewOnceFromEntry({{ viewOnce: {{ ...viewOnce, parts: 9 }} }}),
        badMime: vo.viewOnceFromEntry({{ viewOnce: {{ ...viewOnce, mime: "text/html" }} }}),
      }}));
    """)
    assert result["same"] is True
    assert result["parts"] == 2
    assert result["size"] == result["storedSize"] == 512 * 1024 + 5000 + 16
    assert result["storedIsPlain"] is False
    assert result["mime"] == "audio/webm;codecs=opus"
    assert result["durationMs"] == 4200
    assert result["calls"][:4] == [
        "POST /api/chat/direct-messages/ID/media",
        "PUT /api/chat/direct-messages/ID/media/ID/parts/0",
        "PUT /api/chat/direct-messages/ID/media/ID/parts/1",
        "POST /api/chat/direct-messages/ID/media/ID/finish",
    ]
    assert "POST /api/chat/direct-messages/ID/media/ID/consume" in result["calls"]
    assert result["gone"] == 410
    assert result["status"] == "opened"
    assert result["tampered"] is None
    assert result["wrongKind"] is None
    assert result["badParts"] is None
    assert result["badMime"] is None


def test_wrong_key_does_not_decrypt():
    result = _run(f"""
      import {{ webcrypto }} from "node:crypto";
      import * as vo from {json.dumps(VIEW_ONCE.as_uri())};
      const sealed = await vo.encryptViewOnceMedia(new Uint8Array([1, 2, 3]), webcrypto);
      const other = await vo.encryptViewOnceMedia(new Uint8Array([1]), webcrypto);
      let failed = false;
      try {{ await vo.decryptViewOnceMedia(sealed.ciphertext, other.key, sealed.iv, webcrypto); }}
      catch (_) {{ failed = true; }}
      const ok = await vo.decryptViewOnceMedia(sealed.ciphertext, sealed.key, sealed.iv, webcrypto);
      process.stdout.write(JSON.stringify({{ failed, ok: [...ok],
        keyLength: sealed.key.length, ivLength: sealed.iv.length }}));
    """)
    assert result == {"failed": True, "ok": [1, 2, 3], "keyLength": 43,
                      "ivLength": 16}


def test_transport_holds_view_once_receipts_and_relays_media_states():
    result = _run(f"""
      import {{ createChatRoomTransport }} from {json.dumps(TRANSPORT.as_uri())};
      import {{ decryptObject, derivePassphraseKey, encryptObject }} from {json.dumps(CRYPTO.as_uri())};
      import {{ acceptSealKey, createRoomSealStore, isSealKeyFrame, sealFrame, sealKeyFrame }}
        from {json.dumps((PUBLIC / "chat-seal.js").as_uri())};
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
        constructor(url) {{ this.url = url; this.readyState = 0; this.sent = [];
          this.listeners = new Map(); FakeWebSocket.instances.push(this); }}
        addEventListener(type, callback) {{
          const list = this.listeners.get(type) || []; list.push(callback);
          this.listeners.set(type, list); }}
        emit(type, event = {{}}) {{
          for (const callback of this.listeners.get(type) || []) callback(event); }}
        open() {{ this.readyState = 1; this.emit("open"); }}
        send(value) {{ this.sent.push(value); }}
        close() {{ this.readyState = 3; }}
      }}
      const media = [];
      const transport = createChatRoomTransport({{
        authorize: async (room) => ({{ scope: room.scope, passphrase: "room secret",
          roomName: "direct-alpha", webSocketUrl: "/ws" }}),
        resolveSeal: (room, name) => sealStore.resolve(room.scope, name, "", dm),
        isDirectRoom: () => true,
        holdReceipt: (plain) => Boolean(plain.viewOnce),
        onMedia: (id, state, room) => media.push([id, state, room.scope]),
        WebSocketImpl: FakeWebSocket,
        locationLike: {{ protocol: "https:", host: "relay.example" }},
      }});
      await transport.connect({{ scope: "direct:alpha" }});
      const socket = FakeWebSocket.instances[0];
      socket.open();
      const key = await derivePassphraseKey("room secret", "direct-alpha");
      const settle = async () => {{
        for (let i = 0; i < 20; i += 1) await new Promise((r) => setTimeout(r, 10));
      }};
      socket.emit("message", {{ data: JSON.stringify({{ kind: "forkmesh-history-end", v: 1 }}) }});
      await settle();
      const myBundle = (await Promise.all(socket.sent.map((raw) =>
        decryptObject(JSON.parse(raw), key)))).find(isSealKeyFrame);
      await acceptSealKey(myBundle, peerSeal);
      socket.emit("message", {{ data: JSON.stringify(
        await encryptObject(await sealKeyFrame(peerSeal), key)) }});
      await settle();
      const fromPeer = async (plain) =>
        encryptObject(await sealFrame(plain, peerSeal, "direct-alpha"), key);
      const held = await fromPeer({{ type: "chat", id: "vo", viewOnce: {{ v: 1 }} }});
      held.persist = true; held.ref = "a".repeat(40);
      const normal = await fromPeer({{ type: "chat", id: "txt", text: "hi" }});
      normal.persist = true; normal.ref = "b".repeat(40);
      socket.emit("message", {{ data: JSON.stringify(held) }});
      socket.emit("message", {{ data: JSON.stringify(normal) }});
      await settle();
      const before = socket.sent.filter((s) => s.includes("forkmesh-received"));
      const released = transport.releaseReceipt("vo");
      const again = transport.releaseReceipt("vo");
      await settle();
      const after = socket.sent.filter((s) => s.includes("forkmesh-received"));
      for (const frame of [
        {{ kind: "forkmesh-media", v: 1, id: {json.dumps(MEDIA)}, state: "opened" }},
        {{ kind: "forkmesh-media", v: 1, id: "bad", state: "opened" }},
        {{ kind: "forkmesh-media", v: 1, id: {json.dumps(MEDIA)}, state: "ready" }},
      ]) socket.emit("message", {{ data: JSON.stringify(frame) }});
      await settle();
      process.stdout.write(JSON.stringify({{
        before: before.map((s) => JSON.parse(s).refs),
        after: after.map((s) => JSON.parse(s).refs),
        released, again, media,
      }}));
    """)
    assert result["before"] == [["b" * 40]]
    assert result["after"] == [["b" * 40], ["a" * 40]]
    assert result["released"] is True
    assert result["again"] is False
    assert result["media"] == [[MEDIA, "opened", "direct:alpha"]]


def test_chat_page_wires_view_once_without_save_actions():
    assert 'id="chat-view-once-video"' in CHAT_HTML
    assert 'id="chat-view-once-voice"' in CHAT_HTML
    assert "holdReceipt: (plain) => holdsViewOnceReceipt(plain)" in CHAT_TEXT
    assert "onMedia: (id, state) => applyViewOnceState(id, state)" in CHAT_TEXT
    assert "const viewOnce = isDirectMessageKey(scope) ? viewOnceFromEntry(entry) : null;" in CHAT_TEXT
    assert "const attachment = viewOnce ? null : attachmentFromEntry(entry);" in CHAT_TEXT
    viewer = (PUBLIC / "chat-view-once.js").read_text(encoding="utf-8")
    assert "download" not in viewer.split("export function openViewOnceViewer")[1] \
        .replace("nodownload", "")
    assert "revokeObjectURL" in viewer
    assert "registerViewOnceFilterStages" in RECORDER_TEXT
    assert "const viewOnceFilterSupport = installViewOnceFilters();" in CHAT_TEXT
    assert "...viewOnceFilterChoices(viewOnceFilterSupport)," in CHAT_TEXT
    filters = (PUBLIC / "chat-view-once-filters.js").read_text(encoding="utf-8")
    assert "registerViewOnceFilterStages({" in filters
    assert "createFaceStage" in filters and "createVoiceStage" in filters


def test_face_stage_must_start_or_nothing_is_recorded():
    filters = PUBLIC / "chat-view-once-filters.js"
    result = _run(f"""
      import * as filters from {json.dumps(filters.as_uri())};
      const made = [];
      const fake = (options) => {{
        const stage = {{
          options, stopped: false, readyAfter: options.readyAfter ?? 2, polls: 0,
          track: options.noTrack ? null : {{ kind: "video" }},
          face: () => options.faceId === "nope" ? "" : options.faceId,
          background: () => options.backgroundId,
          ready() {{ this.polls += 1; return this.polls >= this.readyAfter; }},
          stop() {{ this.stopped = true; }},
        }};
        made.push(stage);
        return stage;
      }};
      let clock = 0;
      const run = async (choice, extra = {{}}) => {{
        try {{
          const stage = await filters.startViewOnceFaceStage({{}}, choice, {{
            createFaceStageImpl: (options) => fake({{ ...options, ...extra }}),
            readyMs: 1000, now: () => clock, wait: async (ms) => {{ clock += ms; }},
          }});
          return {{ ok: true, face: stage.face(), background: stage.background() }};
        }} catch (error) {{
          return {{ ok: false, error: error.message, stopped: made.at(-1).stopped }};
        }}
      }};
      const ok = await run({{ faceId: "theo", backgroundId: "office", backgroundImage: "" }});
      const unknown = await run({{ faceId: "nope", backgroundId: "" }});
      const noTrack = await run({{ faceId: "theo", backgroundId: "" }}, {{ noTrack: true }});
      const slow = await run({{ faceId: "", backgroundId: "blur" }}, {{ readyAfter: 1e9 }});
      const store = new Map([
        ["forkmesh.chat.callFace", "ren"],
        ["forkmesh.chat.callBackground", "custom"],
      ]);
      const storage = {{ getItem: (key) => store.get(key) ?? null }};
      const choices = filters.viewOnceFilterChoices(
        {{ faces: true, backgrounds: true, voices: true }}, storage);
      const plain = filters.viewOnceFilterChoices(
        {{ faces: false, backgrounds: false, voices: false }}, {{ getItem: () => null }});
      process.stdout.write(JSON.stringify({{
        ok, unknown, noTrack, slow,
        faces: choices.faces.map(([id]) => id),
        backgrounds: choices.backgrounds.map(([id]) => id),
        voices: choices.voices.map(([id]) => id),
        defaults: choices.defaults,
        match: choices.resolveVoice("face", "theo"),
        plain: {{ faces: plain.faces, backgrounds: plain.backgrounds, voices: plain.voices,
          defaults: plain.defaults }},
      }}));
    """)
    assert result["ok"] == {"ok": True, "face": "theo", "background": "office"}
    assert result["unknown"] == {"ok": False, "error": "face_unavailable", "stopped": True}
    assert result["noTrack"] == {"ok": False, "error": "face_stage_unavailable", "stopped": True}
    assert result["slow"] == {"ok": False, "error": "face_stage_timeout", "stopped": True}
    assert result["faces"] == ["", "maya", "theo", "kofi", "ingrid", "ren"]
    assert result["backgrounds"] == [
        "", "blur", "blur-strong", "world", "office", "meeting", "library", "skyline"]
    assert result["voices"][:2] == ["face", "off"]
    assert result["defaults"] == {
        "face": "ren", "background": "", "backgroundImage": "", "voice": "face"}
    assert result["match"] == "theo"
    assert result["plain"] == {
        "faces": [], "backgrounds": [], "voices": [],
        "defaults": {"face": "", "background": "", "backgroundImage": "", "voice": "face"}}
