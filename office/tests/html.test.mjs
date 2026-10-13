import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join as joinPath } from 'node:path';
import { html, join, trusted, setHtml, esc } from '../public/html.js';

const PUBLIC = new URL('../public/', import.meta.url).pathname;

function files(dir) {
  const out = [];
  for (const name of readdirSync(dir)) {
    const p = joinPath(dir, name);
    if (statSync(p).isDirectory()) out.push(...files(p));
    else if (name.endsWith('.js')) out.push(p);
  }
  return out;
}

test('interpolations are escaped unless they are markup', () => {
  const name = '<img src=x onerror=alert(1)>"\'`&';
  assert.equal(html`<b title="${name}">${name}</b>`.toString(),
    '<b title="&lt;img src=x onerror=alert(1)&gt;&quot;&#39;&#96;&amp;">&lt;img src=x onerror=alert(1)&gt;&quot;&#39;&#96;&amp;</b>');
  assert.equal(html`<i>${html`<b>ok</b>`}</i>`.toString(), '<i><b>ok</b></i>');
  assert.equal(html`${['<a>', html`<b></b>`]}`.toString(), '&lt;a&gt;<b></b>');
  assert.equal(html`${null}${undefined}${false}${true}${0}`.toString(), '0');
  assert.equal(join([html`<b>1</b>`, '<2>'], ',').toString(), '<b>1</b>,&lt;2&gt;');
  assert.equal(esc('a&b'), 'a&amp;b');
});

test('setHtml refuses plain strings', () => {
  const el = { innerHTML: '' };
  assert.throws(() => setHtml(el, '<b>x</b>'), TypeError);
  setHtml(el, trusted('<b>x</b>'));
  assert.equal(el.innerHTML, '<b>x</b>');
});

test('no client module writes markup outside setHtml or evaluates strings', () => {
  for (const f of files(PUBLIC)) {
    if (f.endsWith('/html.js')) continue;
    const src = readFileSync(f, 'utf8');
    assert.ok(!/\.innerHTML\s*=|\.outerHTML\s*=|insertAdjacentHTML|document\.write/.test(src), f);
    assert.ok(!/\beval\(|new Function\(/.test(src), f);
    assert.ok(!/https?:\/\/(?!www\.w3\.org)/.test(src), `${f} must not load or link third-party origins`);
    for (const m of src.matchAll(/trusted\(([^)]*)\)/g)) {
      assert.ok(/^`<svg|^renderText\(|^markup|^`<svg class/.test(m[1].trim()) || m[1].includes('renderText('), `${f}: trusted(${m[1].slice(0, 40)})`);
    }
  }
});
