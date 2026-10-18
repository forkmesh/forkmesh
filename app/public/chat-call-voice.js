export const CALL_VOICE_STORAGE_KEY = "forkmesh.chat.callVoice";
export const CALL_VOICE_PITCH_KEY = "forkmesh.chat.callVoicePitch";
export const CALL_VOICE_MATCH = "face";
export const CALL_VOICE_OFF = "off";
export const CALL_VOICE_PROCESSOR = "fm-call-voice";
export const CALL_VOICE_WORKLET_URL = "/chat-call-voice-worklet.js";
const RESUME_TIMEOUT_MS = 2000;
const MODULE_TIMEOUT_MS = 6000;
const MONITOR_RAMP_S = 0.015;
const HEADPHONE_LABEL = /head(phone|set)|ear(bud|phone)|airpod|buds|bluetooth|hands-?free|\bbt\b|wireless/i;
const SPEAKER_LABEL = /speaker|built-?in output|display audio|hdmi/i;

export const CALL_VOICES = Object.freeze([
  Object.freeze({ id: "maya", label: "Maya", pitchHz: 196, tract: 1.14, brightness: 0, breath: 0.04 }),
  Object.freeze({ id: "theo", label: "Theo", pitchHz: 124, tract: 1.03, brightness: 0.1, breath: 0 }),
  Object.freeze({ id: "kofi", label: "Kofi", pitchHz: 96, tract: 0.93, brightness: -0.15, breath: 0 }),
  Object.freeze({ id: "ingrid", label: "Ingrid", pitchHz: 228, tract: 1.21, brightness: 0.12, breath: 0.08 }),
  Object.freeze({ id: "ren", label: "Ren", pitchHz: 210, tract: 1.17, brightness: 0.05, breath: 0.02 }),
  Object.freeze({ id: "deep", label: "Deep man", pitchHz: 84, tract: 0.9, brightness: -0.25, breath: 0, range: 0.85 }),
  Object.freeze({ id: "older-man", label: "Older man", pitchHz: 128, tract: 0.97, brightness: -0.1, breath: 0.1, range: 0.8, jitter: 0.012, tremor: 0.012 }),
  Object.freeze({ id: "young-man", label: "Young man", pitchHz: 138, tract: 1.06, brightness: 0.12, breath: 0.02, range: 1.15 }),
  Object.freeze({ id: "warm", label: "Warm woman", pitchHz: 188, tract: 1.12, brightness: -0.1, breath: 0.06 }),
  Object.freeze({ id: "low", label: "Low woman", pitchHz: 165, tract: 1.1, brightness: -0.05, breath: 0.03, range: 0.9 }),
  Object.freeze({ id: "older-woman", label: "Older woman", pitchHz: 176, tract: 1.12, brightness: -0.08, breath: 0.1, range: 0.85, jitter: 0.012, tremor: 0.015 }),
  Object.freeze({ id: "teen", label: "Teen", pitchHz: 252, tract: 1.25, brightness: 0.15, breath: 0.04, range: 1.2 }),
  Object.freeze({ id: "neutral", label: "Neutral", pitchHz: 158, tract: 1.08, brightness: 0, breath: 0.03, range: 0.9 }),
  Object.freeze({ id: "robot", label: "Robot", pitchHz: 118, tract: 1, brightness: 0.1, breath: 0, range: 0 }),
]);

export function callVoiceById(id) {
  return CALL_VOICES.find((voice) => voice.id === id) || null;
}

export function normalizeVoiceChoice(choice) {
  const value = String(choice ?? "").trim().toLowerCase();
  if (value === CALL_VOICE_OFF) return CALL_VOICE_OFF;
  if (callVoiceById(value)) return value;
  return CALL_VOICE_MATCH;
}

export function resolveCallVoice(choice, faceId) {
  const value = normalizeVoiceChoice(choice);
  if (value === CALL_VOICE_OFF) return "";
  if (value === CALL_VOICE_MATCH) return callVoiceById(String(faceId || ""))?.id || "";
  return value;
}

export function callVoiceLabel(id) {
  return callVoiceById(id)?.label || "Your own";
}

export function callVoiceChoices() {
  return [
    [CALL_VOICE_MATCH, "Match face"],
    [CALL_VOICE_OFF, "Your own voice"],
    ...CALL_VOICES.map((voice) => [voice.id, voice.label]),
  ];
}

export function callVoicesSupported(globalRef = globalThis) {
  const Context = globalRef.AudioContext || globalRef.webkitAudioContext;
  return Boolean(
    Context &&
    globalRef.AudioWorkletNode &&
    Context.prototype &&
    "audioWorklet" in Context.prototype &&
    typeof Context.prototype.createMediaStreamDestination === "function",
  );
}

