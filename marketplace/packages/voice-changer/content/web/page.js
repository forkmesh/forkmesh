window.ForkMeshDashboard.registerPage("voice-changer", ({ select }) => {
  const root = select('[data-view="voice-changer"]');
  if (!root) return;
  const el = (name) => root.querySelector(`[data-vc="${name}"]`);
  const startButton = el("start");
  const stopButton = el("stop");
  const recordButton = el("record");
  const stopRecordButton = el("stop-record");
  const timer = el("timer");
  const voiceSlot = el("voice-slot");
  const readout = el("readout");
  const pitchOwn = el("pitch-own");
  const pitchNow = el("pitch-now");
  const pitchTarget = el("pitch-target");
  const latency = el("latency");
  const statusLine = el("status");
  const clipPanel = el("clip");
  const clipPlayer = el("clip-player");
  const clipDownload = el("clip-download");
  const clipDiscard = el("clip-discard");
  const clipMeta = el("clip-meta");

  const state = {
    m: null,
    support: null,
    stream: null,
    voice: null,
    voiceUnmount: null,
    starting: false,
    recording: null,
    clip: null,
    clipUrl: "",
    stats: null,
    generation: 0,
  };

  function status(text, tone = "") {
    statusLine.textContent = String(text || "");
    if (tone) statusLine.dataset.tone = tone;
    else delete statusLine.dataset.tone;
  }

  function loadModules() {
    return Promise.all([import("/chat-call-voice.js"), import("/call-studio.js")])
      .then(([voice, studio]) => ({ voice, studio }));
  }

  function hz(value) {
    const number = Math.round(Number(value) || 0);
    return number > 0 ? `${number} Hz` : "-";
  }

  function renderStats() {
    const stats = state.stats;
    const preset = state.m?.voice.callVoiceById(state.voice?.voice() || "");
    pitchOwn.textContent = hz(stats?.userPitchHz);
    pitchNow.textContent = stats?.voiced ? hz(stats.pitchHz) : "-";
    pitchTarget.textContent = preset ? hz(preset.pitchHz) : "Your own";
    const delay = state.voice?.monitoring() ? state.voice.monitorLatencyMs() : Number(stats?.latencyMs) || 0;
    latency.hidden = !(delay > 0);
    latency.textContent = delay > 0 ? `About ${Math.round(delay)} ms between speaking and hearing the changed voice.` : "";
  }

  function render() {
    const on = Boolean(state.stream && state.voice);
    const recording = Boolean(state.recording);
    startButton.hidden = Boolean(state.stream);
    startButton.disabled = state.starting || !state.support?.media || !state.support?.voices;
    stopButton.hidden = !state.stream;
    recordButton.hidden = !on || recording;
    recordButton.disabled = !state.support?.recording;
    stopRecordButton.hidden = !recording;
    timer.hidden = !recording;
    readout.hidden = !on;
    renderStats();
  }

  function trimChoices() {
    const choice = voiceSlot.querySelector('[data-voice-preview="choice"]');
    const match = choice?.querySelector(`option[value="${state.m.voice.CALL_VOICE_MATCH}"]`);
    if (!choice || !match) return;
    const wasMatch = choice.value === match.value;
    match.remove();
    if (wasMatch) choice.value = state.m.voice.CALL_VOICE_OFF;
    choice.addEventListener("change", renderStats);
  }

  async function startMicrophone() {
    if (state.stream || state.starting || !state.m) return;
    const { studio, voice } = state.m;
    state.starting = true;
    const generation = ++state.generation;
    status("Starting the microphone…");
    render();
    let stream = null;
    try {
      stream = await studio.openStudioMedia({ video: false });
      if (generation !== state.generation) {
        studio.stopStudioStream(stream);
        return;
      }
      state.stream = stream;
      const saved = voice.savedVoiceChoice();
      const preview = await studio.startStudioVoice({
        micStream: stream,
        choice: saved === voice.CALL_VOICE_MATCH ? voice.CALL_VOICE_OFF : saved,
        onStats: (stats) => {
          if (generation !== state.generation) return;
          state.stats = stats;
          renderStats();
        },
      });
      if (generation !== state.generation) {
        preview.stop({ keepStream: true });
        return;
      }
      if (!preview.supported) {
        preview.stop({ keepStream: true });
        throw new Error("voice_unavailable");
      }
      state.voice = preview;
      state.voiceUnmount = voice.mountVoicePreviewControls(voiceSlot, preview, { documentRef: document });
      trimChoices();
      status(preview.headphones ? "Live. You hear the changed voice in your headphones." : "Live. Put on headphones and press Hear myself.");
    } catch (error) {
      const micStarted = Boolean(state.stream);
      if (!micStarted) studio.stopStudioStream(stream);
      stopMicrophone();
      status(micStarted ? "The voice changer could not start in this browser." : studio.studioMediaError(error), "error");
    } finally {
      if (generation === state.generation) state.starting = false;
      render();
    }
  }

  function stopMicrophone() {
    state.generation += 1;
    state.starting = false;
    if (state.recording) state.recording.stop().catch(() => {});
    try {
      state.voiceUnmount?.();
    } catch (_) {}
    state.voiceUnmount = null;
    try {
      state.voice?.stop({ keepStream: true });
    } catch (_) {}
    state.voice = null;
    state.stats = null;
    state.m?.studio.stopStudioStream(state.stream);
    state.stream = null;
    render();
  }

  function clearClip() {
    if (state.clipUrl) URL.revokeObjectURL(state.clipUrl);
    state.clipUrl = "";
    state.clip = null;
    clipPlayer.replaceChildren();
    clipPanel.hidden = true;
  }

  function showClip(result) {
    const { studio } = state.m;
    clearClip();
    state.clip = result;
    state.clipUrl = URL.createObjectURL(result.blob);
    const player = document.createElement("audio");
    player.controls = true;
    player.src = state.clipUrl;
    player.setAttribute("aria-label", "Play back your clip");
    clipPlayer.append(player);
    clipMeta.textContent = `${studio.formatStudioTime(result.durationMs)} · ${studio.formatStudioSize(result.blob.size)}`;
    clipPanel.hidden = false;
  }

  async function startRecording() {
    if (!state.voice || state.recording) return;
    const { studio } = state.m;
    const maxLabel = studio.formatStudioTime(studio.STUDIO_CLIP_MAX_MS);
    try {
      state.recording = studio.startStudioClip([state.voice.track()], "voice", {
        onTick: (ms) => {
          timer.textContent = `${studio.formatStudioTime(ms)} / ${maxLabel}`;
        },
      });
    } catch (_) {
      status("Recording is not available in this browser.", "error");
      return;
    }
    timer.textContent = `0:00 / ${maxLabel}`;
    status("Recording…");
    render();
    try {
      const result = await state.recording.done;
      if (result.blob.size) {
        showClip(result);
        status("Clip ready. Play it back, download it or discard it.");
      } else {
        status("Nothing was recorded. Try again.", "error");
      }
    } catch (_) {
      status("The recording failed. Try again.", "error");
    } finally {
      state.recording = null;
      render();
    }
  }

  voiceSlot.addEventListener("click", renderStats);
  startButton.addEventListener("click", () => void startMicrophone());
  stopButton.addEventListener("click", () => {
    stopMicrophone();
    status("Microphone stopped.");
  });
  recordButton.addEventListener("click", () => void startRecording());
  stopRecordButton.addEventListener("click", () => state.recording?.stop().catch(() => {}));
  clipDownload.addEventListener("click", () => {
    if (!state.clip) return;
    const { studio } = state.m;
    studio.downloadStudioBlob(state.clip.blob, studio.studioFileName("voice-changer", studio.studioClipExtension(state.clip.mime)));
  });
  clipDiscard.addEventListener("click", () => {
    clearClip();
    status("Clip discarded.");
  });
  window.addEventListener("pagehide", () => {
    stopMicrophone();
    clearClip();
  });

  render();
  loadModules().then((modules) => {
    state.m = modules;
    state.support = modules.studio.studioSupport(globalThis);
    if (!state.support.media) status("This browser does not offer a microphone to this page. Open it over https in a current browser.", "error");
    else if (!state.support.voices) status("Voice changing is not available in this browser.", "error");
    render();
    window.lucide?.createIcons?.();
  }).catch(() => {
    status("The voice changer could not load. Refresh the page to try again.", "error");
    startButton.disabled = true;
  });
});
