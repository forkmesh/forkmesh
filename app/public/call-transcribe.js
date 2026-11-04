export const TRANSCRIBE_LANGUAGES = Object.freeze([
  ["en-US", "English (US)"],
  ["en-GB", "English (UK)"],
  ["es-ES", "Español"],
  ["fr-FR", "Français"],
  ["de-DE", "Deutsch"],
  ["it-IT", "Italiano"],
  ["pt-BR", "Português (Brasil)"],
  ["nl-NL", "Nederlands"],
  ["pl-PL", "Polski"],
  ["sv-SE", "Svenska"],
  ["ja-JP", "日本語"],
  ["ko-KR", "한국어"],
  ["zh-CN", "中文 (简体)"],
  ["hi-IN", "हिन्दी"],
]);
export const TRANSCRIPT_MAX_LINES = 5000;
const RESTART_MS = 250;
const FATAL_ERRORS = new Set(["not-allowed", "service-not-allowed", "language-not-supported", "audio-capture"]);

export function speechRecognitionClass(scope = globalThis) {
  return scope.SpeechRecognition || scope.webkitSpeechRecognition || null;
}

export function offlineCapable(Recognition) {
  if (!Recognition) return false;
  const proto = Recognition.prototype || {};
  const staticCheck = typeof Recognition.available === "function" || typeof Recognition.availableOnDevice === "function";
  return staticCheck && ("processLocally" in proto || "mode" in proto);
}

export function transcribeLanguage(scope = globalThis) {
  const wanted = String(scope.navigator?.language || "en-US");
  const exact = TRANSCRIBE_LANGUAGES.find(([code]) => code.toLowerCase() === wanted.toLowerCase());
  if (exact) return exact[0];
  const base = wanted.split("-")[0].toLowerCase();
  return (TRANSCRIBE_LANGUAGES.find(([code]) => code.split("-")[0] === base) || TRANSCRIBE_LANGUAGES[0])[0];
}

export function transcribesTracks(Recognition) {
  return offlineCapable(Recognition) && typeof Recognition.available === "function";
}

export async function offlineTranscriptionStatus(lang, scope = globalThis) {
  const Recognition = speechRecognitionClass(scope);
  if (!offlineCapable(Recognition)) return "unsupported";
  try {
    const value = typeof Recognition.available === "function"
      ? await Recognition.available({ langs: [lang], processLocally: true })
      : await Recognition.availableOnDevice(lang);
    const status = String(value || "").toLowerCase();
    return ["available", "downloadable", "downloading", "unavailable"].includes(status) ? status : "unavailable";
  } catch (_) {
    return "unavailable";
  }
}

export async function installOfflineTranscription(lang, scope = globalThis) {
  const Recognition = speechRecognitionClass(scope);
  if (!offlineCapable(Recognition)) return false;
  try {
    if (typeof Recognition.install === "function") {
      return Boolean(await Recognition.install({ langs: [lang], processLocally: true }));
    }
    if (typeof Recognition.installOnDevice === "function") return Boolean(await Recognition.installOnDevice(lang));
  } catch (_) {}
  return false;
}

function forceLocal(recognition) {
  if ("processLocally" in recognition || "processLocally" in Object.getPrototypeOf(recognition)) {
    recognition.processLocally = true;
    return recognition.processLocally === true;
  }
  if ("mode" in recognition || "mode" in Object.getPrototypeOf(recognition)) {
    recognition.mode = "ondevice-only";
    return recognition.mode === "ondevice-only";
  }
  return false;
}

