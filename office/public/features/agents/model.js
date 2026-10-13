export const POLL_MS = 30000;
export const MAX_BOTS = 24;
export const KEEP_FINISHED_MS = 8 * 3600e3;
export const RELAY_MAX = 24;

export const STATES = {
  queued: { label: 'Queued', color: '#9aa1ad', live: true },
  running: { label: 'Working', color: '#3b45e0', live: true, pulse: true },
  waiting: { label: 'Waiting', color: '#f59e0b', live: true },
  success: { label: 'Done', color: '#22c55e', live: false },
  failed: { label: 'Failed', color: '#ef4444', live: false },
  stopped: { label: 'Stopped', color: '#3a3f4b', live: false },
};

const ALIASES = {
  queued: 'queued', pending: 'queued', open: 'queued', todo: 'queued', claimed: 'queued',
  running: 'running', active: 'running', working: 'running', started: 'running', in_progress: 'running',
  waiting: 'waiting', security_pending: 'waiting', needs_input: 'waiting', paused: 'waiting', blocked: 'waiting', review: 'waiting',
  success: 'success', done: 'success', completed: 'success', complete: 'success', merged: 'success', succeeded: 'success',
  failed: 'failed', error: 'failed', rejected: 'failed', crashed: 'failed',
  stopped: 'stopped', cancelled: 'stopped', canceled: 'stopped', killed: 'stopped', aborted: 'stopped',
};

export const MODELS = {
  opus: { label: 'Opus', shell: '#b8892f', glow: '#b07bff' },
  sonnet: { label: 'Sonnet', shell: '#d09a3c', glow: '#ff70bd' },
  haiku: { label: 'Haiku', shell: '#c3ced1', glow: '#59e5e8' },
  fable: { label: 'Fable', shell: '#a08434', glow: '#4fd9c2' },
  sol: { label: 'Sol', shell: '#e9b23a', glow: '#ffd25e' },
  luna: { label: 'Luna', shell: '#7868c8', glow: '#9678ff' },
  terra: { label: 'Terra', shell: '#68b875', glow: '#79e77f' },
};

export const EMBLEMS = {
  opus: ['11111', '10001', '10101', '10001', '11111'],
  sonnet: ['00110', '00101', '00100', '01100', '11100'],
  haiku: ['00000', '01110', '01110', '01110', '00000'],
  fable: ['11011', '10101', '10101', '10101', '11011'],
  sol: ['10101', '01110', '11111', '01110', '10101'],
  luna: ['01110', '11000', '10000', '11000', '01110'],
  terra: ['00100', '01110', '11111', '01110', '00100'],
};

const ID_RE = /^[A-Za-z0-9._:-]{1,64}$/;
const text = (v, n) => String(v ?? '').replace(/\s+/g, ' ').trim().slice(0, n);
const num = (v) => {
  const n = Number(v);
  return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0;
};

export function normState(agentStatus, taskStatus) {
  const a = ALIASES[String(agentStatus || '').trim().toLowerCase()];
  if (a) return a;
  const t = String(taskStatus || '').trim().toLowerCase();
  if (t === 'done' || t === 'completed') return 'success';
  if (t === 'active' || t === 'running') return 'running';
  return 'queued';
}

export function modelKey(model, provider) {
  const m = String(model || '').toLowerCase();
  for (const k of Object.keys(MODELS)) if (m.includes(k)) return k;
  const p = String(provider || '').toLowerCase();
  if (p.startsWith('claude') || p === 'anthropic') return 'sonnet';
  if (p === 'codex') return 'sol';
  if (p === 'openai') return 'luna';
  return 'terra';
}

const PROVIDERS = { claude: 'Claude', codex: 'Codex', openai: 'OpenAI', gemini: 'Gemini', cursor: 'Cursor' };
export function providerLabel(p) {
  const k = String(p || '').toLowerCase();
  if (PROVIDERS[k]) return PROVIDERS[k];
  for (const [key, label] of Object.entries(PROVIDERS)) if (k.startsWith(key)) return label;
  return k ? k[0].toUpperCase() + k.slice(1) : 'Agent';
}

export function effortLabel(s) {
  const k = text(s, 16).toLowerCase();
  return k ? k[0].toUpperCase() + k.slice(1) : '';
}

