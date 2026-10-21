export const GROUP_CALL_TYPES = new Set(["group-call-presence", "group-call-key"]);

const HEARTBEAT_MS = 20000;
const PRESENCE_TTL_MS = 60000;
const ROTATE_DELAY_MS = 1000;
const PING_MS = 20000;
const RECONNECT_TRIES = 5;
const RECONNECT_BASE_MS = 1000;
const PEER_RE = /^[0-9a-f]{32}$/;

function bytesToBase64(bytes) {
  let text = "";
  for (const byte of bytes) text += String.fromCharCode(byte);
  return btoa(text);
}

function base64ToBytes(text) {
  const raw = atob(String(text || ""));
  const bytes = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i += 1) bytes[i] = raw.charCodeAt(i);
  return bytes;
}

export function groupCallEncryptionMode(scope = globalThis) {
  if (typeof scope.RTCRtpScriptTransform === "function") return "script";
  const sender = scope.RTCRtpSender?.prototype;
  if (sender && typeof sender.createEncodedStreams === "function") return "streams";
  return "";
}

export function createGroupCalls({
  sendFrame,
  fetchTicket,
  accountName,
  onChange = () => {},
  workerUrl = "/chat-group-call-crypto.js",
  scope = globalThis,
}) {
  const presence = new Map();
  const owners = new Map();
  const keyring = new Map();
  let call = null;
  let lastError = "";

  function roster(channelKey) {
    const now = Date.now();
    const entries = presence.get(channelKey);
    if (!entries) return [];
    const live = [];
    for (const [peer, entry] of entries) {
      if (now - entry.at > PRESENCE_TTL_MS) entries.delete(peer);
      else live.push({ peer, name: entry.name });
    }
    return live;
  }

  function nameFor(channelKey, peer) {
    return presence.get(channelKey)?.get(peer)?.name || "";
  }

  function claim(channelKey, peer, name) {
    if (!owners.has(channelKey)) owners.set(channelKey, new Map());
    const bound = owners.get(channelKey);
    const owner = bound.get(peer);
    const now = Date.now();
    if (owner && now - owner.at <= PRESENCE_TTL_MS && owner.name !== name) return false;
    bound.set(peer, { name, at: now });
    return true;
  }

  function release(channelKey, peer) {
    owners.get(channelKey)?.delete(peer);
    keyring.get(channelKey)?.delete(peer);
  }

  function notePresence(channelKey, peer, name, state) {
    if (!presence.has(channelKey)) presence.set(channelKey, new Map());
    const entries = presence.get(channelKey);
    if (state === "out") {
      entries.delete(peer);
      release(channelKey, peer);
    } else {
      entries.set(peer, { name, at: Date.now() });
    }
  }

  function announce(state) {
    if (!call) return;
    void sendFrame(call.channelKey, "group-call-presence", {
      peer: call.peer,
      call: state,
      media: call.media,
    });
  }

  function shareKey() {
    if (!call?.sendKey) return;
    void sendFrame(call.channelKey, "group-call-key", {
      peer: call.peer,
      keyId: call.sendKeyId,
      key: bytesToBase64(call.sendKey),
    });
  }

  function newKey() {
    const raw = new Uint8Array(16);
    scope.crypto.getRandomValues(raw);
    return raw;
  }

  function useKey(raw, keyId) {
    call.worker.postMessage({ op: "send-key", raw: raw.buffer.slice(0), keyId });
  }

  function freshKey(current) {
    current.sendKeyId = (current.sendKeyId + 1) & 0xff;
    current.sendKey = newKey();
    useKey(current.sendKey, current.sendKeyId);
    shareKey();
  }

  function rotateKey() {
    if (!call) return;
    const current = call;
    const raw = newKey();
    current.sendKeyId = (current.sendKeyId + 1) & 0xff;
    current.sendKey = raw;
    shareKey();
    const keyId = current.sendKeyId;
    setTimeout(() => {
      if (call === current && current.sendKeyId === keyId) useKey(raw, keyId);
    }, ROTATE_DELAY_MS);
  }

  function handKey(peer, keyId, raw) {
    if (!call) return;
    call.worker.postMessage({ op: "key", peer, keyId, raw: raw.buffer.slice(0) });
  }

  function handleFrame(plain, channelKey) {
    if (!plain || !GROUP_CALL_TYPES.has(plain.type)) return;
    const peer = String(plain.peer || "");
    if (!PEER_RE.test(peer)) return;
    if (call && peer === call.peer) return;
    const name = String(plain.sender || "").trim().toLowerCase();
    if (!name) return;
    if (plain.type === "group-call-presence") {
      const state = plain.call === "out" ? "out" : "in";
      if (!claim(channelKey, peer, name)) return;
      const known = presence.get(channelKey)?.has(peer);
      notePresence(channelKey, peer, name, state);
      if (call && call.channelKey === channelKey) {
        if (state === "in" && !known) shareKey();
        if (state === "out") {
          call.worker.postMessage({ op: "forget", peer });
          rotateKey();
        }
        labelTiles();
      }
      onChange();
      return;
    }
    const keyId = Number(plain.keyId);
    if (!Number.isInteger(keyId) || keyId < 0 || keyId > 255) return;
    let raw;
    try {
      raw = base64ToBytes(plain.key);
    } catch (_) {
      return;
    }
    if (raw.length !== 16) return;
    if (!claim(channelKey, peer, name)) return;
    if (!keyring.has(channelKey)) keyring.set(channelKey, new Map());
    keyring.get(channelKey).set(peer, { keyId, raw, name });
    if (call && call.channelKey === channelKey) handKey(peer, keyId, raw);
  }

  function attachSender(sender, kind) {
    if (call.mode === "script") {
      sender.transform = new scope.RTCRtpScriptTransform(call.worker, { direction: "encrypt", kind });
      return;
    }
    const { readable, writable } = sender.createEncodedStreams();
    call.worker.postMessage({ op: "stream", direction: "encrypt", kind, readable, writable }, [readable, writable]);
  }

  function attachReceiver(receiver, kind, peer) {
    if (call.mode === "script") {
      receiver.transform = new scope.RTCRtpScriptTransform(call.worker, { direction: "decrypt", kind, peer });
      return;
    }
    const { readable, writable } = receiver.createEncodedStreams();
    call.worker.postMessage({ op: "stream", direction: "decrypt", kind, peer, readable, writable }, [readable, writable]);
  }

  function labelTiles() {
    if (!call) return;
    for (const [peer, tile] of call.tiles) {
      tile.name = nameFor(call.channelKey, peer) || "Someone";
    }
    onChange();
  }

  function handleTrack(event) {
    const stream = event.streams[0];
    const peer = stream?.id || "";
    if (!call || !PEER_RE.test(peer) || call.decorated.has(event.receiver)) return;
    call.decorated.add(event.receiver);
    attachReceiver(event.receiver, event.track.kind, peer);
    let tile = call.tiles.get(peer);
    if (!tile) {
      tile = { peer, stream: new scope.MediaStream(), name: nameFor(call.channelKey, peer) || "Someone" };
      call.tiles.set(peer, tile);
    }
    tile.stream.addTrack(event.track);
    event.track.onended = () => {
      tile.stream.removeTrack(event.track);
      onChange();
    };
    onChange();
  }

  async function handleSignal(current, message) {
    if (call !== current) return;
    const pc = current.pc;
    if (!pc) return;
    if (message.type === "joined") {
      current.peer = message.peer;
      announce("in");
      freshKey(current);
      for (const [peer, entry] of keyring.get(current.channelKey) || []) {
        handKey(peer, entry.keyId, entry.raw);
      }
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      current.ws.send(JSON.stringify({ type: "offer", sdp: pc.localDescription.sdp }));
    } else if (message.type === "answer") {
      await pc.setRemoteDescription({ type: "answer", sdp: message.sdp });
      current.state = "connecting";
    } else if (message.type === "offer") {
      await pc.setRemoteDescription({ type: "offer", sdp: message.sdp });
      const answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);
      current.ws.send(JSON.stringify({ type: "answer", sdp: pc.localDescription.sdp, rev: message.rev }));
    } else if (message.type === "peer-left") {
      const tile = current.tiles.get(message.peer);
      if (tile) current.tiles.delete(message.peer);
      current.worker.postMessage({ op: "forget", peer: message.peer });
      rotateKey();
    } else if (message.type === "error") {
      if (message.error === "room_full") {
        current.error = "This call is full.";
        leave();
      } else {
        connectionLost(current, "The call could not connect.");
      }
    }
    onChange();
  }

  async function join(channelKey, channelId, media) {
    if (call) return { error: "Leave the current call first." };
    const mode = groupCallEncryptionMode(scope);
    if (!mode) return { error: "This browser cannot join end-to-end encrypted group calls." };
    let local;
    try {
      local = await scope.navigator.mediaDevices.getUserMedia({
        audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true },
        video: media === "video" ? { width: { ideal: 640 }, height: { ideal: 360 }, frameRate: { ideal: 24 } } : false,
      });
    } catch (_) {
      return { error: "Allow the microphone" + (media === "video" ? " and camera" : "") + " to join the call." };
    }
    const current = {
      channelKey,
      channelId,
      media,
      mode,
      peer: "",
      pc: null,
      ws: null,
      worker: new scope.Worker(workerUrl, { name: "forkmesh-group-call-crypto" }),
      local,
      tiles: new Map(),
      decorated: new WeakSet(),
      sendKey: newKey(),
      sendKeyId: 0,
      muted: false,
      cameraOff: false,
      state: "joining",
      error: "",
      timers: [],
      retries: 0,
      reconnecting: null,
    };
    call = current;
    current.timers.push(setInterval(() => announce("in"), HEARTBEAT_MS));
    current.timers.push(setInterval(() => {
      if (current.ws?.readyState === 1) current.ws.send(JSON.stringify({ type: "ping" }));
    }, PING_MS));
    const connected = await connect(current);
    if (!connected && call === current) {
      current.error = "Group calls are not available right now.";
      leave();
      return { error: current.error };
    }
    onChange();
    return { ok: true };
  }

  function dropConnection(current) {
    if (current.peer) {
      void sendFrame(current.channelKey, "group-call-presence", { peer: current.peer, call: "out" });
    }
    current.peer = "";
    const { ws, pc } = current;
    current.ws = null;
    current.pc = null;
    if (ws) {
      ws.onclose = null;
      ws.onmessage = null;
      try {
        if (ws.readyState === 1) ws.send(JSON.stringify({ type: "leave" }));
        ws.close();
      } catch (_) {}
    }
    if (pc) {
      pc.ontrack = null;
      pc.onconnectionstatechange = null;
      pc.close();
    }
    current.tiles.clear();
    current.decorated = new WeakSet();
  }

  function connectionLost(current, reason) {
    if (call !== current || current.reconnecting) return;
    dropConnection(current);
    if (current.retries >= RECONNECT_TRIES) {
      current.error = reason;
      leave();
      return;
    }
    const wait = RECONNECT_BASE_MS * 2 ** current.retries;
    current.retries += 1;
    current.state = "reconnecting";
    onChange();
    current.reconnecting = setTimeout(async () => {
      current.reconnecting = null;
      if (call !== current) return;
      if (!(await connect(current)) && call === current) connectionLost(current, reason);
    }, wait);
  }

  async function connect(current) {
    let access;
    try {
      access = await fetchTicket(current.channelId);
    } catch (_) {
      return false;
    }
    if (call !== current || !access?.url || !access?.ticket) return false;
    const config = { bundlePolicy: "max-bundle", rtcpMuxPolicy: "require" };
    if (current.mode === "streams") config.encodedInsertableStreams = true;
    const pc = new scope.RTCPeerConnection(config);
    current.pc = pc;
    for (const kind of ["audio", "video"]) {
      const track = current.local.getTracks().find((item) => item.kind === kind) || null;
      const transceiver = track
        ? pc.addTransceiver(track, { direction: "sendonly", streams: [current.local] })
        : pc.addTransceiver(kind, { direction: "sendonly" });
      attachSender(transceiver.sender, kind);
    }
    pc.ontrack = handleTrack;
    pc.onconnectionstatechange = () => {
      if (call !== current || current.pc !== pc) return;
      if (pc.connectionState === "connected") {
        current.state = "live";
        current.retries = 0;
      }
      if (pc.connectionState === "failed") connectionLost(current, "The call connection failed.");
      onChange();
    };
    const ws = new scope.WebSocket(access.url);
    current.ws = ws;
    ws.onopen = () => ws.send(JSON.stringify({ type: "join", ticket: access.ticket }));
    ws.onmessage = (event) => {
      let message;
      try {
        message = JSON.parse(event.data);
      } catch (_) {
        return;
      }
      void handleSignal(current, message).catch(() => {});
    };
    ws.onclose = () => {
      if (current.ws === ws) connectionLost(current, "The call ended.");
    };
    return true;
  }

  function leave() {
    const current = call;
    if (!current) return;
    call = null;
    if (current.reconnecting) clearTimeout(current.reconnecting);
    for (const timer of current.timers) clearInterval(timer);
    dropConnection(current);
    current.local.getTracks().forEach((track) => track.stop());
    current.worker.terminate();
    lastError = current.error;
    onChange();
  }

  function toggleMute() {
    if (!call) return;
    call.muted = !call.muted;
    call.local.getAudioTracks().forEach((track) => { track.enabled = !call.muted; });
    onChange();
  }

  function toggleCamera() {
    if (!call) return;
    call.cameraOff = !call.cameraOff;
    call.local.getVideoTracks().forEach((track) => { track.enabled = !call.cameraOff; });
    onChange();
  }

  return {
    handleFrame,
    join,
    leave,
    toggleMute,
    toggleCamera,
    roster,
    get active() { return call; },
    get lastError() { return lastError; },
    accountName,
  };
}
