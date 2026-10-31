const TWO16 = 65536;
const TWO32 = 4294967296;

function mod(v, m) {
  const r = v % m;
  return r < 0 ? r + m : r;
}

export function near16(v, ref) {
  let d = mod(v - mod(ref, TWO16), TWO16);
  if (d >= 32768) d -= TWO16;
  return ref + d;
}

export function near32(v, ref) {
  let d = mod(v - mod(ref, TWO32), TWO32);
  if (d >= 2147483648) d -= TWO32;
  return ref + d;
}

export class Unwrapper16 {
  constructor(init = {}) {
    this._last = init._last ?? 0;
    this.ok = init.ok ?? false;
  }

  unwrap(v) {
    if (!this.ok) {
      this.ok = true;
      this._last = v;
      return this._last;
    }
    const c = near16(v, this._last);
    if (c > this._last) this._last = c;
    return c;
  }

  last() {
    return this.ok ? this._last : null;
  }
}

export class Unwrapper32 {
  constructor(init = {}) {
    this.last = init.last ?? 0;
    this.ok = init.ok ?? false;
  }

  unwrap(v) {
    if (!this.ok) {
      this.ok = true;
      this.last = v;
      return this.last;
    }
    const c = near32(v, this.last);
    if (c > this.last) this.last = c;
    return c;
  }
}