export function botFromTask(task) {
  if (!task || typeof task !== 'object' || num(task.deletedAt)) return null;
  const ag = task.agent && typeof task.agent === 'object' ? task.agent : {};
  const id = text(ag.sessionId || task.agentSessionId, 64);
  if (!ID_RE.test(id)) return null;
  const provider = text(ag.provider, 40).toLowerCase();
  const model = text(ag.model, 64);
  const mk = modelKey(model, provider);
  const startedBy = text(ag.startedBy, 40);
  const state = normState(ag.status, task.status);
  const createdAt = num(task.createdAt);
  return {
    id,
    taskId: text(task.id, 64),
    title: text(task.title, 120) || 'Untitled task',
    prose: text(ag.prose, 200),
    provider,
    model,
    modelKey: mk,
    modelLabel: MODELS[mk].label,
    effort: effortLabel(ag.strength),
    state,
    owner: text(ag.runBy || task.createdBy || task.ownership?.createdBy, 64).toLowerCase(),
    label: startedBy || `${providerLabel(provider)} ${id.slice(-4)}`,
    repository: text(task.repository, 201),
    startedAt: num(task.startedAt) || createdAt,
    createdAt,
    at: num(task.updatedAt) || num(task.completedAt) || createdAt,
  };
}

export function botFromRun(run) {
  if (!run || typeof run !== 'object') return null;
  const sid = text(run.sessionId, 40);
  const owner = text(run.owner, 64).toLowerCase();
  const id = `${owner}:${sid}`;
  if (!owner || !sid || !ID_RE.test(id)) return null;
  const provider = text(run.provider, 40).toLowerCase();
  const model = text(run.model, 64);
  const mk = modelKey(model, provider);
  const startedBy = text(run.startedBy, 40);
  const startedAt = num(run.startedAt);
  const at = num(run.updatedAt) || startedAt;
  return {
    id,
    taskId: '',
    title: text(run.title, 120) || `${providerLabel(provider)} agent run`,
    prose: text(run.prose, 200),
    provider,
    model,
    modelKey: mk,
    modelLabel: MODELS[mk].label,
    effort: effortLabel(run.strength),
    state: normState(run.status),
    owner,
    label: startedBy || `${providerLabel(provider)} ${sid.slice(-4)}`,
    repository: '',
    startedAt: startedAt || at,
    createdAt: startedAt || at,
    at,
  };
}

export function botsFromPayload(payload, now = Date.now()) {
  const tasks = Array.isArray(payload) ? payload : Array.isArray(payload?.tasks) ? payload.tasks : [];
  const runs = Array.isArray(payload?.agentRuns) ? payload.agentRuns : [];
  const seen = new Map();
  for (const b of [...tasks.map(botFromTask), ...runs.map(botFromRun)]) {
    if (!b) continue;
    if (!STATES[b.state].live && now - b.at > KEEP_FINISHED_MS) continue;
    const prev = seen.get(b.id);
    if (!prev || b.at > prev.at) seen.set(b.id, b);
  }
  return [...seen.values()]
    .sort((a, b) => (STATES[b.state].live - STATES[a.state].live) || b.createdAt - a.createdAt || (a.id < b.id ? -1 : 1))
    .slice(0, MAX_BOTS);
}

export function ownerKeys(account) {
  if (!account) return [];
  return [...new Set([account.id, account.name].map((v) => text(v, 64).toLowerCase()).filter(Boolean))];
}

export function personKeys(o) {
  const fm = o?.ex?.fm;
  if (!fm || typeof fm !== 'object') return [];
  return [fm.a, fm.n].map((v) => text(v, 64).toLowerCase()).filter(Boolean);
}

export function isMine(bot, keys) {
  return !!bot.owner && keys.includes(bot.owner);
}

export function findOwner(bot, me, others, myKeys) {
  if (!bot.owner) return null;
  if (me && (myKeys.includes(bot.owner) || personKeys(me).includes(bot.owner))) return me;
  for (const o of others) if (personKeys(o).includes(bot.owner)) return o;
  return null;
}

export function anchorFor(o) {
  const d = o?.ex?.desk;
  if (d && Number.isInteger(d.x) && Number.isInteger(d.y)) return { x: d.x, y: d.y, desk: true };
  if (!o) return null;
  return { x: o.rx ?? o.x, y: o.ry ?? o.y, desk: false };
}

const RING = [[0.95, 0.2], [-0.95, 0.2], [0.55, 0.9], [-0.55, 0.9]];
export function slotOffset(i) {
  const row = i >> 2;
  const [dx, dy] = RING[i & 3];
  return [dx * (1 + row * 0.75), dy + row * 0.6];
}

