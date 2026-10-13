import {
  CALL_FACES,
  CALL_FACE_STORAGE_KEY,
  callFaceById,
  callFacesSupported,
  createFaceStage,
} from "./chat-call-faces.js";
import {
  callVoiceChoices,
  callVoiceLabel,
  callVoicesSupported,
  createVoiceStage,
  normalizeVoiceChoice,
  rememberCallerPitch,
  rememberVoiceChoice,
  resolveCallVoice,
  savedCallerPitch,
  savedVoiceChoice,
} from "./chat-call-voice.js";

const RING_TIMEOUT_MS = 45000;
const INVITE_RESEND_MS = 3000;
const INVITE_STALE_MS = 9000;
const DISCONNECT_GRACE_MS = 8000;
const VOICE_READY_MS = 2500;
const AUDIO_CONSTRAINTS = { echoCancellation: true, noiseSuppression: true };
const VIDEO_CONSTRAINTS = {
  width: { ideal: 640 },
  height: { ideal: 360 },
  frameRate: { ideal: 24 },
};

export const CALL_FRAME_TYPES = new Set([
  "call-invite",
  "call-accept",
  "call-decline",
  "call-offer",
  "call-answer",
  "call-ice",
  "call-bye",
]);

export function callsSupported(globalRef = globalThis) {
  return Boolean(
    globalRef.isSecureContext &&
    globalRef.RTCPeerConnection &&
    globalRef.navigator?.mediaDevices?.getUserMedia,
  );
}

