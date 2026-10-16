window.ForkMeshDashboard.registerPage("video-mix-dj-studio", ({ select }) => {
  const root = select('[data-view="video-mix-dj-studio"]');
  if (!root) return;
  const el = (name) => root.querySelector(`[data-vmdj="${name}"]`);
  const key = "forkmesh.videoMixDjStudio.v1";
  const decks = {
    a: { id: "", url: "", cue: 0, level: 100, rate: 1, loop: false, player: null, ready: false, title: "", duration: 0, time: 0, playing: false },
    b: { id: "", url: "", cue: 0, level: 100, rate: 1, loop: false, player: null, ready: false, title: "", duration: 0, time: 0, playing: false },
  };
  const state = { crossfader: 0, interval: null, message: "Load a video into each deck to begin." };

  function safeNumber(value, fallback, low, high) {
    const number = Number(value);
    return Number.isFinite(number) ? Math.max(low, Math.min(high, number)) : fallback;
  }

  function restore() {
    try {
      const saved = JSON.parse(localStorage.getItem(key) || "{}");
      state.crossfader = safeNumber(saved.crossfader, 0, -100, 100);
      for (const name of ["a", "b"]) {
        const value = saved.decks?.[name] || {};
        decks[name].url = typeof value.url === "string" ? value.url : "";
        decks[name].id = videoId(decks[name].url);
        decks[name].cue = safeNumber(value.cue, 0, 0, 864000);
        decks[name].level = safeNumber(value.level, 100, 0, 100);
        decks[name].rate = safeNumber(value.rate, 1, 0.25, 2);
        decks[name].loop = value.loop === true;
      }
    } catch (_) {}
  }

  function persist() {
    const saved = { crossfader: state.crossfader, decks: {} };
    for (const name of ["a", "b"]) {
      const deck = decks[name];
      saved.decks[name] = { url: deck.url, cue: deck.cue, level: deck.level, rate: deck.rate, loop: deck.loop };
    }
    try { localStorage.setItem(key, JSON.stringify(saved)); } catch (_) {}
  }

  function status(text, tone = "") {
    state.message = text;
    const target = el("status");
    target.textContent = text;
    if (tone) target.dataset.tone = tone;
    else delete target.dataset.tone;
  }

  function videoId(value) {
    const raw = String(value || "").trim();
    if (/^[A-Za-z0-9_-]{11}$/.test(raw)) return raw;
    try {
      const url = new URL(raw);
      if (/(^|\.)youtu\.be$/i.test(url.hostname)) {
        const id = url.pathname.split("/").filter(Boolean)[0] || "";
        return /^[A-Za-z0-9_-]{11}$/.test(id) ? id : "";
      }
      if (/(^|\.)youtube(?:-nocookie)?\.com$/i.test(url.hostname)) return url.searchParams.get("v") || url.pathname.match(/\/(?:embed|shorts|live)\/([A-Za-z0-9_-]{11})/)?.[1] || "";
    } catch (_) {}
    return "";
  }

  function command(deck, func, args = []) {
    if (!deck.player?.contentWindow) return;
    deck.player.contentWindow.postMessage(JSON.stringify({ event: "command", func, args }), "https://www.youtube-nocookie.com");
  }

  function listen(deck) {
    command(deck, "addEventListener", ["onStateChange"]);
    command(deck, "addEventListener", ["onReady"]);
    command(deck, "addEventListener", ["onVideoDataChange"]);
    command(deck, "addEventListener", ["infoDelivery"]);
  }

  function embedUrl(id) {
    const origin = encodeURIComponent(location.origin);
    return `https://www.youtube-nocookie.com/embed/${encodeURIComponent(id)}?enablejsapi=1&playsinline=1&rel=0&origin=${origin}`;
  }

  function loadDeck(name, value = el(`url-${name}`).value) {
    const deck = decks[name];
    const id = videoId(value);
    if (!id) {
      status("Paste a full YouTube link or an 11-character video ID.", "error");
      return;
    }
    deck.id = id;
    deck.url = String(value).trim();
    deck.ready = false;
    deck.playing = false;
    deck.duration = 0;
    deck.time = 0;
    const player = el(`player-${name}`);
    player.hidden = false;
    el(`placeholder-${name}`).hidden = true;
    player.src = embedUrl(id);
    deck.player = player;
    player.onload = () => {
      setTimeout(() => listen(deck), 100);
      setTimeout(() => listen(deck), 700);
    };
    status(`Deck ${name.toUpperCase()} is loading.`, "");
    persist();
    render();
  }

  function destroyDeck(name) {
    const deck = decks[name];
    if (deck.player) deck.player.src = "about:blank";
    Object.assign(deck, { id: "", url: "", cue: 0, level: 100, rate: 1, loop: false, player: null, ready: false, title: "", duration: 0, time: 0, playing: false });
  }

  function volumeMix() {
    const position = (state.crossfader + 100) / 200;
    const aGain = Math.cos(position * Math.PI / 2);
    const bGain = Math.sin(position * Math.PI / 2);
    return { a: aGain, b: bGain };
  }

  function applyMix() {
    const gain = volumeMix();
    for (const name of ["a", "b"]) {
      const deck = decks[name];
      command(deck, "setVolume", [Math.round(deck.level * gain[name])]);
      if (deck.player) deck.player.style.opacity = String(gain[name]);
    }
  }

  function formatTime(seconds) {
    const total = Math.max(0, Math.floor(Number(seconds) || 0));
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor(total % 3600 / 60);
    const secs = total % 60;
    return hours ? `${hours}:${String(minutes).padStart(2, "0")}:${String(secs).padStart(2, "0")}` : `${minutes}:${String(secs).padStart(2, "0")}`;
  }

  function play(name) {
    const deck = decks[name];
    if (!deck.player) return;
    command(deck, deck.playing ? "pauseVideo" : "playVideo");
  }

  function cue(name) {
    const deck = decks[name];
    if (!deck.player) return;
    command(deck, "seekTo", [deck.cue, true]);
    command(deck, "pauseVideo");
    deck.time = deck.cue;
    deck.playing = false;
    render();
  }

  function sync() {
    const source = decks.a;
    const target = decks.b;
    if (!source.player || !target.player) {
      status("Load both decks before syncing.", "error");
      return;
    }
    target.rate = source.rate;
    command(target, "setPlaybackRate", [source.rate]);
    command(target, "seekTo", [source.time, true]);
    status("Deck B now matches Deck A’s position and tempo.");
    persist();
    render();
  }

  function render() {
    const gain = volumeMix();
    for (const name of ["a", "b"]) {
      const deck = decks[name];
      el(`url-${name}`).value = deck.url;
      el(`title-${name}`).textContent = deck.title || (deck.id ? "Loading video…" : "Ready for a video");
      el(`play-${name}`).textContent = deck.playing ? "Pause" : "Play";
      el(`play-${name}`).disabled = !deck.player;
      el(`cue-${name}`).disabled = !deck.player;
      el(`set-cue-${name}`).disabled = !deck.player;
      el(`loop-${name}`).disabled = !deck.player;
      el(`loop-${name}`).setAttribute("aria-pressed", deck.loop ? "true" : "false");
      el(`seek-${name}`).disabled = !deck.player || !deck.duration;
      el(`seek-${name}`).value = String(deck.duration ? Math.round(deck.time / deck.duration * 1000) : 0);
      el(`time-${name}`).textContent = formatTime(deck.time);
      el(`duration-${name}`).textContent = formatTime(deck.duration);
      el(`level-${name}`).value = String(deck.level);
      el(`level-value-${name}`).textContent = `${Math.round(deck.level)}%`;
      el(`rate-${name}`).value = String(deck.rate);
      el(`rate-value-${name}`).textContent = `${deck.rate.toFixed(2)}×`;
      if (deck.player) deck.player.style.opacity = String(gain[name]);
    }
    el("crossfader").value = String(state.crossfader);
    el("cross-value").textContent = state.crossfader === 0 ? "Center" : state.crossfader < 0 ? `${Math.abs(state.crossfader)}% A` : `${state.crossfader}% B`;
  }

  function refresh() {
    for (const name of ["a", "b"]) {
      const deck = decks[name];
      if (deck.loop && deck.playing && deck.time >= deck.cue + 8) command(deck, "seekTo", [deck.cue, true]);
    }
    render();
  }

  function handleMessage(event) {
    if (event.origin !== "https://www.youtube-nocookie.com" || typeof event.data !== "string") return;
    let data;
    try { data = JSON.parse(event.data); } catch (_) { return; }
    const deck = Object.values(decks).find((item) => item.player?.contentWindow === event.source);
    if (!deck) return;
    const info = data.info || {};
    if (typeof info.currentTime === "number") deck.time = info.currentTime;
    if (typeof info.duration === "number" && info.duration > 0) deck.duration = info.duration;
    if (typeof info.videoData?.title === "string") deck.title = info.videoData.title;
    if (data.event === "onStateChange") deck.playing = Number(data.info) === 1;
    if (data.event === "onReady") {
      deck.ready = true;
      command(deck, "setPlaybackRate", [deck.rate]);
      applyMix();
    }
  }

  function changeCrossfader(value) {
    state.crossfader = safeNumber(value, 0, -100, 100);
    applyMix();
    persist();
    render();
  }

  for (const name of ["a", "b"]) {
    el(`form-${name}`).addEventListener("submit", (event) => { event.preventDefault(); loadDeck(name); });
    el(`play-${name}`).addEventListener("click", () => play(name));
    el(`cue-${name}`).addEventListener("click", () => cue(name));
    el(`set-cue-${name}`).addEventListener("click", () => { decks[name].cue = decks[name].time; status(`Deck ${name.toUpperCase()} cue set at ${formatTime(decks[name].cue)}.`); persist(); render(); });
    el(`loop-${name}`).addEventListener("click", () => { decks[name].loop = !decks[name].loop; status(decks[name].loop ? `Deck ${name.toUpperCase()} loops eight seconds from its cue.` : `Deck ${name.toUpperCase()} loop is off.`); persist(); render(); });
    el(`seek-${name}`).addEventListener("input", (event) => { const deck = decks[name]; if (!deck.duration) return; deck.time = deck.duration * Number(event.target.value) / 1000; render(); });
    el(`seek-${name}`).addEventListener("change", () => command(decks[name], "seekTo", [decks[name].time, true]));
    el(`level-${name}`).addEventListener("input", (event) => { decks[name].level = safeNumber(event.target.value, 100, 0, 100); applyMix(); persist(); render(); });
    el(`rate-${name}`).addEventListener("change", (event) => { decks[name].rate = safeNumber(event.target.value, 1, 0.25, 2); command(decks[name], "setPlaybackRate", [decks[name].rate]); persist(); render(); });
  }
  el("crossfader").addEventListener("input", (event) => changeCrossfader(event.target.value));
  el("cut-a").addEventListener("click", () => changeCrossfader(-100));
  el("cut-center").addEventListener("click", () => changeCrossfader(0));
  el("cut-b").addEventListener("click", () => changeCrossfader(100));
  el("sync").addEventListener("click", sync);
  el("play-both").addEventListener("click", () => { command(decks.a, "playVideo"); command(decks.b, "playVideo"); });
  el("pause-both").addEventListener("click", () => { command(decks.a, "pauseVideo"); command(decks.b, "pauseVideo"); });
  el("save").addEventListener("click", () => { persist(); status("Set saved on this device."); });
  el("reset").addEventListener("click", () => { destroyDeck("a"); destroyDeck("b"); state.crossfader = 0; try { localStorage.removeItem(key); } catch (_) {} status("Studio cleared."); render(); });
  window.addEventListener("message", handleMessage);
  window.addEventListener("keydown", (event) => {
    if (event.defaultPrevented || /INPUT|SELECT|TEXTAREA/.test(document.activeElement?.tagName || "")) return;
    const keyName = event.key.toLowerCase();
    if (keyName === "1") play("a");
    else if (keyName === "2") play("b");
    else if (keyName === "q") cue("a");
    else if (keyName === "w") cue("b");
    else if (event.key === "ArrowLeft") { event.preventDefault(); changeCrossfader(state.crossfader - 5); }
    else if (event.key === "ArrowRight") { event.preventDefault(); changeCrossfader(state.crossfader + 5); }
  });
  window.addEventListener("pagehide", () => { if (state.interval) clearInterval(state.interval); for (const name of ["a", "b"]) command(decks[name], "pauseVideo"); });
  restore();
  render();
  for (const name of ["a", "b"]) if (decks[name].id) loadDeck(name, decks[name].url);
  state.interval = window.setInterval(refresh, 250);
});