export function savedVoiceChoice(storage = globalThis.localStorage) {
  try {
    return normalizeVoiceChoice(storage?.getItem(CALL_VOICE_STORAGE_KEY));
  } catch (_) {
    return CALL_VOICE_MATCH;
  }
}

export function rememberVoiceChoice(choice, storage = globalThis.localStorage) {
  try {
    storage?.setItem(CALL_VOICE_STORAGE_KEY, normalizeVoiceChoice(choice));
  } catch (_) {}
}

export function savedCallerPitch(storage = globalThis.localStorage) {
  try {
    const hz = Number(storage?.getItem(CALL_VOICE_PITCH_KEY));
    return hz > 50 && hz < 500 ? hz : 0;
  } catch (_) {
    return 0;
  }
}

export function rememberCallerPitch(hz, storage = globalThis.localStorage) {
  const value = Math.round(Number(hz) || 0);
  if (!(value > 50 && value < 500)) return;
  try {
    storage?.setItem(CALL_VOICE_PITCH_KEY, String(value));
  } catch (_) {}
}

export function headphonesInLabels(labels) {
  return (labels || []).some((label) => HEADPHONE_LABEL.test(label) && !SPEAKER_LABEL.test(label));
}

export async function headphonesLikely(globalRef = globalThis) {
  try {
    const devices = await globalRef.navigator.mediaDevices.enumerateDevices();
    const outputs = devices.filter((device) => device.kind === "audiooutput");
    const chosen = outputs.filter((device) => device.deviceId === "default");
    return headphonesInLabels((chosen.length ? chosen : outputs).map((device) => device.label || ""));
  } catch (_) {
    return false;
  }
}

export async function createVoiceStage({
  sourceTrack,
  voiceId = "",
  userPitchHz = 0,
  moduleUrl = CALL_VOICE_WORKLET_URL,
  onStats = () => {},
  globalRef = globalThis,
} = {}) {
  const Context = globalRef.AudioContext || globalRef.webkitAudioContext;
  if (!sourceTrack || !Context || !globalRef.AudioWorkletNode) {
    throw new Error("voice changing is not supported here");
  }
  const context = new Context({ latencyHint: "interactive" });
  let stopped = false;
  try {
    await Promise.race([
      context.audioWorklet.addModule(moduleUrl),
      new Promise((_, reject) => globalRef.setTimeout(() => reject(new Error("voice module timed out")), MODULE_TIMEOUT_MS)),
    ]);
    const source = context.createMediaStreamSource(new globalRef.MediaStream([sourceTrack]));
    const node = new globalRef.AudioWorkletNode(context, CALL_VOICE_PROCESSOR, {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [1],
      channelCount: 1,
      channelCountMode: "explicit",
      channelInterpretation: "speakers",
      processorOptions: { preset: callVoiceById(voiceId), userPitchHz },
    });
    const destination = context.createMediaStreamDestination();
    destination.channelCount = 1;
    const monitorGain = context.createGain();
    monitorGain.gain.value = 0;
    source.connect(node);
    node.connect(destination);
    node.connect(monitorGain);
    let monitoring = false;
    if (context.state !== "running") {
      await Promise.race([
        Promise.resolve(context.resume()).catch(() => {}),
        new Promise((resolve) => globalRef.setTimeout(resolve, RESUME_TIMEOUT_MS)),
      ]);
    }
    if (context.state !== "running") throw new Error("audio is not running");
    const track = destination.stream.getAudioTracks()[0];
    if (!track) throw new Error("no processed audio track");
    let current = callVoiceById(voiceId)?.id || "";
    let latest = null;
    node.port.onmessage = (event) => {
      if (event.data?.type !== "stats") return;
      latest = event.data;
      try {
        onStats(latest);
      } catch (_) {}
    };
    return {
      track,
      context,
      voice: () => current,
      stats: () => latest,
      latencyMs: () => Math.round(((context.baseLatency || 0) * 1000)) + (latest?.latencyMs || 0),
      monitorLatencyMs: () => Math.round(((context.baseLatency || 0) + (context.outputLatency || 0)) * 1000) +
        (latest?.latencyMs || 0),
      monitoring: () => monitoring,
      monitor(on) {
        if (stopped) return false;
        const next = Boolean(on);
        if (next === monitoring) return monitoring;
        monitoring = next;
        const now = context.currentTime;
        if (next) monitorGain.connect(context.destination);
        monitorGain.gain.cancelScheduledValues(now);
        monitorGain.gain.setValueAtTime(monitorGain.gain.value, now);
        monitorGain.gain.linearRampToValueAtTime(next ? 1 : 0, now + MONITOR_RAMP_S);
        if (!next) {
          globalRef.setTimeout(() => {
            if (!monitoring && !stopped) {
              try {
                monitorGain.disconnect();
              } catch (_) {}
            }
          }, MONITOR_RAMP_S * 1000 + 50);
        }
        return monitoring;
      },
      setVoice(id) {
        if (stopped) return current;
        current = callVoiceById(id)?.id || "";
        node.port.postMessage({ type: "voice", preset: callVoiceById(current) });
        return current;
      },
      stop() {
        if (stopped) return;
        stopped = true;
        try {
          node.port.postMessage({ type: "stop" });
          source.disconnect();
          node.disconnect();
          monitorGain.disconnect();
        } catch (_) {}
        try {
          track.stop();
        } catch (_) {}
        Promise.resolve(context.close()).catch(() => {});
      },
    };
  } catch (error) {
    stopped = true;
    Promise.resolve(context.close()).catch(() => {});
    throw error;
  }
}

