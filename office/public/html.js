const tt = globalThis.trustedTypes;
const policy = tt && typeof tt.createPolicy === 'function' ? tt.createPolicy('office', { createHTML: (s) => s }) : null;
const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;', '`': '&#96;' };

export class Html {
  constructor(s) {
    this.s = s;
  }
  toString() {
    return this.s;
  }
}

export const esc = (v) => String(v ?? '').replace(/[&<>"'`]/g, (c) => ESC[c]);

function part(v) {
  if (v instanceof Html) return v.s;
  if (Array.isArray(v)) return v.map(part).join('');
  if (v === null || v === undefined || v === false || v === true) return '';
  return esc(v);
}

export function html(strings, ...values) {
  let out = strings[0];
  for (let i = 0; i < values.length; i++) out += part(values[i]) + strings[i + 1];
  return new Html(out);
}

export const join = (list, sep = '') => new Html(list.map(part).join(sep));

export function trusted(markup) {
  return markup instanceof Html ? markup : new Html(String(markup));
}

export function setHtml(el, h) {
  if (!(h instanceof Html)) throw new TypeError('setHtml needs html`` markup');
  el.innerHTML = policy ? policy.createHTML(h.s) : h.s;
}

export function fragment(h) {
  const t = document.createElement('template');
  setHtml(t, h);
  return t.content;
}
