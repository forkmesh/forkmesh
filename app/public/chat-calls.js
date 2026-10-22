import {
  CALL_FACES,
  CALL_FACE_STORAGE_KEY,
  callBackgroundsSupported,
  callFaceById,
  callFacesSupported,
  createFaceStage,
} from "./chat-call-faces.js";
import {
  CUSTOM_FACE_ID,
  customFaceFromFile,
  rememberCustomFaceImage,
  savedCustomFaceImage,
} from "./chat-call-face-custom.js";
import {
  CALL_BACKGROUNDS,
  callBackgroundById,
  callBackgroundThumbUrl,
  customBackgroundFromFile,
  rememberCallBackground,
  savedCallBackground,
} from "./chat-call-backgrounds.js";
import {
  createVoicePreview,
  mountVoicePreviewControls,
} from "./chat-call-voice.js";
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
import {
  CALL_ROUTE_CHECK_MS,
  CALL_TEST_CLIP_MS,
  callMediaConstraints,
  callRouteLabel,
  readCallRoute,
  recordTestClip,
  tuneCallSdp,
  tuneCallSenders,
} from "./chat-call-quality.js";
import { createCallTones } from "./chat-call-tones.js";

const RING_TIMEOUT_MS = 45000;
const INVITE_RESEND_MS = 3000;
const INVITE_STALE_MS = 9000;
const DISCONNECT_GRACE_MS = 8000;
const VOICE_READY_MS = 2500;
const VIBRATE_PATTERN = [600, 300, 600, 1500];
const VIBRATE_EVERY_MS = 3000;
const TITLE_FLASH_MS = 1000;
const LOCAL_CORNER_KEY = "fm.call.previewCorner";
const LOCAL_CORNERS = new Set(["top-left", "top-right", "bottom-left", "bottom-right"]);

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

export function askCallNotifications(NotificationImpl = globalThis.Notification) {
  if (NotificationImpl?.permission !== "default" || typeof NotificationImpl.requestPermission !== "function") return;
  try {
    Promise.resolve(NotificationImpl.requestPermission()).catch(() => {});
  } catch (_) {}
}

