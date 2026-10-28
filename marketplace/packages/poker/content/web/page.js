window.ForkMeshDashboard.registerPage("poker", ({ select: $, selectAll: $$, session, escapeHtml }) => {
  const API = "/api/poker";
  const SUITS = { c: "♣", d: "♦", h: "♥", s: "♠" };
  const SUIT_WORDS = { c: "clubs", d: "diamonds", h: "hearts", s: "spades" };
  const RANK_WORDS = { 2: "two", 3: "three", 4: "four", 5: "five", 6: "six", 7: "seven", 8: "eight", 9: "nine", T: "ten", J: "jack", Q: "queen", K: "king", A: "ace" };
  const CHIP_COLORS = [[5000, "#ef6c00"], [1000, "#f9a825"], [500, "#6a1b9a"], [100, "#212121"], [25, "#2e7d32"], [5, "#c62828"], [1, "#eceff1"]];
  const BOT_LABELS = { "river-rita": "River Rita", "lucky-lou": "Lucky Lou", "pocket-pete": "Pocket Pete", "chip-charlie": "Chip Charlie", "bluff-betty": "Bluff Betty", "flush-frank": "Flush Frank", "nuts-nora": "Nuts Nora", "turn-tina": "Turn Tina", "ace-adams": "Ace Adams", "stack-sam": "Stack Sam" };
  const TAGS = { "small blind": "SB", "big blind": "BB", "all in": "All in" };
  const store = { get: (key, fallback) => { try { const value = localStorage.getItem(key); return value === null ? fallback : value === "1"; } catch (_) { return fallback; } }, set: (key, value) => { try { localStorage.setItem(key, value ? "1" : "0"); } catch (_) {} } };
  const state = {
    lobby: null, table: null, tableId: "", lastSeq: 0, offset: 0, wakeTimer: 0, loading: false, again: false,
    side: "chat", sideOpen: false, chatSeen: 0, preAction: "", preHand: 0, raiseTo: 0, prev: null, menuOpen: false,
    claimTimer: 0, raf: 0, lastTick: -1, turnFor: "", busy: false,
    muted: store.get("forkmesh.poker.muted", false), voice: store.get("forkmesh.poker.voice", false),
  };
  const currentSession = () => session();

  function chips(value) { return Number(value || 0).toLocaleString(); }
  function label(seat) { return seat?.bot ? (BOT_LABELS[seat.name] || seat.name) : String(seat?.name || ""); }
  function hue(name) { let h = 0; for (const c of String(name)) h = (h * 31 + c.charCodeAt(0)) % 360; return h; }
  function serverNow() { return Date.now() + state.offset; }
  function stake() { return state.table ? { small: state.table.small, big: state.table.big } : { small: 5, big: 10 }; }
  function message(kind, value = "") {
    const open = $$("dialog.pk-dialog[open]");
    if (open.length && kind === "error") {
      const slot = open[open.length - 1].querySelector("[data-poker-dialog-error]");
      if (slot) { slot.querySelector("p").textContent = value; slot.classList.toggle("hidden", !value); return; }
    }
    if (state.tableId && value) { toast(value, kind); return; }
    const node = $(`[data-poker-${kind}]`); if (!node) return;
    node.querySelector("[data-poker-message-text]").textContent = value; node.classList.toggle("hidden", !value);
  }
  function toast(text, kind) {
    const node = document.createElement("div");
    node.setAttribute("role", kind === "error" ? "alert" : "status");
    node.style.cssText = "position:absolute;left:50%;top:56px;transform:translateX(-50%);z-index:40;padding:8px 14px;border-radius:10px;font-size:13px;font-weight:600;box-shadow:0 8px 20px rgba(0,0,0,.5);max-width:90%;text-align:center;" + (kind === "error" ? "background:#b71c1c;color:#fff" : "background:#fff3c4;color:#1a1306");
    node.textContent = text; $(`[data-poker-room]`).append(node);
    setTimeout(() => node.remove(), 3200);
  }
  function friendly(error) {
    const map = { not_enough_chips: "You don't have enough chips for that.", claim_not_ready: "Your free chips aren't ready yet.", table_full: "That table is full.", seat_taken: "Someone just took that seat.", already_seated: "You're already sitting at this table.", not_your_turn: "It's not your turn.", hand_changed: "That hand is over.", invalid_amount: "That amount isn't allowed.", invalid_buy_in: "Pick a buy-in inside the table's range.", too_many_tables: "You already run five tables. Close one first.", owner_only: "Only the table's owner can do that.", table_not_found: "That table is closed.", slow_down: "Slow down a little.", busy: "The table is busy. Try again.", hand_in_progress: "You can close the table between hands.", not_seated: "Sit down at the table first." };
    return map[error?.payload?.error] || String(error?.message || error);
  }
  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    if (currentSession()?.sessionToken) headers.authorization = `Bearer ${currentSession().sessionToken}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(API + path, { method, headers, cache: "no-store", body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(String(payload.error || `HTTP ${response.status}`).replaceAll("_", " "));
      error.status = response.status; error.payload = payload; throw error;
    }
    return payload;
  }

  const audio = { ctx: null, master: null, noise: null };
  function audioContext() {
    if (state.muted) return null;
    if (!audio.ctx) {
      const Context = window.AudioContext || window.webkitAudioContext; if (!Context) return null;
      audio.ctx = new Context(); audio.master = audio.ctx.createGain(); audio.master.gain.value = 0.7; audio.master.connect(audio.ctx.destination);
      const length = audio.ctx.sampleRate; audio.noise = audio.ctx.createBuffer(1, length, audio.ctx.sampleRate);
      const data = audio.noise.getChannelData(0); for (let i = 0; i < length; i += 1) data[i] = Math.random() * 2 - 1;
    }
    if (audio.ctx.state === "suspended") audio.ctx.resume().catch(() => {});
    return audio.ctx;
  }
  function burst(ctx, at, duration, { type = "bandpass", freq = 3000, freqEnd = 0, q = 1, gain = 0.3, attack = 0.002 } = {}) {
    const source = ctx.createBufferSource(); source.buffer = audio.noise;
    const filter = ctx.createBiquadFilter(); filter.type = type; filter.Q.value = q; filter.frequency.setValueAtTime(freq, at);
    if (freqEnd) filter.frequency.exponentialRampToValueAtTime(freqEnd, at + duration);
    const amp = ctx.createGain(); amp.gain.setValueAtTime(0.0001, at); amp.gain.exponentialRampToValueAtTime(gain, at + attack); amp.gain.exponentialRampToValueAtTime(0.0001, at + duration);
    source.connect(filter).connect(amp).connect(audio.master); source.start(at, Math.random() * 0.5); source.stop(at + duration + 0.03);
  }
  function tone(ctx, at, freq, duration, { type = "sine", gain = 0.2, freqEnd = 0, attack = 0.004 } = {}) {
    const osc = ctx.createOscillator(); osc.type = type; osc.frequency.setValueAtTime(freq, at);
    if (freqEnd) osc.frequency.exponentialRampToValueAtTime(freqEnd, at + duration);
    const amp = ctx.createGain(); amp.gain.setValueAtTime(0.0001, at); amp.gain.exponentialRampToValueAtTime(gain, at + attack); amp.gain.exponentialRampToValueAtTime(0.0001, at + duration);
    osc.connect(amp).connect(audio.master); osc.start(at); osc.stop(at + duration + 0.03);
  }
  const SOUNDS = {
    shuffle(ctx, t) { for (let i = 0; i < 10; i += 1) burst(ctx, t + i * 0.032 + (Math.random() - 0.5) * 0.012, 0.012, { freq: 3000, q: 1.3, gain: 0.08 + 0.3 * Math.sin(Math.PI * i / 9) }); },
    deal(ctx, t) { burst(ctx, t, 0.045, { freq: 5000, freqEnd: 1500, q: 0.9, gain: 0.35 }); },
    chip(ctx, t) { for (let i = 0; i < 3; i += 1) { const at = t + i * 0.045; tone(ctx, at, 3200, 0.025, { gain: 0.1 }); tone(ctx, at, 5100, 0.025, { gain: 0.06 }); burst(ctx, at, 0.005, { type: "highpass", freq: 4000, gain: 0.12, attack: 0.001 }); } },
    check(ctx, t) { for (let i = 0; i < 2; i += 1) { tone(ctx, t + i * 0.12, 150, 0.06, { gain: 0.45 }); burst(ctx, t + i * 0.12, 0.03, { type: "lowpass", freq: 700, gain: 0.18 }); } },
    fold(ctx, t) { burst(ctx, t, 0.18, { type: "lowpass", freq: 1200, gain: 0.22, attack: 0.07 }); },
    turn(ctx, t) { tone(ctx, t, 880, 0.14, { gain: 0.14 }); tone(ctx, t + 0.14, 1320, 0.14, { gain: 0.12 }); },
    clock(ctx, t) { tone(ctx, t, 1000, 0.02, { gain: 0.08 }); },
    allin(ctx, t) { tone(ctx, t, 110, 0.4, { gain: 0.3, freqEnd: 220, attack: 0.05 }); for (let i = 0; i < 6; i += 1) SOUNDS.chip(ctx, t + 0.06 * i); },
    win(ctx, t) { [523.25, 659.25, 783.99, 1046.5].forEach((freq, i) => tone(ctx, t + i * 0.11, freq, 0.22, { type: "triangle", gain: 0.16 })); for (let i = 0; i < 5; i += 1) SOUNDS.chip(ctx, t + 0.5 + i * 0.07); },
  };
  function play(names) {
    const ctx = audioContext(); if (!ctx) return;
    let at = ctx.currentTime + 0.02;
    for (const name of names) { if (SOUNDS[name]) SOUNDS[name](ctx, at); at += name === "shuffle" ? 0.38 : name === "win" ? 0.6 : 0.11; }
  }
  function speak(text) {
    if (!state.voice || state.muted || !window.speechSynthesis || !text) return;
    const spoken = String(text).replace(/(10|[2-9JQKA])([♣♦♥♠])/g, (_m, rank, suit) => {
      const r = rank === "10" ? "T" : rank; const s = Object.keys(SUITS).find((key) => SUITS[key] === suit);
      return `${RANK_WORDS[r]} of ${SUIT_WORDS[s]},`;
    }).replace(/#/g, "number ");
    const line = new SpeechSynthesisUtterance(spoken); line.rate = 1.05; line.pitch = 0.95; line.volume = 0.9;
    window.speechSynthesis.cancel(); window.speechSynthesis.speak(line);
  }

  function cardHtml(card, cls = "", style = "") {
    if (!card || card === "??") return `<div class="pk-card back ${cls}" style="${style}" aria-label="Face-down card"></div>`;
    const rank = card[0] === "T" ? "10" : card[0], suit = SUITS[card[1]] || "", red = card[1] === "h" || card[1] === "d";
    const face = "JQK".includes(card[0]);
    const pip = face ? `<div class="pip face"><span>${rank}</span></div>` : `<div class="pip">${suit}</div>`;
    return `<div class="pk-card ${red ? "red" : ""} ${cls}" style="${style}" aria-label="${RANK_WORDS[card[0]]} of ${SUIT_WORDS[card[1]]}"><div class="ix">${rank}<i>${suit}</i></div>${pip}<div class="ix b">${rank}<i>${suit}</i></div></div>`;
  }
  function chipColumns(amount) {
    let rest = Math.max(0, Math.round(Number(amount || 0)));
    const columns = [];
    for (const [value, color] of CHIP_COLORS) {
      const count = Math.floor(rest / value); if (!count) continue;
      rest -= count * value; columns.push({ color, count: Math.min(count, 9) });
      if (columns.length === 3) break;
    }
    return columns;
  }
  function chipStackHtml(amount) {
    return `<div class="pk-chips">${chipColumns(amount).map((column) => `<div class="pk-col" style="height:calc(var(--u) * ${(1.5 + 0.5 * (column.count - 1)).toFixed(2)})">${Array.from({ length: column.count }, (_v, i) => `<div class="pk-chip${i === column.count - 1 ? " top" : ""}" style="--c:${column.color};bottom:calc(var(--u) * ${(0.5 * i).toFixed(2)})"></div>`).join("")}</div>`).join("")}</div>`;
  }

  function seatAngle(rotated, count) {
    const half = Math.floor(count / 2), step = 310 / count;
    return 90 + (((rotated + half) % count) - half) * step;
  }
  function polar(angle, k, portrait) {
    const a = angle * Math.PI / 180;
    return { x: 50 + 50 * k * Math.cos(a), y: 50 + 50 * k * Math.sin(a) * (portrait ? 0.985 : 1) };
  }
  function geometry(view, portrait) {
    const count = view.seatCount, base = view.mySeat >= 0 ? view.mySeat : 0;
    return view.seats.map((seat) => {
      const rotated = (seat.index - base + count) % count, angle = seatAngle(rotated, count);
      return {
        angle, me: seat.index === view.mySeat,
        avatar: polar(angle, 0.985, portrait),
        cards: seat.index === view.mySeat ? polar(angle, portrait ? 0.6 : 0.5, portrait) : portrait ? holeSpot(angle) : polar(angle, 0.68, portrait),
        bet: polar(angle, portrait ? 0.46 : 0.43, portrait),
        puck: polar(angle + (portrait ? 14 : 11), portrait ? 0.62 : 0.58, portrait),
      };
    });
  }
  function holeSpot(angle) {
    const avatar = polar(angle, 0.985, true);
    return { x: avatar.x + (avatar.x < 50 ? 13 : -13), y: avatar.y + 1.5 };
  }
  const DEALER_POINT = { x: 50, y: -4 };
  const POT_POINT = (portrait) => ({ x: 50, y: portrait ? 37 : 31 });

  function layoutStage() {
    const stage = $(`[data-poker-stage]`), table = $(`[data-poker-table]`); if (!stage || !table) return false;
    const width = stage.clientWidth, height = stage.clientHeight;
    const portrait = width / Math.max(1, height) < 0.9;
    const unit = portrait ? Math.min(width / 78, height / 120) : Math.min(width / 118, height / 74);
    $(`[data-view="poker"]`).style.setProperty("--u", `${Math.max(3, unit).toFixed(2)}px`);
    table.classList.toggle("portrait", portrait);
    return portrait;
  }

  function tagHtml(action, fresh) {
    if (!action?.kind) return "";
    const kind = String(action.kind), text = TAGS[kind] || kind;
    const cls = kind === "all in" ? "allin" : kind.split(" ")[0];
    const amount = ["call", "bet", "raise", "all in"].includes(kind) && action.amount ? ` ${chips(action.amount)}` : "";
    return `<div class="pk-tag ${cls}${fresh ? " new" : ""}">${escapeHtml(text + amount)}</div>`;
  }
  function renderTable(view, fresh) {
    const portrait = layoutStage();
    const places = geometry(view, portrait);
    const prev = state.prev && state.prev.id === view.id ? state.prev : null;
    const newHand = !prev || prev.handNumber !== view.handNumber;
    const dealtNow = fresh.some((event) => event.kind === "deal");
    const table = $(`[data-poker-table]`), tableW = table.clientWidth, tableH = table.clientHeight;
    const bestCards = new Set(view.phase === "showdown" ? (view.lastHand?.winners || []).flatMap((winner) => winner.best || []) : []);
    const parts = [];
    let dealOrder = 0;
    view.seats.forEach((seat, index) => {
      const place = places[index], at = place.avatar, before = prev?.seats[index];
      if (seat.empty) {
        const canSit = view.mySeat < 0;
        parts.push(`<div class="pk-seat" style="left:${at.x}%;top:${at.y}%">${canSit ? `<button type="button" class="pk-empty" data-poker-sit-seat="${index}">Sit<br>here</button>` : `<div class="pk-empty" style="opacity:.35">Empty</div>`}</div>`);
        return;
      }
      const classes = ["pk-seat", seat.isTurn ? "turn" : "", seat.status === "folded" ? "folded" : "", seat.status === "out" || seat.status === "leaving" ? "out" : "", seat.away ? "away" : "", seat.winner ? "winner" : "", place.me ? "me" : ""].filter(Boolean).join(" ");
      const ring = seat.isTurn ? `<svg class="pk-ring" viewBox="0 0 100 100" aria-hidden="true"><circle data-poker-ring cx="50" cy="50" r="46" stroke="#66bb6a" stroke-dasharray="289" stroke-dashoffset="0"/></svg>` : "";
      parts.push(`<div class="${classes}" style="left:${at.x}%;top:${at.y}%" data-poker-seat="${index}">
        ${tagHtml(seat.lastAction, JSON.stringify(seat.lastAction) !== JSON.stringify(before?.lastAction))}
        <div class="pk-avatar" style="--hue:${hue(seat.name)}">${ring}<span>${escapeHtml(label(seat).charAt(0).toUpperCase())}</span>${seat.bot ? `<span class="pk-badge">BOT</span>` : ""}</div>
        <div class="pk-plate"><div class="pk-name" title="${escapeHtml(label(seat))}">${escapeHtml(label(seat))}${place.me ? " (you)" : ""}</div><div class="pk-stack">${chips(seat.stack)}</div></div>
        ${seat.won ? `<div class="pk-won${before?.won !== seat.won ? " new" : ""}">+${chips(seat.won)}</div>` : ""}
      </div>`);
      if (seat.cards?.length) {
        const holeAt = place.cards;
        const cards = seat.cards.map((card, cardIndex) => {
          let cls = seat.status === "folded" ? "dim" : "";
          let style = "";
          if (dealtNow && newHand !== false && card) {
            const order = cardIndex * view.seats.filter((other) => other.cards?.length).length + dealOrder;
            const fx = (DEALER_POINT.x - holeAt.x) * tableW / 100, fy = (DEALER_POINT.y - holeAt.y) * tableH / 100;
            cls += " deal"; style = `--fx:${fx.toFixed(0)}px;--fy:${fy.toFixed(0)}px;--delay:${order * 90}ms`;
          } else if (prev && view.phase === "showdown" && card !== "??" && (prev.seats[index]?.cards || [])[cardIndex] === "??") {
            cls += " flip"; style = `--delay:${cardIndex * 120}ms`;
          }
          if (bestCards.has(card)) cls += " best";
          return cardHtml(card, cls, style);
        }).join("");
        dealOrder += 1;
        parts.push(`<div class="pk-hole${place.me ? " me" : ""}" style="left:${holeAt.x}%;top:${holeAt.y}%">${cards}</div>`);
      }
      if (seat.bet > 0) {
        const betAt = place.bet;
        parts.push(`<div class="pk-bet${before?.bet !== seat.bet ? " new" : ""}" data-poker-bet="${index}" style="left:${betAt.x}%;top:${betAt.y}%">${chipStackHtml(seat.bet)}<span class="pk-bet-amount">${chips(seat.bet)}</span></div>`);
      }
    });
    const prevBoard = prev && !newHand ? prev.board.length : 0;
    const board = Array.from({ length: 5 }, (_v, i) => {
      const card = view.board[i];
      if (!card) return `<div class="pk-slot"></div>`;
      const cls = [i >= prevBoard && fresh.some((event) => ["flop", "turn", "river"].includes(event.kind)) ? "flip" : "", bestCards.has(card) ? "best" : ""].join(" ");
      return cardHtml(card, cls, `--delay:${Math.max(0, i - prevBoard) * 140}ms`);
    }).join("");
    parts.push(`<div class="pk-board">${board}</div>`);
    const pot = view.pots.reduce((sum, item) => sum + item.amount, 0);
    if (pot > 0) {
      const side = view.pots.length > 1 ? `<div class="pk-sidepots">${view.pots.map((item, i) => `${i ? "Side" : "Main"} ${chips(item.amount)}`).join(" · ")}</div>` : "";
      const potAt = POT_POINT(portrait);
      parts.push(`<div class="pk-pot" style="left:${potAt.x}%;top:${potAt.y}%">${chipStackHtml(pot)}<div class="pk-pot-label">Pot ${chips(view.potTotal)}</div>${side}</div>`);
    } else if (view.potTotal > 0) {
      parts.push(`<div class="pk-pot" style="top:${POT_POINT(portrait).y}%"><div class="pk-pot-label">Pot ${chips(view.potTotal)}</div></div>`);
    }
    if (view.phase === "showdown" && view.lastHand?.winners?.length) {
      const top = view.lastHand.winners[0];
      const seat = view.seats[top.seat];
      const name = seat && !seat.empty ? label(seat) : top.name;
      parts.push(`<div class="pk-banner${fresh.some((event) => event.kind === "win") ? " new" : ""}">${escapeHtml(name)} wins ${chips(top.amount)}${top.handName ? ` · ${escapeHtml(top.handName)}` : ""}</div>`);
    } else if (view.phase === "waiting") {
      parts.push(`<div class="pk-banner" style="background:rgba(0,0,0,.5);color:#fff3c4;animation:none">${view.seats.filter((seat) => !seat.empty).length < 2 ? "Waiting for players" : "Next hand starting"}</div>`);
    }
    $(`[data-poker-layer]`).innerHTML = parts.join("");
    const speech = $(`[data-poker-speech]`);
    if (speech.textContent !== view.dealerLine) {
      speech.textContent = view.dealerLine || "";
      speech.classList.remove("fresh"); void speech.offsetWidth; speech.classList.add("fresh");
    }
    speech.classList.toggle("hidden", !view.dealerLine);
    const puck = $(`[data-poker-puck]`);
    if (view.dealer >= 0 && view.seats[view.dealer] && !view.seats[view.dealer].empty && view.phase !== "waiting") {
      const at = places[view.dealer].puck; puck.style.left = `${at.x}%`; puck.style.top = `${at.y}%`; puck.classList.remove("hidden");
      puck.textContent = portrait ? "D" : "DEALER";
    } else puck.classList.add("hidden");
    animateChips(view, prev, fresh, places, portrait);
  }
  function flyTo(from, to, amount, delay = 0) {
    const table = $(`[data-poker-table]`); if (!table || !amount) return;
    const node = document.createElement("div");
    node.className = "pk-fly"; node.style.left = `${from.x}%`; node.style.top = `${from.y}%`;
    node.innerHTML = chipStackHtml(amount); table.append(node);
    const dx = (to.x - from.x) * table.clientWidth / 100, dy = (to.y - from.y) * table.clientHeight / 100;
    const motion = node.animate([
      { transform: "translate(-50%,-50%) scale(1)", opacity: 1 },
      { transform: `translate(calc(-50% + ${dx}px), calc(-50% + ${dy}px)) scale(.9)`, opacity: 1, offset: 0.85 },
      { transform: `translate(calc(-50% + ${dx}px), calc(-50% + ${dy}px)) scale(.8)`, opacity: 0 },
    ], { duration: 620, delay, easing: "cubic-bezier(.3,.7,.3,1)", fill: "both" });
    motion.onfinish = () => node.remove();
  }
  function animateChips(view, prev, fresh, places, portrait) {
    if (!prev || !fresh.length) return;
    const potAt = POT_POINT(portrait);
    const streetEnded = fresh.some((event) => ["flop", "turn", "river", "showdown", "win"].includes(event.kind));
    if (streetEnded && prev.handNumber === view.handNumber) {
      prev.seats.forEach((seat, index) => { if (!seat.empty && seat.bet > 0 && places[index]) flyTo(places[index].bet, potAt, seat.bet); });
    }
    fresh.filter((event) => event.kind === "win" && places[event.seat]).forEach((event, i) => flyTo(potAt, places[event.seat].avatar, event.amount, 380 + i * 160));
  }

  function soundsFor(fresh, view) {
    const names = [];
    for (const event of fresh) {
      if (event.kind === "shuffle") names.push("shuffle");
      else if (event.kind === "deal") for (let i = 0; i < Math.min(18, (event.amount || 2) * 2); i += 1) names.push("deal");
      else if (event.kind === "flop") names.push("deal", "deal", "deal");
      else if (event.kind === "turn" || event.kind === "river") names.push("deal");
      else if (["blinds", "call", "bet", "raise", "join"].includes(event.kind)) names.push("chip");
      else if (event.kind === "allin") names.push("allin");
      else if (event.kind === "check") names.push("check");
      else if (event.kind === "fold" || event.kind === "timeout") names.push("fold");
      else if (event.kind === "win" && !names.includes("win")) names.push(event.seat === view.mySeat ? "win" : "chip");
    }
    return names;
  }

  function mySeat() { return state.table && state.table.mySeat >= 0 ? state.table.seats[state.table.mySeat] : null; }
  function raiseBounds() { const legal = state.table?.legal; return legal?.raise ? legal.raise : null; }
  function presetTarget(fraction) {
    const view = state.table, legal = view.legal, seat = mySeat(), bounds = raiseBounds(); if (!bounds) return 0;
    if (fraction === "all") return bounds.max;
    const target = seat.bet + legal.call + Math.round(fraction * (view.potTotal + legal.call));
    return Math.max(bounds.min, Math.min(bounds.max, target));
  }
  function renderActions() {
    const view = state.table, root = $(`[data-poker-actions]`), seat = mySeat();
    const myHand = view.myHand?.name ? `<span class="pk-myhand">${escapeHtml(view.myHand.name)}</span>` : "";
    if (!seat) {
      const empty = view.seats.find((item) => item.empty);
      root.innerHTML = `<div class="pk-actions-row"><span class="pk-note flex-1">You're watching. ${empty ? "Pick an empty seat to play." : "Every seat is taken."}</span>${empty ? `<button type="button" class="pk-act call" style="flex:0 1 180px" data-poker-sit-seat="${empty.index}">Sit down</button>` : ""}</div>`;
      return;
    }
    const legal = view.legal;
    if (legal) {
      const bounds = legal.raise;
      if (bounds) state.raiseTo = Math.max(bounds.min, Math.min(bounds.max, state.raiseTo || bounds.min));
      const callLabel = legal.check ? "Check" : legal.call >= seat.stack ? "Call all in" : "Call";
      const raiseWord = view.currentBet > 0 ? "Raise to" : "Bet";
      root.innerHTML = `<div class="pk-actions-row">
        ${myHand}
        <button type="button" class="pk-act fold" data-poker-act="fold">Fold<small>F</small></button>
        <button type="button" class="pk-act call" data-poker-act="${legal.check ? "check" : "call"}">${callLabel}${legal.check ? "" : ` ${chips(legal.call)}`}<small>C</small></button>
        ${bounds ? `<div class="pk-raise">
          <button type="button" class="pk-preset" data-poker-preset="0.5">½ pot</button>
          <button type="button" class="pk-preset pk-hide-sm" data-poker-preset="0.75">¾ pot</button>
          <button type="button" class="pk-preset" data-poker-preset="1">Pot</button>
          <input type="range" data-poker-raise-range min="${bounds.min}" max="${bounds.max}" step="${view.small}" value="${state.raiseTo}" aria-label="${raiseWord} amount" />
          <input type="number" data-poker-raise-input min="${bounds.min}" max="${bounds.max}" step="${view.small}" value="${state.raiseTo}" aria-label="${raiseWord} amount" />
        </div>
        <button type="button" class="pk-act raise" data-poker-act="raise">${raiseWord} <span data-poker-raise-label>${chips(state.raiseTo)}</span><small>R</small></button>` : ""}
        <button type="button" class="pk-act allin" data-poker-act="allin">All in ${chips(legal.allIn)}<small>A</small></button>
      </div>`;
      return;
    }
    if (seat.away) {
      root.innerHTML = `<div class="pk-actions-row"><span class="pk-note flex-1">You're sitting out. You won't be dealt in.</span><button type="button" class="pk-act call" style="flex:0 1 180px" data-poker-back-in>I'm back</button></div>`;
      return;
    }
    const inHand = ["active", "allin"].includes(seat.status) && ["preflop", "flop", "turn", "river"].includes(view.phase);
    if (seat.stack <= 0 && !inHand && !seat.rebuy) {
      root.innerHTML = `<div class="pk-actions-row"><span class="pk-note flex-1">You're out of chips.</span><button type="button" class="pk-act raise" style="flex:0 1 180px" data-poker-rebuy-open>Add chips</button></div>`;
      return;
    }
    if (inHand && seat.status === "active") {
      const pre = (value, text) => `<button type="button" class="pk-pre" aria-pressed="${state.preAction === value}" data-poker-pre="${value}">${text}</button>`;
      root.innerHTML = `<div class="pk-actions-row">${myHand}<span class="pk-note flex-1">Waiting for ${escapeHtml(label(view.seats[view.toAct]) || "the dealer")}…</span>${pre("checkfold", "Check / fold")}${pre("callany", "Call any")}</div>`;
      return;
    }
    const note = view.phase === "showdown" ? "Hand over. The next hand is being dealt in a moment." : seat.status === "allin" ? "You're all in." : seat.status === "folded" ? "You folded this hand." : view.phase === "waiting" ? "Waiting for the next hand." : "You'll be dealt in next hand.";
    root.innerHTML = `<div class="pk-actions-row">${myHand}<span class="pk-note flex-1">${note}</span></div>`;
  }

  function timeLabel(at) { return new Date(Number(at || 0)).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" }); }
  function renderSide() {
    const view = state.table, feed = $(`[data-poker-feed]`);
    $$(`[data-poker-tab]`).forEach((tab) => tab.setAttribute("aria-selected", String(tab.dataset.pokerTab === state.side)));
    $(`[data-poker-chat-form]`).classList.toggle("hidden", state.side !== "chat");
    if (state.side === "chat") {
      feed.innerHTML = view.chat.length ? view.chat.map((line) => `<div class="line"><time>${timeLabel(line.at)}</time><span class="who">${escapeHtml(line.name)}</span> ${escapeHtml(line.text)}</div>`).join("") : `<p class="pk-note" style="padding:8px 0">No messages yet. Say hello to the table.</p>`;
      if (state.sideOpen) state.chatSeen = Math.max(state.chatSeen, ...view.chat.map((line) => line.at), 0);
    } else if (state.side === "log") {
      feed.innerHTML = view.log.map((line) => `<div class="line ${line.seat < 0 ? "dealer" : ""}"><time>${timeLabel(line.at)}</time>${escapeHtml(line.text)}</div>`).join("") || `<p class="pk-note">The hand log fills in as the game is played.</p>`;
    } else {
      const last = view.lastHand;
      feed.innerHTML = last ? `<div class="line"><b>Hand #${last.number}</b></div>
        <div class="line">Board <span class="pk-mini-cards">${last.board.map((card) => cardHtml(card)).join("")}</span></div>
        ${last.winners.map((winner) => `<div class="line"><span class="who">${escapeHtml(BOT_LABELS[winner.name] || winner.name)}</span> won ${chips(winner.amount)}${winner.handName ? ` with ${escapeHtml(winner.handName)}` : ""} ${winner.cards?.length ? `<span class="pk-mini-cards">${winner.cards.map((card) => cardHtml(card)).join("")}</span>` : ""}</div>`).join("")}
        ${last.shown.filter((item) => !last.winners.some((winner) => winner.seat === item.seat)).map((item) => `<div class="line">${escapeHtml(BOT_LABELS[item.name] || item.name)} showed <span class="pk-mini-cards">${item.cards.map((card) => cardHtml(card)).join("")}</span> ${escapeHtml(item.handName)}</div>`).join("")}
        <div class="line"><b>Provably fair</b><p class="pk-note">Before the deal the table sealed each of the 52 shuffled positions with its own secret salt and published one SHA-256 over all of them. After the hand it reveals the salts of the cards that were shown, never folded cards, so you can check each one sat where the deck said.</p>
        <div class="pk-hash">Commitment ${escapeHtml(last.commitment)}</div>${(last.reveal?.cards || []).map((item) => `<div class="pk-hash">#${item.index} ${escapeHtml(item.card)} · salt ${escapeHtml(item.salt)}</div>`).join("")}
        <button type="button" class="pk-tb" style="margin-top:6px" data-poker-verify>Check it now</button> <span data-poker-verify-result class="pk-note"></span></div>
        <div class="line"><b>Recent hands</b>${view.history.slice().reverse().map((item) => `<div>#${item.number} ${escapeHtml(item.winners.map((name) => BOT_LABELS[name] || name).join(", "))} · ${chips(item.amount)}${item.handName ? ` · ${escapeHtml(item.handName)}` : ""}</div>`).join("")}</div>`
        : `<p class="pk-note">No hand has finished at this table yet.${view.commitment ? ` This hand's commitment: <span class="pk-hash">${escapeHtml(view.commitment)}</span>` : ""}</p>`;
    }
    feed.scrollTop = state.side === "hand" ? 0 : feed.scrollHeight;
    const unread = view.chat.filter((line) => line.at > state.chatSeen).length;
    $(`[data-poker-unread]`).innerHTML = unread && (!state.sideOpen || state.side !== "chat") ? `<span class="pk-dot" aria-label="${unread} new"></span>` : "";
  }
  function renderMenu() {
    const view = state.table, seat = mySeat(), menu = $(`[data-poker-menu]`);
    const items = [];
    if (seat) {
      items.push(seat.away ? ["back-in", "I'm back", "play"] : ["away", "Sit out next hands", "coffee"]);
      items.push(["rebuy", "Add chips", "coins"]);
    }
    items.push(["voice", state.voice ? "Turn dealer voice off" : "Turn dealer voice on", "mic"]);
    items.push(["fair", "Provably fair shuffle", "shield-check"]);
    if (view.ownerIsMe) {
      items.push(["bot-add", "Add a bot", "bot"]);
      if (view.seats.some((item) => !item.empty && item.bot)) items.push(["bot-remove", "Remove a bot", "user-minus"]);
    }
    if (seat) items.push(["leave", "Leave table", "log-out", "danger"]);
    if (view.ownerIsMe) items.push(["close", "Close table", "x-circle", "danger"]);
    menu.innerHTML = items.map(([id, text, icon, cls]) => `<button type="button" role="menuitem" data-poker-menu-item="${id}" class="${cls || ""}"><i data-lucide="${icon}"></i>${escapeHtml(text)}</button>`).join("");
    menu.classList.toggle("hidden", !state.menuOpen);
    $(`[data-poker-menu-button]`).setAttribute("aria-expanded", String(state.menuOpen));
    window.lucide?.createIcons?.();
  }
  function renderRoom(fresh = []) {
    const view = state.table;
    $(`[data-poker-title]`).textContent = view.name;
    $(`[data-poker-blinds]`).textContent = `Blinds ${chips(view.small)} / ${chips(view.big)}${view.handNumber ? ` · hand #${view.handNumber}` : ""}${view.private ? " · private" : ""}`;
    $(`[data-poker-code]`).textContent = view.code;
    $(`[data-poker-sound]`).setAttribute("aria-pressed", String(!state.muted));
    $(`[data-poker-sound]`).innerHTML = `<i data-lucide="${state.muted ? "volume-x" : "volume-2"}"></i>`;
    $(`[data-poker-side]`).classList.toggle("closed", !state.sideOpen);
    renderTable(view, fresh);
    renderActions();
    renderSide();
    renderMenu();
    startClock();
  }

  function startClock() {
    cancelAnimationFrame(state.raf);
    const view = state.table;
    if (!view || view.toAct < 0 || !view.actDeadline) return;
    const frame = () => {
      const ring = $(`[data-poker-ring]`); if (!ring || !state.table) return;
      const total = Math.max(1, view.actDeadline - view.actStartedAt), left = Math.max(0, view.actDeadline - serverNow());
      ring.setAttribute("stroke-dashoffset", String(289 * (1 - left / total)));
      ring.setAttribute("stroke", left < 5000 ? "#ef5350" : left < 10000 ? "#ffb300" : "#66bb6a");
      const seconds = Math.ceil(left / 1000);
      if (view.toAct === view.mySeat && seconds <= 5 && seconds > 0 && seconds !== state.lastTick) { state.lastTick = seconds; play(["clock"]); }
      if (left > 0) state.raf = requestAnimationFrame(frame);
    };
    state.raf = requestAnimationFrame(frame);
  }
  function scheduleWake(view) {
    clearTimeout(state.wakeTimer);
    if (!view?.wakeAt) return;
    const delay = Math.max(0, view.wakeAt - serverNow()) + 150;
    state.wakeTimer = setTimeout(() => { void send("/tick"); }, Math.min(delay, 2147483000));
  }

  function applyTable(view, { first = false } = {}) {
    if (!view) return;
    if (state.table && state.table.id === view.id && view.version < state.table.version) return;
    state.offset = Number(view.now || Date.now()) - Date.now();
    const maxSeq = Math.max(0, ...view.events.map((event) => event.seq));
    const fresh = first || state.table?.id !== view.id ? [] : view.events.filter((event) => event.seq > state.lastSeq).sort((a, b) => a.seq - b.seq);
    state.lastSeq = Math.max(first ? maxSeq : state.lastSeq, maxSeq);
    if (first) state.chatSeen = Math.max(0, ...view.chat.map((line) => line.at));
    if (state.preHand !== view.handNumber) { state.preAction = ""; state.preHand = view.handNumber; state.raiseTo = 0; }
    state.table = view;
    renderRoom(fresh);
    state.prev = JSON.parse(JSON.stringify(view));
    const turnKey = `${view.id}:${view.handNumber}:${view.toAct}:${view.phase}`;
    const sounds = soundsFor(fresh, view);
    if (view.legal && state.turnFor !== turnKey) {
      state.turnFor = turnKey; state.lastTick = -1;
      if (!first) sounds.push("turn");
      navigator.vibrate?.(30);
      if (state.preAction) {
        const action = state.preAction === "callany" ? (view.legal.check ? "check" : "call") : (view.legal.check ? "check" : "fold");
        state.preAction = ""; void act(action);
      }
    }
    if (sounds.length) play(sounds);
    if (!first && fresh.some((event) => ["flop", "turn", "river", "win", "shuffle"].includes(event.kind))) speak(view.dealerLine);
    scheduleWake(view);
  }

  async function loadTable(first = false) {
    if (state.loading) { state.again = true; return; }
    state.loading = true;
    try {
      const payload = await request(`/tables/${state.tableId}`);
      applyTable(payload.table, { first });
    } catch (error) {
      if (error.status === 404) { message("error", "That table is closed."); showLobby(); }
      else if (first) message("error", `Could not open the table: ${friendly(error)}`);
    } finally {
      state.loading = false;
      if (state.again) { state.again = false; void loadTable(); }
    }
  }
  async function send(path, body = {}) {
    if (!state.tableId) return null;
    try {
      const payload = await request(`/tables/${state.tableId}${path}`, "POST", body);
      applyTable(payload.table);
      return payload;
    } catch (error) {
      if (path !== "/tick") message("error", friendly(error));
      if (error.status === 404 && path === "/tick") showLobby();
      else if (error.status === 409) void loadTable();
      return null;
    }
  }
  async function act(action, amount = 0) {
    if (state.busy) return;
    state.busy = true;
    try { await send("/act", { action, amount, hand: state.table?.handNumber }); }
    finally { state.busy = false; }
  }

  function renderLobby() {
    const lobby = state.lobby; if (!lobby) return;
    const wallet = lobby.wallet;
    $(`[data-poker-chips]`).textContent = chips(wallet.chips);
    const claim = $(`[data-poker-claim]`);
    claim.disabled = !wallet.canClaim;
    claim.textContent = wallet.canClaim ? `Collect ${chips(wallet.claimAmount)} free chips` : "Free chips collected";
    clearInterval(state.claimTimer);
    const note = $(`[data-poker-claim-note]`);
    const updateNote = () => {
      if (wallet.canClaim || !wallet.nextClaimAt) { note.textContent = wallet.onTables ? `${chips(wallet.onTables)} more on tables` : ""; return; }
      const left = Math.max(0, wallet.nextClaimAt - serverNow());
      const hours = Math.floor(left / 3600000), minutes = Math.floor((left % 3600000) / 60000);
      note.textContent = `Next free chips in ${hours ? `${hours} h ` : ""}${minutes} min${wallet.onTables ? ` · ${chips(wallet.onTables)} on tables` : ""}`;
      if (left <= 0) { clearInterval(state.claimTimer); wallet.canClaim = true; renderLobby(); }
    };
    updateNote();
    if (!wallet.canClaim) state.claimTimer = setInterval(updateNote, 30000);
    const rate = wallet.handsPlayed ? Math.round(100 * wallet.handsWon / wallet.handsPlayed) : 0;
    $(`[data-poker-stats]`).innerHTML = [
      ["Hands played", chips(wallet.handsPlayed)], ["Hands won", chips(wallet.handsWon)], ["Win rate", `${rate}%`],
      ["Biggest pot", chips(wallet.biggestPot)], ["Best hand", escapeHtml(wallet.bestHand || "—")],
    ].map(([name, value]) => `<div class="pk-stat"><span>${name}</span><b>${value}</b></div>`).join("");
    $(`[data-poker-hero-cards]`).innerHTML = ["As", "Kh", "Qd"].map((card) => cardHtml(card)).join("");
    const tables = lobby.tables || [];
    $(`[data-poker-tables]`).innerHTML = tables.length ? tables.map((item) => `<article class="rounded-lg border border-border bg-card pk-table-card">
        <div class="pk-mini-felt">${item.players}/${item.seats}</div>
        <div class="min-w-0">
          <div class="flex flex-wrap items-center gap-2"><h3 class="truncate text-sm font-semibold text-foreground">${escapeHtml(item.name)}</h3>${item.mine ? `<span class="pk-pill mine">Your seat</span>` : ""}${item.invited ? `<span class="pk-pill mine">Invited</span>` : ""}${item.private ? `<span class="pk-pill">Private</span>` : ""}</div>
          <p class="text-xs text-muted-foreground">Blinds ${chips(item.small)} / ${chips(item.big)} · buy-in ${chips(item.minBuyIn)}–${chips(item.maxBuyIn)} · ${item.humans} ${item.humans === 1 ? "player" : "players"}${item.bots ? ` + ${item.bots} ${item.bots === 1 ? "bot" : "bots"}` : ""} · ${item.turnSeconds} s clock · hosted by ${escapeHtml(item.owner)}</p>
        </div>
        <button type="button" class="fm-btn fm-btn-sm" data-poker-open="${escapeHtml(item.id)}">${item.mine ? "Return" : item.players >= item.seats ? "Watch" : "Open"}</button>
      </article>`).join("") : `<div class="fm-blankslate rounded-lg border border-border bg-card"><i data-lucide="spade"></i><p class="fm-blankslate-title">No open tables yet</p><p class="fm-blankslate-desc">Start a table and invite friends, or practise against bots.</p></div>`;
    window.lucide?.createIcons?.();
  }
  async function loadLobby() {
    try {
      const payload = await request("");
      state.offset = Number(payload.now || Date.now()) - Date.now();
      state.lobby = payload; message("error", ""); renderLobby();
    } catch (error) {
      message("error", `Poker is unavailable: ${friendly(error)}`);
    }
  }

  function showLobby() {
    clearTimeout(state.wakeTimer); cancelAnimationFrame(state.raf);
    state.tableId = ""; state.table = null; state.prev = null; state.menuOpen = false;
    $(`[data-poker-room]`).classList.add("hidden"); $(`[data-poker-lobby]`).classList.remove("hidden");
    const url = new URL(location.href); url.searchParams.delete("table"); history.replaceState(history.state, "", url);
    void loadLobby();
  }
  function openTable(id) {
    if (!/^[0-9a-f]{32}$/.test(String(id || ""))) return;
    state.tableId = id; state.table = null; state.prev = null; state.lastSeq = 0; state.turnFor = "";
    state.sideOpen = window.matchMedia("(min-width: 901px)").matches;
    $(`[data-poker-lobby]`).classList.add("hidden"); $(`[data-poker-room]`).classList.remove("hidden");
    message("error", ""); message("notice", "");
    const url = new URL(location.href); url.searchParams.set("table", id); history.replaceState(history.state, "", url);
    audioContext();
    void loadTable(true);
  }

  function showDialog(name) {
    const dialog = $(`[data-poker-dialog="${name}"]`);
    dialog.querySelectorAll("[data-poker-dialog-error],[data-poker-dialog-notice]").forEach((slot) => slot.classList.add("hidden"));
    if (!dialog.open) dialog.showModal();
    return dialog;
  }
  function dialogNotice(dialog, text) { const slot = dialog.querySelector("[data-poker-dialog-notice]"); if (slot) { slot.querySelector("p").textContent = text; slot.classList.toggle("hidden", !text); } }
  function wallet() { return Number(state.table?.wallet ?? state.lobby?.wallet?.chips ?? 0); }
  function stakesList() { return state.lobby?.stakes || []; }
  function setRange(input, min, max, value, step) { input.min = String(min); input.max = String(Math.max(min, max)); input.step = String(step); input.value = String(Math.max(min, Math.min(max, value))); }
  function openCreate() {
    const dialog = showDialog("create"), form = dialog.querySelector("form");
    const select = form.elements.stakes;
    if (!select.options.length) select.innerHTML = stakesList().map((item) => `<option value="${item.id}">${escapeHtml(item.label)} · blinds ${chips(item.small)} / ${chips(item.big)} · buy-in ${chips(item.minBuyIn)}–${chips(item.maxBuyIn)}</option>`).join("");
    syncCreate();
  }
  function syncCreate() {
    const form = $(`[data-poker-create-form]`), item = stakesList().find((entry) => entry.id === form.elements.stakes.value) || stakesList()[0]; if (!item) return;
    const top = Math.min(item.maxBuyIn, wallet());
    setRange(form.elements.buyIn, item.minBuyIn, top, Math.min(top, item.maxBuyIn), item.small);
    const seats = Number(form.elements.seats.value);
    [...form.elements.bots.options].forEach((option) => { option.disabled = Number(option.value) > seats - 1; });
    if (Number(form.elements.bots.value) > seats - 1) form.elements.bots.value = String(seats - 1);
    $(`[data-poker-create-buyin]`).textContent = top >= item.minBuyIn ? chips(form.elements.buyIn.value) : `You need ${chips(item.minBuyIn)} chips`;
  }
  function openSit(seat) {
    const view = state.table, dialog = showDialog("sit"), form = dialog.querySelector("form");
    const top = Math.min(view.maxBuyIn, wallet());
    form.elements.seat.value = String(seat);
    setRange(form.elements.buyIn, view.minBuyIn, top, top, view.small);
    $(`[data-poker-sit-range]`).textContent = `Buy in for ${chips(view.minBuyIn)} to ${chips(view.maxBuyIn)} chips. You have ${chips(wallet())}.`;
    $(`[data-poker-sit-buyin]`).textContent = top >= view.minBuyIn ? chips(form.elements.buyIn.value) : "Not enough chips";
  }
  function openRebuy() {
    const view = state.table, seat = mySeat(); if (!seat) return;
    const dialog = showDialog("rebuy"), form = dialog.querySelector("form");
    const room = Math.min(view.maxBuyIn - seat.stack - (seat.rebuy || 0), wallet());
    setRange(form.elements.amount, Math.min(view.small, Math.max(0, room)), Math.max(0, room), room, view.small);
    $(`[data-poker-rebuy-range]`).textContent = room > 0 ? `Top up to the table's maximum of ${chips(view.maxBuyIn)}. You have ${chips(wallet())} in your wallet.` : "You're already at the table's maximum, or your wallet is empty.";
    $(`[data-poker-rebuy-amount]`).textContent = chips(form.elements.amount.value);
  }
  async function quickPlay() {
    const item = stakesList()[0], chipsLeft = Number(state.lobby?.wallet?.chips || 0);
    if (!item || chipsLeft < item.minBuyIn) { message("error", "You need more chips. Collect your free chips first."); return; }
    try {
      const payload = await request("/tables", "POST", { name: "Practice table", stakes: item.id, seats: 6, bots: 5, turnSeconds: 30, buyIn: Math.min(item.maxBuyIn, chipsLeft) });
      openTable(payload.table.id);
    } catch (error) { message("error", `Could not start a practice table: ${friendly(error)}`); }
  }
  async function verifyLast() {
    const last = state.table?.lastHand, out = $(`[data-poker-verify-result]`); if (!last || !out) return;
    const sha = async (text) => [...new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text)))].map((byte) => byte.toString(16).padStart(2, "0")).join("");
    try {
      const hashes = last.reveal?.hashes || [];
      let good = hashes.length === 52 && await sha(hashes.join(",")) === last.commitment;
      for (const item of last.reveal?.cards || []) good = good && await sha(`${item.salt}:${item.index}:${item.card}`) === hashes[item.index];
      out.textContent = good ? `Matches: the ${last.reveal.cards.length} cards shown were in the deck committed to before the deal.` : "Does not match.";
    } catch (_) { out.textContent = "This browser cannot compute SHA-256 here."; }
  }

  function onClick(event) {
    const target = event.target.closest("button, [data-poker-tab]"); if (!target) {
      if (state.menuOpen && !event.target.closest("[data-poker-menu]")) { state.menuOpen = false; renderMenu(); }
      return;
    }
    const data = target.dataset;
    if (data.pokerOpen) { openTable(data.pokerOpen); return; }
    if ("pokerNew" in data) { openCreate(); return; }
    if ("pokerQuick" in data) { void quickPlay(); return; }
    if ("pokerClaim" in data) {
      target.disabled = true;
      request("/claim", "POST", {}).then((payload) => { state.lobby.wallet = payload.wallet; play(["chip", "chip", "chip"]); message("notice", `Collected ${chips(payload.wallet.claimAmount)} free chips.`); renderLobby(); })
        .catch((error) => { message("error", friendly(error)); void loadLobby(); });
      return;
    }
    if ("pokerDialogClose" in data) { target.closest("dialog").close(); return; }
    if ("pokerBack" in data) { showLobby(); return; }
    if ("pokerSitSeat" in data) { openSit(Number(data.pokerSitSeat)); return; }
    if ("pokerAct" in data) {
      const action = data.pokerAct; audioContext();
      if (action === "raise") void act("raise", state.raiseTo); else void act(action);
      return;
    }
    if ("pokerPreset" in data) { state.raiseTo = presetTarget(Number(data.pokerPreset)); syncRaise(); return; }
    if ("pokerPre" in data) { state.preAction = state.preAction === data.pokerPre ? "" : data.pokerPre; renderActions(); return; }
    if ("pokerBackIn" in data) { void send("/away", { away: false }); return; }
    if ("pokerRebuyOpen" in data) { openRebuy(); return; }
    if ("pokerVerify" in data) { void verifyLast(); return; }
    if ("pokerSound" in data) { state.muted = !state.muted; store.set("forkmesh.poker.muted", state.muted); if (state.muted) window.speechSynthesis?.cancel(); else play(["chip"]); renderRoom(); return; }
    if ("pokerSideToggle" in data) { state.sideOpen = !state.sideOpen; renderRoom(); return; }
    if ("pokerTab" in data) { state.side = data.pokerTab; renderSide(); return; }
    if ("pokerInvite" in data) {
      const dialog = showDialog("invite"); $(`[data-poker-invite-code]`).textContent = state.table.code; dialog.querySelector("input").focus(); return;
    }
    if ("pokerCopyLink" in data) {
      const url = new URL("/dashboard/poker", location.origin); url.searchParams.set("table", state.table.id);
      navigator.clipboard?.writeText(url.toString()).then(() => dialogNotice(target.closest("dialog"), "Link copied."), () => dialogNotice(target.closest("dialog"), url.toString()));
      return;
    }
    if ("pokerMenuButton" in data) { state.menuOpen = !state.menuOpen; renderMenu(); return; }
    if (data.pokerMenuItem) { state.menuOpen = false; renderMenu(); void menuAction(data.pokerMenuItem); }
  }
  async function menuAction(id) {
    const view = state.table;
    if (id === "away") await send("/away", { away: true });
    else if (id === "back-in") await send("/away", { away: false });
    else if (id === "rebuy") openRebuy();
    else if (id === "voice") { state.voice = !state.voice; store.set("forkmesh.poker.voice", state.voice); if (state.voice) speak(view.dealerLine); renderMenu(); }
    else if (id === "fair") { state.side = "hand"; state.sideOpen = true; renderRoom(); }
    else if (id === "bot-add") await send("/bots", { add: true });
    else if (id === "bot-remove") { const bot = [...view.seats].reverse().find((seat) => !seat.empty && seat.bot); if (bot) await send("/bots", { remove: bot.index }); }
    else if (id === "leave") { if (await send("/leave")) toast(view.phase === "waiting" || view.phase === "showdown" ? "Your chips are back in your wallet." : "You'll leave when this hand ends.", "notice"); }
    else if (id === "close" && window.confirm("Close this table? Every player's chips go back to their wallet.")) {
      try { await request(`/tables/${view.id}`, "DELETE"); showLobby(); } catch (error) { message("error", friendly(error)); }
    }
  }
  function syncRaise() {
    const range = $(`[data-poker-raise-range]`), input = $(`[data-poker-raise-input]`), labelNode = $(`[data-poker-raise-label]`);
    if (range) range.value = String(state.raiseTo); if (input) input.value = String(state.raiseTo); if (labelNode) labelNode.textContent = chips(state.raiseTo);
  }
  function onInput(event) {
    const target = event.target;
    if (target.matches("[data-poker-raise-range], [data-poker-raise-input]")) {
      const bounds = raiseBounds(); if (!bounds) return;
      const value = Number(target.value || 0);
      state.raiseTo = target.matches("[data-poker-raise-input]") && event.type === "input" ? value : Math.max(bounds.min, Math.min(bounds.max, value));
      if (event.type === "change") state.raiseTo = Math.max(bounds.min, Math.min(bounds.max, state.raiseTo));
      syncRaise();
      return;
    }
    if (target.closest("[data-poker-create-form]")) { syncCreate(); return; }
    if (target.closest("[data-poker-sit-form]") && target.name === "buyIn") { $(`[data-poker-sit-buyin]`).textContent = chips(target.value); return; }
    if (target.closest("[data-poker-rebuy-form]") && target.name === "amount") { $(`[data-poker-rebuy-amount]`).textContent = chips(target.value); }
  }
  async function onSubmit(event) {
    const form = event.target;
    if (form.matches("[data-poker-code-form]")) {
      event.preventDefault();
      const code = $(`[data-poker-code-input]`).value.trim().toUpperCase();
      if (!/^[A-Z]{6}$/.test(code)) { message("error", "A table code is six letters."); return; }
      try { const payload = await request(`/codes/${code}`); openTable(payload.tableId); } catch (error) { message("error", error.status === 404 ? "No open table has that code." : friendly(error)); }
      return;
    }
    if (form.matches("[data-poker-chat-form]")) {
      event.preventDefault();
      const input = $(`[data-poker-chat-input]`), text = input.value.trim(); if (!text) return;
      input.value = ""; await send("/chat", { text });
      return;
    }
    if (!form.closest("dialog.pk-dialog")) return;
    event.preventDefault();
    if (event.submitter?.value === "cancel") { form.closest("dialog").close(); return; }
    const values = Object.fromEntries(new FormData(form).entries());
    if (form.matches("[data-poker-create-form]")) {
      try {
        const payload = await request("/tables", "POST", { name: values.name, stakes: values.stakes, seats: Number(values.seats), turnSeconds: Number(values.turnSeconds), bots: Number(values.bots), private: values.private === "on", buyIn: Number(values.buyIn) });
        form.closest("dialog").close(); form.reset(); openTable(payload.table.id);
      } catch (error) { message("error", friendly(error)); }
    } else if (form.matches("[data-poker-sit-form]")) {
      const payload = await send("/sit", { seat: Number(values.seat), buyIn: Number(values.buyIn) });
      if (payload) { form.closest("dialog").close(); play(["chip", "chip"]); }
    } else if (form.matches("[data-poker-rebuy-form]")) {
      const payload = await send("/rebuy", { amount: Number(values.amount) });
      if (payload) { form.closest("dialog").close(); play(["chip"]); }
    } else if (form.matches("[data-poker-invite-form]")) {
      const names = String(values.names || "").split(/[\s,]+/).map((name) => name.trim()).filter(Boolean);
      if (!names.length) { message("error", "Type at least one username."); return; }
      const payload = await send("/invite", { names });
      if (payload) {
        const dialog = form.closest("dialog");
        dialogNotice(dialog, [payload.invited.length ? `Invited ${payload.invited.join(", ")}.` : "", payload.skipped.length ? `No ForkMesh user named ${payload.skipped.join(", ")}.` : ""].filter(Boolean).join(" "));
        form.elements.names.value = "";
      }
    }
  }
  function onKey(event) {
    if (!state.table || $(`[data-poker-room]`).classList.contains("hidden")) return;
    if (event.target.closest("input, textarea, select, dialog") || event.metaKey || event.ctrlKey || event.altKey) return;
    const legal = state.table.legal; if (!legal) return;
    const key = event.key.toLowerCase();
    if (key === "f") { event.preventDefault(); void act("fold"); }
    else if (key === "c") { event.preventDefault(); void act(legal.check ? "check" : "call"); }
    else if (key === "a") { event.preventDefault(); void act("allin"); }
    else if (key === "r" && legal.raise) { event.preventDefault(); void act("raise", state.raiseTo); }
  }

  const root = $(`[data-view="poker"]`);
  root.addEventListener("click", onClick);
  root.addEventListener("input", onInput);
  root.addEventListener("change", onInput);
  root.addEventListener("submit", (event) => { void onSubmit(event); });
  document.addEventListener("keydown", onKey);
  window.addEventListener("forkmesh:package-event", (event) => {
    const id = String(event.detail?.id || "");
    if (id && id !== "poker") return;
    if (state.tableId) void loadTable(); else void loadLobby();
  });
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState !== "visible") return;
    if (state.tableId) void loadTable(); else void loadLobby();
  });
  if (window.ResizeObserver) new ResizeObserver(() => { if (state.table) renderTable(state.table, []); }).observe($(`[data-poker-stage]`));
  const start = new URLSearchParams(location.search).get("table");
  void loadLobby();
  if (start) openTable(start);
});
