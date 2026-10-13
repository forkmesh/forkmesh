export const DT = 50;

export class Path {
  constructor(x, y, lift = 0) {
    this.s = [];
    this.x = x;
    this.y = y;
    this.lift = lift;
    this.push({});
  }

  push({ wild = false, phase = '' } = {}) {
    this.s.push({ x: this.x, y: this.y, lift: this.lift, wild, phase });
  }

  go(x, y, { speed = 4, lift = this.lift, wild = false, bump = 0, bumps = 0, phase = '' } = {}) {
    const x0 = this.x, y0 = this.y, l0 = this.lift;
    const dist = Math.hypot(x - x0, y - y0);
    const n = Math.max(1, Math.ceil((dist / speed) * (1000 / DT)));
    for (let i = 1; i <= n; i++) {
      const t = i / n;
      this.x = x0 + (x - x0) * t;
      this.y = y0 + (y - y0) * t;
      this.lift = l0 + (lift - l0) * t + (bumps ? bump * Math.abs(Math.sin(t * Math.PI * bumps)) : 0);
      this.push({ wild, phase });
    }
    this.lift = lift;
    return this;
  }

  hold(ms, opts = {}) {
    const n = Math.max(1, Math.round(ms / DT));
    for (let i = 0; i < n; i++) this.push(opts);
    return this;
  }

  spiral(cx, cy, r0, r1, turns, { speed = 6, lift = this.lift, wild = true } = {}) {
    const a0 = Math.atan2(this.y - cy, this.x - cx);
    const l0 = this.lift;
    const len = Math.PI * (r0 + r1) * turns;
    const n = Math.max(1, Math.ceil((len / speed) * (1000 / DT)));
    for (let i = 1; i <= n; i++) {
      const t = i / n;
      const a = a0 + t * turns * Math.PI * 2;
      const r = r0 + (r1 - r0) * t;
      this.x = cx + Math.cos(a) * r;
      this.y = cy + Math.sin(a) * r;
      this.lift = l0 + (lift - l0) * t;
      this.push({ wild });
    }
    return this;
  }

  wobble(ms, { cx, cy, ax, ay, period = 2400, lift = this.lift, wild = true } = {}) {
    const n = Math.max(1, Math.round(ms / DT));
    for (let i = 1; i <= n; i++) {
      const a = (i * DT * Math.PI * 2) / period;
      this.x = cx + Math.sin(a) * ax;
      this.y = cy + Math.sin(a * 2) * ay;
      this.lift = lift + 0.15 * Math.sin(a * 3);
      this.push({ wild });
    }
    return this;
  }

  arc(x, y, ms, peak, opts = {}) {
    const x0 = this.x, y0 = this.y, l0 = this.lift;
    const n = Math.max(1, Math.round(ms / DT));
    for (let i = 1; i <= n; i++) {
      const t = i / n;
      this.x = x0 + (x - x0) * t;
      this.y = y0 + (y - y0) * t;
      this.lift = l0 * (1 - t) + peak * 4 * t * (1 - t);
      this.push({ wild: true, ...opts });
    }
    this.lift = 0;
    return this;
  }

  done(kind) {
    const s = this.s;
    return { kind, s, duration: s.length * DT, sample: (t) => s[Math.max(0, Math.min(s.length - 1, Math.floor(t / DT)))] };
  }
}