export function createTranscriber({
  lang,
  scope = globalThis,
  onChange = () => {},
  onError = () => {},
  now = () => Date.now(),
  setTimeoutImpl = globalThis.setTimeout.bind(globalThis),
} = {}) {
  const Recognition = speechRecognitionClass(scope);
  const sources = new Map();
  const lines = [];
  let running = false;
  let trackInput = transcribesTracks(Recognition) ? null : false;

  function changed() {
    try {
      onChange();
    } catch (_) {}
  }

  function push(source, text) {
    const clean = String(text || "").replace(/\s+/g, " ").trim();
    if (!clean) return;
    lines.push({ at: now(), speaker: source.label(), text: clean });
    if (lines.length > TRANSCRIPT_MAX_LINES) lines.splice(0, lines.length - TRANSCRIPT_MAX_LINES);
  }

  function begin(source) {
    if (!running || !sources.has(source.id) || source.recognition) return;
    if (source.track && trackInput === false) {
      source.failed = true;
      return;
    }
    let recognition;
    try {
      recognition = new Recognition();
    } catch (_) {
      return;
    }
    if (!forceLocal(recognition)) {
      running = false;
      onError("This browser cannot keep speech recognition on the device, so the transcript is off.");
      changed();
      return;
    }
    recognition.lang = lang;
    recognition.continuous = true;
    recognition.interimResults = true;
    source.recognition = recognition;
    recognition.onresult = (event) => {
      let interim = "";
      for (let i = event.resultIndex; i < event.results.length; i += 1) {
        const result = event.results[i];
        const text = result?.[0]?.transcript || "";
        if (result.isFinal) push(source, text);
        else interim += text;
      }
      source.interim = interim.trim();
      changed();
    };
    recognition.onerror = (event) => {
      const code = String(event?.error || "");
      if (code === "language-not-supported" || code === "service-not-allowed") {
        stop();
        onError("Offline speech recognition is not ready for this language. Download it first.");
        return;
      }
      if (FATAL_ERRORS.has(code)) {
        source.failed = true;
        if (source.track) trackInput = false;
      }
    };
    recognition.onend = () => {
      if (source.recognition === recognition) source.recognition = null;
      source.interim = "";
      changed();
      if (running && sources.has(source.id) && !source.failed) setTimeoutImpl(() => begin(source), RESTART_MS);
    };
    try {
      if (source.track) {
        recognition.start(source.track);
        trackInput = true;
      } else {
        recognition.start();
      }
    } catch (_) {
      source.recognition = null;
      source.failed = true;
      if (source.track) trackInput = false;
    }
    changed();
  }

  function addSource(id, label, track = null) {
    if (sources.has(id)) {
      const known = sources.get(id);
      known.label = typeof label === "function" ? label : () => String(label || "");
      return;
    }
    const source = {
      id,
      label: typeof label === "function" ? label : () => String(label || ""),
      track,
      recognition: null,
      interim: "",
      failed: false,
    };
    sources.set(id, source);
    begin(source);
  }

  function removeSource(id) {
    const source = sources.get(id);
    if (!source) return;
    sources.delete(id);
    try {
      source.recognition?.abort?.();
    } catch (_) {}
    source.recognition = null;
    changed();
  }

  function start() {
    if (!offlineCapable(Recognition)) return false;
    running = true;
    for (const source of sources.values()) {
      source.failed = false;
      begin(source);
    }
    changed();
    return true;
  }

  function stop() {
    running = false;
    for (const source of sources.values()) {
      const recognition = source.recognition;
      source.recognition = null;
      source.interim = "";
      try {
        recognition?.stop?.();
      } catch (_) {}
    }
    changed();
  }

  return {
    start,
    stop,
    addSource,
    removeSource,
    get running() { return running; },
    get lines() { return lines.slice(); },
    get remoteAudio() { return trackInput !== false; },
    live() {
      return [...sources.values()].filter((source) => source.interim).map((source) => ({ speaker: source.label(), text: source.interim }));
    },
    listening() {
      return [...sources.values()].filter((source) => source.recognition).map((source) => source.id);
    },
    ids() {
      return [...sources.keys()];
    },
    clear() {
      lines.length = 0;
      changed();
    },
  };
}

function clock(at, locale) {
  try {
    return new Date(at).toLocaleTimeString(locale || undefined, { hour: "2-digit", minute: "2-digit", second: "2-digit" });
  } catch (_) {
    return new Date(at).toISOString().slice(11, 19);
  }
}

export function transcriptText(lines, { title = "", locale = "" } = {}) {
  const head = title ? [title, ""] : [];
  return head.concat((lines || []).map((line) => `[${clock(line.at, locale)}] ${line.speaker || "Someone"}: ${line.text}`)).join("\n") + "\n";
}

export function transcriptFileName(title, date = new Date()) {
  const pad = (value) => String(value).padStart(2, "0");
  const stamp = `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}-${pad(date.getHours())}${pad(date.getMinutes())}`;
  const safe = String(title || "meeting").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 48) || "meeting";
  return `${safe}-transcript-${stamp}.txt`;
}