export function createChatCalls({
  iceServers = [],
  selfId = "",
  accountName = () => "",
  sendSignal = () => Promise.resolve(false),
  partnerFor = () => "",
  onMissed = () => {},
  onActiveChange = () => {},
  getUserMediaImpl = (constraints) =>
    globalThis.navigator.mediaDevices.getUserMedia(constraints),
  RTCPeerConnectionImpl = globalThis.RTCPeerConnection,
  documentRef = globalThis.document,
  storage = globalThis.localStorage,
  facesSupported = () => callFacesSupported(documentRef),
  createFaceStageImpl = createFaceStage,
  voicesSupported = () => callVoicesSupported(globalThis),
  createVoiceStageImpl = createVoiceStage,
  voiceModuleUrl = new URL("./chat-call-voice-worklet.js", import.meta.url).href,
  setTimeoutImpl = globalThis.setTimeout.bind(globalThis),
  clearTimeoutImpl = globalThis.clearTimeout.bind(globalThis),
  setIntervalImpl = globalThis.setInterval.bind(globalThis),
  clearIntervalImpl = globalThis.clearInterval.bind(globalThis),
} = {}) {
  const panel = documentRef.querySelector("#chat-call-panel");
  const peerLabel = documentRef.querySelector("#chat-call-peer");
  const stateLabel = documentRef.querySelector("#chat-call-state");
  const remoteVideo = documentRef.querySelector("#chat-call-remote");
  const localVideo = documentRef.querySelector("#chat-call-local");
  const muteBtn = documentRef.querySelector("#chat-call-mute");
  const cameraBtn = documentRef.querySelector("#chat-call-camera");
  const hangupBtn = documentRef.querySelector("#chat-call-hangup");
  const incomingEl = documentRef.querySelector("#chat-call-incoming");
  const incomingLabel = documentRef.querySelector("#chat-call-incoming-label");
  const acceptBtn = documentRef.querySelector("#chat-call-accept");
  const declineBtn = documentRef.querySelector("#chat-call-decline");
  const faceRow = documentRef.querySelector("#chat-call-face-row");
  const faceSelect = documentRef.querySelector("#chat-call-face");
  const voiceRow = documentRef.querySelector("#chat-call-voice-row");
  const voiceSelect = documentRef.querySelector("#chat-call-voice");
  const voiceState = documentRef.querySelector("#chat-call-voice-state");

  let call = null;
  let invite = null;

  function newCallId() {
    return (globalThis.crypto?.randomUUID && globalThis.crypto.randomUUID()) ||
      String(Math.random()).slice(2) + Date.now();
  }

  function ownAccountFrame(plain) {
    const own = String(accountName() || "").trim().toLowerCase();
    return Boolean(own) &&
      String(plain?.sender || "").trim().toLowerCase() === own;
  }

  function setPhase(phase, text) {
    if (!call) return;
    call.phase = phase;
    if (stateLabel) stateLabel.textContent = String(text || "");
  }

  function renderPanel() {
    if (!panel || !call) return;
    if (peerLabel) peerLabel.textContent = "@" + call.partner;
    if (remoteVideo) remoteVideo.hidden = call.media !== "video";
    if (cameraBtn) cameraBtn.hidden = call.media !== "video";
    if (faceRow) faceRow.hidden = call.media !== "video" || !facesSupported();
    if (faceSelect) faceSelect.value = call.stage?.face() || "";
    if (muteBtn) muteBtn.textContent = call.muted ? "Unmute" : "Mute";
    if (cameraBtn) {
      cameraBtn.textContent = call.cameraOff ? "Camera on" : "Camera off";
    }
    panel.classList.toggle("is-video", call.media === "video");
    renderVoice();
    panel.hidden = false;
  }

  function renderVoice() {
    if (!call) return;
    const supported = voicesSupported();
    if (voiceRow) voiceRow.hidden = !supported;
    if (voiceSelect) voiceSelect.value = call.voiceChoice;
    if (voiceState) {
      voiceState.textContent = "Voice: " + callVoiceLabel(call.voiceStage?.voice() || "");
    }
  }

  function voiceFace() {
    if (!call) return "";
    return call.media === "video" ? call.stage?.face() || "" : savedFace();
  }

  function audioSender() {
    return call?.pc?.getTransceivers?.().find(
      (transceiver) => transceiver.receiver?.track?.kind === "audio",
    )?.sender || null;
  }

  function sendAudio() {
    const track = outgoingTrack("audio");
    const sender = audioSender();
    if (sender && track && sender.track !== track) {
      Promise.resolve(sender.replaceTrack(track)).catch(() => {});
    }
  }

  function applyVoice() {
    if (!call) return Promise.resolve();
    const current = call;
    const id = resolveCallVoice(current.voiceChoice, voiceFace());
    if (current.voiceStage) {
      current.voiceStage.setVoice(id);
      renderVoice();
      return Promise.resolve();
    }
    if (current.voiceStarting) return current.voiceStarting;
    if (!id || !voicesSupported()) {
      renderVoice();
      return Promise.resolve();
    }
    const mic = current.localStream.getAudioTracks()[0];
    if (!mic) return Promise.resolve();
    current.voiceStarting = Promise.resolve()
      .then(() => createVoiceStageImpl({
        sourceTrack: mic,
        voiceId: id,
        userPitchHz: savedCallerPitch(storage),
        moduleUrl: voiceModuleUrl,
        onStats: (stats) => rememberCallerPitch(stats?.userPitchHz, storage),
      }))
      .then((stage) => {
        if (call !== current) {
          stage.stop();
          return;
        }
        current.voiceStage = stage;
        stage.setVoice(resolveCallVoice(current.voiceChoice, voiceFace()));
        sendAudio();
      })
      .catch(() => {})
      .finally(() => {
        if (call === current) {
          current.voiceStarting = null;
          renderVoice();
        }
      });
    return current.voiceStarting;
  }

  function voiceReady() {
    const pending = call?.voiceStarting;
    if (!pending) return Promise.resolve();
    return Promise.race([
      pending,
      new Promise((resolve) => setTimeoutImpl(resolve, VOICE_READY_MS)),
    ]);
  }

  function hideIncoming() {
    if (invite?.staleTimer) clearTimeoutImpl(invite.staleTimer);
    invite = null;
    if (incomingEl) incomingEl.hidden = true;
  }

  function signal(type, extra) {
    if (!call) return Promise.resolve(false);
    return Promise.resolve(
      sendSignal(call.channelKey, type, { call: call.callId, ...extra }),
    ).catch(() => false);
  }

  function clearCallTimers() {
    if (!call) return;
    if (call.inviteTimer) clearIntervalImpl(call.inviteTimer);
    if (call.ringTimer) clearTimeoutImpl(call.ringTimer);
    if (call.disconnectTimer) clearTimeoutImpl(call.disconnectTimer);
    call.inviteTimer = null;
    call.ringTimer = null;
    call.disconnectTimer = null;
  }

  function savedFace() {
    try {
      return callFaceById(storage?.getItem(CALL_FACE_STORAGE_KEY) || "")?.id || "";
    } catch (_) {
      return "";
    }
  }

  function rememberFace(id) {
    try {
      if (id) storage?.setItem(CALL_FACE_STORAGE_KEY, id);
      else storage?.removeItem(CALL_FACE_STORAGE_KEY);
    } catch (_) {}
  }

  function outgoingTrack(kind) {
    if (!call) return null;
    if (kind === "video" && call.stage?.track) return call.stage.track;
    if (kind === "audio" && call.voiceStage?.track) return call.voiceStage.track;
    return call.localStream.getTracks().find((value) => value.kind === kind) ||
      null;
  }

  function videoSender() {
    return call?.pc?.getTransceivers?.().find(
      (transceiver) => transceiver.receiver?.track?.kind === "video",
    )?.sender || null;
  }

  function useFace(id) {
    if (!call || call.media !== "video") return;
    const face = callFaceById(id);
    if (face && !facesSupported()) return;
    if (face && call.stage) {
      call.stage.setFace(face.id);
    } else if (face) {
      try {
        call.stage = createFaceStageImpl({
          cameraStream: call.localStream,
          faceId: face.id,
          documentRef,
        });
      } catch (_) {
        call.stage = null;
      }
    } else if (call.stage) {
      call.stage.stop();
      call.stage = null;
    }
    const track = outgoingTrack("video");
    if (track) track.enabled = !call.cameraOff;
    const sender = videoSender();
    if (sender && track && sender.track !== track) {
      Promise.resolve(sender.replaceTrack(track)).catch(() => {});
    }
    attachLocalPreview();
    renderPanel();
  }

  function teardown() {
    if (!call) return;
    clearCallTimers();
    try {
      call.stage?.stop();
    } catch (_) {}
    try {
      call.voiceStage?.stop();
    } catch (_) {}
    try {
      call.pc?.close();
    } catch (_) {}
    for (const track of call.localStream?.getTracks() || []) {
      try {
        track.stop();
      } catch (_) {}
    }
    call = null;
    if (localVideo) localVideo.srcObject = null;
    if (remoteVideo) remoteVideo.srcObject = null;
    if (panel) panel.hidden = true;
    onActiveChange();
  }

  function endCall({ notifyPeer = false, reason = "" } = {}) {
    if (!call) return;
    if (notifyPeer) void signal("call-bye", reason ? { reason } : {});
    teardown();
  }

  async function localMedia(media) {
    return getUserMediaImpl({
      audio: AUDIO_CONSTRAINTS,
      video: media === "video" ? VIDEO_CONSTRAINTS : false,
    });
  }

  function attachLocalPreview() {
    if (!call) return;
    if (localVideo) {
      const track = call.media === "video" ? outgoingTrack("video") : null;
      localVideo.srcObject = track ? new MediaStream([track]) : null;
      localVideo.hidden = call.media !== "video";
    }
  }

  function watchDisconnect(pc) {
    if (!call) return;
    if (call.disconnectTimer) clearTimeoutImpl(call.disconnectTimer);
    call.disconnectTimer = setTimeoutImpl(() => {
      if (call && call.pc === pc && pc.connectionState === "disconnected") {
        endCall({ notifyPeer: false });
      }
    }, DISCONNECT_GRACE_MS);
  }

  function createPeer() {
    const pc = new RTCPeerConnectionImpl({ iceServers });
    call.pc = pc;
    call.remoteStream = new MediaStream();
    pc.onicecandidate = (event) => {
      if (event.candidate) {
        void signal("call-ice", { to: call.peerId, ice: event.candidate });
      }
    };
    pc.ontrack = (event) => {
      if (!call || call.pc !== pc) return;
      if (!call.remoteStream.getTracks().includes(event.track)) {
        call.remoteStream.addTrack(event.track);
      }
      if (remoteVideo && remoteVideo.srcObject !== call.remoteStream) {
        remoteVideo.srcObject = call.remoteStream;
      }
    };
    pc.onconnectionstatechange = () => {
      if (!call || call.pc !== pc) return;
      if (pc.connectionState === "connected") {
        setPhase("live", "Connected · encrypted peer-to-peer");
      } else if (
        pc.connectionState === "failed" ||
        pc.connectionState === "closed"
      ) {
        endCall({ notifyPeer: false });
      } else if (pc.connectionState === "disconnected") {
        watchDisconnect(pc);
      }
    };
    return pc;
  }

  async function flushPendingIce() {
    if (!call?.pc?.remoteDescription) return;
    for (const candidate of call.pendingIce.splice(0)) {
      try {
        await call.pc.addIceCandidate(candidate);
      } catch (_) {}
    }
  }

  async function start(channelKey, partner, media) {
    if (call || !channelKey || !partner) return false;
    const kind = media === "video" ? "video" : "audio";
    const localStream = await localMedia(kind);
    if (call) {
      for (const track of localStream.getTracks()) track.stop();
      return false;
    }
    call = {
      channelKey,
      callId: newCallId(),
      partner: String(partner),
      media: kind,
      role: "caller",
      phase: "ringing-out",
      peerId: "",
      pc: null,
      localStream,
      remoteStream: null,
      pendingIce: [],
      muted: false,
      cameraOff: false,
      stage: null,
      voiceChoice: savedVoiceChoice(storage),
      voiceStage: null,
      voiceStarting: null,
      inviteTimer: null,
      ringTimer: null,
      disconnectTimer: null,
    };
    if (kind === "video") useFace(savedFace());
    void applyVoice();
    attachLocalPreview();
    renderPanel();
    setPhase("ringing-out", `Calling @${call.partner}…`);
    const ring = () => void signal("call-invite", { media: kind });
    ring();
    call.inviteTimer = setIntervalImpl(ring, INVITE_RESEND_MS);
    call.ringTimer = setTimeoutImpl(() => {
      const missed = call;
      endCall({ notifyPeer: true, reason: "missed" });
      if (missed) onMissed(missed.channelKey, missed.media);
    }, RING_TIMEOUT_MS);
    onActiveChange();
    return true;
  }

  async function acceptInvite() {
    const pending = invite;
    if (!pending || call) return;
    let localStream;
    try {
      localStream = await localMedia(pending.media);
    } catch (error) {
      hideIncoming();
      void sendSignal(pending.channelKey, "call-decline", {
        call: pending.callId,
        to: pending.from,
        reason: "media",
      });
      throw error;
    }
    if (call || invite !== pending) {
      for (const track of localStream.getTracks()) track.stop();
      return;
    }
    hideIncoming();
    call = {
      channelKey: pending.channelKey,
      callId: pending.callId,
      partner: pending.partner,
      media: pending.media,
      role: "callee",
      phase: "connecting",
      peerId: pending.from,
      pc: null,
      localStream,
      remoteStream: null,
      pendingIce: [],
      muted: false,
      cameraOff: false,
      stage: null,
      voiceChoice: savedVoiceChoice(storage),
      voiceStage: null,
      voiceStarting: null,
      inviteTimer: null,
      ringTimer: null,
      disconnectTimer: null,
    };
    if (call.media === "video") useFace(savedFace());
    void applyVoice();
    attachLocalPreview();
    renderPanel();
    setPhase("connecting", "Connecting…");
    const accepted = call;
    await voiceReady();
    if (call !== accepted) return;
    void signal("call-accept", { to: pending.from });
    onActiveChange();
  }

  function declineInvite() {
    const pending = invite;
    hideIncoming();
    if (!pending) return;
    void sendSignal(pending.channelKey, "call-decline", {
      call: pending.callId,
      to: pending.from,
      reason: "declined",
    });
  }

  function showIncoming(channelKey, plain) {
    const media = plain.media === "video" ? "video" : "audio";
    const partner = String(partnerFor(channelKey) || plain.sender || "peer");
    if (invite?.staleTimer) clearTimeoutImpl(invite.staleTimer);
    invite = {
      channelKey,
      callId: String(plain.call),
      from: String(plain.senderId),
      partner,
      media,
      staleTimer: null,
    };
    invite.staleTimer = setTimeoutImpl(hideIncoming, INVITE_STALE_MS);
    if (incomingLabel) {
      incomingLabel.textContent =
        `@${partner} is starting an encrypted ${media} call`;
    }
    if (incomingEl) incomingEl.hidden = false;
  }

  async function handleCallerAccept(plain) {
    if (call.peerId) return;
    clearCallTimers();
    call.peerId = String(plain.senderId);
    setPhase("connecting", "Connecting…");
    const accepted = call;
    await voiceReady();
    if (call !== accepted) return;
    const pc = createPeer();
    for (const kind of ["audio", "video"]) {
      const track = outgoingTrack(kind);
      pc.addTransceiver(track || kind, { direction: "sendrecv" });
    }
    try {
      await pc.setLocalDescription();
      await signal("call-offer", { to: call.peerId, sdp: pc.localDescription });
    } catch (_) {
      endCall({ notifyPeer: true });
    }
  }

  async function handleCalleeOffer(plain) {
    try {
      const pc = call.pc || createPeer();
      await pc.setRemoteDescription(plain.sdp);
      for (const transceiver of pc.getTransceivers()) {
        transceiver.direction = "sendrecv";
        const track = outgoingTrack(transceiver.receiver.track.kind);
        if (track) await transceiver.sender.replaceTrack(track);
      }
      await pc.setLocalDescription();
      await signal("call-answer", { to: call.peerId, sdp: pc.localDescription });
      await flushPendingIce();
    } catch (_) {
      endCall({ notifyPeer: true });
    }
  }

  async function handleFrame(plain, scope) {
    const type = String(plain?.type || "");
    if (!CALL_FRAME_TYPES.has(type)) return;
    const callId = String(plain.call || "");
    const from = String(plain.senderId || "");
    if (!callId || !from || from === selfId) return;
    const fromOwnAccount = ownAccountFrame(plain);
    if (plain.to && plain.to !== selfId) {
      if (fromOwnAccount && invite?.callId === callId &&
          (type === "call-accept" || type === "call-decline")) {
        hideIncoming();
      }
      return;
    }
    if (type === "call-invite") {
      if (fromOwnAccount || call?.callId === callId) return;
      if (call) {
        void sendSignal(scope, "call-decline", {
          call: callId,
          to: from,
          reason: "busy",
        });
        return;
      }
      showIncoming(scope, plain);
      return;
    }
    if (type === "call-bye") {
      if (invite?.callId === callId) hideIncoming();
      if (call?.callId === callId) endCall({ notifyPeer: false });
      return;
    }
    if (!call || call.callId !== callId) return;
    if (type === "call-accept" && call.role === "caller") {
      await handleCallerAccept(plain);
    } else if (type === "call-decline" && call.role === "caller") {
      endCall({ notifyPeer: false });
    } else if (type === "call-offer" && call.role === "callee" &&
        from === call.peerId) {
      await handleCalleeOffer(plain);
    } else if (type === "call-answer" && call.role === "caller" &&
        from === call.peerId && call.pc) {
      try {
        await call.pc.setRemoteDescription(plain.sdp);
        await flushPendingIce();
      } catch (_) {
        endCall({ notifyPeer: true });
      }
    } else if (type === "call-ice" && from === call.peerId && plain.ice) {
      if (call.pc?.remoteDescription) {
        try {
          await call.pc.addIceCandidate(plain.ice);
        } catch (_) {}
      } else {
        call.pendingIce.push(plain.ice);
      }
    }
  }

  function toggleMute() {
    if (!call) return;
    call.muted = !call.muted;
    for (const track of call.localStream.getAudioTracks()) {
      track.enabled = !call.muted;
    }
    renderPanel();
  }

  function toggleCamera() {
    if (!call || call.media !== "video") return;
    call.cameraOff = !call.cameraOff;
    for (const track of call.localStream.getVideoTracks()) {
      track.enabled = !call.cameraOff;
    }
    if (call.stage?.track) call.stage.track.enabled = !call.cameraOff;
    renderPanel();
  }

  muteBtn?.addEventListener("click", toggleMute);
  cameraBtn?.addEventListener("click", toggleCamera);
  hangupBtn?.addEventListener("click", () => endCall({ notifyPeer: true }));
  acceptBtn?.addEventListener("click", () => {
    acceptInvite().catch(() => {});
  });
  declineBtn?.addEventListener("click", declineInvite);
  if (faceSelect && !faceSelect.options.length) {
    for (const [value, label] of [["", "Your own face"],
      ...CALL_FACES.map((face) => [face.id, face.label])]) {
      const option = documentRef.createElement("option");
      option.value = value;
      option.textContent = label;
      faceSelect.append(option);
    }
  }
  faceSelect?.addEventListener("change", () => {
    rememberFace(callFaceById(faceSelect.value)?.id || "");
    useFace(faceSelect.value);
    void applyVoice();
  });
  if (voiceSelect && !voiceSelect.options.length) {
    for (const [value, label] of callVoiceChoices()) {
      const option = documentRef.createElement("option");
      option.value = value;
      option.textContent = label;
      voiceSelect.append(option);
    }
  }
  function chooseVoice(choice) {
    const value = normalizeVoiceChoice(choice);
    rememberVoiceChoice(value, storage);
    if (!call) return;
    call.voiceChoice = value;
    void applyVoice();
    renderVoice();
  }
  voiceSelect?.addEventListener("change", () => chooseVoice(voiceSelect.value));

  return {
    start,
    handleFrame,
    hangUp: () => endCall({ notifyPeer: true }),
    active: () => Boolean(call),
    activeChannelKey: () => call?.channelKey || "",
    setFace: (id) => {
      rememberFace(callFaceById(id)?.id || "");
      useFace(id);
      void applyVoice();
    },
    face: () => call?.stage?.face() || "",
    setVoice: chooseVoice,
    voiceChoice: () => call?.voiceChoice || savedVoiceChoice(storage),
    voice: () => call?.voiceStage?.voice() || "",
    voiceReady: () => call?.voiceStarting || Promise.resolve(),
    monitorVoice: (on) => Boolean(call?.voiceStage?.monitor(on)),
  };
}