export function createChatCalls({
  iceServers = [],
  selfId = "",
  accountName = () => "",
  sendSignal = () => Promise.resolve(false),
  partnerFor = () => "",
  onMissed = () => {},
  onActiveChange = () => {},
  requestStart = null,
  getUserMediaImpl = (constraints) =>
    globalThis.navigator.mediaDevices.getUserMedia(constraints),
  RTCPeerConnectionImpl = globalThis.RTCPeerConnection,
  documentRef = globalThis.document,
  storage = globalThis.localStorage,
  facesSupported = () => callFacesSupported(documentRef),
  backgroundsSupported = () => callBackgroundsSupported(documentRef),
  createFaceStageImpl = createFaceStage,
  createVoicePreviewImpl = createVoicePreview,
  voicesSupported = () => callVoicesSupported(globalThis),
  createVoiceStageImpl = createVoiceStage,
  voiceModuleUrl = new URL("./chat-call-voice-worklet.js", import.meta.url).href,
  setTimeoutImpl = globalThis.setTimeout.bind(globalThis),
  clearTimeoutImpl = globalThis.clearTimeout.bind(globalThis),
  setIntervalImpl = globalThis.setInterval.bind(globalThis),
  clearIntervalImpl = globalThis.clearInterval.bind(globalThis),
  tones = createCallTones({ setIntervalImpl, clearIntervalImpl }),
  NotificationImpl = globalThis.Notification,
  navigatorRef = globalThis.navigator,
  recordTestClipImpl = recordTestClip,
  readRouteImpl = readCallRoute,
  URLImpl = globalThis.URL,
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
  const faceFile = documentRef.querySelector("#chat-call-face-file");
  const voiceRow = documentRef.querySelector("#chat-call-voice-row");
  const voiceSelect = documentRef.querySelector("#chat-call-voice");
  const voiceState = documentRef.querySelector("#chat-call-voice-state");
  const backgroundRow = documentRef.querySelector("#chat-call-background-row");
  const backgroundOptions = documentRef.querySelector("#chat-call-background-options");
  const backgroundFile = documentRef.querySelector("#chat-call-background-file");
  const previewSlot = documentRef.querySelector("#chat-call-preview-slot");
  const startBtn = documentRef.querySelector("#chat-call-start");
  const routeLabel = documentRef.querySelector("#chat-call-route");
  const testRow = documentRef.querySelector("#chat-call-test-row");
  const testBtn = documentRef.querySelector("#chat-call-test");
  const testPlayer = documentRef.querySelector("#chat-call-test-player");
  const incomingKind = documentRef.querySelector("#chat-call-incoming-kind");

  let call = null;
  let invite = null;
  let ringAlert = null;

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
    syncTones();
  }

  function syncTones() {
    if (invite && ringAlert?.callId === invite.callId) tones.ring();
    else if (call?.phase === "ringing-out") tones.ringback();
    else tones.stop();
  }

  function renderRoute() {
    if (!routeLabel) return;
    const text = call?.phase === "live" ? callRouteLabel(call.route) : "";
    routeLabel.textContent = text;
    routeLabel.hidden = !text;
    if (call?.route) routeLabel.setAttribute("data-route", call.route.route);
  }

  function watchRoute(pc) {
    if (!call || call.routeTimer) return;
    const current = call;
    const check = () => {
      if (call !== current || current.pc !== pc) return;
      Promise.resolve()
        .then(() => readRouteImpl(pc))
        .then((found) => {
          if (!found || call !== current || current.pc !== pc) return;
          current.route = found;
          renderRoute();
        })
        .catch(() => {});
    };
    current.routeTimer = setIntervalImpl(check, CALL_ROUTE_CHECK_MS);
    check();
  }

  function renderPanel() {
    if (!panel || !call) return;
    if (peerLabel) peerLabel.textContent = "@" + call.partner;
    if (remoteVideo) remoteVideo.hidden = call.media !== "video";
    if (cameraBtn) cameraBtn.hidden = call.media !== "video";
    if (faceRow) faceRow.hidden = call.media !== "video" || !facesSupported();
    if (faceSelect) faceSelect.value = call.stage?.face() || "";
    renderBackgrounds();
    const previewing = call.phase === "preview";
    panel.classList.toggle("is-preview", previewing);
    if (startBtn) {
      startBtn.hidden = !previewing;
      startBtn.textContent = call.media === "video" ? "Start video call" : "Start voice call";
    }
    if (testRow) testRow.hidden = !previewing;
    renderTest();
    renderRoute();
    if (hangupBtn) hangupBtn.textContent = previewing ? "Close" : "Hang up";
    if (previewSlot) previewSlot.hidden = !previewing;
    if (muteBtn) muteBtn.textContent = call.muted ? "Unmute" : "Mute";
    if (cameraBtn) {
      cameraBtn.textContent = call.cameraOff ? "Camera on" : "Camera off";
    }
    panel.classList.toggle("is-video", call.media === "video");
    renderVoice();
    if (voiceRow && call.phase === "preview") voiceRow.hidden = true;
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
    return call.media === "video" ? call.faceId || call.stage?.face() || "" : savedFace();
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
    if (current.phase === "preview") {
      current.voicePreview?.setFace(current.faceId);
      return Promise.resolve();
    }
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
    stopRingAlert();
    syncTones();
  }

  function startRingAlert(pending) {
    if (ringAlert?.callId === pending.callId) return;
    stopRingAlert();
    const title = `Incoming ${pending.media === "video" ? "video" : "voice"} call`;
    const body = `@${pending.partner} is calling you`;
    ringAlert = { callId: pending.callId, notification: null, vibrateTimer: null, titleTimer: null, title: documentRef.title };
    const vibrate = () => {
      try {
        navigatorRef?.vibrate?.(VIBRATE_PATTERN);
      } catch (_) {}
    };
    vibrate();
    ringAlert.vibrateTimer = setIntervalImpl(vibrate, VIBRATE_EVERY_MS);
    if (typeof ringAlert.title === "string") {
      let flip = false;
      ringAlert.titleTimer = setIntervalImpl(() => {
        flip = !flip;
        documentRef.title = flip ? `📞 ${body}` : ringAlert?.title || "";
      }, TITLE_FLASH_MS);
    }
    const away = Boolean(documentRef.hidden) || (typeof documentRef.hasFocus === "function" && !documentRef.hasFocus());
    if (away && NotificationImpl?.permission === "granted") {
      try {
        const notification = new NotificationImpl(title, {
          body,
          tag: `forkmesh-call-${pending.callId}`,
          requireInteraction: true,
          renotify: true,
          silent: false,
        });
        notification.onclick = () => {
          try {
            globalThis.focus?.();
          } catch (_) {}
          notification.close?.();
        };
        ringAlert.notification = notification;
      } catch (_) {}
    }
  }

  function stopRingAlert() {
    const current = ringAlert;
    ringAlert = null;
    if (!current) return;
    if (current.vibrateTimer) clearIntervalImpl(current.vibrateTimer);
    if (current.titleTimer) clearIntervalImpl(current.titleTimer);
    if (typeof current.title === "string") documentRef.title = current.title;
    try {
      navigatorRef?.vibrate?.(0);
    } catch (_) {}
    try {
      current.notification?.close?.();
    } catch (_) {}
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
    if (call.routeTimer) clearIntervalImpl(call.routeTimer);
    call.inviteTimer = null;
    call.ringTimer = null;
    call.disconnectTimer = null;
    call.routeTimer = null;
  }

  function knownFace(id) {
    if (id === CUSTOM_FACE_ID) return savedCustomFaceImage(storage) ? CUSTOM_FACE_ID : "";
    return callFaceById(id)?.id || "";
  }

  function savedFace() {
    try {
      return knownFace(storage?.getItem(CALL_FACE_STORAGE_KEY) || "");
    } catch (_) {
      return "";
    }
  }

  function watchCustomFace() {
    const started = Date.now();
    const check = () => {
      if (!call || call.faceId !== CUSTOM_FACE_ID || !call.stage?.faceState) return;
      const state = call.stage.faceState();
      if (state === "failed") {
        if (stateLabel) stateLabel.textContent = "No face found in that photo. Try a clear, front-facing photo.";
        rememberFace("");
        useFace("");
        renderPanel();
        return;
      }
      if (state === "loading" && Date.now() - started < 30000) setTimeoutImpl(check, 250);
    };
    setTimeoutImpl(check, 250);
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
    const face = knownFace(id);
    if (face && !facesSupported()) return;
    call.faceId = face;
    call.faceImage = face === CUSTOM_FACE_ID ? savedCustomFaceImage(storage) : "";
    refreshStage();
    if (face === CUSTOM_FACE_ID) watchCustomFace();
  }

  function useBackground(id, image = "") {
    if (!call || call.media !== "video") return;
    const background = callBackgroundById(id);
    if (background?.id && !backgroundsSupported()) return;
    call.backgroundId = background?.id || "";
    call.backgroundImage = background?.custom ? image : "";
    refreshStage();
  }

  function refreshStage() {
    if (!call || call.media !== "video") return;
    const wanted = Boolean(call.faceId || call.backgroundId);
    if (wanted && call.stage) {
      if (call.stage.face() !== call.faceId || (call.faceId === CUSTOM_FACE_ID && call.stageFaceImage !== call.faceImage)) {
        call.stage.setFace(call.faceId, call.faceImage || "");
        call.stageFaceImage = call.faceImage || "";
      }
      if ((call.stage.background?.() || "") !== call.backgroundId || call.backgroundId === "custom") {
        call.stage.setBackground?.(call.backgroundId, call.backgroundImage);
      }
    } else if (wanted) {
      try {
        call.stage = createFaceStageImpl({
          cameraStream: call.localStream,
          faceId: call.faceId,
          faceImage: call.faceImage || "",
          backgroundId: call.backgroundId,
          backgroundImage: call.backgroundImage,
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

  function renderBackgrounds() {
    if (!backgroundRow || !call) return;
    backgroundRow.hidden = call.media !== "video" || !backgroundsSupported();
    for (const button of backgroundOptions?.querySelectorAll?.("[data-background]") || []) {
      const on = button.getAttribute("data-background") === (call.backgroundId || "");
      button.setAttribute("aria-checked", on ? "true" : "false");
      button.classList.toggle("is-selected", on);
    }
  }

  function chooseBackground(id, image = "") {
    const background = callBackgroundById(id);
    if (!background) return;
    if (background.custom && !image) {
      const saved = savedCallBackground(storage);
      if (saved.id === background.id) image = saved.image;
      else {
        backgroundFile?.click?.();
        return;
      }
    }
    rememberCallBackground(background.id, image, storage);
    useBackground(background.id, image);
  }

  function buildBackgroundOptions() {
    if (!backgroundOptions || backgroundOptions.querySelector?.("[data-background]")) return;
    for (const background of CALL_BACKGROUNDS) {
      const button = documentRef.createElement("button");
      button.type = "button";
      button.className = "chat-call-background-option";
      button.setAttribute("role", "radio");
      button.setAttribute("data-background", background.id);
      button.setAttribute("aria-checked", "false");
      button.title = background.label;
      const swatch = documentRef.createElement("span");
      swatch.className = `chat-call-background-swatch is-${background.mode}${background.custom ? " is-custom" : ""}`;
      const thumb = callBackgroundThumbUrl(background);
      if (thumb) swatch.style.backgroundImage = `url("${thumb}")`;
      if (background.custom) {
        const saved = savedCallBackground(storage);
        if (saved.image) swatch.style.backgroundImage = `url("${saved.image}")`;
      }
      const name = documentRef.createElement("span");
      name.className = "chat-call-background-name";
      name.textContent = background.label;
      button.append(swatch, name);
      button.addEventListener("click", () => chooseBackground(background.id));
      backgroundOptions.append(button);
    }
  }

  function teardown() {
    if (!call) return;
    clearCallTimers();
    clearTest();
    stopVoicePreview();
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
    syncTones();
    renderRoute();
    if (localVideo) localVideo.srcObject = null;
    if (remoteVideo) remoteVideo.srcObject = null;
    if (panel) panel.hidden = true;
    onActiveChange();
  }

  function endCall({ notifyPeer = false, reason = "" } = {}) {
    if (!call) return;
    if (notifyPeer && call.phase !== "preview") void signal("call-bye", reason ? { reason } : {});
    teardown();
  }

  function hangUp() {
    if (call?.role === "caller" && call.phase === "ringing-out") {
      const missed = call;
      endCall({ notifyPeer: true, reason: "missed" });
      onMissed(missed.channelKey, missed.media);
      return;
    }
    endCall({ notifyPeer: true });
  }

  async function localMedia(media) {
    return getUserMediaImpl(callMediaConstraints(media));
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
        setPhase("live", "Connected · end-to-end encrypted");
        renderRoute();
        watchRoute(pc);
        void tuneCallSenders(pc);
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

  function useSaved() {
    const saved = savedCallBackground(storage);
    call.faceId = facesSupported() ? savedFace() : "";
    call.faceImage = call.faceId === CUSTOM_FACE_ID ? savedCustomFaceImage(storage) : "";
    call.backgroundId = backgroundsSupported() ? saved.id : "";
    call.backgroundImage = saved.image;
    refreshStage();
  }

  function takePreview(media) {
    if (!call || call.phase !== "preview") return null;
    const kept = call;
    stopVoicePreview({ keepStream: true });
    if (kept.media !== (media === "video" ? "video" : "audio")) {
      teardown();
      return null;
    }
    clearCallTimers();
    clearTest();
    call = null;
    return kept;
  }

  function testTracks() {
    if (!call) return [];
    const audio = call.voicePreview?.track?.() || call.localStream.getAudioTracks()[0] || null;
    return [call.media === "video" ? outgoingTrack("video") : null, audio].filter(Boolean);
  }

  function clearTest() {
    const current = call;
    if (!current) return;
    const recording = current.testRecording;
    current.testRecording = null;
    if (recording) Promise.resolve(recording.stop()).catch(() => {});
    if (current.testUrl) {
      try {
        URLImpl?.revokeObjectURL?.(current.testUrl);
      } catch (_) {}
    }
    current.testUrl = "";
    current.testLeftMs = 0;
    if (testPlayer) {
      try {
        testPlayer.pause?.();
      } catch (_) {}
      testPlayer.removeAttribute?.("src");
      testPlayer.hidden = true;
    }
  }

  function renderTest() {
    if (!testBtn || !call) return;
    if (call.testRecording) {
      testBtn.textContent = `Stop test (${Math.ceil(call.testLeftMs / 1000)}s left)`;
      testBtn.setAttribute("aria-pressed", "true");
    } else {
      testBtn.textContent = call.testUrl ? "Record again" : "Record a 10-second test clip";
      testBtn.setAttribute("aria-pressed", "false");
    }
  }

  async function recordTest() {
    if (!call || call.phase !== "preview") return false;
    const current = call;
    if (current.testRecording) {
      await current.testRecording.stop();
      return true;
    }
    clearTest();
    let recording;
    try {
      recording = recordTestClipImpl(testTracks(), {
        maxMs: CALL_TEST_CLIP_MS,
        onTick: (ms, max) => {
          if (call !== current) return;
          current.testLeftMs = Math.max(0, max - ms);
          renderTest();
        },
      });
    } catch (_) {
      if (stateLabel) stateLabel.textContent = "Recording a test clip is not available in this browser.";
      return false;
    }
    current.testRecording = recording;
    current.testLeftMs = CALL_TEST_CLIP_MS;
    if (stateLabel) stateLabel.textContent = "Recording a test clip · speak and look at the camera";
    renderTest();
    try {
      const clip = await recording.done;
      if (call !== current || current.testRecording !== recording) return false;
      current.testRecording = null;
      if (!clip?.blob?.size) throw new Error("empty_clip");
      current.testUrl = URLImpl.createObjectURL(clip.blob);
      if (testPlayer) {
        testPlayer.src = current.testUrl;
        testPlayer.classList.toggle("is-audio", !clip.video);
        testPlayer.hidden = false;
        Promise.resolve(testPlayer.play?.()).catch(() => {});
      }
      if (stateLabel) stateLabel.textContent = "Playing your test clip · this is how you will look and sound";
      return true;
    } catch (_) {
      if (call === current) {
        current.testRecording = null;
        if (stateLabel) stateLabel.textContent = "The test clip could not be recorded. Try again.";
      }
      return false;
    } finally {
      if (call === current) renderTest();
    }
  }

  function stopVoicePreview(options) {
    try {
      call?.voicePreview?.stop(options);
    } catch (_) {}
    try {
      call?.voicePreviewUnmount?.();
    } catch (_) {}
    if (call) {
      call.voicePreview = null;
      call.voicePreviewUnmount = null;
    }
  }

  async function preview(channelKey, partner, media = "video") {
    if (call || !channelKey || !partner) return false;
    askCallNotifications(NotificationImpl);
    const kind = media === "video" ? "video" : "audio";
    const localStream = await localMedia(kind);
    if (call) {
      for (const track of localStream.getTracks()) track.stop();
      return false;
    }
    call = {
      channelKey,
      callId: "",
      partner: String(partner),
      media: kind,
      role: "preview",
      phase: "preview",
      peerId: "",
      pc: null,
      localStream,
      remoteStream: null,
      pendingIce: [],
      muted: false,
      cameraOff: false,
      stage: null,
      faceId: "",
      backgroundId: "",
      backgroundImage: "",
      voiceChoice: savedVoiceChoice(storage),
      voiceStage: null,
      voiceStarting: null,
      voicePreview: null,
      voicePreviewUnmount: null,
      inviteTimer: null,
      ringTimer: null,
      disconnectTimer: null,
      routeTimer: null,
      route: null,
      testRecording: null,
      testUrl: "",
      testLeftMs: 0,
    };
    if (kind === "video") useSaved();
    setPhase("preview", kind === "video"
      ? "Check your camera and voice · only you can see this"
      : "Check your microphone · only you can hear this");
    renderPanel();
    const previewing = call;
    Promise.resolve()
      .then(() => createVoicePreviewImpl({ micStream: localStream, faceId: call.faceId, storage }))
      .then((voice) => {
        if (call !== previewing) {
          voice?.stop({ keepStream: true });
          return;
        }
        call.voicePreview = voice;
        call.voicePreviewUnmount = mountVoicePreviewControls(previewSlot, voice, { documentRef });
      })
      .catch(() => {});
    onActiveChange();
    return true;
  }

  async function start(channelKey, partner, media) {
    const previewed = call?.phase === "preview" && call.channelKey === channelKey ? takePreview(media) : null;
    if (call || !channelKey || !partner) return false;
    const kind = media === "video" ? "video" : "audio";
    const localStream = previewed?.localStream || await localMedia(kind);
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
      stage: previewed?.stage || null,
      faceId: "",
      backgroundId: "",
      backgroundImage: "",
      voiceChoice: savedVoiceChoice(storage),
      voiceStage: null,
      voiceStarting: null,
      inviteTimer: null,
      ringTimer: null,
      disconnectTimer: null,
      routeTimer: null,
      route: null,
    };
    if (kind === "video" && previewed) {
      call.faceId = previewed.faceId;
      call.faceImage = previewed.faceImage || "";
      call.stageFaceImage = previewed.stageFaceImage || "";
      call.backgroundId = previewed.backgroundId;
      call.backgroundImage = previewed.backgroundImage;
      refreshStage();
    } else if (kind === "video") {
      useSaved();
    }
    void applyVoice();
    attachLocalPreview();
    renderPanel();
    setPhase("ringing-out", `Calling @${call.partner}…`);
    const ring = () => void signal("call-invite", { media: kind });
    ring();
    call.inviteTimer = setIntervalImpl(ring, INVITE_RESEND_MS);
    call.ringTimer = setTimeoutImpl(hangUp, RING_TIMEOUT_MS);
    onActiveChange();
    return true;
  }

  async function acceptInvite() {
    const pending = invite;
    const previewed = call?.phase === "preview" ? takePreview(pending?.media) : null;
    if (!pending || call) return;
    let localStream = previewed?.localStream || null;
    try {
      localStream = localStream || await localMedia(pending.media);
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
      stage: previewed?.stage || null,
      faceId: "",
      backgroundId: "",
      backgroundImage: "",
      voiceChoice: savedVoiceChoice(storage),
      voiceStage: null,
      voiceStarting: null,
      inviteTimer: null,
      ringTimer: null,
      disconnectTimer: null,
      routeTimer: null,
      route: null,
    };
    if (call.media === "video" && previewed) {
      call.faceId = previewed.faceId;
      call.faceImage = previewed.faceImage || "";
      call.stageFaceImage = previewed.stageFaceImage || "";
      call.backgroundId = previewed.backgroundId;
      call.backgroundImage = previewed.backgroundImage;
      refreshStage();
    } else if (call.media === "video") {
      useSaved();
    }
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
    if (incomingLabel) incomingLabel.textContent = `@${partner}`;
    if (incomingKind) {
      incomingKind.textContent = media === "video" ? "Incoming video call" : "Incoming voice call";
    }
    if (incomingEl) incomingEl.hidden = false;
    startRingAlert(invite);
    syncTones();
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
      await tuneCallSenders(pc);
      await signal("call-offer", { to: call.peerId, sdp: tuneCallSdp(pc.localDescription) });
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
      await tuneCallSenders(pc);
      await signal("call-answer", { to: call.peerId, sdp: tuneCallSdp(pc.localDescription) });
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

  function savedLocalCorner() {
    try {
      const corner = storage?.getItem(LOCAL_CORNER_KEY) || "";
      return LOCAL_CORNERS.has(corner) ? corner : "bottom-right";
    } catch (_) {
      return "bottom-right";
    }
  }

  function placeLocal(corner) {
    if (localVideo?.dataset) localVideo.dataset.corner = corner;
  }

  function watchLocalDrag() {
    if (!localVideo?.addEventListener) return;
    placeLocal(savedLocalCorner());
    let drag = null;
    localVideo.addEventListener("pointerdown", (event) => {
      if (!call || call.phase === "preview" || localVideo.hidden) return;
      const box = localVideo.parentElement?.getBoundingClientRect?.();
      const own = localVideo.getBoundingClientRect?.();
      if (!box || !own) return;
      drag = {
        id: event.pointerId,
        box,
        dx: event.clientX - own.left,
        dy: event.clientY - own.top,
        width: own.width,
        height: own.height,
      };
      localVideo.setPointerCapture?.(event.pointerId);
      localVideo.classList?.add("is-dragging");
      event.preventDefault?.();
    });
    localVideo.addEventListener("pointermove", (event) => {
      if (!drag || event.pointerId !== drag.id) return;
      const left = Math.min(Math.max(event.clientX - drag.box.left - drag.dx, 0), drag.box.width - drag.width);
      const top = Math.min(Math.max(event.clientY - drag.box.top - drag.dy, 0), drag.box.height - drag.height);
      Object.assign(localVideo.style, { left: `${left}px`, top: `${top}px`, right: "auto", bottom: "auto" });
    });
    const drop = (event) => {
      if (!drag || event.pointerId !== drag.id) return;
      const own = localVideo.getBoundingClientRect();
      const x = own.left + own.width / 2 - drag.box.left;
      const y = own.top + own.height / 2 - drag.box.top;
      const corner = `${y < drag.box.height / 2 ? "top" : "bottom"}-${x < drag.box.width / 2 ? "left" : "right"}`;
      drag = null;
      localVideo.classList?.remove("is-dragging");
      Object.assign(localVideo.style, { left: "", top: "", right: "", bottom: "" });
      placeLocal(corner);
      try {
        storage?.setItem(LOCAL_CORNER_KEY, corner);
      } catch (_) {}
    };
    localVideo.addEventListener("pointerup", drop);
    localVideo.addEventListener("pointercancel", drop);
  }

  watchLocalDrag();
  muteBtn?.addEventListener("click", toggleMute);
  cameraBtn?.addEventListener("click", toggleCamera);
  hangupBtn?.addEventListener("click", hangUp);
  acceptBtn?.addEventListener("click", () => {
    acceptInvite().catch(() => {});
  });
  declineBtn?.addEventListener("click", declineInvite);
  if (faceSelect && !faceSelect.options.length) {
    for (const [value, label] of [["", "Your own face"],
      ...CALL_FACES.map((face) => [face.id, face.label]), [CUSTOM_FACE_ID, "Your photo…"]]) {
      const option = documentRef.createElement("option");
      option.value = value;
      option.textContent = label;
      faceSelect.append(option);
    }
  }
  faceSelect?.addEventListener("change", () => {
    if (faceSelect.value === CUSTOM_FACE_ID) {
      faceFile?.click?.();
      if (!savedCustomFaceImage(storage)) {
        faceSelect.value = call?.faceId || "";
        return;
      }
    }
    rememberFace(knownFace(faceSelect.value));
    useFace(faceSelect.value);
    void applyVoice();
  });
  faceFile?.addEventListener("change", () => {
    const file = faceFile.files?.[0];
    faceFile.value = "";
    if (!file) return;
    customFaceFromFile(file, documentRef).then((image) => {
      rememberCustomFaceImage(image, storage);
      rememberFace(CUSTOM_FACE_ID);
      useFace(CUSTOM_FACE_ID);
      renderPanel();
      void applyVoice();
    }).catch(() => {});
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
  buildBackgroundOptions();
  backgroundFile?.addEventListener("change", () => {
    const file = backgroundFile.files?.[0];
    backgroundFile.value = "";
    if (!file) return;
    customBackgroundFromFile(file, documentRef).then((image) => {
      const swatch = backgroundOptions?.querySelector?.('[data-background="custom"] .chat-call-background-swatch');
      if (swatch) swatch.style.backgroundImage = `url("${image}")`;
      chooseBackground("custom", image);
    }).catch(() => {});
  });
  startBtn?.addEventListener("click", () => {
    if (call?.phase !== "preview") return;
    if (requestStart) requestStart(call.media);
    else start(call.channelKey, call.partner, call.media).catch(() => {});
  });
  testBtn?.addEventListener("click", () => {
    recordTest().catch(() => {});
  });

  return {
    start,
    preview,
    previewing: () => call?.phase === "preview",
    recordTest,
    route: () => call?.route || null,
    setBackground: (id, image = "") => chooseBackground(id, image),
    background: () => call?.backgroundId || "",
    stage: () => call?.stage || null,
    handleFrame,
    hangUp,
    active: () => Boolean(call),
    activeChannelKey: () => call?.channelKey || "",
    setFace: (id, image = "") => {
      if (id === CUSTOM_FACE_ID && image) rememberCustomFaceImage(image, storage);
      rememberFace(knownFace(id));
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
