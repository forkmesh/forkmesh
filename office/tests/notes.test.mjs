import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { setNote, noteLine, NOTE_MAX } from '../public/notes.js';

const PUBLIC = new URL('../public/', import.meta.url).pathname;

function el() {
  const e = {
    hidden: true, innerHTML: '', dataset: {}, attrs: {}, classes: new Set(), children: [], parent: null,
    classList: { add: (c) => e.classes.add(c) },
    setAttribute(k, v) { e.attrs[k] = v; },
    append(c) { c.parent = e; e.children.push(c); },
    remove() { if (e.parent) e.parent.children.splice(e.parent.children.indexOf(e), 1); e.parent = null; },
    get firstElementChild() { return e.children[0] || null; },
  };
  return e;
}
const click = (at, note) => at.onclick({ target: { closest: () => ({ dataset: { note } }) } });
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

function files(dir, ext) {
  const out = [];
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) out.push(...files(p, ext));
    else if (ext.some((x) => name.endsWith(x))) out.push(p);
  }
  return out;
}

test('a note renders inline with its tone and escapes its text', () => {
  const at = el();
  setNote(at, { title: '<b>Saved</b>', body: 'x & y', tone: 'ok' });
  assert.equal(at.hidden, false);
  assert.ok(at.classes.has('note'));
  assert.equal(at.dataset.tone, 'ok');
  assert.equal(at.attrs.role, 'status');
  assert.match(at.innerHTML, /&lt;b&gt;Saved&lt;\/b&gt;/);
  assert.match(at.innerHTML, /x &amp; y/);
  setNote(at, { title: 'Failed', tone: 'error' });
  assert.equal(at.attrs.role, 'alert');
  setNote(at, null);
  assert.equal(at.hidden, true);
  assert.equal(at.innerHTML, '');
});

test('an error line never clears itself; an info line may', async () => {
  const err = el(), info = el();
  setNote(err, { title: 'Could not save', tone: 'error', ms: 5 });
  setNote(info, { title: 'Saved', ms: 5 });
  await wait(20);
  assert.equal(err.hidden, false);
  assert.equal(info.hidden, true);
});

test('an offer button clears its line and runs the action', () => {
  const at = el();
  let ran = 0;
  setNote(at, { title: 'Log coffee?', actions: [{ label: 'Log it', run: () => ran++ }] });
  assert.match(at.innerHTML, /data-note="0"[^>]*>Log it</);
  click(at, '0');
  assert.equal(ran, 1);
  assert.equal(at.hidden, true);
});

test('status lines are keyed: one per key, newest kept, at most a few', () => {
  const box = el();
  noteLine(box, { key: 'a', title: 'First' }, el);
  noteLine(box, { key: 'a', title: 'Second' }, el);
  assert.equal(box.children.length, 1);
  assert.match(box.children[0].innerHTML, /Second/);
  for (const k of ['b', 'c', 'd', 'e']) noteLine(box, { key: k, title: k }, el);
  assert.equal(box.children.length, NOTE_MAX);
  assert.deepEqual(box.children.map((c) => c.dataset.key), ['c', 'd', 'e']);
  noteLine(box, { key: 'd' }, el);
  assert.deepEqual(box.children.map((c) => c.dataset.key), ['c', 'e']);
  assert.match(box.children[0].innerHTML, /note-x/);
  click(box.children[0], 'x');
  assert.deepEqual(box.children.map((c) => c.dataset.key), ['e']);
});

test('the office has no toast stack: no container, no toast styles, no toast calls in features', () => {
  for (const p of files(PUBLIC, ['.js', '.css', '.html'])) {
    const src = readFileSync(p, 'utf8');
    assert.doesNotMatch(src, /#toasts|id="toasts"|\.toast\b|class="toast|'toast /, p);
    if (!p.endsWith('app.js')) assert.doesNotMatch(src, /api\.toast\(/, p);
  }
  const page = readFileSync(join(PUBLIC, 'index.html'), 'utf8');
  const dock = page.slice(page.indexOf('<div id="dock">'), page.indexOf('<div id="bar">'));
  assert.match(dock, /<div id="notes"/);
});
