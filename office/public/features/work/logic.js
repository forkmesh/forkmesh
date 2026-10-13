export const DESK_TOPS = new Set(['M', 'D']);
export const FAST_POLL_MS = 60e3;
export const SLOW_POLL_MS = 5 * 60e3;
export const TASK_STATUSES = ['idle', 'active', 'done'];
export const STATUS_LABEL = { idle: 'To do', active: 'In progress', done: 'Done' };

export function findDesks(obj) {
  const out = [];
  for (let y = 1; y < obj.length; y++) {
    const row = obj[y];
    for (let x = 0; x < row.length; x++) {
      if (row[x] !== '^') continue;
      const top = obj[y - 1][x];
      if (DESK_TOPS.has(top)) out.push({ x, y, mx: x, my: y - 1, monitor: top === 'M' });
    }
  }
  return out;
}

export const deskKey = (x, y) => `${x},${y}`;

export function deskAt(desks, tx, ty) {
  return desks.find((d) => (d.mx === tx && d.my === ty) || (d.x === tx && d.y === ty)) || null;
}

export function ownerKey(u) {
  if (u?.acct) return 'a:' + String(u.acct).slice(0, 80);
  const n = String(u?.name || '').trim().toLowerCase().slice(0, 24);
  return n ? 'n:' + n : '';
}

export function cleanText(v, max) {
  const s = String(v ?? '')
    .replace(/[\u0000-\u001f\u007f<>]/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
  return s.slice(0, max);
}

export function cleanTask(v) {
  if (!v || typeof v !== 'object') return undefined;
  const id = String(v.id ?? '');
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(id)) return undefined;
  const title = cleanText(v.title, 160);
  if (!title) return undefined;
  return { id, title };
}

export function claimDesk(desks, key, name, x, y, valid) {
  if (!key) return { error: 'no_owner' };
  if (!valid.some((d) => d.x === x && d.y === y)) return { error: 'not_a_desk' };
  for (const [k, d] of Object.entries(desks)) if (k !== key && d.x === x && d.y === y) return { error: 'taken', by: d.name };
  const next = { ...desks, [key]: { x, y, name: cleanText(name, 24) || 'Someone' } };
  return { desks: next, prev: desks[key] || null };
}

export function releaseDesk(desks, key) {
  if (!desks[key]) return { desks, prev: null };
  const next = { ...desks };
  const prev = next[key];
  delete next[key];
  return { desks: next, prev };
}

export function deskList(desks) {
  return Object.values(desks).map((d) => (d.deco ? { x: d.x, y: d.y, name: d.name, deco: d.deco } : { x: d.x, y: d.y, name: d.name }));
}

const lower = (v) => String(v ?? '').trim().toLowerCase();
const num = (v) => (Number.isFinite(+v) ? +v : 0);

export function normTask(t) {
  if (!t || typeof t !== 'object' || !t.id) return null;
  const status = TASK_STATUSES.includes(t.status) ? t.status : 'idle';
  return {
    id: String(t.id),
    title: cleanText(t.title, 160) || 'Untitled task',
    status,
    assignee: lower(t.assignee),
    assigneeKind: String(t.assigneeKind || 'user'),
    createdBy: lower(t.createdBy),
    department: lower(t.department),
    team: lower(t.team),
    priority: num(t.priority) || 50,
    progress: num(t.progress),
    elapsedMs: num(t.elapsedMs),
    startedAt: num(t.startedAt),
    replyCount: num(t.replyCount),
    lastReplyAt: num(t.lastReplyAt),
    updatedAt: num(t.updatedAt),
  };
}

export function parseTaskPage(json) {
  const ok = json && typeof json === 'object';
  const list = ok && Array.isArray(json.tasks) ? json.tasks : [];
  const next = ok && Number.isInteger(json.nextOffset) && json.hasMore ? json.nextOffset : null;
  return {
    tasks: list.map(normTask).filter(Boolean),
    nextOffset: next,
    actor: ok ? lower(json.actor) : '',
    serverNow: ok ? num(json.serverNow) : 0,
    members: ok && Array.isArray(json.members) ? json.members.map(lower).filter(Boolean) : [],
  };
}