export async function createVoicePreview({
  micStream = null,
  choice = null,
  faceId = "",
  storage = globalThis.localStorage,
  moduleUrl = CALL_VOICE_WORKLET_URL,
  getUserMediaImpl = (constraints) => globalThis.navigator.mediaDevices.getUserMedia(constraints),
  createVoiceStageImpl = createVoiceStage,
  headphonesImpl = () => headphonesLikely(),
  globalRef = globalThis,
} = {}) {
  const ownStream = !micStream;
  const stream = micStream || await getUserMediaImpl({
    audio: { echoCancellation: true, noiseSuppression: true },
  });
  const mic = stream.getAudioTracks()[0];
  let currentChoice = normalizeVoiceChoice(choice ?? savedVoiceChoice(storage));
  let currentFace = String(faceId || "");
  let stage = null;
  try {
    stage = await createVoiceStageImpl({
      sourceTrack: mic,
      voiceId: resolveCallVoice(currentChoice, currentFace),
      userPitchHz: savedCallerPitch(storage),
      moduleUrl,
      onStats: (stats) => rememberCallerPitch(stats?.userPitchHz, storage),
      globalRef,
    });
  } catch (_) {
    stage = null;
  }
  const headphones = await Promise.resolve(headphonesImpl()).catch(() => false);
  if (stage && headphones) stage.monitor(true);
  const apply = () => stage?.setVoice(resolveCallVoice(currentChoice, currentFace));
  return {
    stream,
    supported: Boolean(stage),
    headphones,
    track: () => stage?.track || mic || null,
    voice: () => stage?.voice() || "",
    choice: () => currentChoice,
    setChoice(next) {
      currentChoice = normalizeVoiceChoice(next);
      rememberVoiceChoice(currentChoice, storage);
      apply();
      return currentChoice;
    },
    setFace(id) {
      currentFace = String(id || "");
      apply();
    },
    monitor: (on) => (stage ? stage.monitor(on) : false),
    monitoring: () => Boolean(stage?.monitoring()),
    monitorLatencyMs: () => stage?.monitorLatencyMs() || 0,
    stop({ keepStream = !ownStream } = {}) {
      stage?.stop();
      stage = null;
      if (!keepStream) for (const track of stream.getTracks()) track.stop();
    },
  };
}

export function mountVoicePreviewControls(container, preview, { documentRef = globalThis.document } = {}) {
  if (!container || !preview) return () => {};
  const row = documentRef.createElement("div");
  row.className = "chat-call-voice-preview";
  const label = documentRef.createElement("label");
  label.className = "chat-call-face chat-call-voice";
  const caption = documentRef.createElement("span");
  caption.textContent = "Voice";
  const select = documentRef.createElement("select");
  select.setAttribute("data-voice-preview", "choice");
  for (const [value, text] of callVoiceChoices()) {
    const option = documentRef.createElement("option");
    option.value = value;
    option.textContent = text;
    select.append(option);
  }
  select.value = preview.choice();
  const state = documentRef.createElement("span");
  state.setAttribute("aria-live", "polite");
  label.append(caption, select, state);
  const hear = documentRef.createElement("button");
  hear.type = "button";
  hear.setAttribute("data-voice-preview", "monitor");
  const hint = documentRef.createElement("p");
  hint.className = "chat-call-meta";
  const render = () => {
    state.textContent = "Voice: " + callVoiceLabel(preview.voice());
    const on = preview.monitoring();
    hear.textContent = on ? "Stop hearing myself" : "Hear myself";
    hear.setAttribute("aria-pressed", on ? "true" : "false");
    hear.disabled = !preview.supported;
    hint.textContent = !preview.supported
      ? "Voice changing is not available in this browser."
      : preview.headphones
        ? "Headphones detected: you hear your changed voice as the other person will."
        : "Use headphones to hear yourself; on a speaker your voice will echo and may howl.";
  };
  select.addEventListener("change", () => {
    preview.setChoice(select.value);
    render();
  });
  hear.addEventListener("click", () => {
    preview.monitor(!preview.monitoring());
    render();
  });
  row.append(label, hear, hint);
  container.append(row);
  render();
  return () => row.remove();
}
