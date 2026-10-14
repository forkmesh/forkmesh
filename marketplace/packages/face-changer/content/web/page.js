window.ForkMeshDashboard.registerPage("face-changer", ({ select }) => {
  const root = select('[data-view="face-changer"]');
  if (!root) return;
  const el = (name) => root.querySelector(`[data-fc="${name}"]`);
  const video = el("video");
  const empty = el("empty");
  const emptyText = el("empty-text");
  const startButton = el("start");
  const stopButton = el("stop");
  const recordButton = el("record");
  const stopRecordButton = el("stop-record");
  const snapshotButton = el("snapshot");
  const popoutButton = el("popout");
  const recBadge = el("rec");
  const statusLine = el("status");
  const faceSelect = el("face");
  const faceFile = el("face-file");
  const backgroundGrid = el("backgrounds");
  const backgroundFile = el("background-file");
  const outfitGrid = el("outfits");
  const stageBox = root.querySelector(".fc-stage");
  const voiceSlot = el("voice-slot");
  const voiceEmpty = el("voice-empty");
  const voiceLabel = el("voice-label");
  const clipPanel = el("clip");
  const clipPlayer = el("clip-player");
  const clipDownload = el("clip-download");
  const clipShare = el("clip-share");
  const clipDiscard = el("clip-discard");
  const clipMeta = el("clip-meta");

  const state = {
    m: null,
    support: null,
    stream: null,
    stage: null,
    voice: null,
    voiceUnmount: null,
    voiceStarting: null,
    faceId: "",
    faceImage: "",
    backgroundId: "",
    backgroundImage: "",
    outfitId: "",
    starting: false,
    recording: null,
    clip: null,
    clipUrl: "",
    popout: null,
    native: false,
    unwatchFace: null,
    generation: 0,
  };

  function status(text, tone = "") {
    statusLine.textContent = String(text || "");
    if (tone) statusLine.dataset.tone = tone;
    else delete statusLine.dataset.tone;
  }

  function loadModules() {
    return Promise.all([
      import("/chat-call-faces.js"),
      import("/chat-call-face-custom.js"),
      import("/chat-call-backgrounds.js"),
      import("/chat-call-voice.js"),
      import("/call-studio.js"),
      import("/chat-call-outfits.js"),
    ]).then(([faces, custom, backgrounds, voice, studio, outfits]) => ({ faces, custom, backgrounds, voice, studio, outfits }));
  }

  function live() {
    return Boolean(state.stream && state.stage);
  }

  function render() {
    const on = live();
    const recording = Boolean(state.recording);
    video.hidden = !on;
    empty.hidden = on;
    startButton.disabled = state.starting || !state.support?.media || !(state.support?.faces || state.support?.backgrounds);
    stopButton.disabled = !on;
    recordButton.hidden = recording;
    recordButton.disabled = !on || !state.support?.recording;
    stopRecordButton.hidden = !recording;
    snapshotButton.disabled = !on;
    popoutButton.disabled = !on;
    popoutButton.hidden = state.native;
    const popped = Boolean(state.popout && !state.popout.closed());
    popoutButton.setAttribute("aria-pressed", popped ? "true" : "false");
    recBadge.hidden = !recording;
    faceSelect.disabled = !state.support?.faces || recording;
    faceSelect.value = state.faceId;
    for (const button of backgroundGrid.querySelectorAll("[data-background]")) {
      const chosen = button.getAttribute("data-background") === state.backgroundId;
      button.setAttribute("aria-checked", chosen ? "true" : "false");
      button.disabled = !state.support?.backgrounds || recording;
    }
    for (const button of outfitGrid.querySelectorAll("[data-outfit]")) {
      const chosen = button.getAttribute("data-outfit") === state.outfitId;
      button.setAttribute("aria-checked", chosen ? "true" : "false");
      button.disabled = !state.support?.backgrounds || recording;
    }
    fitStage();
    voiceEmpty.hidden = Boolean(state.voiceUnmount);
    voiceLabel.hidden = Boolean(state.voiceUnmount);
  }

  function buildFaceOptions() {
    const { studio } = state.m;
    faceSelect.replaceChildren();
    for (const [value, label] of studio.studioFaceChoices()) {
      const option = document.createElement("option");
      option.value = value;
      option.textContent = label;
      faceSelect.append(option);
    }
  }

  function swatchImage(background) {
    const { backgrounds } = state.m;
    if (background.custom) return background.id === state.backgroundId ? state.backgroundImage : backgrounds.savedCallBackground().image;
    return backgrounds.callBackgroundThumbUrl(background);
  }

  function buildBackgroundOptions() {
    const { backgrounds } = state.m;
    backgroundGrid.replaceChildren();
    for (const background of backgrounds.CALL_BACKGROUNDS) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "fc-bg";
      button.setAttribute("role", "radio");
      button.setAttribute("data-background", background.id);
      button.setAttribute("aria-checked", "false");
      button.title = background.label;
      const swatch = document.createElement("span");
      swatch.className = `fc-swatch is-${background.mode}${background.custom ? " is-custom" : ""}`;
      const image = swatchImage(background);
      if (image) swatch.style.backgroundImage = `url("${image}")`;
      const name = document.createElement("span");
      name.className = "fc-bg-name";
      name.textContent = background.label;
      button.append(swatch, name);
      button.addEventListener("click", () => chooseBackground(background.id));
      backgroundGrid.append(button);
    }
  }

  function fitStage() {
    const width = live() ? video.videoWidth : 0;
    const height = live() ? video.videoHeight : 0;
    stageBox.style.aspectRatio = width && height ? `${width} / ${height}` : "";
    stageBox.classList.toggle("is-portrait", Boolean(width && height && height > width));
  }

  function buildOutfitOptions() {
    const { outfits } = state.m;
    outfitGrid.replaceChildren();
    const pending = [];
    for (const outfit of outfits.CALL_OUTFITS) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "fc-bg";
      button.setAttribute("role", "radio");
      button.setAttribute("data-outfit", outfit.id);
      button.setAttribute("aria-checked", "false");
      button.title = outfit.label;
      const swatch = document.createElement("span");
      swatch.className = outfit.id ? "fc-swatch is-outfit" : "fc-swatch is-outfit is-off";
      if (outfit.id) pending.push([outfit.id, swatch]);
      const name = document.createElement("span");
      name.className = "fc-bg-name";
      name.textContent = outfit.label;
      button.append(swatch, name);
      button.addEventListener("click", () => chooseOutfit(outfit.id));
      outfitGrid.append(button);
    }
    const next = () => {
      const entry = pending.shift();
      if (!entry) return;
      try {
        const image = outfits.callOutfitThumbnail(entry[0], document);
        if (image) entry[1].style.backgroundImage = `url("${image}")`;
      } catch (_) {}
      setTimeout(next, 0);
    };
    setTimeout(next, 0);
  }

  function chooseOutfit(id) {
    const { outfits } = state.m;
    const outfit = outfits.callOutfitById(id);
    if (!outfit) return;
    outfits.rememberCallOutfit(outfit.id);
    state.outfitId = outfit.id;
    state.stage?.setOutfit(state.outfitId);
    render();
  }

  function applyFace() {
    if (state.stage) {
      state.stage.setFace(state.faceId, state.faceImage || "");
      watchCustomFace();
    }
    if (!state.voice) return;
    state.voice.setFace(state.faceId);
    const label = voiceSlot.querySelector(".chat-call-voice-preview [aria-live]");
    if (label) label.textContent = "Voice: " + state.m.voice.callVoiceLabel(state.voice.voice());
  }

  function watchCustomFace() {
    state.unwatchFace?.();
    state.unwatchFace = null;
    if (!state.stage || state.faceId !== state.m.custom.CUSTOM_FACE_ID) return;
    state.unwatchFace = state.m.studio.watchStudioFace(state.stage, {
      onFailed: () => {
        status("No face found in that photo. Try a clear, front-facing photo.", "error");
        chooseFace("");
      },
    });
  }

  function chooseFace(id) {
    const { studio } = state.m;
    state.faceId = studio.rememberStudioFace(id);
    state.faceImage = studio.studioFaceImage(state.faceId);
    applyFace();
    render();
  }

  function chooseBackground(id, image = "") {
    const { backgrounds } = state.m;
    const background = backgrounds.callBackgroundById(id);
    if (!background) return;
    if (background.custom && !image) {
      const saved = backgrounds.savedCallBackground();
      if (saved.id === background.id && saved.image) image = saved.image;
      else {
        backgroundFile.click();
        return;
      }
    }
    backgrounds.rememberCallBackground(background.id, image);
    state.backgroundId = background.id;
    state.backgroundImage = background.custom ? image : "";
    state.stage?.setBackground(state.backgroundId, state.backgroundImage);
    render();
  }

  function startVoice(generation) {
    const { studio } = state.m;
    if (!state.support.voices || !state.stream) return;
    state.voiceStarting = studio.startStudioVoice({ micStream: state.stream, faceId: state.faceId })
      .then((voice) => {
        if (generation !== state.generation) {
          voice.stop({ keepStream: true });
          return;
        }
        state.voice = voice;
        state.voiceUnmount = state.m.voice.mountVoicePreviewControls(voiceSlot, voice, { documentRef: document });
        render();
      })
      .catch(() => {})
      .finally(() => {
        if (generation === state.generation) state.voiceStarting = null;
      });
  }

  async function startCamera() {
    if (state.stream || state.starting || !state.m) return;
    const { studio, faces } = state.m;
    state.starting = true;
    const generation = ++state.generation;
    status("Starting the camera…");
    render();
    let stream = null;
    try {
      stream = await studio.openStudioMedia({ video: true });
      if (generation !== state.generation) {
        studio.stopStudioStream(stream);
        return;
      }
      state.stream = stream;
      state.stage = faces.createFaceStage({
        cameraStream: stream,
        faceId: state.support.faces ? state.faceId : "",
        faceImage: state.support.faces ? state.faceImage : "",
        backgroundId: state.support.backgrounds ? state.backgroundId : "",
        backgroundImage: state.support.backgrounds ? state.backgroundImage : "",
        outfitId: state.support.backgrounds ? state.outfitId : "",
      });
      if (!state.stage?.track) throw new Error("face_stage_unavailable");
      video.srcObject = new MediaStream([state.stage.track]);
      video.play?.()?.catch?.(() => {});
      watchCustomFace();
      status("Live. Only you can see this.");
      startVoice(generation);
    } catch (error) {
      const stageFailed = Boolean(state.stream);
      if (!stageFailed) studio.stopStudioStream(stream);
      stopCamera();
      status(
        stageFailed ? "The face changer could not start in this browser." : studio.studioMediaError(error, { video: true }),
        "error",
      );
    } finally {
      if (generation === state.generation) state.starting = false;
      render();
    }
  }

  function stopCamera() {
    state.generation += 1;
    state.starting = false;
    if (state.recording) state.recording.stop().catch(() => {});
    closePopout();
    state.unwatchFace?.();
    state.unwatchFace = null;
    try {
      state.voiceUnmount?.();
    } catch (_) {}
    state.voiceUnmount = null;
    try {
      state.voice?.stop({ keepStream: true });
    } catch (_) {}
    state.voice = null;
    try {
      state.stage?.stop();
    } catch (_) {}
    state.stage = null;
    state.m?.studio.stopStudioStream(state.stream);
    state.stream = null;
    video.srcObject = null;
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
    const player = document.createElement("video");
    player.controls = true;
    player.playsInline = true;
    player.src = state.clipUrl;
    player.setAttribute("aria-label", "Play back your clip");
    clipPlayer.append(player);
    clipMeta.textContent = `${studio.formatStudioTime(result.durationMs)} · ${studio.formatStudioSize(result.blob.size)}`;
    clipShare.hidden = !studio.canShareStudioFiles(result.mime);
    clipDownload.hidden = state.native;
    clipPanel.hidden = false;
  }

  async function shareBlob(blob, name, title) {
    try {
      const outcome = await state.m.studio.shareStudioBlob(blob, name, { title });
      if (outcome === "unsupported") status("Sharing is not available in this browser. Download it instead.", "error");
      else if (outcome !== "cancelled") status("");
    } catch (_) {
      status("It could not be shared. Try again.", "error");
    }
  }

  function shareClip() {
    if (!state.clip) return;
    const { studio } = state.m;
    void shareBlob(state.clip.blob, studio.studioFileName("face-changer", studio.studioClipExtension(state.clip.mime)), "Face Changer clip");
  }

  async function startRecording() {
    if (!live() || state.recording) return;
    const { studio } = state.m;
    const audio = state.voice?.track() || state.stream.getAudioTracks()[0] || null;
    const maxLabel = studio.formatStudioTime(studio.STUDIO_CLIP_MAX_MS);
    try {
      state.recording = studio.startStudioClip([state.stage.track, audio], "video", {
        onTick: (ms) => {
          recBadge.textContent = `${studio.formatStudioTime(ms)} / ${maxLabel}`;
        },
      });
    } catch (_) {
      status("Recording is not available in this browser.", "error");
      return;
    }
    recBadge.textContent = `0:00 / ${maxLabel}`;
    status("Recording…");
    render();
    try {
      const result = await state.recording.done;
      if (result.blob.size) {
        showClip(result);
        status(clipShare.hidden
          ? "Clip ready. Play it back, download it or discard it."
          : "Clip ready. Play it back, share it or discard it.");
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

  function stopRecording() {
    state.recording?.stop().catch(() => {});
  }

  async function takeSnapshot() {
    if (!state.stage) return;
    const { studio } = state.m;
    try {
      const blob = await studio.studioSnapshot(state.stage.canvas);
      if (state.native) {
        await shareBlob(blob, studio.studioFileName("face-changer", "png"), "Face Changer snapshot");
        return;
      }
      studio.downloadStudioBlob(blob, studio.studioFileName("face-changer", "png"));
      status("Snapshot saved.");
    } catch (_) {
      status("The snapshot could not be taken.", "error");
    }
  }

  function closePopout() {
    const popout = state.popout;
    state.popout = null;
    popout?.close();
  }

  function togglePopout() {
    if (!state.stage) return;
    if (state.popout && !state.popout.closed()) {
      closePopout();
      render();
      return;
    }
    const canvas = state.stage.canvas;
    const width = Math.min(960, Math.max(320, Math.round((canvas?.width || 640) * 0.75)));
    const height = Math.round(width * ((canvas?.height || 360) / (canvas?.width || 640)));
    state.popout = state.m.studio.popOutStudioTrack(state.stage.track, {
      title: "Face Changer",
      name: "forkmesh-face-changer",
      width,
      height,
      onClose: () => {
        state.popout = null;
        render();
      },
    });
    status(state.popout
      ? "Popped out. Share that window in any call or meeting app."
      : "Allow pop-ups for this page to pop the picture out.", state.popout ? "" : "error");
    render();
  }

  faceSelect.addEventListener("change", () => {
    const { custom } = state.m;
    if (faceSelect.value === custom.CUSTOM_FACE_ID) {
      faceFile.click();
      if (!custom.savedCustomFaceImage()) {
        faceSelect.value = state.faceId;
        return;
      }
    }
    status("");
    chooseFace(faceSelect.value);
  });
  faceFile.addEventListener("change", () => {
    const file = faceFile.files?.[0];
    faceFile.value = "";
    if (!file) return;
    status("Reading your photo…");
    state.m.studio.studioFaceFromFile(file).then(() => {
      status("");
      chooseFace(state.m.custom.CUSTOM_FACE_ID);
    }).catch(() => status("That file could not be read as a photo.", "error"));
  });
  backgroundFile.addEventListener("change", () => {
    const file = backgroundFile.files?.[0];
    backgroundFile.value = "";
    if (!file) return;
    state.m.studio.studioBackgroundFromFile(file).then((image) => {
      const swatch = backgroundGrid.querySelector('[data-background="custom"] .fc-swatch');
      if (swatch) swatch.style.backgroundImage = `url("${image}")`;
      chooseBackground("custom", image);
    }).catch(() => status("That file could not be read as a photo.", "error"));
  });
  video.addEventListener("loadedmetadata", fitStage);
  video.addEventListener("resize", fitStage);
  startButton.addEventListener("click", () => void startCamera());
  stopButton.addEventListener("click", () => {
    stopCamera();
    status("Camera stopped.");
  });
  recordButton.addEventListener("click", () => void startRecording());
  stopRecordButton.addEventListener("click", stopRecording);
  snapshotButton.addEventListener("click", () => void takeSnapshot());
  popoutButton.addEventListener("click", togglePopout);
  clipDownload.addEventListener("click", () => {
    if (!state.clip) return;
    const { studio } = state.m;
    studio.downloadStudioBlob(state.clip.blob, studio.studioFileName("face-changer", studio.studioClipExtension(state.clip.mime)));
  });
  clipShare.addEventListener("click", shareClip);
  clipDiscard.addEventListener("click", () => {
    clearClip();
    status("Clip discarded.");
  });
  window.addEventListener("pagehide", () => {
    stopCamera();
    clearClip();
  });

  render();
  loadModules().then((modules) => {
    state.m = modules;
    const { studio, backgrounds, outfits } = modules;
    state.support = studio.studioSupport(globalThis);
    state.native = Boolean(studio.studioShareBridge());
    state.faceId = state.support.faces ? studio.savedStudioFace() : "";
    state.faceImage = studio.studioFaceImage(state.faceId);
    const saved = state.support.backgrounds ? backgrounds.savedCallBackground() : { id: "", image: "" };
    state.backgroundId = saved.id;
    state.backgroundImage = saved.image;
    state.outfitId = state.support.backgrounds ? outfits.savedCallOutfit() : "";
    buildFaceOptions();
    buildBackgroundOptions();
    buildOutfitOptions();
    if (!state.support.media) {
      emptyText.textContent = "This browser does not offer a camera to this page. Open it over https in a current browser.";
    } else if (!state.support.faces && !state.support.backgrounds) {
      emptyText.textContent = "The face changer needs WebGL, which this browser has turned off.";
    }
    render();
    window.lucide?.createIcons?.();
  }).catch(() => {
    emptyText.textContent = "The face changer could not load. Refresh the page to try again.";
    startButton.disabled = true;
  });
});
