window.ForkMeshDashboard.registerPage("video-mix-dj-studio", ({ select }) => {
  const root = select('[data-view="video-mix-dj-studio"]');
  if (!root) return;
  if (new URLSearchParams(location.search).get("presentation") === "studio") document.documentElement.classList.add("vmdj-fullscreen");
  const el = (name) => root.querySelector(`[data-vmdj="${name}"]`);
  const key = "forkmesh.videoMixDjStudio.v1";
  const decks = {
    a: { id: "", url: "", cue: 0, level: 100, rate: 1, loop: false, repeat: false, player: null, ready: false, title: "", duration: 0, time: 0, playing: false, start: 0, resume: false },
    b: { id: "", url: "", cue: 0, level: 100, rate: 1, loop: false, repeat: false, player: null, ready: false, title: "", duration: 0, time: 0, playing: false, start: 0, resume: false },
  };
  const state = { crossfader: 0, interval: null, message: "Load a video into each deck to begin.", sampler: { context: null, level: 80 }, samples: [null, null, null, null], sampleCursor: 0, scratch: { a: null, b: null }, modal: "", modalOpener: null, searchTarget: "a", searchResults: [], searchBusy: false, searchEndpoint: "", savedAt: 0, leaving: false, autoDj: false, autoDjSeed: "", autoDjAt: 0, autoDjChoices: [], autoDjBusy: false, autoDjPlayed: [], autoDjHeard: [], autoDjEnded: "", autoDjMix: null };
  const autoDjNoise = new Set(["official", "video", "audio", "music", "lyrics", "lyric", "visualizer", "visualiser", "hd", "hq", "4k", "mv", "remastered", "remaster", "explicit", "clean", "version", "full", "feat", "ft", "featuring", "topic"]);
  const autoDjNotSongs = ["got talent", "agt", "bgt", "x factor", "xfactor", "the voice", "american idol", "pop idol", "idols", "golden buzzer", "audition", "auditions", "judges", "simon cowell", "reaction", "reactions", "reacts", "reacting", "first time hearing", "first time listening", "vocal coach", "cover by", "covered by", "cover of", "acoustic cover", "piano cover", "guitar cover", "drum cover", "bass cover", "karaoke", "backing track", "sing along", "tutorial", "lesson", "how to play", "how to sing", "remix", "mashup", "nightcore", "sped up", "slowed", "reverb", "8d", "bass boosted", "parody", "medley", "interview", "trailer", "episode", "compilation", "highlights", "rehearsal", "behind the scenes", "documentary", "podcast", "live at", "live from", "live performance", "full performance", "concert", "tiny desk", "sings", "performs", "tribute"];
  const autoDjNotOriginal = ["live", "cover", "covers", "acoustic", "unplugged", "instrumental", "karaoke", "remix", "session", "sessions", "demo"];

  function safeNumber(value, fallback, low, high) {
    const number = Number(value);
    return Number.isFinite(number) ? Math.max(low, Math.min(high, number)) : fallback;
  }

  function restore() {
    try {
      const saved = JSON.parse(localStorage.getItem(key) || "{}");
      state.crossfader = safeNumber(saved.crossfader, 0, -100, 100);
      state.autoDj = saved.autoDj === true;
      state.sampler.level = safeNumber(saved.sampler?.level, 80, 0, 100);
      state.samples = Array.from({ length: 4 }, (_, index) => {
        const sample = saved.samples?.[index] || {};
        return ["a", "b"].includes(sample.deck) ? { deck: sample.deck, start: safeNumber(sample.start, 0, 0, 864000), duration: safeNumber(sample.duration, 8, 1, 30) } : null;
      });
      for (const name of ["a", "b"]) {
        const value = saved.decks?.[name] || {};
        decks[name].url = typeof value.url === "string" ? value.url : "";
        decks[name].id = videoId(decks[name].url);
        decks[name].cue = safeNumber(value.cue, 0, 0, 864000);
        decks[name].level = safeNumber(value.level, 100, 0, 100);
        decks[name].rate = safeNumber(value.rate, 1, 0.25, 2);
        decks[name].loop = value.loop === true;
        decks[name].repeat = value.repeat === true;
        decks[name].start = safeNumber(value.position, decks[name].cue, 0, 864000);
        decks[name].resume = value.playing === true && Boolean(decks[name].id);
      }
    } catch (_) {}
  }

  function persist() {
    const saved = { crossfader: state.crossfader, autoDj: state.autoDj, sampler: { level: state.sampler.level }, samples: state.samples, decks: {} };
    for (const name of ["a", "b"]) {
      const deck = decks[name];
      saved.decks[name] = { url: deck.url, cue: deck.cue, level: deck.level, rate: deck.rate, loop: deck.loop, repeat: deck.repeat, position: deck.time, playing: deck.playing || deck.resume };
    }
    state.savedAt = Date.now();
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
    const raw = String(value || "").trim().replace(/^<|>$/g, "");
    if (/^[A-Za-z0-9_-]{11}$/.test(raw)) return raw;
    try {
      const url = new URL(/^(?:www\.|m\.|music\.)?(?:youtube(?:-nocookie)?\.com|youtu\.be)\//i.test(raw) ? `https://${raw}` : raw);
      const host = url.hostname.replace(/^(?:www\.|m\.|music\.)/i, "").toLowerCase();
      if (host === "youtu.be") {
        const id = url.pathname.split("/").filter(Boolean)[0] || url.searchParams.get("v") || "";
        return /^[A-Za-z0-9_-]{11}$/.test(id) ? id : "";
      }
      if (host === "youtube.com" || host === "youtube-nocookie.com") {
        const attributed = url.pathname === "/attribution_link" ? new URL(url.searchParams.get("u") || "", "https://www.youtube.com").searchParams.get("v") : "";
        const id = url.searchParams.get("v") || url.searchParams.get("vi") || url.searchParams.get("video_id") || attributed || url.pathname.match(/\/(?:embed|shorts|live|v|e|watch)\/([A-Za-z0-9_-]{11})/)?.[1] || url.hash.match(/[?&]v=([A-Za-z0-9_-]{11})/)?.[1] || "";
        return /^[A-Za-z0-9_-]{11}$/.test(id) ? id : "";
      }
    } catch (_) {}
    return raw.match(/(?:youtu\.be\/|youtube(?:-nocookie)?\.com\/(?:watch\?(?:[^#\s]*&)?v=|(?:embed|shorts|live|v|e)\/))([A-Za-z0-9_-]{11})/i)?.[1] || "";
  }

  function canonicalUrl(id) {
    return `https://www.youtube.com/watch?v=${id}`;
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
    command(deck, "addEventListener", ["onError"]);
  }

  function embedUrl(id, start = 0) {
    const origin = encodeURIComponent(location.origin);
    const from = start >= 1 ? `&start=${Math.floor(start)}` : "";
    return `https://www.youtube-nocookie.com/embed/${encodeURIComponent(id)}?enablejsapi=1&playsinline=1&rel=0${from}&origin=${origin}`;
  }

  function loadDeck(name, value = el(`url-${name}`).value, start = 0, resume = false) {
    const deck = decks[name];
    const id = videoId(value);
    if (!id) {
      status("Paste a full YouTube link or an 11-character video ID.", "error");
      return;
    }
    deck.id = id;
    deck.url = canonicalUrl(id);
    deck.ready = false;
    deck.playing = false;
    deck.duration = 0;
    deck.time = start;
    deck.start = start;
    deck.resume = resume;
    const player = el(`player-${name}`);
    player.hidden = false;
    el(`placeholder-${name}`).hidden = true;
    player.src = embedUrl(id, start);
    deck.player = player;
    player.onload = () => {
      setTimeout(() => listen(deck), 100);
      setTimeout(() => listen(deck), 700);
    };
    status(`Deck ${name.toUpperCase()} is loading.`, "");
    persist();
    render();
  }

  function openModal(name, deck = "") {
    state.modalOpener = document.activeElement;
    state.modal = name;
    if (deck) state.searchTarget = deck;
    const modal = el("modal");
    const titles = { "load-a": "Load Deck A", "load-b": "Load Deck B", search: "Search YouTube", "deck-a": "Deck A controls", "deck-b": "Deck B controls", mixer: "Mixer controls", sampler: "Sampler", set: "Set controls" };
    for (const panel of modal.querySelectorAll("[data-vmdj-panel]")) panel.hidden = panel.dataset.vmdjPanel !== name;
    el("modal-title").textContent = titles[name] || "DJ controls";
    modal.hidden = false;
    document.body.classList.add("vmdj-modal-open");
    const focus = modal.querySelector(`[data-vmdj-panel="${name}"] input, [data-vmdj-panel="${name}"] button`);
    setTimeout(() => focus?.focus(), 0);
    render();
  }

  function closeModal() {
    el("modal").hidden = true;
    document.body.classList.remove("vmdj-modal-open");
    state.modal = "";
    const opener = state.modalOpener;
    state.modalOpener = null;
    if (opener instanceof HTMLElement && opener.isConnected) opener.focus();
  }

  function searchCards() {
    const target = el("search-results");
    target.replaceChildren();
    if (state.searchBusy) target.textContent = "Searching YouTube…";
    else if (state.searchResults.length) {
      for (const result of state.searchResults) {
        const button = document.createElement("button");
        const image = document.createElement("img");
        const copy = document.createElement("span");
        const title = document.createElement("strong");
        const author = document.createElement("small");
        const action = document.createElement("b");
        button.type = "button";
        button.className = "vmdj-search-result";
        image.alt = "";
        image.src = result.thumbnail;
        title.textContent = result.title;
        author.textContent = result.author || "YouTube";
        action.textContent = "Load";
        copy.append(title, author);
        button.append(image, copy, action);
        button.addEventListener("click", () => { loadDeck(state.searchTarget, canonicalUrl(result.id)); closeModal(); });
        target.append(button);
      }
    } else target.textContent = "Search by song, artist, remix, set, or video title.";
  }

  function fetchJson(url) {
    return fetch(url).then((response) => response.ok ? response.json() : Promise.reject(new Error(`HTTP ${response.status}`)));
  }

  async function invidious() {
    if (!state.searchEndpoint) {
      const instances = await fetchJson("https://api.invidious.io/instances.json?sort_by=health");
      const instance = instances.find(([, details]) => details.api && details.cors && /^https:\/\//.test(details.uri || ""));
      if (!instance) throw new Error("No searchable YouTube index is available.");
      state.searchEndpoint = instance[1].uri.replace(/\/$/, "");
    }
    return state.searchEndpoint;
  }

  function keyWords(text) {
    return String(text || "").toLowerCase().replace(/[^\p{L}\p{N}&]+/gu, " ").split(" ").filter((word) => word && !autoDjNoise.has(word)).join(" ");
  }

  function songKey(title) {
    const name = String(title || "").replace(/\([^)]*\)|\[[^\]]*\]|\{[^}]*\}/g, " ");
    const parts = name.split(/ [-–—|] /);
    const tail = parts.slice(1).join(" ");
    return keyWords(parts.length > 1 && keyWords(tail) ? tail : name);
  }

  function spaced(text) {
    return ` ${String(text || "").toLowerCase().replace(/[^\p{L}\p{N}&]+/gu, " ").trim()} `;
  }

  function qualifiers(title) {
    const parts = [...String(title || "").matchAll(/\(([^)]*)\)|\[([^\]]*)\]|\{([^}]*)\}/g)].map((match) => match[1] ?? match[2] ?? match[3]);
    const piped = String(title || "").split(" | ");
    const dashed = piped[0].replace(/ [–—] /g, " - ").split(" - ");
    return parts.concat(piped.slice(1), dashed.slice(2)).map(spaced);
  }

  function isSong(item) {
    const text = spaced(`${item.title} ${item.author}`);
    if (autoDjNotSongs.some((phrase) => text.includes(` ${phrase} `))) return false;
    const last = spaced(item.title).trim().split(" ").pop();
    if (["cover", "covers", "karaoke", "instrumental"].includes(last)) return false;
    return !qualifiers(item.title).some((part) => autoDjNotOriginal.some((word) => part.includes(` ${word} `)));
  }

  function isOriginal(item) {
    const author = spaced(item.author);
    if (author.endsWith(" topic ") || author.includes("vevo")) return true;
    const title = spaced(item.title);
    if ([" official ", " remastered ", " remaster ", " original "].some((word) => title.includes(word))) return true;
    const parts = String(item.title || "").replace(/ [–—|] /g, " - ").split(" - ");
    const artist = keyWords(item.author);
    return parts.length > 1 && artist !== "" && keyWords(parts[0]) === artist;
  }

  function viewCount(row) {
    if (Number(row.viewCount) > 0) return Number(row.viewCount);
    const match = String(row.viewCountText || "").toLowerCase().replace(/,/g, "").match(/([0-9]+(?:\.[0-9]+)?)\s*([kmb]?)/);
    return match ? Number(match[1]) * ({ k: 1e3, m: 1e6, b: 1e9 }[match[2]] || 1) : 0;
  }

  function autoDjEligible(rows, avoid, heard) {
    const keys = new Set(heard.filter(Boolean));
    const seen = new Set();
    const found = [];
    for (const row of Array.isArray(rows) ? rows : []) {
      if (row.type && row.type !== "video") continue;
      const id = videoId(row.videoId || row.url);
      if (!id || seen.has(id)) continue;
      seen.add(id);
      const rank = seen.size - 1;
      const seconds = Number(row.lengthSeconds) || 0;
      if (avoid.has(id) || seconds < 60 || seconds > 720) continue;
      const item = { id, title: String(row.title || "Untitled video"), author: String(row.author || ""), views: viewCount(row), likes: null, rank };
      if (isSong(item)) found.push(item);
    }
    return found.filter(isOriginal).concat(found.filter((item) => !isOriginal(item))).filter((item) => {
      const key = songKey(item.title);
      if (key && keys.has(key)) return false;
      if (key) keys.add(key);
      return true;
    });
  }

  function autoDjRanked(found) {
    const span = Math.max(0, ...found.map((item) => item.rank)) + 1;
    const score = (item) => {
      const likes = item.likes ?? item.views * 0.01;
      const related = span > 1 ? 1 - item.rank / (span - 1) : 1;
      const ratio = item.likes !== null && item.views > 0 ? Math.min(0.1, likes / item.views) * 10 : 0;
      return Math.log10(1 + Math.max(0, likes)) + ratio + related * 2;
    };
    return found.map((item, index) => ({ item, index, original: isOriginal(item), score: score(item) })).sort((left, right) => right.original - left.original || right.score - left.score || left.index - right.index).map(({ item }) => item);
  }

  async function findAutoDj(name) {
    const seed = decks[name];
    const id = seed.id;
    state.autoDjSeed = id;
    state.autoDjAt = Date.now();
    state.autoDjChoices = [];
    state.autoDjBusy = true;
    render();
    const avoid = new Set([decks.a.id, decks.b.id, ...state.autoDjPlayed].filter(Boolean));
    const heard = [songKey(seed.title), ...state.autoDjHeard];
    try {
      const endpoint = await invidious();
      const video = await fetchJson(`${endpoint}/api/v1/videos/${encodeURIComponent(id)}?fields=recommendedVideos`);
      let found = autoDjEligible(video.recommendedVideos, avoid, heard);
      const query = songKey(seed.title).split(" ").slice(0, 6).join(" ");
      if (!found.length && query.length >= 2) found = autoDjEligible(await fetchJson(`${endpoint}/api/v1/search?q=${encodeURIComponent(query)}&type=video`), avoid, heard);
      const top = found.slice(0, 6);
      await Promise.all(top.map(async (item) => {
        try {
          const counts = await fetchJson(`${endpoint}/api/v1/videos/${encodeURIComponent(item.id)}?fields=likeCount,viewCount`);
          if (Number.isFinite(counts.likeCount) && counts.likeCount >= 0) item.likes = counts.likeCount;
          if (Number(counts.viewCount) > 0) item.views = Number(counts.viewCount);
        } catch (_) {}
      }));
      if (state.autoDjSeed !== id) return;
      state.autoDjChoices = autoDjRanked(top).concat(autoDjRanked(found.slice(6)));
      if (!found.length) status("Auto DJ found nothing new like this song. It tries again shortly.", "error");
    } catch (_) {
      state.searchEndpoint = "";
      if (state.autoDjSeed === id) status("Auto DJ can’t reach YouTube suggestions right now. It tries again shortly.", "error");
    } finally {
      if (state.autoDjSeed === id) {
        state.autoDjBusy = false;
        render();
      }
    }
  }

  function liveDeck() {
    const playing = ["a", "b"].filter((name) => decks[name].playing);
    return playing.length > 1 ? (state.crossfader <= 0 ? "a" : "b") : playing[0] || "";
  }

  function autoDjTick() {
    if (!state.autoDj || state.autoDjMix) return;
    const live = liveDeck() || state.autoDjEnded;
    const deck = live && decks[live];
    if (!deck?.id || deck.repeat) return;
    if (state.autoDjSeed !== deck.id || (!state.autoDjBusy && !state.autoDjChoices.length && Date.now() - state.autoDjAt > 30000)) findAutoDj(live);
    const left = deck.duration ? (deck.duration - deck.time) / deck.rate : Infinity;
    if ((left <= 10 || state.autoDjEnded === live) && state.autoDjSeed === deck.id && state.autoDjChoices.length) startAutoDjMix(live);
  }

  function startAutoDjMix(from) {
    const to = from === "a" ? "b" : "a";
    if (decks[to].playing) return;
    const pick = state.autoDjChoices.find((item) => item.id !== decks.a.id && item.id !== decks.b.id && !state.autoDjPlayed.includes(item.id));
    if (!pick) return;
    state.autoDjPlayed = [pick.id, ...state.autoDjPlayed].slice(0, 200);
    state.autoDjHeard = [songKey(decks[from].title), songKey(pick.title), ...state.autoDjHeard].filter(Boolean).slice(0, 200);
    state.autoDjEnded = "";
    state.autoDjMix = { from, to, began: Date.now(), fading: 0, start: state.crossfader };
    loadDeck(to, canonicalUrl(pick.id), 0, true);
    status(`Auto DJ: ${pick.title} is up next on Deck ${to.toUpperCase()}.`);
    autoDjFade();
  }

  function autoDjFade() {
    const mix = state.autoDjMix;
    if (!mix) return;
    const now = Date.now();
    if (!mix.fading) {
      if (!decks[mix.to].playing && now - mix.began < 6000) { setTimeout(autoDjFade, 40); return; }
      mix.fading = now;
      mix.start = state.crossfader;
    }
    const progress = Math.min(1, (now - mix.fading) / 8000);
    const eased = progress * progress * (3 - 2 * progress);
    state.crossfader = Math.round(mix.start + ((mix.to === "b" ? 100 : -100) - mix.start) * eased);
    applyMix();
    if (progress < 1) { setTimeout(autoDjFade, 40); return; }
    command(decks[mix.from], "pauseVideo");
    state.autoDjMix = null;
    persist();
    render();
  }

  function toggleAutoDj() {
    state.autoDj = !state.autoDj;
    Object.assign(state, { autoDjSeed: "", autoDjChoices: [], autoDjBusy: false, autoDjEnded: "", autoDjMix: null });
    status(state.autoDj ? (liveDeck() ? "Auto DJ on: when this song ends, a related song people liked plays on the next deck." : "Auto DJ on: play a song and related songs people liked follow it.") : "Auto DJ off.");
    persist();
    render();
  }

  async function searchVideos(query) {
    const text = String(query || "").trim();
    if (text.length < 2) { status("Enter at least two characters to search YouTube.", "error"); return; }
    state.searchBusy = true;
    state.searchResults = [];
    searchCards();
    try {
      const results = await fetchJson(`${await invidious()}/api/v1/search?q=${encodeURIComponent(text)}&type=video`);
      state.searchResults = results.map((result) => ({ id: videoId(result.videoId || result.url), title: String(result.title || "Untitled video"), author: String(result.author || ""), thumbnail: String(result.videoThumbnails?.find((image) => image.quality === "medium")?.url || result.videoThumbnails?.[0]?.url || `https://i.ytimg.com/vi/${videoId(result.videoId || result.url)}/mqdefault.jpg`) })).filter((result) => result.id).slice(0, 12);
      if (!state.searchResults.length) status("No YouTube videos matched that search.", "error");
    } catch (_) {
      state.searchEndpoint = "";
      status("YouTube search is temporarily unavailable. Paste any YouTube link to load it directly.", "error");
    } finally {
      state.searchBusy = false;
      searchCards();
    }
  }

  function destroyDeck(name) {
    const deck = decks[name];
    if (deck.player) deck.player.src = "about:blank";
    Object.assign(deck, { id: "", url: "", cue: 0, level: 100, rate: 1, loop: false, repeat: false, player: null, ready: false, title: "", duration: 0, time: 0, playing: false, start: 0, resume: false });
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

  function captureSection(name) {
    const deck = decks[name];
    if (!deck.player || !deck.duration) {
      status(`Let Deck ${name.toUpperCase()} load before capturing a section.`, "error");
      return;
    }
    const slot = state.sampleCursor;
    state.samples[slot] = { deck: name, start: deck.time, duration: Math.min(8, Math.max(1, deck.duration - deck.time)) };
    state.sampleCursor = (slot + 1) % state.samples.length;
    status(`Captured ${formatTime(state.samples[slot].duration)} from Deck ${name.toUpperCase()} in Section ${slot + 1}.`);
    persist();
    render();
  }

  function playSection(index) {
    const sample = state.samples[index];
    const deck = sample && decks[sample.deck];
    if (!sample || !deck?.player) return;
    command(deck, "seekTo", [sample.start, true]);
    command(deck, "playVideo");
    const startedAt = sample.start;
    window.setTimeout(() => {
      if (Math.abs(deck.time - startedAt) < sample.duration + 2) command(deck, "pauseVideo");
    }, sample.duration * 1000 + 120);
    status(`Playing Section ${index + 1} from Deck ${sample.deck.toUpperCase()}.`);
  }

  function platterAngle(event, platter) {
    const rect = platter.getBoundingClientRect();
    return Math.atan2(event.clientY - rect.top - rect.height / 2, event.clientX - rect.left - rect.width / 2);
  }

  function scratchStart(name, event) {
    const deck = decks[name];
    if (!deck.player || !deck.duration) return;
    const platter = el(`platter-${name}`);
    state.scratch[name] = { pointer: event.pointerId, angle: platterAngle(event, platter), resume: deck.playing };
    platter.setPointerCapture?.(event.pointerId);
    platter.classList.add("is-scratching");
    command(deck, "pauseVideo");
    event.preventDefault();
  }

  function scratchMove(name, event) {
    const scratch = state.scratch[name];
    const deck = decks[name];
    if (!scratch || scratch.pointer !== event.pointerId || !deck.duration) return;
    const angle = platterAngle(event, el(`platter-${name}`));
    let change = angle - scratch.angle;
    if (change > Math.PI) change -= Math.PI * 2;
    if (change < -Math.PI) change += Math.PI * 2;
    scratch.angle = angle;
    deck.time = Math.max(0, Math.min(deck.duration, deck.time + change / (Math.PI * 2) * deck.duration));
    command(deck, "seekTo", [deck.time, true]);
    render();
    event.preventDefault();
  }

  function scratchEnd(name, event) {
    const scratch = state.scratch[name];
    if (!scratch || scratch.pointer !== event.pointerId) return;
    state.scratch[name] = null;
    const platter = el(`platter-${name}`);
    platter.releasePointerCapture?.(event.pointerId);
    platter.classList.remove("is-scratching");
    if (scratch.resume) command(decks[name], "playVideo");
  }

  function samplerContext() {
    if (state.sampler.context) return state.sampler.context;
    const Audio = window.AudioContext || window.webkitAudioContext;
    if (!Audio) return null;
    state.sampler.context = new Audio();
    return state.sampler.context;
  }

  function samplerGain(context, duration, peak = 0.45) {
    const gain = context.createGain();
    const now = context.currentTime;
    gain.gain.setValueAtTime(0.0001, now);
    gain.gain.exponentialRampToValueAtTime(Math.max(0.0001, peak * state.sampler.level / 100), now + .004);
    gain.gain.exponentialRampToValueAtTime(0.0001, now + duration);
    gain.connect(context.destination);
    return gain;
  }

  function noise(context, duration, filterFrequency) {
    const source = context.createBufferSource();
    const buffer = context.createBuffer(1, Math.ceil(context.sampleRate * duration), context.sampleRate);
    const data = buffer.getChannelData(0);
    for (let index = 0; index < data.length; index += 1) data[index] = Math.random() * 2 - 1;
    source.buffer = buffer;
    const filter = context.createBiquadFilter();
    filter.type = "highpass";
    filter.frequency.value = filterFrequency;
    source.connect(filter);
    return { source, output: filter };
  }

  function tone(context, type, startFrequency, endFrequency, duration, peak = .4) {
    const source = context.createOscillator();
    source.type = type;
    const now = context.currentTime;
    source.frequency.setValueAtTime(startFrequency, now);
    source.frequency.exponentialRampToValueAtTime(Math.max(1, endFrequency), now + duration);
    const gain = samplerGain(context, duration, peak);
    source.connect(gain);
    source.start(now);
    source.stop(now + duration + .02);
  }

  function flashPad(name) {
    const pad = root.querySelector(`[data-vmdj-pad="${name}"]`);
    if (!pad) return;
    pad.classList.add("is-playing");
    setTimeout(() => pad.classList.remove("is-playing"), 90);
  }

  function triggerPad(name) {
    const context = samplerContext();
    if (!context) {
      status("This browser does not support the local sampler.", "error");
      return;
    }
    context.resume().catch(() => {});
    const now = context.currentTime;
    if (name === "kick") tone(context, "sine", 155, 43, .32, .8);
    if (name === "tom") tone(context, "sine", 190, 80, .36, .65);
    if (name === "perc") tone(context, "triangle", 740, 160, .16, .36);
    if (name === "riser") tone(context, "sawtooth", 160, 1300, .75, .25);
    if (name === "stab") {
      for (const frequency of [196, 247, 294]) tone(context, "sawtooth", frequency, frequency * .98, .22, .13);
    }
    if (["snare", "hat", "clap"].includes(name)) {
      const duration = name === "hat" ? .07 : name === "clap" ? .3 : .18;
      const source = noise(context, duration, name === "hat" ? 6000 : name === "clap" ? 1000 : 1800);
      const gain = samplerGain(context, duration, name === "hat" ? .22 : .5);
      source.output.connect(gain);
      source.source.start(now);
      source.source.stop(now + duration);
      if (name === "clap") {
        for (const delay of [.045, .09]) {
          const delayed = noise(context, .12, 1000);
          delayed.output.connect(samplerGain(context, .12, .25));
          delayed.source.start(now + delay);
          delayed.source.stop(now + delay + .12);
        }
      }
    }
    flashPad(name);
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
      el(`capture-${name}`).disabled = !deck.player || !deck.duration;
      el(`loop-${name}`).setAttribute("aria-pressed", deck.loop ? "true" : "false");
      el(`repeat-${name}`).setAttribute("aria-pressed", deck.repeat ? "true" : "false");
      el(`seek-${name}`).disabled = !deck.player || !deck.duration;
      el(`seek-${name}`).value = String(deck.duration ? Math.round(deck.time / deck.duration * 1000) : 0);
      el(`time-${name}`).textContent = formatTime(deck.time);
      el(`duration-${name}`).textContent = formatTime(deck.duration);
      el(`level-${name}`).value = String(deck.level);
      el(`level-value-${name}`).textContent = `${Math.round(deck.level)}%`;
      el(`rate-${name}`).value = String(deck.rate);
      el(`rate-value-${name}`).textContent = `${deck.rate.toFixed(2)}×`;
      const artwork = el(`art-${name}`);
      artwork.src = deck.id ? `https://i.ytimg.com/vi/${encodeURIComponent(deck.id)}/hqdefault.jpg` : "";
      artwork.hidden = !deck.id;
      if (deck.player) deck.player.style.opacity = String(gain[name]);
      const platter = el(`platter-${name}`);
      platter.classList.toggle("is-spinning", deck.playing && !state.scratch[name]);
      platter.setAttribute("aria-valuenow", String(deck.duration ? Math.round(deck.time / deck.duration * 100) : 0));
    }
    el("crossfader").value = String(state.crossfader);
    el("auto-dj").setAttribute("aria-pressed", String(state.autoDj));
    el("auto-dj").textContent = state.autoDj && state.autoDjBusy ? "Auto DJ…" : "Auto DJ";
    el("cross-value").textContent = state.crossfader === 0 ? "Center" : state.crossfader < 0 ? `${Math.abs(state.crossfader)}% A` : `${state.crossfader}% B`;
    el("search-target-a").classList.toggle("is-active", state.searchTarget === "a");
    el("search-target-b").classList.toggle("is-active", state.searchTarget === "b");
    el("search-target-a").setAttribute("aria-pressed", String(state.searchTarget === "a"));
    el("search-target-b").setAttribute("aria-pressed", String(state.searchTarget === "b"));
    el("sampler-level").value = String(state.sampler.level);
    for (const [index, sample] of state.samples.entries()) {
      const button = el(`section-${index}`);
      button.disabled = !sample;
      button.querySelector(".vmdj-pad-name").textContent = sample ? `Section ${index + 1}` : `Section ${index + 1}`;
      button.querySelector(".vmdj-pad-key").textContent = sample ? `${sample.deck.toUpperCase()} · ${formatTime(sample.start)}` : "Empty";
    }
  }

  function refresh() {
    for (const name of ["a", "b"]) {
      const deck = decks[name];
      if (deck.loop && deck.playing && deck.time >= deck.cue + 8) command(deck, "seekTo", [deck.cue, true]);
    }
    autoDjTick();
    if ((decks.a.playing || decks.b.playing) && Date.now() - state.savedAt > 5000) persist();
    render();
  }

  function handleMessage(event) {
    if (event.origin !== "https://www.youtube-nocookie.com" || typeof event.data !== "string") return;
    let data;
    try { data = JSON.parse(event.data); } catch (_) { return; }
    const name = ["a", "b"].find((side) => decks[side].player?.contentWindow === event.source);
    const deck = name && decks[name];
    if (!deck) return;
    if (data.event === "onError" && state.autoDj && state.autoDjMix?.to === name) {
      const from = state.autoDjMix.from;
      state.autoDjMix = null;
      startAutoDjMix(from);
      return;
    }
    const info = data.info || {};
    if (typeof info.currentTime === "number") deck.time = info.currentTime;
    if (typeof info.duration === "number" && info.duration > 0) deck.duration = info.duration;
    if (typeof info.videoData?.title === "string") deck.title = info.videoData.title;
    if (data.event === "onStateChange") {
      const playerState = Number(data.info);
      deck.playing = playerState === 1;
      if (playerState === 0 && deck.repeat) {
        deck.time = 0;
        command(deck, "seekTo", [0, true]);
        command(deck, "playVideo");
        status(`Deck ${name.toUpperCase()} repeats its song.`);
      } else if (playerState === 0 && state.autoDj) state.autoDjEnded = name;
      if (playerState === 1 || playerState === 0 || (playerState === 2 && !state.leaving)) deck.resume = false;
      if (!state.leaving) persist();
    }
    if (data.event === "onReady") {
      deck.ready = true;
      command(deck, "setPlaybackRate", [deck.rate]);
      applyMix();
      if (deck.start > 0) command(deck, "seekTo", [deck.start, true]);
      deck.start = 0;
      if (deck.resume) command(deck, "playVideo");
    }
  }

  function changeCrossfader(value) {
    state.autoDjMix = null;
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
    el(`repeat-${name}`).addEventListener("click", () => { decks[name].repeat = !decks[name].repeat; status(decks[name].repeat ? `Deck ${name.toUpperCase()} repeats its song when it ends.` : `Deck ${name.toUpperCase()} repeat is off.`); persist(); render(); });
    el(`capture-${name}`).addEventListener("click", () => captureSection(name));
    el(`seek-${name}`).addEventListener("input", (event) => { const deck = decks[name]; if (!deck.duration) return; deck.time = deck.duration * Number(event.target.value) / 1000; render(); });
    el(`seek-${name}`).addEventListener("change", () => command(decks[name], "seekTo", [decks[name].time, true]));
    el(`level-${name}`).addEventListener("input", (event) => { decks[name].level = safeNumber(event.target.value, 100, 0, 100); applyMix(); persist(); render(); });
    el(`rate-${name}`).addEventListener("change", (event) => { decks[name].rate = safeNumber(event.target.value, 1, 0.25, 2); command(decks[name], "setPlaybackRate", [decks[name].rate]); persist(); render(); });
    const platter = el(`platter-${name}`);
    platter.addEventListener("pointerdown", (event) => scratchStart(name, event));
    platter.addEventListener("pointermove", (event) => scratchMove(name, event));
    platter.addEventListener("pointerup", (event) => scratchEnd(name, event));
    platter.addEventListener("pointercancel", (event) => scratchEnd(name, event));
  }
  el("open-load-a").addEventListener("click", () => openModal("load-a", "a"));
  el("open-load-b").addEventListener("click", () => openModal("load-b", "b"));
  el("open-search").addEventListener("click", () => openModal("search"));
  el("auto-dj").addEventListener("click", toggleAutoDj);
  el("open-deck-a").addEventListener("click", () => openModal("deck-a", "a"));
  el("open-deck-b").addEventListener("click", () => openModal("deck-b", "b"));
  el("open-mixer").addEventListener("click", () => openModal("mixer"));
  el("open-sampler").addEventListener("click", () => openModal("sampler"));
  el("open-set").addEventListener("click", () => openModal("set"));
  el("close-modal").addEventListener("click", closeModal);
  el("modal").addEventListener("click", (event) => { if (event.target === el("modal")) closeModal(); });
  el("modal").addEventListener("keydown", (event) => {
    if (event.key !== "Tab") return;
    const focusable = [...el("modal").querySelectorAll("button:not(:disabled), input:not(:disabled), select:not(:disabled), textarea:not(:disabled), [tabindex]:not([tabindex=\"-1\"])")].filter((element) => !element.closest("[hidden]"));
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    if (!first || !last) return;
    if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
    else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
  });
  el("search-form").addEventListener("submit", (event) => { event.preventDefault(); searchVideos(el("search-query").value); });
  for (const name of ["a", "b"]) el(`search-target-${name}`).addEventListener("click", () => { state.searchTarget = name; render(); });
  el("crossfader").addEventListener("input", (event) => changeCrossfader(event.target.value));
  el("cut-a").addEventListener("click", () => changeCrossfader(-100));
  el("cut-center").addEventListener("click", () => changeCrossfader(0));
  el("cut-b").addEventListener("click", () => changeCrossfader(100));
  el("sync").addEventListener("click", sync);
  el("play-both").addEventListener("click", () => { command(decks.a, "playVideo"); command(decks.b, "playVideo"); });
  el("pause-both").addEventListener("click", () => { command(decks.a, "pauseVideo"); command(decks.b, "pauseVideo"); });
  el("sampler-level").addEventListener("input", (event) => { state.sampler.level = safeNumber(event.target.value, 80, 0, 100); persist(); });
  for (const pad of root.querySelectorAll("[data-vmdj-pad]")) pad.addEventListener("click", () => triggerPad(pad.dataset.vmdjPad));
  for (const index of [0, 1, 2, 3]) el(`section-${index}`).addEventListener("click", () => playSection(index));
  el("save").addEventListener("click", () => { persist(); status("Set saved on this device."); });
  el("reset").addEventListener("click", () => { destroyDeck("a"); destroyDeck("b"); state.crossfader = 0; state.autoDjMix = null; state.autoDjEnded = ""; try { localStorage.removeItem(key); } catch (_) {} status("Studio cleared."); closeModal(); render(); });
  window.addEventListener("message", handleMessage);
  window.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && state.modal) { closeModal(); return; }
    if (event.defaultPrevented || /INPUT|SELECT|TEXTAREA/.test(document.activeElement?.tagName || "")) return;
    const keyName = event.key.toLowerCase();
    if (keyName === "1") play("a");
    else if (keyName === "2") play("b");
    else if (keyName === "q") cue("a");
    else if (keyName === "w") cue("b");
    else if ({ e: "kick", r: "snare", t: "hat", y: "clap", u: "tom", i: "perc", o: "stab", p: "riser" }[keyName]) triggerPad({ e: "kick", r: "snare", t: "hat", y: "clap", u: "tom", i: "perc", o: "stab", p: "riser" }[keyName]);
    else if (/^[3-6]$/.test(keyName)) playSection(Number(keyName) - 3);
    else if (event.key === "ArrowLeft") { event.preventDefault(); changeCrossfader(state.crossfader - 5); }
    else if (event.key === "ArrowRight") { event.preventDefault(); changeCrossfader(state.crossfader + 5); }
  });
  window.addEventListener("pagehide", () => {
    if (state.interval) clearInterval(state.interval);
    state.interval = null;
    state.leaving = true;
    for (const name of ["a", "b"]) decks[name].resume = decks[name].playing || decks[name].resume;
    persist();
    for (const name of ["a", "b"]) command(decks[name], "pauseVideo");
  });
  window.addEventListener("pageshow", (event) => {
    if (!event.persisted) return;
    state.leaving = false;
    if (!state.interval) state.interval = window.setInterval(refresh, 250);
    for (const name of ["a", "b"]) if (decks[name].resume) command(decks[name], "playVideo");
  });
  restore();
  render();
  for (const name of ["a", "b"]) if (decks[name].id) loadDeck(name, decks[name].url, decks[name].start, decks[name].resume);
  state.interval = window.setInterval(refresh, 250);
});