export function apiError(status, json) {
  const code = json && typeof json === 'object' ? String(json.error || '') : '';
  const map = {
    invalid_session: 'Sign in with ForkMesh to see your org’s tasks.',
    org_member_required: 'You are not a member of this organization.',
    assignee_only: 'Only the assignee can start or stop this timer.',
    assignee_or_manager_only: 'Only the assignee or a manager can complete this task.',
    active_task_exists: 'You already have a running timer. Stop it first.',
    task_completed: 'This task is already done.',
    task_not_found: 'That task no longer exists.',
  };
  if (map[code]) return map[code];
  if (status === 401) return map.invalid_session;
  return code ? code.replace(/_/g, ' ') : `Request failed (${status})`;
}

export function snapshot(tasks) {
  const s = {};
  for (const t of tasks) s[t.id] = { s: t.status, a: t.assignee, c: t.replyCount, rt: t.lastReplyAt };
  return s;
}

export function diffTasks(prev, tasks, me) {
  const who = lower(me);
  if (!prev || !who) return [];
  const out = [];
  for (const t of tasks) {
    const p = prev[t.id];
    const mine = t.assignee === who || t.createdBy === who;
    if (t.assignee === who && (!p || p.a !== who) && t.createdBy !== who && t.status !== 'done') {
      out.push({ kind: 'assigned', task: t });
      continue;
    }
    if (!p) continue;
    if (p.s !== t.status && mine) out.push({ kind: 'status', task: t, from: p.s, to: t.status });
    if (t.replyCount > p.c && t.lastReplyAt > p.rt) out.push({ kind: 'reply', task: t, since: p.rt, involved: mine });
  }
  return out;
}

export function mentionRe(name) {
  const n = lower(name).replace(/[^a-z0-9-]/g, '');
  if (!n) return null;
  return new RegExp(`(^|[^A-Za-z0-9._%+-])@${n}(?![a-z0-9-])`, 'i');
}

export function findMentions(replies, me, since) {
  const re = mentionRe(me);
  const who = lower(me);
  if (!re || !Array.isArray(replies)) return [];
  return replies.filter((r) => r && num(r.createdAt) > since && lower(r.author) !== who && re.test(String(r.body || '')));
}

export function newReplies(replies, me, since) {
  const who = lower(me);
  if (!Array.isArray(replies)) return [];
  return replies.filter((r) => r && num(r.createdAt) > since && lower(r.author) !== who);
}

export function filterTasks(tasks, { scope = 'mine', status = 'open', q = '' } = {}, me = '') {
  const who = lower(me);
  const query = lower(q);
  return tasks.filter((t) => {
    if (scope === 'mine' && t.assignee !== who && t.createdBy !== who) return false;
    if (status === 'open' && t.status === 'done') return false;
    if (TASK_STATUSES.includes(status) && t.status !== status) return false;
    if (query && !t.title.toLowerCase().includes(query) && !t.assignee.includes(query)) return false;
    return true;
  });
}

export function sortTasks(tasks) {
  const rank = { active: 0, idle: 1, done: 2 };
  return [...tasks].sort((a, b) => rank[a.status] - rank[b.status] || a.priority - b.priority || b.updatedAt - a.updatedAt);
}

export function pollInterval({ panelOpen, atDesk }) {
  return panelOpen || atDesk ? FAST_POLL_MS : SLOW_POLL_MS;
}

export function fmtElapsed(ms) {
  const m = Math.floor(Math.max(0, ms) / 60e3);
  if (m < 60) return `${m}m`;
  const h = Math.floor(m / 60);
  return h < 24 ? `${h}h ${m % 60}m` : `${Math.floor(h / 24)}d ${h % 24}h`;
}

export function liveElapsed(t, now) {
  return t.status === 'active' && t.fetchedAt ? t.elapsedMs + Math.max(0, now - t.fetchedAt) : t.elapsedMs;
}

export function pingTargets(task, me) {
  const who = lower(me);
  const set = new Set();
  for (const n of [task?.assignee, task?.createdBy]) {
    const v = lower(n);
    if (v && v !== who && v !== 'agent') set.add(v);
  }
  return [...set];
}

export function short(s, n) {
  const t = String(s || '');
  return t.length > n ? t.slice(0, n - 1).trimEnd() + '…' : t;
}
