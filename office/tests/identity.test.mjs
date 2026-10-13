import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const el = () => ({ className: '', hidden: false, innerHTML: '', textContent: '', querySelector: () => null, querySelectorAll: () => [], addEventListener() {}, after() {} });

async function run({ session, search = '', stored = null, entry = null, fail = false }) {
  const calls = { connect: 0, replaced: null, assigned: null, fetched: [], stored: {}, notices: [] };
  const store = { [`office-org`]: stored };
  const title = el();
  const lead = el();
  const form = { ...el(), querySelector: (s) => (s === 'h1' ? title : s === 'p' ? lead : null) };
  const nodes = { '#join-form': form, '#join': el(), '#app': el() };
  globalThis.document = { createElement: el, querySelectorAll: () => [], dispatchEvent() {} };
  globalThis.CustomEvent = class { constructor(type, init) { this.type = type; this.detail = init?.detail; } };
  globalThis.location = {
    search, pathname: '/office', href: 'https://app.example/office' + search, protocol: 'https:', host: 'app.example', origin: 'https://app.example',
    replace: (u) => { calls.replaced = u; },
    assign: (u) => { calls.assigned = u; },
  };
  globalThis.history = { replaceState() {} };
  globalThis.addEventListener = () => {};
  globalThis.fetch = async (url, init = {}) => {
    calls.fetched.push(String(url));
    if (fail) throw new Error('offline');
    if (String(url).startsWith('/api/office/entry')) {
      const status = entry ?? 200;
      return { ok: status === 200, status, json: async () => ({ ws: '/api/office/ws?ticket=t', iceServers: [] }) };
    }
    const q = new URL(String(url), 'https://app.example').searchParams.get('org');
    const body = typeof session === 'function' ? session(q) : session;
    return { ok: true, status: 200, json: async () => body };
  };
  const seen = [];
  const api = {
    $: (s) => nodes[s] || null, tint: () => '#fff', notice: (title, body) => calls.notices.push({ title, body }), setNote() {}, connect: () => { calls.connect++; },
    store: { get: (k) => store[k] ?? null, set: (k, v) => { store[k] = v; calls.stored[k] = v; } },
    onDenied() {}, onInit() {}, onMessage() {}, personBadge() {},
  };
  const { default: setup } = await import(`../public/features/identity.js?${Math.random()}`);
  const watch = setInterval(() => seen.push(title.innerHTML), 0);
  setup(api);
  seen.push(title.innerHTML);
  await new Promise((r) => setTimeout(r, 20));
  clearInterval(watch);
  seen.push(title.innerHTML);
  return { calls, api, title, lead, seen };
}

const account = { name: 'alice', av: 1 };
const gate = /get you in/i;

test('the static page opens on a neutral loading state, not the sign-in gate', () => {
  const page = readFileSync(new URL('../public/index.html', import.meta.url), 'utf8');
  assert.doesNotMatch(page, gate);
  assert.match(page, /<h1 id="join-title">Opening your office…<\/h1>/);
});

test('a signed-out visitor goes straight to login', async () => {
  const { calls } = await run({ session: { signedIn: false }, search: '?org=acme' });
  assert.equal(calls.replaced, '/login?next=' + encodeURIComponent('/office?org=acme'));
  assert.equal(calls.connect, 0);
});

test('a member of exactly one org enters with no click, never sees the gate, and it is remembered', async () => {
  const { calls, seen, title } = await run({ session: { signedIn: true, account, orgs: [{ name: 'acme', role: 'member' }], org: 'acme', teams: [] } });
  assert.equal(calls.connect, 1);
  assert.equal(calls.stored['office-org'], 'acme');
  assert.equal(calls.replaced, null);
  assert.ok(seen.every((h) => !gate.test(h)), seen.join(' | '));
  assert.match(title.innerHTML, /Entering the acme office/);
});

test('several orgs and nothing remembered enter the default org with no picker', async () => {
  const orgs = [{ name: 'acme', role: 'member' }, { name: 'globex', role: 'owner' }];
  const { calls, seen } = await run({ session: { signedIn: true, account, orgs, org: 'acme' } });
  assert.equal(calls.connect, 1);
  assert.equal(calls.stored['office-org'], 'acme');
  assert.ok(seen.every((h) => !gate.test(h)));
});

test('several orgs with a remembered one enter that office', async () => {
  const orgs = [{ name: 'acme', role: 'member' }, { name: 'globex', role: 'owner' }];
  const { calls } = await run({ stored: 'globex', session: (q) => ({ signedIn: true, account, orgs, org: q || 'acme' }) });
  assert.equal(calls.connect, 1);
  assert.ok(calls.fetched[0].endsWith('?org=globex'));
});

test('a remembered org the account left enters the org the server chose', async () => {
  const orgs = [{ name: 'acme', role: 'member' }, { name: 'initech', role: 'member' }];
  const { calls } = await run({ stored: 'globex', session: { signedIn: true, account, orgs, org: 'acme' } });
  assert.equal(calls.connect, 1);
  assert.equal(calls.stored['office-org'], 'acme');
});

test('a requested org the account is not in enters its own office with a notice', async () => {
  const orgs = [{ name: 'acme', role: 'member' }];
  const { calls } = await run({ search: '?org=globex', session: { signedIn: true, account, orgs, org: 'acme' } });
  assert.equal(calls.connect, 1);
  assert.equal(calls.notices.length, 1);
  assert.match(calls.notices[0].title, /not a member of globex/);
  assert.match(calls.notices[0].body, /acme office instead/);
});

test('signed in without an organization does not connect or show the gate', async () => {
  const { calls, title } = await run({ session: { signedIn: true, account, orgs: [], org: null } });
  assert.equal(calls.connect, 0);
  assert.equal(calls.replaced, null);
  assert.doesNotMatch(title.innerHTML, gate);
  assert.match(title.innerHTML, /No office yet/);
});

test('an unreachable session check shows a connection error, not the gate', async () => {
  const { calls, title } = await run({ fail: true, session: null });
  assert.equal(calls.connect, 0);
  assert.equal(calls.replaced, null);
  assert.doesNotMatch(title.innerHTML, gate);
});

test('an ended session at entry sends the person to login', async () => {
  const { api, calls } = await run({ entry: 401, session: { signedIn: true, account, orgs: [{ name: 'acme', role: 'member' }], org: 'acme' } });
  assert.equal(await api.socketSource(), null);
  assert.equal(calls.replaced, '/login?next=' + encodeURIComponent('/office'));
});

test('TURN credentials are refetched from entry only once they are 8 minutes old', async () => {
  const { api, calls } = await run({ session: { signedIn: true, account, orgs: [{ name: 'acme', role: 'member' }], org: 'acme' } });
  assert.ok(await api.socketSource());
  const entries = () => calls.fetched.filter((u) => u.startsWith('/api/office/entry')).length;
  const before = entries();
  api.refreshIce();
  await new Promise((r) => setTimeout(r, 5));
  assert.equal(entries(), before);
  const realNow = Date.now;
  Date.now = () => realNow() + 8 * 60 * 1000 + 1;
  try {
    api.refreshIce();
    api.refreshIce();
    await new Promise((r) => setTimeout(r, 5));
  } finally {
    Date.now = realNow;
  }
  assert.equal(entries(), before + 1);
  assert.deepEqual(api.iceServers, []);
});