export function layout(bots, me, others, myKeys, ok) {
  const groups = new Map();
  const out = new Map();
  for (const b of bots) {
    const o = findOwner(b, me, others, myKeys);
    if (!o) continue;
    if (!groups.has(o.id)) groups.set(o.id, { o, list: [] });
    groups.get(o.id).list.push(b);
  }
  for (const { o, list } of groups.values()) {
    const a = anchorFor(o);
    const ax = a.desk ? a.x : Math.round(a.x), ay = a.desk ? a.y : Math.round(a.y);
    const spots = [];
    for (let i = 0; spots.length < list.length && i < 48; i++) {
      const [dx, dy] = slotOffset(i);
      const x = ax + 0.5 + dx, y = ay + 0.55 + dy;
      if (!ok || ok(Math.floor(x), Math.floor(y))) spots.push([x - ax, y - ay]);
    }
    for (let i = 0; spots.length < list.length; i++) {
      const [dx, dy] = slotOffset(i);
      spots.push([0.5 + dx, 0.55 + dy]);
    }
    const fx = a.x - ax, fy = a.y - ay;
    list.forEach((b, i) => {
      out.set(b.id, { x: ax + fx + spots[i][0], y: ay + fy + spots[i][1], owner: o.id, desk: a.desk });
    });
  }
  return out;
}

export function diffStates(prev, bots) {
  const changes = [];
  for (const b of bots) {
    const p = prev.get(b.id);
    if (p && p !== b.state) changes.push({ id: b.id, from: p, to: b.state });
  }
  return changes;
}

export function cleanRelay(list, now = Date.now()) {
  if (!Array.isArray(list)) return [];
  const out = [];
  for (const c of list.slice(0, RELAY_MAX)) {
    if (!c || typeof c !== 'object') continue;
    const id = String(c.id ?? '');
    const st = String(c.st ?? '');
    const at = Number(c.at);
    if (!ID_RE.test(id) || !STATES[st] || !Number.isInteger(at) || at <= 0 || at > now + 60e3) continue;
    out.push({ id, st, at });
  }
  return out;
}

export function applyRelay(bots, list) {
  const byId = new Map(bots.map((b) => [b.id, b]));
  const changed = [];
  for (const c of list) {
    const b = byId.get(c.id);
    if (!b || c.at <= b.at || c.st === b.state) continue;
    b.state = c.st;
    b.at = c.at;
    changed.push(b.id);
  }
  return changed;
}

export function relTime(ts, now = Date.now()) {
  if (!ts) return '';
  const s = Math.max(0, Math.round((now - ts) / 1000));
  if (s < 45) return 'just now';
  const m = Math.round(s / 60);
  if (m < 60) return `${m}m ago`;
  const h = Math.round(m / 60);
  if (h < 36) return `${h}h ago`;
  return `${Math.round(h / 24)}d ago`;
}

export function filterBots(bots, mode, keys) {
  return mode === 'mine' ? bots.filter((b) => isMine(b, keys)) : bots;
}

export function counts(bots) {
  const c = { live: 0, running: 0 };
  for (const b of bots) {
    if (STATES[b.state].live) c.live++;
    if (b.state === 'running') c.running++;
  }
  return c;
}

export function stationLayout(bots, stations) {
  const out = new Map();
  const order = [...bots].sort((a, b) => (a.createdAt - b.createdAt) || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  order.slice(0, stations.length).forEach((b, i) => {
    const [x, y] = stations[i];
    out.set(b.id, { x: x + 0.5, y: y + 0.62, station: i });
  });
  return out;
}

export function wrapText(value, width, measure, maxLines) {
  const words = String(value || '').replace(/\s+/g, ' ').trim().split(' ').filter(Boolean);
  const lines = [];
  let cur = '';
  for (let i = 0; i < words.length; i++) {
    let w = words[i];
    while (measure(w) > width && w.length > 1) {
      let cut = w.length - 1;
      while (cut > 1 && measure(w.slice(0, cut)) > width) cut--;
      if (cur) { lines.push(cur); cur = ''; }
      lines.push(w.slice(0, cut));
      w = w.slice(cut);
    }
    const t = cur ? cur + ' ' + w : w;
    if (measure(t) > width && cur) { lines.push(cur); cur = w; } else cur = t;
  }
  if (cur) lines.push(cur);
  if (lines.length <= maxLines) return lines;
  const kept = lines.slice(0, maxLines);
  let last = kept[maxLines - 1];
  while (last && measure(last + '…') > width) last = last.slice(0, -1);
  kept[maxLines - 1] = last + '…';
  return kept;
}

export function stepBot(list, id, delta) {
  if (!list.length) return null;
  const i = list.findIndex((b) => b.id === id);
  const n = list.length;
  return list[((i < 0 ? (delta > 0 ? -1 : 0) : i) + delta + n) % n].id;
}
