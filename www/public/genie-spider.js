(() => {
  if (window.ForkMeshGenie) return;

  const GLOW = [64, 242, 219, 1];
  const GLOW_SOFT = [107, 219, 240, 1];
  const GLOW_DEEP = [13, 140, 143, 1];
  const LILAC = [143, 102, 209, 1];
  const VIOLET = [102, 61, 168, 1];
  const PLUM = [54, 33, 97, 1];
  const SHELL = [26, 26, 51, 1];
  const SHELL_LIGHT = [56, 56, 92, 1];
  const PITCH = [8, 8, 20, 1];
  const DEEP_SEA = [26, 77, 102, 1];
  const SILVER = [204, 209, 224, 1];
  const SILVER_DARK = [117, 122, 143, 1];
  const SICKLY = [107, 158, 92, 1];
  const WHITE = [255, 255, 255, 1];

  const SHOULDERS = [[35, 76], [30, 71], [28, 65], [29, 59]];
  const GROUND = 96.2;
  const KNEES = [[27, 79], [20, 72], [15, 61], [13.5, 51]];
  const FEET = [[31, GROUND], [17.5, GROUND], [8.5, GROUND], [2.5, GROUND]];

  const DURATIONS = {
    wave: 1.8, cheer: 1.6, surprise: 1.3, nod: 0.9, dizzy: 1.7, eureka: 1.4, giggle: 1.1,
  };

  const clamp01 = (v) => Math.min(Math.max(v, 0), 1);
  const clamp = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
  const alpha = (c, o) => [c[0], c[1], c[2], clamp01(c[3] * o)];
  const css = (c) => `rgba(${c[0]},${c[1]},${c[2]},${c[3]})`;
  const pt = (x, y) => ({ x, y });
  const add = (a, b) => pt(a.x + b.x, a.y + b.y);
  const sub = (a, b) => pt(a.x - b.x, a.y - b.y);
  const scale = (a, k) => pt(a.x * k, a.y * k);
  const mix = (a, b, k) => add(a, scale(sub(b, a), k));
  const rad = (deg) => (deg * Math.PI) / 180;

  function oval(x, y, rx, ry) {
    const p = new Path2D();
    p.ellipse(x, y, Math.max(rx, 0.0001), Math.max(ry, 0.0001), 0, 0, Math.PI * 2);
    return p;
  }

  function arcPath(c, r, from, to) {
    const p = new Path2D();
    for (let k = 0; k <= 24; k += 1) {
      const a = rad(from + ((to - from) * k) / 24);
      const x = c.x + Math.cos(a) * r;
      const y = c.y + Math.sin(a) * r;
      if (k === 0) p.moveTo(x, y);
      else p.lineTo(x, y);
    }
    return p;
  }

  function stops(gradient, colors) {
    colors.forEach((c, i) => gradient.addColorStop(colors.length > 1 ? i / (colors.length - 1) : 0, css(c)));
    return gradient;
  }

  function painter(ctx, s) {
    const is = (r) => s.reaction === r;
    const radial = (c, r, colors) => stops(ctx.createRadialGradient(c.x, c.y, 0, c.x, c.y, r), colors);
    const linear = (a, b, colors) => stops(ctx.createLinearGradient(a.x, a.y, b.x, b.y), colors);
    const fill = (path, style) => {
      ctx.fillStyle = Array.isArray(style) ? css(style) : style;
      ctx.fill(path);
    };
    const stroke = (path, color, width) => {
      ctx.lineCap = "round";
      ctx.lineJoin = "round";
      ctx.lineWidth = width;
      ctx.strokeStyle = css(color);
      ctx.stroke(path);
    };
    const glow = (x, y, r, color) => {
      if (r <= 0) return;
      fill(oval(x, y, r, r), radial(pt(x, y), r, [color, alpha(color, 0)]));
    };

    const duration = DURATIONS[s.reaction] || 0;
    const envelope = () => (s.reaction ? Math.sin(Math.PI * clamp01(s.reactionAge / duration)) : 0);
    const rise = () => {
      if (!s.reaction) return 0;
      return clamp01(s.reactionAge / 0.22) * clamp01((duration - s.reactionAge) / 0.26);
    };
    const walk = clamp01(s.walking);
    const air = clamp01(s.air);
    const landing = clamp01(s.land);
    const vitality = clamp01(s.vitality);
    const magic = clamp01(s.magic || 0);
    const energy = 0.45 + 0.55 * vitality;
    const surprise = Math.max(is("surprise") ? envelope() : 0, air * 0.7);
    const dizzy = is("dizzy") ? envelope() : 0;
    const eureka = is("eureka") ? rise() : 0;
    const giggle = is("giggle") ? rise() : 0;
    const cheer = is("cheer") ? rise() : 0;
    const nod = is("nod") ? Math.sin((s.reactionAge / DURATIONS.nod) * Math.PI * 2) * 1.8 : 0;
    const waving = s.mood === "greeting" ? 1 : is("wave") ? rise() : eureka;
    const bob = Math.sin(s.t * 1.6 * energy) * 1.4 - Math.abs(Math.sin(s.stridePhase)) * 2.2 * walk
      - giggle * Math.abs(Math.sin(s.reactionAge * 17)) * 2.2 + nod;
    const sway = Math.sin(s.t * 0.8) * 1.2 + dizzy * Math.sin(s.reactionAge * 9) * 9 + s.facing * 7 * walk;
    const sick = s.condition === "sick";
    const tint = sick ? SICKLY : GLOW;
    const glowStrength = Math.min(0.35 + 0.65 * vitality
      + (s.mood === "thinking" ? 0.2 * (0.5 + 0.5 * Math.sin(s.t * 6)) : 0) + eureka * 0.4, 1);
    const swayCos = Math.cos(rad(sway));
    const swaySin = Math.sin(rad(sway));
    const toBody = (g) => {
      const dx = g.x - 50;
      const dy = g.y - 90;
      return pt(dx * swayCos + dy * swaySin + 50, -dx * swaySin + dy * swayCos + 90 - bob);
    };

    function look() {
      let x = s.facing * 1.1 * walk;
      let y = 0;
      if (s.gaze) {
        x = x * 0.3 + clamp(s.gaze.x, -1, 1) * 1.5;
        y = clamp(s.gaze.y, -1, 1) * 1.3;
      }
      if (s.mood === "idle" && walk < 0.1 && !s.gaze) {
        x += Math.sin(s.t * 0.37) * Math.sin(s.t * 0.91) * 0.9;
        y += Math.sin(s.t * 0.53) * 0.4;
      }
      return pt(x, y);
    }

    function lid() {
      const cycle = s.t % 4.3;
      const blink = cycle < 0.14 ? Math.sin((cycle / 0.14) * Math.PI) : 0;
      const tired = { sick: 0.45, hungry: 0.28, peckish: 0.12 }[s.condition] || 0;
      return Math.min(Math.max(blink, tired - surprise * 0.5, giggle * 0.6), 0.95);
    }

    function aura() {
      const pulse = 0.5 + 0.5 * Math.sin(s.t * 2.1);
      glow(50, 60, 38 + pulse * 3, alpha(s.mood === "thinking" ? VIOLET : tint, 0.2 * glowStrength));
      if (magic > 0) glow(50, 58, 44 + pulse * 4, alpha(LILAC, 0.42 * magic));
    }

    function shadow() {
      const grounded = 1 - air;
      if (grounded <= 0.02) return;
      const y = GROUND + 0.6;
      ctx.save();
      ctx.translate(50, y);
      ctx.scale(1, 0.11);
      ctx.translate(-50, -y);
      fill(oval(50, y, 36, 36), radial(pt(50, y), 36,
        [alpha(PITCH, 0.42 * grounded), alpha(PITCH, 0.18 * grounded), alpha(PITCH, 0)]));
      ctx.restore();
    }

    function tail() {
      const swish = Math.sin(s.t * 1.9) * 3 + s.facing * -4 * walk;
      const p = new Path2D();
      p.moveTo(45, 79);
      p.bezierCurveTo(39, 84, 49 + swish, 87, 42 + swish, 92);
      p.bezierCurveTo(35 + swish, 97.5, 47 + swish, 101, 53 + swish * 1.3, 98.5);
      p.bezierCurveTo(49.5 + swish, 97, 44.5 + swish, 96.2, 45.8 + swish, 93);
      p.bezierCurveTo(58.5 + swish, 87, 55, 82, 56, 79);
      p.closePath();
      fill(p, linear(pt(42, 80), pt(52, 99), [SHELL_LIGHT, SHELL, PITCH]));
      stroke(p, alpha(tint, 0.32 * glowStrength), 0.45);
    }

    function legPose(index, side) {
      const m = (q) => (side < 0 ? pt(q[0], q[1]) : pt(100 - q[0], q[1]));
      const shoulder = m(SHOULDERS[index]);
      let knee = m(KNEES[index]);
      let foot = m(FEET[index]);
      const group = (index % 2 === 0) === (side < 0) ? 0 : Math.PI;
      const swing = s.stridePhase + group;
      const lift = Math.max(0, Math.sin(swing)) * 6 * walk;
      const reach = Math.cos(swing) * 3 * walk * (s.facing === 0 ? 1 : s.facing);
      const breathe = Math.sin(s.t * 1.6 * energy + index) * 0.5;
      foot = toBody(pt(foot.x + reach, foot.y - lift));
      knee = pt(knee.x + reach * 0.5, knee.y - lift * 0.55 - breathe + (1 - vitality) * 3);
      if (air > 0.01) {
        const dx = shoulder.x - 50;
        const dy = shoulder.y - 62;
        const length = Math.max(Math.hypot(dx, dy), 0.01);
        const flail = Math.sin(s.stridePhase + index * 1.3 + (side < 0 ? 0 : 0.7));
        knee = mix(knee, pt(shoulder.x + (dx / length) * 13, shoulder.y + (dy / length) * 13 - 4 + flail * 2), air);
        foot = mix(foot, pt(shoulder.x + (dx / length) * 27, shoulder.y + (dy / length) * 27 - 3 + flail * 6), air);
      }
      if (index === 0) {
        const raised = side < 0 ? Math.max(waving, cheer) : cheer;
        if (raised > 0.01) {
          const wiggle = Math.sin(s.reactionAge * 13 + (side < 0 ? 0 : 1.4)) * 6 * raised;
          foot = mix(foot, m([10 + wiggle * (side < 0 ? 1 : -1), 38]), raised);
          knee = mix(knee, m([21, 56]), raised);
        }
        const pointing = side === (s.pointSide || -1) ? clamp01(s.point || 0) : 0;
        if (pointing > 0.01) {
          foot = mix(foot, m([1.5, 66 + Math.sin(s.t * 5) * 1.2]), pointing);
          knee = mix(knee, m([15, 77]), pointing);
        }
      }
      return { shoulder, knee, foot };
    }

    function drawLeg(leg, back) {
      const shade = back ? 0.85 : 1;
      const g = glowStrength;
      const outward = leg.foot.x < 50 ? -1 : 1;
      fill(oval(leg.shoulder.x, leg.shoulder.y, 3, 3),
        radial(sub(leg.shoulder, pt(0.8, 0.9)), 3.4, [alpha(SHELL_LIGHT, shade), alpha(PITCH, shade)]));
      const upper = new Path2D();
      upper.moveTo(leg.shoulder.x, leg.shoulder.y);
      upper.lineTo(leg.knee.x, leg.knee.y);
      stroke(upper, alpha(PITCH, shade), 6.2);
      stroke(upper, alpha(SHELL, shade), 4.6);
      const sheen = new Path2D();
      sheen.moveTo(leg.shoulder.x, leg.shoulder.y - 1.1);
      sheen.lineTo(leg.knee.x, leg.knee.y - 1.1);
      stroke(sheen, alpha(SHELL_LIGHT, 0.75 * shade), 1.1);
      const dx = leg.foot.x - leg.knee.x;
      const dy = leg.foot.y - leg.knee.y;
      const length = Math.max(Math.hypot(dx, dy), 0.01);
      let nx = -dy / length;
      let ny = dx / length;
      if (nx * outward < 0 || (Math.abs(nx) < 0.05 && ny > 0)) {
        nx = -nx;
        ny = -ny;
      }
      const n = pt(nx, ny);
      const mid = pt(leg.knee.x + dx * 0.42, leg.knee.y + dy * 0.42);
      const bulge = Math.min(length * 0.24, 5.5);
      const claw = new Path2D();
      const a = add(leg.knee, scale(n, 3.7));
      const c1 = add(mid, scale(n, bulge + 3));
      const c2 = add(mid, scale(n, bulge * 0.5 - 0.6));
      const b = sub(leg.knee, scale(n, 3.1));
      claw.moveTo(a.x, a.y);
      claw.quadraticCurveTo(c1.x, c1.y, leg.foot.x, leg.foot.y);
      claw.quadraticCurveTo(c2.x, c2.y, b.x, b.y);
      claw.closePath();
      const gradient = ctx.createLinearGradient(leg.knee.x, leg.knee.y, leg.foot.x, leg.foot.y);
      gradient.addColorStop(0, css(alpha(SHELL_LIGHT, shade)));
      gradient.addColorStop(0.35, css(alpha(SHELL, shade)));
      gradient.addColorStop(0.72, css(alpha(PITCH, shade)));
      gradient.addColorStop(1, css(alpha(tint, g * shade)));
      fill(claw, gradient);
      const rim = new Path2D();
      const r0 = add(leg.knee, scale(n, 2.4));
      const r1 = add(mid, scale(n, bulge + 1.7));
      const r2 = mix(leg.knee, leg.foot, 0.82);
      rim.moveTo(r0.x, r0.y);
      rim.quadraticCurveTo(r1.x, r1.y, r2.x, r2.y);
      stroke(rim, alpha(SILVER, 0.28 * shade), 0.7);
      const vein = new Path2D();
      const v0 = mix(leg.knee, leg.foot, 0.12);
      const v1 = add(mid, scale(n, bulge * 0.8));
      const v2 = mix(leg.knee, leg.foot, 0.62);
      vein.moveTo(v0.x, v0.y);
      vein.quadraticCurveTo(v1.x, v1.y, v2.x, v2.y);
      stroke(vein, alpha(tint, 0.7 * g * shade), 0.75);
      glow(leg.foot.x, leg.foot.y, 3.6, alpha(tint, 0.6 * g * shade));
      glow(leg.knee.x, leg.knee.y, 6, alpha(tint, 0.42 * g * shade));
      fill(oval(leg.knee.x, leg.knee.y, 3.9, 3.9),
        radial(sub(leg.knee, pt(1, 1.2)), 4.4, [alpha(SHELL_LIGHT, shade), alpha(PITCH, shade)]));
      fill(oval(leg.knee.x, leg.knee.y, 2.2, 2.2), radial(sub(leg.knee, pt(0.5, 0.5)), 2.4,
        [alpha(WHITE, 0.9 * shade), alpha(tint, g * shade), alpha(GLOW_DEEP, shade)]));
      stroke(oval(leg.knee.x, leg.knee.y, 3.9, 3.9), alpha(SILVER_DARK, 0.5 * shade), 0.35);
    }

    function legs(front) {
      for (const index of [front ? 1 : 3, front ? 0 : 2]) {
        for (const side of [-1, 1]) drawLeg(legPose(index, side), !front);
      }
    }

    function head() {
      const face = oval(50, 61, 25.5, 25);
      fill(face, radial(pt(41, 54), 34, [SHELL_LIGHT, SHELL, PITCH]));
      if (sick) fill(face, alpha(SICKLY, 0.16));
      stroke(arcPath(pt(50, 61), 24.6, 15, 80), alpha(tint, 0.4 * glowStrength), 0.8);
      ctx.save();
      ctx.clip(face);
      fill(oval(37, 77, 7, 4), alpha(WHITE, 0.05));
      ctx.restore();
      fill(oval(38, 73.5, 4, 2.2), alpha(VIOLET, 0.24));
      fill(oval(62, 73.5, 4, 2.2), alpha(VIOLET, 0.24));
    }

    function feather() {
      const hunger = 1 - vitality;
      ctx.save();
      ctx.translate(50, 27);
      ctx.rotate(rad(Math.sin(s.t * 1.1) * 4 + hunger * 38));
      ctx.translate(-50, -27);
      const base = pt(50, 28);
      const bend = pt(44, 13);
      const tip = pt(55, 0.5);
      const spine = (u) => add(add(scale(base, (1 - u) * (1 - u)), scale(bend, 2 * (1 - u) * u)), scale(tip, u * u));
      const normal = (u) => {
        const d = add(scale(sub(bend, base), 2 * (1 - u)), scale(sub(tip, bend), 2 * u));
        const length = Math.max(Math.hypot(d.x, d.y), 0.01);
        return pt(-d.y / length, d.x / length);
      };
      const steps = 14;
      const left = [];
      const right = [];
      for (let k = 0; k <= steps; k += 1) {
        const u = k / steps;
        const notch = k % 2 === 1 ? 0.74 : 1;
        const width = 7.4 * Math.pow(Math.sin(Math.PI * Math.min(u * 1.05, 1)), 0.75) * (1 - u * 0.3) * notch;
        const at = spine(u);
        const n = normal(u);
        left.push(add(at, scale(n, width)));
        right.push(sub(at, scale(n, width * 0.92)));
      }
      const leaf = new Path2D();
      leaf.moveTo(base.x, base.y);
      for (const q of left) leaf.lineTo(q.x, q.y);
      for (let k = right.length - 1; k >= 0; k -= 1) leaf.lineTo(right[k].x, right[k].y);
      leaf.closePath();
      fill(leaf, linear(pt(42, 8), pt(58, 26), [SHELL_LIGHT, SHELL, PITCH]));
      stroke(leaf, alpha(PITCH, 0.6), 0.35);
      for (let k = 2; k < steps - 1; k += 2) {
        const from = spine(k / steps - 0.03);
        const barb = new Path2D();
        barb.moveTo(from.x, from.y);
        barb.lineTo(left[k + 1].x, left[k + 1].y);
        barb.moveTo(from.x, from.y);
        barb.lineTo(right[k + 1].x, right[k + 1].y);
        stroke(barb, alpha(PITCH, 0.55), 0.4);
      }
      const vein = new Path2D();
      const end = spine(0.94);
      vein.moveTo(base.x, base.y);
      vein.quadraticCurveTo(bend.x, bend.y, end.x, end.y);
      stroke(vein, alpha(tint, 0.3 * glowStrength), 2.4);
      stroke(vein, alpha(tint, 0.95 * glowStrength), 0.8);
      ctx.restore();
    }

    function silver(path, width) {
      stroke(path, alpha(PITCH, 0.55), width + 1);
      stroke(path, SILVER_DARK, width);
      stroke(path, SILVER, width * 0.62);
      ctx.save();
      ctx.translate(0, -width * 0.18);
      stroke(path, alpha(WHITE, 0.7), width * 0.18);
      ctx.restore();
    }

    function glint(at, r) {
      if (r <= 0.05) return;
      const cross = new Path2D();
      cross.moveTo(at.x - r, at.y);
      cross.lineTo(at.x + r, at.y);
      cross.moveTo(at.x, at.y - r);
      cross.lineTo(at.x, at.y + r);
      stroke(cross, alpha(WHITE, 0.9), 0.45);
    }

    function turban() {
      feather();
      const dome = new Path2D();
      dome.moveTo(21.5, 60);
      dome.bezierCurveTo(15, 39, 29, 23, 50, 23);
      dome.bezierCurveTo(71, 23, 85, 39, 78.5, 60);
      dome.bezierCurveTo(75.5, 51, 64, 44.5, 50, 44.5);
      dome.bezierCurveTo(36, 44.5, 24.5, 51, 21.5, 60);
      dome.closePath();
      fill(dome, linear(pt(36, 22), pt(58, 64), [LILAC, VIOLET, PLUM, SHELL]));
      const g = glowStrength;
      ctx.save();
      ctx.clip(dome);
      for (let k = 0; k < 4; k += 1) {
        const y = 58 - k * 8.5;
        const peak = 31 - k * 6;
        const fold = new Path2D();
        fold.moveTo(18, y);
        fold.bezierCurveTo(26, peak, 70, peak - 4, 82, y - 3);
        stroke(fold, alpha(PITCH, 0.42), 2.2);
        ctx.save();
        ctx.translate(0, 1.5);
        stroke(fold, alpha(LILAC, 0.4), 0.9);
        ctx.restore();
      }
      fill(oval(37, 31, 10, 6), alpha(WHITE, 0.1));
      const trim = new Path2D();
      trim.moveTo(23, 55);
      trim.quadraticCurveTo(20.5, 40, 32, 32);
      trim.moveTo(77, 55);
      trim.quadraticCurveTo(79.5, 40, 68, 32);
      stroke(trim, alpha(tint, 0.32 * g), 2);
      stroke(trim, alpha(tint, 0.85 * g), 0.6);
      ctx.restore();
      const arch = new Path2D();
      arch.moveTo(22.4, 59.6);
      arch.bezierCurveTo(25.4, 51, 36, 44.5, 50, 44.5);
      arch.bezierCurveTo(64, 44.5, 74.6, 51, 77.6, 59.6);
      silver(arch, 3.4);
      const chevron = new Path2D();
      chevron.moveTo(33.5, 28.5);
      chevron.quadraticCurveTo(43, 31, 50, 40);
      chevron.quadraticCurveTo(57, 31, 66.5, 28.5);
      silver(chevron, 2.6);
      const gem = pt(50, 40.5);
      glow(gem.x, gem.y, 8 + eureka * 5 + magic * 8, alpha(tint, Math.min(0.5 * g + 0.45 * magic, 1)));
      fill(oval(gem.x, gem.y, 5.4, 5.4), radial(sub(gem, pt(1.8, 2)), 7, [WHITE, SILVER, SILVER_DARK]));
      fill(oval(gem.x, gem.y, 3.9, 3.9), PITCH);
      fill(oval(gem.x, gem.y, 3, 3), radial(sub(gem, pt(0.8, 0.8)), 3.4, [WHITE, tint, GLOW_DEEP]));
      fill(oval(gem.x - 1, gem.y - 1.1, 0.8, 0.7), alpha(WHITE, 0.95));
      const diamond = new Path2D();
      diamond.moveTo(50, 46.3);
      diamond.lineTo(52.6, 49.8);
      diamond.lineTo(50, 54);
      diamond.lineTo(47.4, 49.8);
      diamond.closePath();
      fill(diamond, linear(pt(48, 46), pt(52, 54), [WHITE, SILVER, SILVER_DARK]));
      stroke(diamond, alpha(PITCH, 0.5), 0.3);
      const inner = new Path2D();
      inner.moveTo(50, 48.2);
      inner.lineTo(51.2, 49.9);
      inner.lineTo(50, 51.8);
      inner.lineTo(48.8, 49.9);
      inner.closePath();
      fill(inner, alpha(tint, g));
      if (s.condition === "thriving") {
        const cycle = s.t % 3.4;
        if (cycle < 0.6) glint(pt(52.6, 38), 2.8 * Math.sin((cycle / 0.6) * Math.PI));
      }
    }

    function eye(c, r, spiral) {
      const g = glowStrength;
      glow(c.x, c.y, r * 1.55, alpha(tint, 0.3 * g));
      fill(oval(c.x, c.y, r * 1.13, r * 1.13), PITCH);
      stroke(oval(c.x, c.y, r, r), alpha(tint, 0.85 * g + 0.15), Math.max(r * 0.11, 0.35));
      ctx.save();
      ctx.clip(oval(c.x, c.y, r * 0.93, r * 0.93));
      if (spiral) {
        fill(oval(c.x, c.y, r, r), PITCH);
        const swirl = new Path2D();
        for (let k = 0; k <= 18; k += 1) {
          const u = k / 18;
          const a = u * Math.PI * 4 + s.reactionAge * 8;
          const x = c.x + Math.cos(a) * r * 0.8 * u;
          const y = c.y + Math.sin(a) * r * 0.8 * u;
          if (k === 0) swirl.moveTo(x, y);
          else swirl.lineTo(x, y);
        }
        stroke(swirl, tint, r * 0.14);
      } else {
        const l = look();
        const shift = pt(c.x + l.x * r * 0.16, c.y + l.y * r * 0.16);
        const iris = ctx.createRadialGradient(shift.x, shift.y + r * 0.45, 0, shift.x, shift.y + r * 0.45, r * 1.05);
        iris.addColorStop(0, css(DEEP_SEA));
        iris.addColorStop(1, css(PITCH));
        fill(oval(c.x, c.y, r, r), iris);
        fill(oval(shift.x, shift.y + r * 0.05, r * 0.58, r * 0.58), alpha(PITCH, 0.9));
        stroke(arcPath(shift, r * 0.72, 25, 155), alpha(tint, 0.55 * g + 0.1), r * 0.13);
        fill(oval(shift.x - r * 0.3, shift.y - r * 0.33, r * 0.27, r * 0.23), alpha(WHITE, 0.96));
        fill(oval(shift.x + r * 0.32, shift.y + r * 0.22, r * 0.11, r * 0.11), alpha(WHITE, 0.75));
      }
      ctx.restore();
      const closed = lid();
      if (closed > 0.02) {
        ctx.save();
        ctx.clip(oval(c.x, c.y, r * 1.02, r * 1.02));
        const drop = r * 2 * closed;
        ctx.fillStyle = css(SHELL);
        ctx.fillRect(c.x - r * 1.1, c.y - r * 1.1, r * 2.2, drop);
        const edge = new Path2D();
        edge.moveTo(c.x - r, c.y - r + drop);
        edge.lineTo(c.x + r, c.y - r + drop);
        stroke(edge, alpha(tint, 0.4 * g), Math.max(r * 0.1, 0.3));
        ctx.restore();
      }
    }

    function eyes() {
      const wide = 1 + surprise * 0.2;
      const spots = [[43.6, 66.8, 6], [56.4, 66.8, 6], [33.4, 60.4, 4.4], [66.6, 60.4, 4.4], [44.2, 55.6, 3.1], [55.8, 55.6, 3.1]];
      spots.forEach((spot, i) => eye(pt(spot[0], spot[1]), spot[2] * wide, dizzy > 0.2 && i < 2));
    }

    function mouth() {
      const c = pt(50, 76.4);
      if (s.mood === "speaking" || s.mood === "greeting") {
        const open = (0.5 + 0.5 * Math.sin(s.t * 17)) * (0.65 + 0.35 * Math.sin(s.t * 6.1));
        const hole = oval(c.x, c.y + 0.3, 2.6, 0.5 + 1.9 * open);
        fill(hole, PITCH);
        stroke(hole, alpha(tint, 0.8), 0.45);
        return;
      }
      if (surprise > 0.05) {
        stroke(oval(c.x, c.y, 1.2 + surprise, 1.2 + surprise * 1.3), alpha(tint, 0.9), 0.55);
        return;
      }
      const smile = { sick: -1.2, hungry: -0.4, peckish: 0.6 }[s.condition] ?? 1.6;
      const grin = smile + giggle * 1.2 + cheer + (s.mood === "thinking" ? -1.2 : 0);
      const line = new Path2D();
      line.moveTo(c.x - 2.6, c.y);
      line.quadraticCurveTo(c.x, c.y + grin, c.x + 2.6, c.y);
      stroke(line, alpha(tint, 0.95), 0.6);
    }

    function burst() {
      if (eureka <= 0.05) return;
      const progress = clamp01(s.reactionAge / DURATIONS.eureka);
      for (let i = 0; i < 8; i += 1) {
        const a = (i / 8) * Math.PI * 2;
        const inner = 7 + progress * 4;
        const outer = inner + 4 * eureka;
        const ray = new Path2D();
        ray.moveTo(50 + Math.cos(a) * inner, 41 + Math.sin(a) * inner);
        ray.lineTo(50 + Math.cos(a) * outer, 41 + Math.sin(a) * outer);
        stroke(ray, alpha(tint, eureka * (1 - progress * 0.7)), 0.7);
      }
    }

    function sweat() {
      const cycle = (s.t % 2.6) / 2.6;
      const y = 57 + cycle * 8;
      const drop = new Path2D();
      drop.moveTo(75, y - 2.4);
      drop.quadraticCurveTo(77.6, y + 1, 75, y + 1.4);
      drop.quadraticCurveTo(72.4, y + 1, 75, y - 2.4);
      fill(drop, alpha(GLOW_SOFT, (1 - cycle) * 0.85));
    }

    aura();
    ctx.save();
    if (landing > 0.01) {
      ctx.translate(50, 93);
      ctx.scale(1 + landing * 0.1, 1 - landing * 0.14);
      ctx.translate(-50, -93);
    }
    shadow();
    ctx.translate(50, 90);
    ctx.rotate(rad(sway));
    ctx.translate(-50, -90 + bob);
    tail();
    legs(false);
    head();
    turban();
    eyes();
    mouth();
    legs(true);
    burst();
    if (sick) sweat();
    ctx.restore();
  }

  const PAGE_LINES = [
    ["/dashboard/agents", "member", "Every agent your account can see lives here."],
    ["/dashboard/repos/new", "member", "Create, import or mirror a repository from here."],
    ["/dashboard/repos", "member", "Mirrored repositories still clone while their host sleeps."],
    ["/dashboard/settings", "member", "Profile, appearance, notifications and payouts live here."],
    ["/dashboard/apps", "member", "Install apps here, or build your own."],
    ["/dashboard", "member", "Your repositories, agents and chat are in the sidebar."],
    ["/chat", "any", "Rooms follow repositories; #general is open to everyone."],
    ["/referral", "member", "Share your link and watch the signups arrive."],
    ["/login", "guest", "Forgot your password? The reset link is here."],
    ["/signup", "guest", "A free account adds a dashboard, chat and hosting."],
    ["/features", "any", "Everything ForkMesh does, grouped by what it helps with."],
    ["/blog/", "any", "More posts and product news are on the blog."],
    ["/blog", "any", "Newest posts first; follow along with the RSS feed."],
    ["/docs", "any", "New here? Start with the Getting started guide."],
    ["/desktop", "any", "The desktop app hosts, mirrors and runs coding agents."],
    ["/mobile", "any", "The iPhone app reviews, merges and runs real Git."],
    ["/market", "any", "Extensions, worlds, avatars and themes share one package format."],
    ["/ecosystem", "any", "Pick a project to see how it works with ForkMesh."],
    ["/hackathon", "any", "Earn half the first month of every company you bring."],
    ["/status", "any", "Live health of the app, relay and core nodes."],
    ["/network", "any", "Every node that mirrors and serves repositories, on one map."],
  ];

  const GUEST_LINES = [
    "Public repositories clone with plain Git, no account needed.",
    "The desktop node hosts and mirrors your repositories.",
    "A free account gives you a dashboard and chat.",
    "New here? Getting started walks through your first run.",
    "Drag me anywhere; right-click to tuck me away.",
  ];

  const MEMBER_LINES = [
    "Agents work in worktrees and send back pull requests.",
    "Add repositories from the web, other forges or desktop.",
    "Your desktop app keeps your repositories reachable as a node.",
    "Share your referral link to bring friends along.",
    "Settings has your profile, theme and notifications.",
    "Drag me anywhere; right-click to tuck me away.",
  ];

  function normalizePath(path) {
    let clean = String(path || "/").split(/[?#]/)[0].replace(/\.html$/, "").replace(/\/index$/, "/");
    if (clean.length > 1) clean = clean.replace(/\/+$/, "");
    return clean || "/";
  }

  function pageTip(path, member) {
    const here = normalizePath(path);
    for (const [prefix, audience, line] of PAGE_LINES) {
      if (audience === "member" && !member) continue;
      if (audience === "guest" && member) continue;
      const matches = prefix.endsWith("/")
        ? here.startsWith(prefix) && here.length > prefix.length
        : here === prefix || here.startsWith(prefix + "/");
      if (matches) return line;
    }
    if (here === "/") {
      return member
        ? "Your dashboard has your repositories, agents and chat."
        : "Scroll down for features, pricing and downloads.";
    }
    return "";
  }

  function briefing(context) {
    const plural = (n, word) => `${n} ${word}${n === 1 ? "" : "s"}`;
    if (context.unread > 0) return `You have ${plural(context.unread, "unread notification")}.`;
    if (context.chatNew > 0) return `${plural(context.chatNew, "new chat message")} since you last looked.`;
    return "";
  }

  function tips(context) {
    const member = Boolean(context.account);
    const list = [briefing(context), pageTip(context.path, member), ...(member ? MEMBER_LINES : GUEST_LINES)];
    return [...new Set(list.filter(Boolean))];
  }

  function wordCount(line) {
    return (String(line).match(/\S+/g) || []).length;
  }

  const BODY = { left: 0.05, right: 0.95, top: 0.18, bottom: 0.97 };

  function footprint(x, o) {
    const b = o.body || BODY;
    const y = typeof o.floor === "function" ? o.floor(x) : Number(o.floor) || 0;
    const gap = o.gap ?? 4;
    return { left: x + b.left * o.size - gap, right: x + b.right * o.size + gap, top: y + b.top * o.size, bottom: y + b.bottom * o.size };
  }

  function footprintCover(x, o) {
    const f = footprint(x, o);
    let covered = 0;
    for (const r of o.rects || []) {
      const w = Math.min(f.right, r.right) - Math.max(f.left, r.left);
      const h = Math.min(f.bottom, r.bottom) - Math.max(f.top, r.top);
      if (w > 0.01 && h > 0.01) covered += w * h;
    }
    return covered;
  }

  function clearX(o) {
    const b = o.body || BODY;
    const gap = o.gap ?? 4;
    const lo = Math.min(o.min, o.max);
    const hi = Math.max(o.min, o.max);
    const want = clamp(Number.isFinite(o.preferred) ? o.preferred : lo, lo, hi);
    const spots = [want, lo, hi, ...(o.extra || [])];
    for (const r of o.rects || []) spots.push(r.left - b.right * o.size - gap, r.right - b.left * o.size + gap);
    let best = null;
    for (const raw of spots) {
      if (!Number.isFinite(raw)) continue;
      const x = clamp(raw, lo, hi);
      const cover = footprintCover(x, o);
      const d = Math.abs(x - want);
      if (!best || cover < best.cover - 0.01 || (Math.abs(cover - best.cover) <= 0.01 && d < best.d)) best = { x, cover, d };
    }
    return best.x;
  }

  const KINDS = ["heading", "short", "button", "link", "nav", "image", "logo", "input", "code", "card"];
  const INTERACTIONS = {
    look: { title: "Eyes on it", fits: KINDS, still: true },
    read: { title: "Reads it, eyes sweeping along", fits: ["heading", "code"] },
    wave: { title: "Waves at it", fits: ["button", "link", "nav", "logo", "image", "heading", "card"] },
    point: { title: "Points a front leg at it", fits: ["button", "link", "nav", "logo", "image", "input"] },
    thread: { title: "Shoots a silk thread to it and tugs", fits: ["heading", "image", "logo", "card", "code", "button"] },
    sparkle: { title: "Sparkles around it", fits: ["logo", "image", "heading", "card"] },
    outline: { title: "A soft glowing outline pulses around it", fits: ["button", "input", "card", "code", "link", "logo"] },
    hop: { title: "Hops toward it", fits: ["button", "link", "logo", "image", "card", "heading"] },
    cheer: { title: "Cheers at a button", fits: ["button"] },
    giggle: { title: "Giggles at a link", fits: ["link", "nav"] },
    surprise: { title: "Surprised by a picture", fits: ["image", "logo"] },
    nod: { title: "Nods along a row of links", fits: ["nav"] },
    count: { title: "Counts the words of a short heading, a dot per word", fits: ["short"] },
    dizzy: { title: "Skims a code block until dizzy", fits: ["code"] },
    type: { title: "Ponders what to type in a box", fits: ["input"] },
  };
  const BOXES = ["heading", "button", "link", "image", "logo", "input", "code", "card"];
  const GENIE_EFFECTS = {
    poof: { title: "Vanishes in a puff of smoke and reappears beside it", fits: KINDS, teleport: true },
    levitate: { title: "Floats up on a curling smoke tail", fits: KINDS },
    lamp: { title: "A golden lamp appears and its smoke curls over to it", fits: KINDS },
    wishes: { title: "Three wish orbs float to it and burst into sparkles", fits: KINDS },
    smokeRings: { title: "Blows smoke rings toward it", fits: KINDS },
    shimmer: { title: "A band of golden light sweeps across it", fits: BOXES },
    stardust: { title: "Stardust pours from the turban and settles along its top", fits: ["heading", "button", "image", "logo", "card", "code", "input"] },
    carpet: { title: "Rides a flying carpet", fits: KINDS },
    vortex: { title: "Sparks spiral into it and flash", fits: ["logo", "image", "button", "card", "heading"] },
    crystal: { title: "Gazes at it through a crystal ball", fits: KINDS },
    runes: { title: "Glowing runes circle it", fits: ["card", "code", "image", "logo", "input", "button", "heading"] },
    halo: { title: "A golden ring rises from it and hovers above", fits: ["logo", "image", "heading", "button", "card"] },
    fireflies: { title: "Glowing motes drift from the turban over to it", fits: KINDS },
    whirl: { title: "Spins round twice in a swirl of smoke", fits: KINDS },
    grow: { title: "Swells up in a cloud of smoke, then shrinks back", fits: KINDS },
    bottle: { title: "Swirls into a glowing bottle and pops back out", fits: KINDS },
    rainbow: { title: "A rainbow arcs from it to the element", fits: KINDS },
    fireworks: { title: "Golden fireworks burst above it", fits: ["heading", "button", "logo", "image", "card"] },
    bubbles: { title: "Magic bubbles float up from it and pop", fits: ["card", "image", "code", "input", "heading", "button"] },
    glitter: { title: "Glitter drifts down over it", fits: ["heading", "image", "logo", "card", "button"] },
    jewelBeam: { title: "The turban jewel shines a beam on it", fits: KINDS },
    ripple: { title: "Rings of light spread out from it", fits: ["button", "link", "nav", "logo", "image", "input"] },
    moons: { title: "Three moons circle the turban, then fly to it", fits: KINDS },
    constellation: { title: "Stars appear around it and join up", fits: ["image", "card", "code", "heading", "logo"] },
    crescent: { title: "A crescent moon rises above it", fits: ["heading", "image", "logo", "card"] },
    mirage: { title: "A wavy ghost of its outline drifts up", fits: ["image", "card", "logo", "button", "code", "input"] },
    coins: { title: "Gold coins tumble down beside it", fits: ["button", "card", "image", "logo", "heading"] },
    flourish: { title: "A curling trail of light writes itself over to it", fits: KINDS },
    incense: { title: "Wisps of incense rise around it, aura violet", fits: KINDS },
    whirlwind: { title: "A little whirlwind of smoke circles it and scatters", fits: KINDS },
  };
  for (const name in GENIE_EFFECTS) INTERACTIONS[name] = { ...GENIE_EFFECTS[name], genie: true };

  function elementKinds(info) {
    const tag = String(info.tag || "").toLowerCase();
    const role = String(info.role || "").toLowerCase();
    const type = String(info.type || "text").toLowerCase();
    const label = String(info.label || "").toLowerCase();
    const kinds = [];
    if (label.includes("logo")) kinds.push("logo");
    if (/^h[1-3]$/.test(tag)) {
      kinds.push("heading");
      if (info.words >= 1 && info.words <= 8) kinds.push("short");
    }
    if (tag === "button" || role === "button" || (tag === "input" && /^(button|submit|reset|checkbox|radio)$/.test(type))) {
      kinds.push("button");
    } else if (tag === "textarea" || (tag === "input" && /^(text|search|email|url|tel|number|password)$/.test(type))) {
      kinds.push("input");
    }
    if (tag === "a") kinds.push(...(info.rowLinks >= 3 ? ["link", "nav"] : ["link"]));
    if (["img", "svg", "picture", "video", "canvas"].includes(tag)) kinds.push("image");
    if (tag === "pre" || tag === "code") kinds.push("code");
    if (tag === "article" || /(^|[\s_-])card/.test(label)) kinds.push("card");
    return [...new Set(kinds)];
  }

  function shuffled(names, rand) {
    const list = names.slice();
    for (let i = list.length - 1; i > 0; i -= 1) {
      const j = Math.floor(rand() * (i + 1));
      [list[i], list[j]] = [list[j], list[i]];
    }
    return list;
  }

  function drawInteraction(bag, kinds, options = {}) {
    const rand = options.rand || Math.random;
    const usable = (name) => {
      const play = INTERACTIONS[name];
      return Boolean(play) && (!options.still || play.still) && play.fits.some((kind) => kinds.includes(kind));
    };
    for (let pass = 0; pass < 2; pass += 1) {
      if (pass === 1 || !bag.length) bag.splice(0, bag.length, ...shuffled(Object.keys(INTERACTIONS), rand));
      const index = bag.findIndex(usable);
      if (index >= 0) return bag.splice(index, 1)[0];
    }
    return "";
  }

  const OFF_KEY = "forkmesh.genie.off";
  const AWAY_KEY = "forkmesh.genie.away";
  const OFF_COOKIE = "forkmesh_genie";
  const FLIGHT = {
    gravity: 2200, drag: 0.00055, maxSpeed: 4200, settle: 240,
    floorBounce: 0.3, wallBounce: 0.42, floorGrip: 0.78,
  };

  function flightStep(state, dt) {
    const span = clamp(Number(dt) || 0, 0, 0.05);
    const x = Number(state.x) || 0;
    const y = Number(state.y) || 0;
    let beforeX = Number(state.vx) || 0;
    let beforeY = Number(state.vy) || 0;
    const launchSpeed = Math.hypot(beforeX, beforeY);
    if (launchSpeed > FLIGHT.maxSpeed) {
      beforeX *= FLIGHT.maxSpeed / launchSpeed;
      beforeY *= FLIGHT.maxSpeed / launchSpeed;
    }
    let vx = beforeX;
    let vy = beforeY + FLIGHT.gravity * span;
    const slow = (velocity) => velocity / (1 + FLIGHT.drag * Math.abs(velocity) * span);
    vx = slow(vx);
    vy = slow(vy);
    const speed = Math.hypot(vx, vy);
    if (speed > FLIGHT.maxSpeed) {
      vx *= FLIGHT.maxSpeed / speed;
      vy *= FLIGHT.maxSpeed / speed;
    }
    return { x: x + (beforeX + vx) * span / 2, y: y + (beforeY + vy) * span / 2, vx, vy };
  }

  function landingVelocity(impact) {
    return impact < FLIGHT.settle ? 0 : -impact * FLIGHT.floorBounce;
  }

  function offFrom(cookie, local) {
    const entry = String(cookie || "").split(";").map((part) => part.trim())
      .find((part) => part.startsWith(OFF_COOKIE + "="));
    const value = entry ? entry.slice(OFF_COOKIE.length + 1) : "";
    if (value === "off") return true;
    if (value === "on") return false;
    return local === "1";
  }

  function offCookie(off, hostname, secure) {
    const host = String(hostname || "").toLowerCase();
    const shared = host === "forkmesh.com" || host.endsWith(".forkmesh.com") ? "; Domain=.forkmesh.com" : "";
    return `${OFF_COOKIE}=${off ? "off" : "on"}; Path=/; Max-Age=31536000; SameSite=Lax${shared}${secure ? "; Secure" : ""}`;
  }

  const api = {
    tips, pageTip, briefing, normalizePath, wordCount, paint: painter,
    clearX, footprintCover, elementKinds, drawInteraction, interactions: INTERACTIONS, genieEffects: Object.keys(GENIE_EFFECTS), body: BODY,
    offFrom, offCookie, flight: FLIGHT, flightStep, landingVelocity,
  };
  window.ForkMeshGenie = api;

  let stop = null;
  const keyOf = (name, key) => {
    try {
      return window[name]?.getItem(key) ?? null;
    } catch (_) {
      return null;
    }
  };
  const setKey = (name, key, value) => {
    try {
      if (value === null) window[name]?.removeItem(key);
      else window[name]?.setItem(key, value);
    } catch (_) {}
  };
  const cookieText = () => {
    try {
      return document.cookie;
    } catch (_) {
      return "";
    }
  };
  const isOff = () => offFrom(cookieText(), keyOf("localStorage", OFF_KEY));
  const hidden = () => isOff() || keyOf("sessionStorage", AWAY_KEY) === "1";
  const announce = () => {
    try {
      window.dispatchEvent(new CustomEvent("forkmesh:genie", { detail: { off: isOff(), hidden: hidden() } }));
    } catch (_) {}
  };

  function setOff(off) {
    setKey("localStorage", OFF_KEY, off ? "1" : null);
    if (!off) setKey("sessionStorage", AWAY_KEY, null);
    try {
      document.cookie = offCookie(off, location.hostname, location.protocol === "https:");
    } catch (_) {}
  }

  let instancePlugin = "";
  function instanceAllows() {
    if (instancePlugin) return instancePlugin === "on";
    if (!document.querySelector?.("script[data-instance-brand]")) {
      instancePlugin = "on";
      return true;
    }
    instancePlugin = "asking";
    fetch("/api/instance", { credentials: "same-origin", headers: { accept: "application/json" } })
      .then((response) => (response.ok ? response.json() : null))
      .then((data) => {
        const plugins = data && Array.isArray(data.plugins) ? data.plugins : [];
        instancePlugin = plugins.some((plugin) => plugin && plugin.id === "genie" && plugin.enabled) ? "on" : "off";
        if (instancePlugin === "on") boot();
      })
      .catch(() => {
        instancePlugin = "off";
      });
    return false;
  }

  function allowed() {
    if (typeof document === "undefined" || !document.documentElement) return false;
    if (document.documentElement.dataset.genie === "off") return false;
    return !hidden() && instanceAllows();
  }

  function boot() {
    if (stop) return;
    if (document.readyState !== "complete") {
      window.addEventListener("load", () => {
        if (window.requestIdleCallback) window.requestIdleCallback(boot, { timeout: 2000 });
        else setTimeout(boot, 200);
      }, { once: true });
      return;
    }
    if (!allowed()) return;
    stop = start() || null;
  }

  api.isOff = isOff;
  api.hidden = hidden;
  api.shown = () => Boolean(stop);
  api.hide = () => {
    setOff(true);
    if (stop) stop();
    announce();
  };
  api.show = () => {
    setOff(false);
    boot();
    announce();
  };

  if (typeof document === "undefined" || !document.documentElement) return;

  const STORAGE = {
    spot: "forkmesh.genie.spot",
    still: "forkmesh.genie.still",
    parked: "forkmesh.genie.parked",
    inbox: "forkmesh.genie.inbox",
  };
  const STEP = 1 / 120;
  const reducedMotion = window.matchMedia?.("(prefers-reduced-motion: reduce)").matches;

  const read = (store, key) => {
    try {
      return store.getItem(key);
    } catch (_) {
      return null;
    }
  };
  const write = (store, key, value) => {
    try {
      if (value === null) store.removeItem(key);
      else store.setItem(key, value);
    } catch (_) {}
  };

  const STYLE = `
.fm-genie{position:fixed;left:0;top:0;width:var(--fm-genie-size);height:var(--fm-genie-size);transform-origin:50% 96%;z-index:950;padding:0;margin:0;border:0;background:none;cursor:grab;touch-action:none;-webkit-tap-highlight-color:transparent;user-select:none;-webkit-user-select:none}
.fm-genie:focus-visible{outline:2px solid rgb(64,242,219);outline-offset:2px;border-radius:50%}
.fm-genie.is-dragging{cursor:grabbing}
.fm-genie canvas{display:block;width:100%;height:100%;pointer-events:none}
.fm-genie-bubble{position:fixed;left:0;top:0;z-index:951;box-sizing:border-box;width:max-content;max-width:min(380px,calc(100vw - 16px));padding:9px 13px;border-radius:14px;border:1px solid rgba(64,242,219,.35);background:rgba(14,14,30,.96);color:#eef0f7;font:14px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;box-shadow:0 14px 36px rgba(0,0,0,.4),0 0 0 1px rgba(0,0,0,.25);text-align:left}
.fm-genie-bubble[hidden]{display:none}
.fm-genie-bubble p{margin:0;color:#eef0f7;overflow-wrap:anywhere}
.fm-genie-row{display:flex;flex-wrap:wrap;align-items:center;gap:8px;margin-top:10px}
.fm-genie-row a,.fm-genie-row button{font:inherit;font-size:12.5px;line-height:1;padding:7px 11px;border-radius:999px;border:1px solid rgba(255,255,255,.18);background:transparent;color:#eef0f7;cursor:pointer;text-decoration:none}
.fm-genie-row a:hover,.fm-genie-row button:hover{border-color:rgba(64,242,219,.8)}
.fm-genie-fx{position:fixed;left:0;top:0;width:100vw;height:100vh;z-index:949;pointer-events:none;overflow:visible}
@media print{.fm-genie,.fm-genie-bubble,.fm-genie-fx{display:none!important}}
`;
  const FX_DEFS = `<defs>
<radialGradient id="fm-genie-smoke"><stop offset="0" stop-color="rgb(190,160,235)" stop-opacity=".7"/><stop offset=".55" stop-color="rgb(143,102,209)" stop-opacity=".38"/><stop offset="1" stop-color="rgb(54,33,97)" stop-opacity="0"/></radialGradient>
<radialGradient id="fm-genie-orb"><stop offset="0" stop-color="#fff"/><stop offset=".35" stop-color="rgb(64,242,219)"/><stop offset="1" stop-color="rgb(64,242,219)" stop-opacity="0"/></radialGradient>
<radialGradient id="fm-genie-mote"><stop offset="0" stop-color="rgb(255,246,200)"/><stop offset=".4" stop-color="rgb(255,206,84)" stop-opacity=".8"/><stop offset="1" stop-color="rgb(255,206,84)" stop-opacity="0"/></radialGradient>
<radialGradient id="fm-genie-bubble" fx=".32" fy=".3"><stop offset="0" stop-color="#fff" stop-opacity=".85"/><stop offset=".22" stop-color="#fff" stop-opacity="0"/><stop offset=".78" stop-color="rgb(143,102,209)" stop-opacity=".18"/><stop offset="1" stop-color="rgb(64,242,219)" stop-opacity=".8"/></radialGradient>
<radialGradient id="fm-genie-ball" fx=".35" fy=".3"><stop offset="0" stop-color="#fff" stop-opacity=".9"/><stop offset=".3" stop-color="rgb(190,160,235)" stop-opacity=".55"/><stop offset="1" stop-color="rgb(54,33,97)" stop-opacity=".8"/></radialGradient>
<linearGradient id="fm-genie-gold" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="rgb(255,240,180)"/><stop offset=".45" stop-color="rgb(255,206,84)"/><stop offset="1" stop-color="rgb(176,120,30)"/></linearGradient>
<linearGradient id="fm-genie-band" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="rgb(255,206,84)" stop-opacity="0"/><stop offset=".4" stop-color="rgb(255,206,84)" stop-opacity=".45"/><stop offset=".5" stop-color="#fff" stop-opacity=".7"/><stop offset=".6" stop-color="rgb(255,206,84)" stop-opacity=".45"/><stop offset="1" stop-color="rgb(255,206,84)" stop-opacity="0"/></linearGradient>
<clipPath id="fm-genie-clip"><rect/></clipPath>
</defs>`;

  function start() {
    if (document.querySelector(".fm-genie")) return null;
    const abort = new AbortController();
    const live = { signal: abort.signal };
    const style = document.createElement("style");
    style.textContent = STYLE;
    document.head.appendChild(style);

    const host = document.createElement("button");
    host.type = "button";
    host.className = "fm-genie";
    host.setAttribute("aria-label", "Genie: click for a tip, drag to move.");
    host.title = "Genie: click for a tip, drag me anywhere";
    const canvas = document.createElement("canvas");
    host.appendChild(canvas);
    const ctx = canvas.getContext("2d");

    const bubble = document.createElement("div");
    bubble.className = "fm-genie-bubble";
    bubble.setAttribute("role", "status");
    bubble.setAttribute("aria-live", "polite");
    bubble.hidden = true;

    document.body.appendChild(host);
    document.body.appendChild(bubble);

    let size = 78;
    let width = window.innerWidth;
    let height = window.innerHeight;
    const s = {
      t: 0, mood: "idle", reaction: "", reactionAge: 0, vitality: 0.85, condition: "happy",
      stridePhase: 0, walking: 0, facing: 0, air: 0, land: 0, gaze: null, magic: 0,
    };
    const spell = { scale: 1, spin: 0, turn: 0, fade: 1 };
    const body = { x: 0, y: 0, vx: 0, vy: 0, mode: "stand", targetX: null };
    const savedSpot = read(localStorage, STORAGE.spot);
    let home = savedSpot === null ? NaN : Number(savedSpot);
    if (!Number.isFinite(home) || home < 0 || home > 1) home = 0.94;
    let still = read(localStorage, STORAGE.still) === "1";
    let parked = read(localStorage, STORAGE.parked) === "1";
    let nextWander = performance.now() + 5000 + Math.random() * 8000;
    let pointer = null;
    let press = null;
    let furniture = { at: -Infinity, rects: [] };
    let gone = false;
    let tipIndex = 0;
    let bubbleTimer = 0;
    let bubbleHovered = false;
    let lastFrame = 0;
    let lastDraw = 0;
    let accumulator = 0;
    let raf = 0;
    let context = { path: location.pathname, account: "", unread: null, chatNew: 0 };
    let fxLayer = null;
    let play = null;
    const bag = [];
    let lift = 0;
    let parkLeft = false;
    let keep = { at: -Infinity, rects: [] };
    let lastGuard = 0;
    let scrollTimer = 0;

    function measure() {
      width = window.innerWidth;
      height = window.innerHeight;
      size = width < 520 ? 62 : 78;
      host.style.setProperty("--fm-genie-size", size + "px");
      const ratio = Math.min(window.devicePixelRatio || 1, 3);
      canvas.width = Math.round(size * ratio);
      canvas.height = Math.round(size * ratio);
      furniture.at = -Infinity;
    }

    function homeX() {
      if (parked) return parkLeft ? -size * 0.58 : width - size * 0.42;
      return home * Math.max(width - size, 0);
    }

    function ownNode(node) {
      return node === host || node === bubble || host.contains(node) || bubble.contains(node)
        || (fxLayer !== null && (node === fxLayer || fxLayer.contains(node)));
    }

    function furnitureRects() {
      const now = performance.now();
      const busy = body.mode !== "stand" || body.targetX !== null;
      if (now - furniture.at < (busy ? 300 : 1500)) return furniture.rects;
      const checked = new Set();
      const rects = [];
      if (document.elementsFromPoint) {
        for (let fx = 4; fx < width; fx += 32) {
          for (const element of document.elementsFromPoint(fx, height - 3)) {
            if (ownNode(element) || element === document.body || element === document.documentElement) continue;
            let node = element;
            for (let depth = 0; node && node !== document.body && depth < 8; depth += 1) {
              if (checked.has(node)) break;
              checked.add(node);
              const position = getComputedStyle(node).position;
              if (position === "fixed" || position === "sticky") {
                const rect = node.getBoundingClientRect();
                if (rect.height > 0 && rect.height < height * 0.45 && rect.bottom >= height - 4) rects.push(rect);
                break;
              }
              node = node.parentElement;
            }
          }
        }
      }
      furniture = { at: now, rects };
      return rects;
    }

    function furnitureTop(x, below) {
      let top = height;
      for (const rect of furnitureRects()) {
        if (rect.right < x + size * 0.3 || rect.left > x + size * 0.7) continue;
        if (below !== undefined && rect.top < below - 1) continue;
        top = Math.min(top, rect.top);
      }
      return Math.max(top, size + 8);
    }

    const KEEP = [
      "a", "button", "input", "select", "textarea", "summary", "label[for]",
      "[role=button]", "[role=link]", "[role=tab]", "[role=menuitem]", "[role=checkbox]", "[role=switch]",
      "[tabindex]:not([tabindex=\"-1\"])", "[onclick]",
      "img", "svg", "picture", "video", "canvas",
      "[class*=logo i]", "[id*=logo i]", "[alt*=logo i]",
    ].join(",");
    const PLAYABLE = "h1,h2,h3,button,a,img,svg,picture,video,input,textarea,[role=button],pre,[class*=logo i],[id*=logo i],[class*=card i],article";
    const SVG_NS = "http://www.w3.org/2000/svg";
    const LEAVES = /^(a|button|input|textarea|img|svg|picture|video|pre|h1|h2|h3)$/i;

    function hostTop(x) {
      return furnitureTop(x) - size;
    }

    function firstHit(x, y) {
      if (!document.elementsFromPoint) return null;
      for (const element of document.elementsFromPoint(x, y)) {
        if (ownNode(element)) continue;
        if (element === document.body || element === document.documentElement) return null;
        return element;
      }
      return null;
    }

    function scanKeep(x0, x1, step, rows) {
      const found = new Set();
      const searched = new Set();
      for (let fx = Math.max(x0, 1); fx <= Math.min(x1, width - 1); fx += step) {
        for (const fy of rows(fx)) {
          if (fy < 0 || fy >= height) continue;
          const hit = firstHit(fx, fy);
          let node = hit;
          for (let depth = 0; node && node !== document.body && depth < 10; depth += 1) {
            if (node.matches && node.matches(KEEP)) found.add(node);
            node = node.parentElement;
          }
          if (!hit || searched.has(hit) || !hit.querySelectorAll) continue;
          searched.add(hit);
          if (hit.getBoundingClientRect().height > height * 0.5) continue;
          let taken = 0;
          for (const inner of hit.querySelectorAll(KEEP)) {
            if (taken >= 40) break;
            taken += 1;
            found.add(inner);
          }
        }
      }
      const limit = width * height * 0.25;
      const rects = [];
      for (const element of found) {
        const outer = element.getBoundingClientRect();
        if (!outer.width || !outer.height || outer.width * outer.height > limit) continue;
        const boxes = element.getClientRects();
        for (let i = 0; i < Math.min(boxes.length, 6); i += 1) {
          const r = boxes[i];
          if (r.width && r.height) rects.push({ left: r.left, right: r.right, top: r.top, bottom: r.bottom });
        }
      }
      return rects;
    }

    function keepRects() {
      const now = performance.now();
      if (now - keep.at < 1000) return keep.rects;
      keep = {
        at: now,
        rects: scanKeep(0, width, 20, (fx) => {
          const top = hostTop(clamp(fx - size / 2, 0, Math.max(width - size, 0)));
          return [top + size * 0.24, top + size * 0.48, top + size * 0.72, top + size * 0.93];
        }),
      };
      return keep.rects;
    }

    function coveredHere() {
      const rects = scanKeep(body.x + size * BODY.left - 4, body.x + size * BODY.right + 4, 10,
        () => [body.y + size * 0.24, body.y + size * 0.48, body.y + size * 0.72, body.y + size * 0.93]);
      return footprintCover(body.x, { size, rects, floor: body.y }) > 0;
    }

    function restSpot(preferred, extra = []) {
      const edges = [];
      for (const r of furniture.rects) edges.push(r.left - size * 0.7 - 1, r.right - size * 0.3 + 1);
      return clearX({
        min: 0,
        max: Math.max(width - size, 0),
        preferred,
        size,
        rects: extra.length ? keepRects().concat(extra) : keepRects(),
        floor: hostTop,
        extra: edges,
      });
    }

    function chooseParkSide() {
      const rects = keepRects();
      const cover = (x) => footprintCover(x, { size, rects, floor: hostTop(clamp(x, 0, Math.max(width - size, 0))) });
      const right = cover(width - size * 0.42);
      parkLeft = right > 0 && cover(-size * 0.58) < right;
    }

    function keepClear() {
      if (gone || document.hidden || press || body.mode !== "stand") return false;
      const from = body.targetX !== null ? body.targetX : body.x;
      let spot;
      if (parked) {
        chooseParkSide();
        spot = homeX();
      } else {
        spot = restSpot(from);
      }
      if (Math.abs(spot - from) <= 1) return false;
      body.targetX = spot;
      wake();
      return true;
    }

    function setAttrs(node, attrs) {
      for (const key in attrs) {
        const value = attrs[key];
        node.setAttribute(key, typeof value === "number" ? String(Math.round(value * 100) / 100) : value);
      }
    }

    function fxNode(tag, attrs, parent) {
      if (!fxLayer) {
        fxLayer = document.createElementNS(SVG_NS, "svg");
        fxLayer.setAttribute("class", "fm-genie-fx");
        fxLayer.setAttribute("aria-hidden", "true");
        fxLayer.innerHTML = FX_DEFS;
        document.body.appendChild(fxLayer);
      }
      const node = document.createElementNS(SVG_NS, tag);
      setAttrs(node, attrs);
      (parent || fxLayer).appendChild(node);
      return node;
    }

    function rowLinks(element) {
      const row = element.closest("nav,ul,ol,menu,[role=navigation],[role=menubar],[role=tablist]") || element.parentElement;
      if (!row) return [element];
      const top = element.getBoundingClientRect().top;
      const links = [];
      let checked = 0;
      for (const link of row.querySelectorAll("a")) {
        if (links.length >= 6 || checked >= 40) break;
        checked += 1;
        const r = link.getBoundingClientRect();
        if (r.width && Math.abs(r.top - top) < 8 && r.right > 0 && r.left < width) links.push([r.left, link]);
      }
      if (!links.some((entry) => entry[1] === element)) return [element];
      return links.sort((a, b) => a[0] - b[0]).map((entry) => entry[1]);
    }

    function wordBoxes(element, cap) {
      const boxes = [];
      const walker = document.createTreeWalker(element, NodeFilter.SHOW_TEXT);
      let nodes = 0;
      for (let node = walker.nextNode(); node && boxes.length < cap && nodes < 40; node = walker.nextNode()) {
        nodes += 1;
        const words = /\S+/g;
        let match;
        while ((match = words.exec(node.data)) && boxes.length < cap) {
          const range = document.createRange();
          range.setStart(node, match.index);
          range.setEnd(node, match.index + match[0].length);
          const r = range.getBoundingClientRect();
          if (r.width) boxes.push({ left: r.left, top: r.top, width: r.width, height: r.height, bottom: r.bottom });
        }
      }
      return boxes;
    }

    function gather(stay) {
      const seen = new Set();
      const found = [];
      const reach = Math.max(420, width * 0.5);
      const cx = body.x + size / 2;
      const consider = (candidate) => {
        let element = candidate;
        const own = [element.getAttribute("class"), element.id, element.getAttribute("alt"), element.getAttribute("aria-label")].join(" ");
        if (/^(svg|img)$/i.test(element.tagName) && !/logo/i.test(own)) {
          const owner = element.parentElement && element.parentElement.closest("a,button,[role=button]");
          if (owner) element = owner;
        }
        if (seen.has(element) || ownNode(element)) return;
        seen.add(element);
        const r = element.getBoundingClientRect();
        if (r.width < 10 || r.height < 8 || r.bottom <= 0 || r.top >= height || r.right <= 0 || r.left >= width) return;
        if (r.width * r.height > width * height * 0.5) return;
        if (!stay && Math.abs(r.left + r.width / 2 - cx) > reach) return;
        const tag = element.tagName.toLowerCase();
        const kinds = elementKinds({
          tag,
          role: element.getAttribute("role"),
          type: element.getAttribute("type"),
          label: [element.getAttribute("class"), element.id, element.getAttribute("alt"), element.getAttribute("aria-label")].join(" "),
          words: /^h[1-3]$/.test(tag) ? (element.textContent.match(/\S+/g) || []).length : 0,
          rowLinks: tag === "a" ? rowLinks(element).length : 0,
        });
        if (kinds.length) found.push({ element, kinds, rect: r });
      };
      const searched = new Set();
      for (let j = 0; j < 12 && found.length < 32; j += 1) {
        for (let i = 0; i < 8 && found.length < 32; i += 1) {
          const hit = firstHit((width * (i + 0.5)) / 8, (height * (j + 0.5)) / 12);
          if (!hit || !hit.closest) continue;
          const match = hit.closest(PLAYABLE);
          if (match) consider(match);
          const scope = match || hit;
          if (searched.has(scope) || (match && LEAVES.test(match.tagName))) continue;
          searched.add(scope);
          const box = scope.getBoundingClientRect();
          if (box.width * box.height > width * height * 0.5) continue;
          let taken = 0;
          for (const inner of scope.querySelectorAll(PLAYABLE)) {
            if (taken >= 6 || found.length >= 32) break;
            taken += 1;
            consider(inner);
          }
        }
      }
      return found;
    }

    function playSpot(r) {
      const cx = body.x + size / 2;
      const floorTop = furnitureTop(clamp(r.left + r.width / 2 - size / 2, 0, Math.max(width - size, 0)));
      const atFloor = r.bottom > floorTop - size * (1 - BODY.top);
      let preferred;
      if (atFloor) {
        preferred = cx < r.left + r.width / 2 ? r.left - size * BODY.right - 8 : r.right - size * BODY.left + 8;
      } else {
        const under = r.width > 28 ? clamp(cx, r.left + 14, r.right - 14) : r.left + r.width / 2;
        preferred = under - size / 2;
      }
      return restSpot(preferred, atFloor ? [{ left: r.left, right: r.right, top: r.top, bottom: r.bottom }] : []);
    }

    function startPlay(stay, forced) {
      if (forced && (!INTERACTIONS[forced] || (stay && !INTERACTIONS[forced].still))) return "";
      const found = gather(stay);
      if (!found.length) return "";
      const name = forced || drawInteraction(bag, [...new Set(found.flatMap((c) => c.kinds))], { still: stay });
      const fitting = found.filter((c) => INTERACTIONS[name] && INTERACTIONS[name].fits.some((k) => c.kinds.includes(k)));
      if (!fitting.length) return "";
      const pick = fitting[Math.floor(Math.random() * fitting.length)];
      play = { name, element: pick.element, rect: pick.rect, phase: "walk", age: 0, duration: 0, done: {}, data: {}, gaze: null, spot: null };
      if (!stay && INTERACTIONS[name].teleport) {
        play.spot = playSpot(pick.rect);
      } else if (!stay) {
        const spot = playSpot(pick.rect);
        if (Math.abs(spot - body.x) > 2) body.targetX = spot;
      }
      wake();
      return name;
    }

    const center = (r) => pt(r.left + r.width / 2, r.top + r.height / 2);
    const nearestOn = (r, p, inset) => pt(
      clamp(p.x, r.left + inset, Math.max(r.left + inset, r.right - inset)),
      clamp(p.y, r.top + inset, Math.max(r.top + inset, r.bottom - inset)),
    );
    const once = (p, key, at, fn) => {
      if (p.age >= at && !p.done[key]) {
        p.done[key] = true;
        fn();
      }
    };
    const SPARK = "M0 -1L.22 -.22L1 0L.22 .22L0 1L-.22 .22L-1 0L-.22 -.22Z";

    function gazeAt(p, target) {
      const ex = body.x + size / 2;
      const ey = body.y - lift + size * 0.62;
      const d = Math.max(Math.hypot(target.x - ex, target.y - ey), 1);
      const k = Math.min(d / 90, 1) / d;
      p.gaze = { x: (target.x - ex) * k, y: (target.y - ey) * k };
    }

    function glance(p) {
      gazeAt(p, center(p.rect));
    }

    const GOLD = "rgb(255,206,84)";
    const TEAL = "rgb(64,242,219)";
    const SPARK_COLORS = [GOLD, TEAL, "rgb(190,160,235)", "rgb(255,255,255)"];
    const RAINBOW = ["rgb(255,99,132)", "rgb(255,159,64)", "rgb(255,206,84)", "rgb(75,192,120)", "rgb(64,242,219)", "rgb(143,102,209)"];
    const RUNES = [
      "M0 -5L4.5 3.5H-4.5Z", "M-4 -4L4 4M4 -4L-4 4M0 -5V5", "M-4 0A4 4 0 1 0 4 0M0 -5V2",
      "M-4 -4H4L-4 4H4", "M0 -5L5 0L0 5L-5 0ZM0 -2V2", "M-5 2Q0 -8 5 2M-3 5H3",
    ];
    const rnd = (lo, hi) => lo + Math.random() * (hi - lo);
    const ease = (v) => {
      const k = clamp01(v);
      return k * k * (3 - 2 * k);
    };
    const swell = (age, a, b, c, d) => ease((age - a) / (b - a)) * (1 - ease((age - c) / (d - c)));
    const curve = (a, c, b, u) => add(add(scale(a, (1 - u) * (1 - u)), scale(c, 2 * (1 - u) * u)), scale(b, u * u));
    const f1 = (v) => v.toFixed(1);
    const spiderAt = (u, v) => pt(body.x + (size * u) / 100, body.y - lift + size * (1 - GROUND / 100) + (size * v) / 100);
    const jewel = () => spiderAt(50, 40.5);
    const crown = () => spiderAt(50, 22);
    const towards = (p) => (center(p.rect).x < body.x + size / 2 ? -1 : 1);
    const fadeOut = (age, from, span) => 1 - ease((age - from) / span);

    function part(p, o) {
      const circle = !o.d;
      const node = fxNode(circle ? "circle" : "path", circle ? { fill: o.fill, opacity: 0 } : { d: o.d, fill: o.fill, opacity: 0 });
      const q = {
        x: 0, y: 0, vx: 0, vy: 0, g: 0, r0: 2, r1: 2, start: 0, life: 1, spin: 0, sway: 0, freq: 3,
        phase: Math.random() * 6.3, fade: "sin", peak: 1, from: "page", ...o, node, circle, hidden: true,
      };
      (p.data.parts = p.data.parts || []).push(q);
      return q;
    }

    function stepParts(p) {
      for (const q of p.data.parts) {
        const tau = p.age - q.start;
        const t = tau / q.life;
        if (t < 0 || t > 1) {
          if (!q.hidden) q.node.setAttribute("opacity", "0");
          q.hidden = true;
          continue;
        }
        q.hidden = false;
        const o = q.from === "target" ? pt(p.rect.left, p.rect.top) : q.from === "spider" ? spiderAt(0, 0) : pt(0, 0);
        const x = o.x + q.x + q.vx * tau + q.sway * Math.sin(tau * q.freq + q.phase);
        const y = o.y + q.y + q.vy * tau + 0.5 * q.g * tau * tau;
        const r = q.r0 + (q.r1 - q.r0) * t;
        const shape = q.fade === "out" ? clamp01(t / 0.12) * (1 - t)
          : q.fade === "pop" ? clamp01(t / 0.1) * clamp01((1 - t) / 0.08) : Math.sin(Math.PI * t);
        if (q.circle) setAttrs(q.node, { cx: x, cy: y, r, opacity: q.peak * shape });
        else setAttrs(q.node, { opacity: q.peak * shape, transform: `translate(${f1(x)} ${f1(y)}) rotate(${Math.round(q.spin * tau)}) scale(${r.toFixed(2)})` });
      }
    }

    function puff(p, x, y, n, o = {}) {
      const big = o.big || 1;
      for (let i = 0; i < n; i += 1) {
        const a = (i / n) * Math.PI * 2 + rnd(-0.3, 0.3);
        const speed = rnd(20, 46) * big;
        part(p, {
          fill: "url(#fm-genie-smoke)", x: x + Math.cos(a) * 4, y: y + Math.sin(a) * 3,
          vx: Math.cos(a) * speed, vy: Math.sin(a) * speed * 0.7 - 14, r0: rnd(5, 9) * big, r1: rnd(14, 22) * big,
          start: (o.start || 0) + rnd(0, 0.08), life: rnd(0.7, 1.1), fade: "out", peak: 0.95,
        });
      }
    }

    function sparks(p, x, y, n, o = {}) {
      for (let i = 0; i < n; i += 1) {
        const a = (i / n) * Math.PI * 2 + rnd(-0.2, 0.2);
        const speed = rnd(40, 90) * (o.spread || 1);
        part(p, {
          d: SPARK, fill: o.fill || SPARK_COLORS[i % SPARK_COLORS.length], x, y, vx: Math.cos(a) * speed, vy: Math.sin(a) * speed,
          g: o.g || 0, r0: rnd(3, 5), r1: 0.5, spin: rnd(-200, 200), start: o.start || 0, life: o.life || 0.8, fade: "out",
          from: o.from || "page",
        });
      }
    }

    function wavyBox(left, top, w, h, amp, phase) {
      const points = [];
      const nx = Math.max(2, Math.round(w / 10));
      const ny = Math.max(2, Math.round(h / 10));
      for (let i = 0; i < nx; i += 1) points.push([left + (w * i) / nx, top, 0, -1]);
      for (let i = 0; i < ny; i += 1) points.push([left + w, top + (h * i) / ny, 1, 0]);
      for (let i = 0; i < nx; i += 1) points.push([left + w - (w * i) / nx, top + h, 0, 1]);
      for (let i = 0; i < ny; i += 1) points.push([left, top + h - (h * i) / ny, -1, 0]);
      return points.map(([x, y, a, b], i) => {
        const k = Math.sin(i * 0.9 + phase) * amp;
        return `${i ? "L" : "M"}${f1(x + a * k)} ${f1(y + b * k)}`;
      }).join("") + "Z";
    }

    function smokeTrail(p, from, to, every, until, o = {}) {
      if (p.data.next === undefined) p.data.next = o.after || 0;
      while (p.data.next <= p.age && p.data.next < until) {
        const travel = o.travel || 1.3;
        part(p, {
          fill: "url(#fm-genie-smoke)", x: from.x, y: from.y, vx: (to.x - from.x) / travel, vy: (to.y - from.y) / travel,
          sway: o.sway || 10, freq: 4, r0: o.r0 || 3, r1: o.r1 || 13, start: p.data.next, life: travel, fade: "out", peak: 0.85,
        });
        p.data.next += every;
      }
    }

    const ACTS = {
      look: {
        duration: 2.4,
        tick(p) {
          glance(p);
        },
      },
      read: {
        duration: 2.9,
        tick(p) {
          const r = p.rect;
          const u = Math.min(p.age / 1.2, 1.999);
          gazeAt(p, pt(r.left + r.width * (u % 1), r.top + r.height * (u < 1 ? 0.35 : 0.65)));
          once(p, "nod", 2.4, () => react("nod"));
        },
      },
      wave: {
        duration: 2,
        tick(p) {
          glance(p);
          once(p, "wave", 0, () => react("wave"));
        },
      },
      point: {
        duration: 2,
        tick(p) {
          glance(p);
          s.pointSide = center(p.rect).x < body.x + size / 2 ? -1 : 1;
          s.point = clamp01(p.age / 0.25) * clamp01((p.duration - p.age) / 0.3);
        },
      },
      thread: {
        duration: 2.4,
        begin(p) {
          p.data.glow = fxNode("path", { fill: "none", stroke: "rgba(64,242,219,.3)", "stroke-width": 4, "stroke-linecap": "round" });
          p.data.line = fxNode("path", { fill: "none", stroke: "rgba(230,236,248,.95)", "stroke-width": 1.3, "stroke-linecap": "round" });
          p.data.knot = fxNode("circle", { r: 2.6, fill: "rgb(230,236,248)", opacity: 0 });
        },
        tick(p) {
          const from = pt(body.x + size / 2, body.y - lift + size * 0.8);
          const to = nearestOn(p.rect, from, 3);
          const reach = clamp01(p.age / 0.35) * (1 - clamp01((p.age - 1.9) / 0.35));
          const tugging = p.age > 0.35 && p.age < 1.85;
          const tug = tugging ? Math.max(0, Math.sin(((p.age - 0.35) / 0.75) * Math.PI * 2)) : 0;
          const end = mix(from, to, reach);
          const mid = mix(from, end, 0.5);
          const sag = (tugging ? 1 - tug : 0.4) * 16 * reach;
          const f = (v) => v.toFixed(1);
          const d = `M${f(from.x)} ${f(from.y)}Q${f(mid.x)} ${f(mid.y + sag)} ${f(end.x)} ${f(end.y)}`;
          p.data.glow.setAttribute("d", d);
          p.data.line.setAttribute("d", d);
          setAttrs(p.data.knot, { cx: end.x, cy: end.y, opacity: reach > 0.98 ? 1 : 0 });
          if (tug > 0.7) s.land = Math.max(s.land, 0.55);
          gazeAt(p, to);
        },
      },
      sparkle: {
        duration: 2.3,
        begin(p) {
          p.data.stars = [];
          for (let i = 0; i < 7; i += 1) {
            p.data.stars.push({
              side: i % 4,
              u: Math.random(),
              delay: i * 0.18,
              scale: 4 + Math.random() * 4,
              node: fxNode("path", { d: SPARK, fill: "rgb(64,242,219)", opacity: 0 }),
            });
          }
          react("eureka");
        },
        tick(p) {
          const r = p.rect;
          for (const star of p.data.stars) {
            const across = r.left + r.width * star.u;
            const down = r.top + r.height * star.u;
            const x = [across, r.right + 6, across, r.left - 6][star.side];
            const y = [r.top - 6, down, r.bottom + 6, down][star.side];
            const local = clamp01((p.age - star.delay) / 0.9);
            const a = Math.sin(Math.PI * local);
            setAttrs(star.node, {
              opacity: a,
              transform: `translate(${x.toFixed(1)} ${y.toFixed(1)}) rotate(${Math.round(local * 90)}) scale(${(a * star.scale).toFixed(2)})`,
            });
          }
          glance(p);
        },
      },
      outline: {
        duration: 2.2,
        begin(p) {
          p.data.halo = fxNode("rect", { fill: "none", stroke: "rgba(64,242,219,.25)", "stroke-width": 7 });
          p.data.ring = fxNode("rect", { fill: "none", stroke: "rgb(64,242,219)", "stroke-width": 2 });
        },
        tick(p) {
          const r = p.rect;
          const a = Math.sin(Math.PI * clamp01(p.age / p.duration)) * (0.65 + 0.35 * Math.sin(p.age * 7));
          const box = { x: r.left - 5, y: r.top - 5, width: r.width + 10, height: r.height + 10, rx: Math.min(12, r.height / 2 + 5), opacity: Math.max(a, 0) };
          setAttrs(p.data.halo, box);
          setAttrs(p.data.ring, box);
          glance(p);
        },
      },
      hop: {
        duration: 1.9,
        tick(p) {
          lift = p.age < 1.1 ? Math.abs(Math.sin((Math.PI * p.age) / 0.55)) * 20 : 0;
          s.air = Math.max(s.air, lift / 40);
          glance(p);
          once(p, "nod", 1.15, () => react("nod"));
        },
      },
      cheer: {
        duration: 1.9,
        tick(p) {
          glance(p);
          once(p, "cheer", 0, () => react("cheer"));
        },
      },
      giggle: {
        duration: 1.6,
        tick(p) {
          glance(p);
          once(p, "giggle", 0, () => react("giggle"));
        },
      },
      surprise: {
        duration: 1.6,
        tick(p) {
          glance(p);
          once(p, "surprise", 0.1, () => react("surprise"));
        },
      },
      nod: {
        begin(p) {
          p.data.links = rowLinks(p.element);
          p.duration = p.data.links.length * 0.55 + 0.4;
          p.data.dot = fxNode("circle", { r: 3, fill: "rgb(64,242,219)" });
        },
        tick(p) {
          const links = p.data.links;
          const i = Math.min(Math.floor(p.age / 0.55), links.length - 1);
          const r = links[i].getBoundingClientRect();
          if (p.data.at !== i) {
            p.data.at = i;
            react("nod");
          }
          setAttrs(p.data.dot, { cx: r.left + r.width / 2, cy: r.bottom + 5 });
          gazeAt(p, center(r));
        },
      },
      count: {
        begin(p) {
          p.data.words = wordBoxes(p.element, 8);
          p.data.top = p.rect.top;
          p.data.left = p.rect.left;
          p.data.numbers = [];
          p.duration = p.data.words.length ? p.data.words.length * 0.45 + 1.3 : 0;
        },
        tick(p) {
          const words = p.data.words;
          const n = words.length;
          if (!n) return;
          const dx = p.rect.left - p.data.left;
          const dy = p.rect.top - p.data.top;
          const shown = Math.min(Math.floor(p.age / 0.45) + 1, n);
          while (p.data.numbers.length < shown) {
            p.data.numbers.push(fxNode("circle", { r: 3, fill: "rgb(64,242,219)" }));
            react("nod");
          }
          p.data.numbers.forEach((node, i) => {
            const w = words[i];
            const above = w.top + dy > 22;
            setAttrs(node, { cx: w.left + dx + w.width / 2, cy: above ? w.top + dy - 6 : w.bottom + dy + 6 });
          });
          const w = words[Math.max(shown, 1) - 1];
          gazeAt(p, pt(w.left + dx + w.width / 2, w.top + dy + w.height / 2));
          once(p, "done", n * 0.45 + 0.1, () => react("eureka"));
        },
      },
      dizzy: {
        duration: 2.9,
        tick(p) {
          const r = p.rect;
          if (p.age < 1.1) {
            gazeAt(p, pt(r.left + r.width * ((p.age / 0.27) % 1), r.top + r.height * (0.15 + (0.7 * p.age) / 1.1)));
          } else {
            p.gaze = null;
          }
          once(p, "dizzy", 1.1, () => react("dizzy"));
        },
      },
      type: {
        duration: 2.6,
        tick(p) {
          glance(p);
          s.mood = "thinking";
        },
      },
      poof: {
        duration: 2.4,
        begin(p) {
          const c = spiderAt(50, 62);
          puff(p, c.x, c.y, 10, { big: 1.1 });
          sparks(p, c.x, c.y - 10, 6, { start: 0.05 });
        },
        tick(p) {
          spell.fade = p.age < 0.45 ? 1 - ease(p.age / 0.3) : ease((p.age - 0.5) / 0.35);
          spell.scale = 0.5 + 0.5 * spell.fade;
          once(p, "jump", 0.45, () => {
            if (p.spot !== null) {
              body.x = p.spot;
              body.y = furnitureTop(body.x) - size;
            }
            const c = spiderAt(50, 62);
            puff(p, c.x, c.y, 10, { start: p.age, big: 1.1 });
            sparks(p, c.x, c.y - 10, 6, { start: p.age + 0.05 });
            s.land = 1;
          });
          once(p, "ta", 1, () => react("cheer"));
          if (p.age > 0.8) glance(p);
        },
      },
      levitate: {
        duration: 3.2,
        tick(p) {
          const h = swell(p.age, 0, 0.8, 2.5, 3.1);
          lift = h * (30 + Math.sin(p.age * 3.4) * 4);
          s.air = Math.max(s.air, h * 0.35);
          s.magic = h * 0.6;
          if (p.data.next === undefined) p.data.next = 0;
          while (p.data.next <= p.age && p.data.next < 2.7) {
            const foot = spiderAt(50 + Math.sin(p.data.next * 7) * 8, 92);
            part(p, {
              fill: "url(#fm-genie-smoke)", x: foot.x, y: foot.y, vx: rnd(-8, 8), vy: rnd(12, 28), sway: 5, freq: 5,
              r0: 3 + 4 * h, r1: 5 + 10 * h, start: p.data.next, life: 0.8, fade: "out", peak: 0.8,
            });
            p.data.next += 0.07;
          }
          glance(p);
        },
      },
      lamp: {
        duration: 3.4,
        begin(p) {
          p.data.side = towards(p);
          const lamp = fxNode("g", { opacity: 0 });
          const metal = { fill: "url(#fm-genie-gold)", stroke: "rgba(120,80,20,.85)", "stroke-width": 0.7 };
          fxNode("path", { d: "M-12 -9Q-21 -11 -18 -4Q-16 -1 -11 -4", fill: "none", stroke: GOLD, "stroke-width": 2 }, lamp);
          fxNode("path", { d: "M10 -9Q19 -9 24 -16L25.5 -15Q21 -5 11 -4Z", ...metal }, lamp);
          fxNode("ellipse", { cx: 0, cy: -1.5, rx: 7, ry: 2.2, ...metal }, lamp);
          fxNode("path", { d: "M-12 -3Q-16 -12 0 -13Q16 -12 12 -3Q0 1 -12 -3Z", ...metal }, lamp);
          fxNode("path", { d: "M-6 -12.5Q0 -20 6 -12.5Z", ...metal }, lamp);
          fxNode("circle", { cx: 0, cy: -20.5, r: 1.8, ...metal }, lamp);
          fxNode("path", { d: "M-7 -9Q-3 -11 2 -10.5", fill: "none", stroke: "rgba(255,255,255,.7)", "stroke-width": 1 }, lamp);
          p.data.lamp = lamp;
        },
        tick(p) {
          const k = size / 78;
          const appear = swell(p.age, 0, 0.35, 3, 3.4);
          const side = p.data.side;
          const base = spiderAt(50 + side * 64, GROUND);
          setAttrs(p.data.lamp, {
            opacity: appear,
            transform: `translate(${f1(base.x)} ${f1(base.y)}) scale(${(side * k * appear).toFixed(3)} ${(k * appear).toFixed(3)})`,
          });
          const tip = pt(base.x + side * k * 25, base.y - k * 15.5);
          smokeTrail(p, tip, center(p.rect), 0.06, 2.4, { after: 0.4 });
          once(p, "wish", 0.5, () => react("eureka"));
          glance(p);
        },
      },
      wishes: {
        duration: 3.2,
        begin(p) {
          p.data.orbs = [0, 0.55, 1.1].map((start) => ({ start, node: fxNode("circle", { fill: "url(#fm-genie-orb)", opacity: 0 }), done: false }));
        },
        tick(p) {
          const from = jewel();
          const to = center(p.rect);
          const top = mix(from, to, 0.5);
          top.y = Math.min(from.y, to.y) - 40 - Math.abs(to.x - from.x) * 0.15;
          s.magic = 0.5 * fadeOut(p.age, 2.4, 0.6);
          for (const orb of p.data.orbs) {
            const u = clamp01((p.age - orb.start) / 0.9);
            const q = curve(from, top, to, ease(u));
            setAttrs(orb.node, { cx: q.x, cy: q.y + Math.sin(p.age * 9) * 1.5, r: 7 + Math.sin(p.age * 11), opacity: u > 0 && u < 1 ? 1 : 0 });
            if (u >= 1 && !orb.done) {
              orb.done = true;
              sparks(p, to.x - p.rect.left, to.y - p.rect.top, 9, { start: p.age, from: "target" });
            }
          }
          once(p, "granted", 2.1, () => react("eureka"));
          glance(p);
        },
      },
      smokeRings: {
        duration: 2.8,
        begin(p) {
          p.data.rings = [0, 0.5, 1].map((start) => ({
            start,
            glow: fxNode("ellipse", { cx: 0, cy: 0, fill: "none", stroke: "rgba(143,102,209,.35)", opacity: 0 }),
            node: fxNode("ellipse", { cx: 0, cy: 0, fill: "none", stroke: "rgb(190,160,235)", opacity: 0 }),
          }));
        },
        tick(p) {
          const from = spiderAt(50 + towards(p) * 20, 72);
          const to = center(p.rect);
          const angle = (Math.atan2(to.y - from.y, to.x - from.x) * 180) / Math.PI;
          for (const ring of p.data.rings) {
            const u = (p.age - ring.start) / 1.4;
            if (u < 0 || u > 1) {
              setAttrs(ring.node, { opacity: 0 });
              setAttrs(ring.glow, { opacity: 0 });
              continue;
            }
            once(p, `blow${ring.start}`, ring.start, () => { s.land = Math.max(s.land, 0.5); });
            const c = mix(from, to, 1 - (1 - u) * (1 - u));
            const radius = 5 + 24 * u;
            const shape = { rx: radius * 0.45, ry: radius, transform: `translate(${f1(c.x)} ${f1(c.y)}) rotate(${Math.round(angle)})` };
            const a = clamp01(u / 0.1) * (1 - u);
            setAttrs(ring.node, { ...shape, "stroke-width": 6 - 4 * u, opacity: a * 0.95 });
            setAttrs(ring.glow, { ...shape, "stroke-width": 10 - 6 * u, opacity: a * 0.5 });
          }
          glance(p);
        },
      },
      shimmer: {
        duration: 2.6,
        begin(p) {
          const g = fxNode("g", { "clip-path": "url(#fm-genie-clip)" });
          p.data.clip = fxLayer.querySelector("#fm-genie-clip rect");
          p.data.band = fxNode("rect", { fill: "url(#fm-genie-band)" }, g);
        },
        tick(p) {
          const r = p.rect;
          setAttrs(p.data.clip, { x: r.left - 2, y: r.top - 2, width: r.width + 4, height: r.height + 4, rx: Math.min(10, r.height / 2) });
          const w = clamp(r.width * 0.3, 24, 90);
          const first = p.age < 1.25;
          const pass = first ? p.age / 1.25 : (p.age - 1.35) / 1.1;
          const x = r.left - w - r.height * 0.4 + (r.width + w + r.height * 0.8) * ease(pass);
          const cy = r.top + r.height / 2;
          setAttrs(p.data.band, {
            x, y: r.top - 2, width: w, height: r.height + 4, opacity: pass >= 0 && pass <= 1 ? (first ? 1 : 0.6) : 0,
            transform: `translate(${f1(x + w / 2)} ${f1(cy)}) skewX(-20) translate(${f1(-x - w / 2)} ${f1(-cy)})`,
          });
          once(p, "glint", 1.2, () => sparks(p, r.width, 0, 5, { start: p.age, from: "target", spread: 0.5, fill: GOLD }));
          glance(p);
        },
      },
      stardust: {
        duration: 3.4,
        begin(p) {
          p.data.grains = [];
          for (let i = 0; i < 36; i += 1) {
            p.data.grains.push({
              start: i * 0.035, u: rnd(0.04, 0.96), lift: rnd(-3, 3),
              node: fxNode("circle", { r: rnd(1, 2), fill: i % 3 ? GOLD : TEAL, opacity: 0 }),
            });
          }
        },
        tick(p) {
          const from = crown();
          const r = p.rect;
          const fade = fadeOut(p.age, 2.8, 0.6);
          s.magic = 0.5 * fade;
          for (const grain of p.data.grains) {
            const u = clamp01((p.age - grain.start) / 0.95);
            const land = pt(r.left + r.width * grain.u, r.top - 3 + grain.lift);
            const top = mix(from, land, 0.5);
            top.y = Math.min(from.y, land.y) - 50;
            const q = curve(from, top, land, ease(u));
            const twinkle = u < 1 ? 1 : 0.45 + 0.55 * Math.abs(Math.sin(p.age * 6 + grain.u * 20));
            setAttrs(grain.node, { cx: q.x, cy: q.y, opacity: p.age < grain.start ? 0 : twinkle * fade });
          }
          glance(p);
        },
      },
      carpet: {
        duration: 3.6,
        begin(p) {
          p.data.side = towards(p);
          p.data.g = fxNode("g", { opacity: 0 });
          p.data.rug = fxNode("path", { fill: "rgb(102,61,168)", stroke: GOLD, "stroke-width": 1.4, "stroke-linejoin": "round" }, p.data.g);
          p.data.stripe = fxNode("path", { fill: "none", stroke: TEAL, "stroke-width": 1, opacity: 0.85 }, p.data.g);
          p.data.fringe = fxNode("path", { fill: "none", stroke: GOLD, "stroke-width": 1, "stroke-linecap": "round" }, p.data.g);
        },
        tick(p) {
          const k = size / 78;
          const away = 1 - ease(p.age / 0.45) + ease((p.age - 3.15) / 0.45);
          const h = swell(p.age, 0.5, 1.1, 2.5, 3.1);
          lift = h * (26 + Math.sin(p.age * 3) * 4);
          const w = 66 * k;
          const thick = 6 * k;
          const cx = body.x + size / 2 - p.data.side * away * size * 1.1;
          const y = body.y + size - lift + 1;
          const wave = (x) => Math.sin(x * 0.12 - p.age * 8) * (1.2 + 2.6 * h) * k;
          const xs = Array.from({ length: 13 }, (_, i) => cx - w / 2 + (w * i) / 12);
          const top = xs.map((x, i) => `${i ? "L" : "M"}${f1(x)} ${f1(y + wave(x - cx))}`).join("");
          const bottom = xs.slice().reverse().map((x) => `L${f1(x)} ${f1(y + thick + wave(x - cx))}`).join("");
          const stripe = xs.map((x, i) => `${i ? "L" : "M"}${f1(x)} ${f1(y + thick / 2 + wave(x - cx))}`).join("");
          let fringe = "";
          for (const end of [xs[0], xs[12]]) {
            const out = end < cx ? -1 : 1;
            for (let j = 0; j < 3; j += 1) {
              const fy = y + wave(end - cx) + thick * (0.2 + j * 0.3);
              fringe += `M${f1(end)} ${f1(fy)}l${f1(out * 3.5 * k)} ${f1(Math.sin(p.age * 9 + j) * k)}`;
            }
          }
          setAttrs(p.data.g, { opacity: 1 - clamp01(away) });
          p.data.rug.setAttribute("d", `${top}${bottom}Z`);
          p.data.stripe.setAttribute("d", stripe);
          p.data.fringe.setAttribute("d", fringe);
          glance(p);
        },
      },
      vortex: {
        duration: 2.8,
        begin(p) {
          p.data.sparks = [];
          for (let i = 0; i < 16; i += 1) {
            p.data.sparks.push({ a: (i / 16) * Math.PI * 2, k: rnd(0.85, 1.15), node: fxNode("path", { d: SPARK, fill: SPARK_COLORS[i % 4], opacity: 0 }) });
          }
          p.data.flash = fxNode("circle", { fill: "url(#fm-genie-mote)", opacity: 0 });
        },
        tick(p) {
          const r = p.rect;
          const c = center(r);
          const u = clamp01(p.age / 1.8);
          const shrink = Math.pow(1 - ease(u), 0.8);
          for (const spark of p.data.sparks) {
            const a = spark.a + u * Math.PI * 5;
            const x = c.x + Math.cos(a) * (r.width / 2 + 18) * spark.k * shrink;
            const y = c.y + Math.sin(a) * (r.height / 2 + 18) * spark.k * shrink;
            setAttrs(spark.node, {
              opacity: u < 1 ? clamp01(p.age / 0.2) : 0,
              transform: `translate(${f1(x)} ${f1(y)}) rotate(${Math.round(p.age * 360)}) scale(${(3.5 - 1.5 * u).toFixed(2)})`,
            });
          }
          const f = clamp01((p.age - 1.8) / 0.7);
          setAttrs(p.data.flash, { cx: c.x, cy: c.y, r: 8 + f * Math.min(Math.max(r.width, r.height) * 0.6, 120), opacity: f > 0 ? Math.sin(Math.PI * f) : 0 });
          once(p, "flash", 1.8, () => react("eureka"));
          glance(p);
        },
      },
      crystal: {
        duration: 3.4,
        begin(p) {
          p.data.side = towards(p);
          const g = fxNode("g", { opacity: 0 });
          p.data.g = g;
          p.data.halo = fxNode("circle", { cy: -17, r: 21, fill: "url(#fm-genie-orb)", opacity: 0.3 }, g);
          fxNode("path", { d: "M-9 0L-6 -6H6L9 0Z", fill: "url(#fm-genie-gold)", stroke: "rgba(120,80,20,.85)", "stroke-width": 0.7 }, g);
          fxNode("circle", { cy: -17, r: 12, fill: "url(#fm-genie-ball)", stroke: "rgba(230,236,248,.7)", "stroke-width": 0.8 }, g);
          p.data.mists = [0, 1, 2].map(() => fxNode("ellipse", { rx: 6.5, ry: 2.4, fill: "rgba(64,242,219,.55)" }, g));
          fxNode("ellipse", { cx: -4, cy: -22, rx: 3, ry: 1.8, fill: "rgba(255,255,255,.75)", transform: "rotate(-30 -4 -22)" }, g);
        },
        tick(p) {
          const k = size / 78;
          const appear = swell(p.age, 0, 0.4, 3, 3.4);
          const base = spiderAt(50 + p.data.side * 60, GROUND);
          setAttrs(p.data.g, { opacity: appear, transform: `translate(${f1(base.x)} ${f1(base.y)}) scale(${(k * appear).toFixed(3)})` });
          p.data.mists.forEach((mist, i) => {
            const a = p.age * 2.2 + i * 2.1;
            const mx = Math.cos(a) * 4.5;
            const my = -17 + Math.sin(a * 1.3) * 5;
            setAttrs(mist, { cx: mx, cy: my, opacity: 0.35 + 0.3 * Math.sin(a * 2), transform: `rotate(${Math.round((a * 40) % 360)} ${f1(mx)} ${f1(my)})` });
          });
          setAttrs(p.data.halo, { opacity: 0.25 + 0.2 * Math.sin(p.age * 5) });
          s.magic = 0.8 * appear;
          glance(p);
        },
      },
      runes: {
        duration: 3.2,
        begin(p) {
          const line = { fill: "none", "stroke-linecap": "round", "stroke-linejoin": "round", opacity: 0 };
          p.data.runes = RUNES.map((d, i) => ({
            i,
            glow: fxNode("path", { d, ...line, stroke: "rgba(64,242,219,.35)", "stroke-width": 3 }),
            line: fxNode("path", { d, ...line, stroke: "rgb(150,255,240)", "stroke-width": 1.1 }),
          }));
        },
        tick(p) {
          const r = p.rect;
          const c = center(r);
          const rx = r.width / 2 + 16;
          const ry = r.height / 2 + 14;
          for (const rune of p.data.runes) {
            const a = (rune.i * Math.PI) / 3 + p.age * 0.9;
            const o = clamp01((p.age - rune.i * 0.12) / 0.3) * fadeOut(p.age, 2.6, 0.6);
            const transform = `translate(${f1(c.x + Math.cos(a) * rx)} ${f1(c.y + Math.sin(a) * ry)}) scale(${(1.3 + 0.15 * Math.sin(p.age * 5 + rune.i)).toFixed(2)})`;
            setAttrs(rune.glow, { transform, opacity: o });
            setAttrs(rune.line, { transform, opacity: o });
          }
          s.magic = 0.5 * fadeOut(p.age, 2.6, 0.6);
          glance(p);
        },
      },
      halo: {
        duration: 3,
        begin(p) {
          p.data.glow = fxNode("ellipse", { fill: "none", stroke: "rgba(255,206,84,.35)", "stroke-width": 6, opacity: 0 });
          p.data.ring = fxNode("ellipse", { fill: "none", stroke: "rgb(255,226,140)", "stroke-width": 2.2, opacity: 0 });
        },
        tick(p) {
          const r = p.rect;
          const c = center(r);
          const rx = clamp(r.width * 0.3, 12, 46);
          const up = ease(p.age / 1.2);
          const rest = Math.max(r.top - 10, 12);
          const y = c.y + (rest - c.y) * up + Math.sin(p.age * 3) * 2 * up;
          const shape = { cx: c.x, cy: y, rx, ry: rx * 0.28, opacity: clamp01(p.age / 0.3) * fadeOut(p.age, 2.4, 0.6) };
          setAttrs(p.data.glow, shape);
          setAttrs(p.data.ring, shape);
          once(p, "shine", 1.2, () => sparks(p, c.x - r.left, rest - r.top, 5, { start: p.age, from: "target", spread: 0.5, fill: GOLD }));
          glance(p);
        },
      },
      fireflies: {
        duration: 3.4,
        begin(p) {
          p.data.flies = [];
          for (let i = 0; i < 10; i += 1) {
            p.data.flies.push({
              start: i * 0.13, u: rnd(0.1, 0.9), v: rnd(0.15, 0.85), bend: rnd(-60, 60), phase: rnd(0, 6.3),
              node: fxNode("circle", { r: 4.5, fill: "url(#fm-genie-mote)", opacity: 0 }),
            });
          }
        },
        tick(p) {
          const from = crown();
          const r = p.rect;
          const fade = fadeOut(p.age, 2.8, 0.6);
          s.magic = 0.4 * fade;
          for (const fly of p.data.flies) {
            const u = clamp01((p.age - fly.start) / 1.3);
            const end = pt(r.left + r.width * fly.u + Math.sin(p.age * 2 + fly.phase) * 8, r.top + r.height * fly.v + Math.cos(p.age * 1.7 + fly.phase) * 6);
            const bend = mix(from, end, 0.5);
            bend.x += fly.bend;
            bend.y -= 40;
            const q = curve(from, bend, end, ease(u));
            const flicker = 0.55 + 0.45 * Math.sin(p.age * 9 + fly.phase * 3);
            setAttrs(fly.node, { cx: q.x, cy: q.y, opacity: p.age < fly.start ? 0 : flicker * fade });
          }
          glance(p);
        },
      },
      whirl: {
        duration: 2.6,
        begin(p) {
          p.data.puffs = Array.from({ length: 12 }, (_, i) => ({ a: (i / 12) * Math.PI * 2, h: rnd(0.2, 0.9), node: fxNode("circle", { fill: "url(#fm-genie-smoke)", opacity: 0 }) }));
        },
        tick(p) {
          spell.spin = 720 * ease(p.age / 1.7);
          const c = spiderAt(50, 62);
          const o = swell(p.age, 0, 0.3, 1.6, 2.2);
          for (const smoke of p.data.puffs) {
            const a = smoke.a + p.age * 7;
            setAttrs(smoke.node, {
              cx: c.x + Math.cos(a) * size * 0.55, cy: c.y + (smoke.h - 0.5) * size * 0.7 + Math.sin(a) * size * 0.12,
              r: size * 0.13 * (1 + 0.3 * Math.sin(a * 2)), opacity: o * 0.9,
            });
          }
          once(p, "done", 1.8, () => react("giggle"));
          if (p.age > 1.8) glance(p);
          else p.gaze = null;
        },
      },
      grow: {
        duration: 2.8,
        begin(p) {
          const c = spiderAt(50, 88);
          puff(p, c.x, c.y, 10, { big: 1.2 });
        },
        tick(p) {
          const g = swell(p.age, 0.1, 0.7, 1.7, 2.3);
          spell.scale = 1 + 0.5 * g + (g > 0.98 ? Math.sin(p.age * 14) * 0.02 : 0);
          s.magic = g;
          once(p, "big", 0.7, () => react("cheer"));
          once(p, "small", 1.7, () => {
            const c = spiderAt(50, 70);
            puff(p, c.x, c.y, 8, { start: p.age });
          });
          glance(p);
        },
      },
      bottle: {
        duration: 3,
        begin(p) {
          const g = fxNode("g", { opacity: 0 });
          p.data.g = g;
          p.data.glow = fxNode("ellipse", { cx: 0, cy: -9, rx: 13, ry: 15, fill: "url(#fm-genie-orb)", opacity: 0 }, g);
          fxNode("path", { d: "M-8 0Q-12 0 -12 -7Q-12 -14 -4 -16V-22H4V-16Q12 -14 12 -7Q12 0 8 0Z", fill: "rgba(143,102,209,.3)", stroke: "rgba(230,236,248,.85)", "stroke-width": 1 }, g);
          fxNode("path", { d: "M-8 -10Q-9 -5 -6 -2", fill: "none", stroke: "rgba(255,255,255,.6)", "stroke-width": 1.2, "stroke-linecap": "round" }, g);
          p.data.cork = fxNode("rect", { x: -3.5, y: -26.5, width: 7, height: 5, rx: 1.2, fill: "rgb(176,120,60)" }, g);
        },
        tick(p) {
          const k = size / 78;
          const base = spiderAt(50, GROUND);
          const wobble = p.age > 1 && p.age < 1.9 ? Math.sin(p.age * 18) * 8 * Math.sin((Math.PI * (p.age - 1)) / 0.9) : 0;
          setAttrs(p.data.g, { opacity: swell(p.age, 0, 0.3, 2.5, 3), transform: `translate(${f1(base.x)} ${f1(base.y)}) rotate(${Math.round(wobble)}) scale(${(k * 1.1).toFixed(3)})` });
          setAttrs(p.data.glow, { opacity: p.age > 0.9 && p.age < 2.1 ? 0.55 + 0.35 * Math.sin(p.age * 8) : 0 });
          const into = ease((p.age - 0.2) / 0.8);
          if (p.age < 1.9) {
            spell.scale = 0.12 + 0.88 * (1 - into);
            spell.turn = 540 * into;
            spell.fade = 1 - ease((p.age - 0.7) / 0.3);
          } else {
            const out = ease((p.age - 1.9) / 0.45);
            spell.scale = 0.12 + 0.88 * out + Math.sin(Math.PI * out) * 0.15;
            spell.turn = 0;
            spell.fade = clamp01(out * 3);
          }
          const tau = Math.max(p.age - 1.9, 0);
          setAttrs(p.data.cork, { transform: `translate(${f1(tau * 24)} ${f1(-150 * tau + 260 * tau * tau)}) rotate(${Math.round(tau * 400)} 0 -24)`, opacity: tau > 0 ? fadeOut(tau, 0.3, 0.3) : 1 });
          once(p, "pop", 1.9, () => {
            puff(p, base.x, base.y - 26 * k, 8, { start: p.age });
            react("surprise");
          });
          if (p.age > 2.2) glance(p);
        },
      },
      rainbow: {
        duration: 3,
        begin(p) {
          p.data.bands = RAINBOW.map((stroke) => fxNode("path", {
            fill: "none", stroke, "stroke-width": 2.6, "stroke-linecap": "round", pathLength: 1, "stroke-dasharray": "1 1", opacity: 0,
          }));
        },
        tick(p) {
          const from = jewel();
          const to = center(p.rect);
          const len = Math.max(Math.hypot(to.x - from.x, to.y - from.y), 1);
          const flip = to.x - from.x < 0 ? -1 : 1;
          const n = pt((flip * (to.y - from.y)) / len, (-flip * (to.x - from.x)) / len);
          const up = 30 + len * 0.35;
          const mid = mix(from, to, 0.5);
          const reveal = ease(p.age / 1.1);
          const fade = fadeOut(p.age, 2.3, 0.7);
          p.data.bands.forEach((node, i) => {
            const o = (2.5 - i) * 2.8;
            const a = add(from, scale(n, o * 0.4));
            const b = add(to, scale(n, o * 0.4));
            const top = add(mid, scale(n, up + o));
            setAttrs(node, {
              d: `M${f1(a.x)} ${f1(a.y)}Q${f1(top.x)} ${f1(top.y)} ${f1(b.x)} ${f1(b.y)}`,
              "stroke-dashoffset": 1 - reveal, opacity: 0.75 * fade,
            });
          });
          once(p, "arc", 1.1, () => react("eureka"));
          glance(p);
        },
      },
      fireworks: {
        duration: 3,
        begin(p) {
          const r = p.rect;
          const sky = Math.max(r.top - 34, 26) - r.top;
          [0, 0.75, 1.5].forEach((start, i) => {
            const x = r.width * [0.5, 0.2, 0.8][i];
            const y = sky - [0, 12, 6][i];
            part(p, { from: "target", fill: GOLD, x, y: 0, vy: y / 0.35, r0: 2.2, r1: 1.2, start, life: 0.35 });
            sparks(p, x, y, 14, { start: start + 0.35, from: "target", spread: 1.1, g: 140, life: 1 });
          });
        },
        tick(p) {
          once(p, "cheer", 0.4, () => react("cheer"));
          glance(p);
        },
      },
      bubbles: {
        duration: 3,
        begin(p) {
          const r = p.rect;
          for (let i = 0; i < 12; i += 1) {
            part(p, {
              from: "target", fill: "url(#fm-genie-bubble)", x: r.width * rnd(0.08, 0.92), y: r.height * rnd(0.5, 1),
              vy: -rnd(40, 75), sway: rnd(4, 9), freq: rnd(2, 4), r0: rnd(3, 6), r1: rnd(6, 10), start: rnd(0, 1.4), life: rnd(1.1, 1.5), fade: "pop",
            });
          }
        },
        tick(p) {
          glance(p);
        },
      },
      glitter: {
        duration: 3.2,
        begin(p) {
          const r = p.rect;
          const fall = clamp((r.height + 30) / 1.3, 30, 120);
          for (let i = 0; i < 30; i += 1) {
            part(p, {
              from: "target", d: SPARK, fill: SPARK_COLORS[i % 4], x: r.width * rnd(0, 1), y: -rnd(4, 24), vy: fall * rnd(0.6, 1),
              sway: rnd(4, 10), freq: rnd(2, 4), spin: rnd(-180, 180), r0: rnd(1.6, 3), r1: rnd(1, 2), start: rnd(0, 1.6), life: rnd(1.1, 1.5),
            });
          }
        },
        tick(p) {
          glance(p);
        },
      },
      jewelBeam: {
        duration: 2.8,
        begin(p) {
          p.data.wash = fxNode("rect", { fill: "rgba(64,242,219,.14)", stroke: "rgba(64,242,219,.7)", "stroke-width": 1.5, opacity: 0 });
          p.data.glow = fxNode("path", { fill: "none", stroke: "rgba(64,242,219,.28)", "stroke-width": 9, "stroke-linecap": "round" });
          p.data.core = fxNode("path", { fill: "none", stroke: "rgb(220,255,250)", "stroke-width": 2, "stroke-linecap": "round" });
          react("eureka");
        },
        tick(p) {
          const r = p.rect;
          const from = jewel();
          const reach = ease(p.age / 0.4);
          const fade = fadeOut(p.age, 2.2, 0.6);
          const end = mix(from, nearestOn(r, from, 4), reach);
          const d = `M${f1(from.x)} ${f1(from.y)}L${f1(end.x)} ${f1(end.y)}`;
          setAttrs(p.data.glow, { d, opacity: fade * (0.7 + 0.3 * Math.sin(p.age * 12)) });
          setAttrs(p.data.core, { d, opacity: fade });
          setAttrs(p.data.wash, {
            x: r.left - 3, y: r.top - 3, width: r.width + 6, height: r.height + 6, rx: Math.min(10, r.height / 2 + 3),
            opacity: reach >= 1 ? fade * (0.6 + 0.4 * Math.sin(p.age * 6)) : 0,
          });
          s.magic = fade;
          glance(p);
        },
      },
      ripple: {
        duration: 2.8,
        begin(p) {
          p.data.rings = [0, 0.4, 0.8, 1.2].map((start) => ({ start, node: fxNode("ellipse", { fill: "none", stroke: TEAL, opacity: 0 }) }));
        },
        tick(p) {
          const r = p.rect;
          const c = center(r);
          for (const ring of p.data.rings) {
            const u = (p.age - ring.start) / 1.4;
            if (u < 0 || u > 1) {
              setAttrs(ring.node, { opacity: 0 });
              continue;
            }
            const g = 1 - (1 - u) * (1 - u);
            setAttrs(ring.node, {
              cx: c.x, cy: c.y, rx: r.width * 0.15 + (r.width * 0.35 + 34) * g, ry: r.height * 0.15 + (r.height * 0.35 + 24) * g,
              "stroke-width": 2.5 * (1 - u) + 0.5, opacity: (1 - u) * 0.9,
            });
          }
          glance(p);
        },
      },
      moons: {
        duration: 3,
        begin(p) {
          p.data.moons = [0, 1, 2].map((i) => ({ i, node: fxNode("circle", { fill: "url(#fm-genie-orb)", opacity: 0 }), done: false }));
        },
        tick(p) {
          const c = spiderAt(50, 16);
          const radius = size * 0.42;
          const to = center(p.rect);
          s.magic = 0.5 * fadeOut(p.age, 1.6, 0.8);
          for (const moon of p.data.moons) {
            const a = p.age * 5 + moon.i * 2.094;
            const orbit = pt(c.x + Math.cos(a) * radius, c.y + Math.sin(a) * radius * 0.3);
            const go = ease((p.age - 1.5 - moon.i * 0.15) / 0.7);
            const q = mix(orbit, to, go);
            q.y -= Math.sin(Math.PI * go) * 30;
            setAttrs(moon.node, { cx: q.x, cy: q.y, r: 5 + Math.sin(p.age * 8 + moon.i) * 0.7, opacity: clamp01(p.age / 0.3) * (go >= 1 ? 0 : 1) });
            if (go >= 1 && !moon.done) {
              moon.done = true;
              sparks(p, to.x - p.rect.left, to.y - p.rect.top, 6, { start: p.age, from: "target", spread: 0.6 });
            }
          }
          if (p.age < 1.5) gazeAt(p, pt(c.x, c.y - 30));
          else glance(p);
        },
      },
      constellation: {
        duration: 3.4,
        begin(p) {
          const stars = [];
          for (let i = 0; i < 7; i += 1) stars.push({ u: rnd(0, 1), v: rnd(0, 1), spin: rnd(-60, 60) });
          stars.sort((a, b) => a.u - b.u);
          stars.forEach((star, i) => { star.node = fxNode("path", { d: SPARK, fill: i % 2 ? "rgb(255,236,170)" : "rgb(220,255,250)", opacity: 0 }); });
          p.data.stars = stars;
          p.data.lines = stars.slice(1).map(() => fxNode("path", { fill: "none", stroke: "rgba(220,255,250,.55)", "stroke-width": 1, pathLength: 1, "stroke-dasharray": "1 1", opacity: 0 }));
        },
        tick(p) {
          const r = p.rect;
          const starAt = (star) => pt(r.left - 14 + (r.width + 28) * star.u, r.top - 14 + (r.height + 28) * star.v);
          const fade = fadeOut(p.age, 2.8, 0.6);
          const stars = p.data.stars;
          stars.forEach((star, i) => {
            const on = clamp01((p.age - i * 0.28) / 0.25);
            const q = starAt(star);
            setAttrs(star.node, {
              opacity: on * fade * (0.7 + 0.3 * Math.sin(p.age * 7 + i)),
              transform: `translate(${f1(q.x)} ${f1(q.y)}) rotate(${Math.round(star.spin * p.age)}) scale(${(2.5 + 1.5 * on).toFixed(2)})`,
            });
          });
          p.data.lines.forEach((node, i) => {
            const a = starAt(stars[i]);
            const b = starAt(stars[i + 1]);
            setAttrs(node, { d: `M${f1(a.x)} ${f1(a.y)}L${f1(b.x)} ${f1(b.y)}`, "stroke-dashoffset": 1 - clamp01((p.age - (i + 1) * 0.28) / 0.3), opacity: fade });
          });
          glance(p);
        },
      },
      crescent: {
        duration: 3.2,
        begin(p) {
          p.data.glow = fxNode("circle", { r: 22, fill: "url(#fm-genie-mote)", opacity: 0 });
          p.data.moon = fxNode("path", { d: "M0 -10A10 10 0 1 0 0 10A6 10 0 0 1 0 -10Z", fill: "url(#fm-genie-gold)", opacity: 0 });
          p.data.stars = Array.from({ length: 5 }, () => ({ dx: rnd(-44, 44), dy: rnd(-24, 12), phase: rnd(0, 6.3), node: fxNode("path", { d: SPARK, fill: "rgb(255,246,200)", opacity: 0 }) }));
        },
        tick(p) {
          const r = p.rect;
          const x = r.left + r.width / 2;
          const sky = Math.max(r.top - 22, 16);
          const y = r.top + 12 + (sky - r.top - 12) * ease(p.age / 1.2);
          const o = clamp01(p.age / 0.3) * fadeOut(p.age, 2.6, 0.6);
          setAttrs(p.data.moon, { opacity: o, transform: `translate(${f1(x)} ${f1(y)}) rotate(${Math.round(-20 + 10 * Math.sin(p.age * 2))})` });
          setAttrs(p.data.glow, { cx: x, cy: y, opacity: 0.6 * o });
          for (const star of p.data.stars) {
            setAttrs(star.node, {
              opacity: p.age > 0.9 ? o * Math.max(0, Math.sin(p.age * 5 + star.phase)) : 0,
              transform: `translate(${f1(x + star.dx)} ${f1(sky + star.dy)}) scale(2.5)`,
            });
          }
          glance(p);
        },
      },
      mirage: {
        duration: 3,
        begin(p) {
          p.data.ghosts = [0, 0.5, 1].map((start, i) => ({
            start,
            node: fxNode("path", { fill: "rgba(143,102,209,.08)", stroke: i % 2 ? "rgba(64,242,219,.75)" : "rgba(190,160,235,.8)", "stroke-width": 1.4, "stroke-linejoin": "round", opacity: 0 }),
          }));
        },
        tick(p) {
          const r = p.rect;
          const w = Math.min(r.width, 360);
          const h = Math.min(r.height, 160);
          const c = center(r);
          for (const ghost of p.data.ghosts) {
            const u = (p.age - ghost.start) / 1.6;
            if (u < 0 || u > 1) {
              setAttrs(ghost.node, { opacity: 0 });
              continue;
            }
            const rise = -34 * ease(u);
            setAttrs(ghost.node, { d: wavyBox(c.x - w / 2, c.y - h / 2 + rise, w, h, 2 + 3 * u, p.age * 7), opacity: Math.sin(Math.PI * u) * 0.9 });
          }
          glance(p);
        },
      },
      coins: {
        duration: 3.2,
        begin(p) {
          const r = p.rect;
          p.data.coins = Array.from({ length: 9 }, (_, i) => ({
            start: i * 0.14, x: r.width * rnd(0.1, 0.9), vx: rnd(-30, 30), vy: -rnd(80, 160), spin: rnd(8, 14),
            node: fxNode("ellipse", { ry: 4.5, fill: "url(#fm-genie-gold)", stroke: "rgba(140,90,20,.9)", "stroke-width": 0.8, opacity: 0 }),
            glint: fxNode("path", { d: SPARK, fill: "#fff", opacity: 0 }),
          }));
        },
        tick(p) {
          const r = p.rect;
          const gravity = 700;
          const ground = Math.min(r.bottom + 6, height - 6) - r.top;
          const y0 = Math.min(r.height * 0.3, 20);
          const fade = fadeOut(p.age, 2.7, 0.5);
          for (const coin of p.data.coins) {
            const tau = p.age - coin.start;
            if (tau < 0) continue;
            const landAt = (-coin.vy + Math.sqrt(coin.vy * coin.vy + 2 * gravity * Math.max(ground - y0, 0))) / gravity;
            const flying = Math.min(tau, landAt);
            const x = r.left + coin.x + coin.vx * flying;
            const y = r.top + Math.min(y0 + coin.vy * tau + 0.5 * gravity * tau * tau, ground);
            const rx = tau < landAt ? Math.max(4.5 * Math.abs(Math.cos(tau * coin.spin)), 0.8) : 4.5;
            setAttrs(coin.node, { cx: x, cy: y, rx, opacity: fade });
            const shine = clamp01((tau - landAt) / 0.4);
            setAttrs(coin.glint, { opacity: shine > 0 && shine < 1 ? Math.sin(Math.PI * shine) * fade : 0, transform: `translate(${f1(x + 2)} ${f1(y - 2)}) scale(3.5)` });
          }
          once(p, "rich", 0.9, () => react("cheer"));
          glance(p);
        },
      },
      flourish: {
        duration: 3,
        begin(p) {
          const line = { fill: "none", "stroke-linecap": "round", "stroke-linejoin": "round", pathLength: 1, "stroke-dasharray": "1 1" };
          p.data.glow = fxNode("path", { ...line, stroke: "rgba(255,206,84,.3)", "stroke-width": 5 });
          p.data.line = fxNode("path", { ...line, stroke: "rgb(255,226,140)", "stroke-width": 1.6 });
          p.data.tip = fxNode("path", { d: SPARK, fill: "#fff", opacity: 0 });
        },
        tick(p) {
          const from = jewel();
          const to = center(p.rect);
          const dx = to.x - from.x;
          const dy = to.y - from.y;
          const len = Math.max(Math.hypot(dx, dy), 1);
          const ux = dx / len;
          const uy = dy / len;
          const loop = clamp(len * 0.07, 8, 18);
          const points = [];
          for (let i = 0; i <= 90; i += 1) {
            const u = i / 90;
            const th = u * Math.PI * 6;
            const along = u * len + Math.sin(th) * loop * 1.2;
            const across = (1 - Math.cos(th)) * loop * 0.9;
            points.push(pt(from.x + ux * along - uy * across, from.y + uy * along + ux * across));
          }
          const d = points.map((q, i) => `${i ? "L" : "M"}${f1(q.x)} ${f1(q.y)}`).join("");
          const drawn = ease(p.age / 1.3);
          const fade = fadeOut(p.age, 2.3, 0.7);
          setAttrs(p.data.glow, { d, "stroke-dashoffset": 1 - drawn, opacity: fade });
          setAttrs(p.data.line, { d, "stroke-dashoffset": 1 - drawn, opacity: fade });
          const head = points[Math.round(drawn * 90)];
          setAttrs(p.data.tip, { opacity: drawn < 1 ? 1 : 0, transform: `translate(${f1(head.x)} ${f1(head.y)}) rotate(${Math.round(p.age * 300)}) scale(4)` });
          once(p, "done", 1.3, () => react("nod"));
          glance(p);
        },
      },
      incense: {
        duration: 3.4,
        begin(p) {
          p.data.wisps = [-34, -14, 14, 34].map((dx, i) => ({
            dx, i,
            node: fxNode("path", { fill: "none", stroke: "rgba(190,160,235,.75)", "stroke-width": 2.2, "stroke-linecap": "round", pathLength: 1, "stroke-dasharray": "1 1", opacity: 0 }),
          }));
        },
        tick(p) {
          const o = swell(p.age, 0, 0.6, 2.7, 3.4);
          const k = size / 78;
          s.magic = o;
          for (const wisp of p.data.wisps) {
            const base = spiderAt(50 + wisp.dx, 80 - Math.abs(wisp.dx) * 0.4);
            let d = "";
            for (let j = 0; j <= 14; j += 1) {
              const x = base.x + Math.sin(j * 0.55 - p.age * 3 + wisp.i * 1.7) * (1.5 + j * 0.7) * k;
              d += `${j ? "L" : "M"}${f1(x)} ${f1(base.y - j * 5 * k)}`;
            }
            setAttrs(wisp.node, { d, "stroke-dashoffset": 1 - ease((p.age - wisp.i * 0.2) / 1.2), opacity: o * 0.8 });
          }
          once(p, "calm", 2.6, () => react("nod"));
          gazeAt(p, pt(body.x + size / 2, body.y - 80));
        },
      },
      whirlwind: {
        duration: 3,
        begin(p) {
          p.data.puffs = Array.from({ length: 18 }, (_, i) => ({ a: i * 2.4, h: i / 18, node: fxNode("circle", { fill: "url(#fm-genie-smoke)", opacity: 0 }) }));
        },
        tick(p) {
          lift = swell(p.age, 0.4, 1, 2, 2.6) * (10 + Math.sin(p.age * 6) * 3);
          s.air = Math.max(s.air, lift / 40);
          const c = spiderAt(50, GROUND);
          const scatter = ease((p.age - 2.2) / 0.7);
          const grow = ease(p.age / 0.8);
          for (const smoke of p.data.puffs) {
            const a = smoke.a + p.age * 8;
            const h = (smoke.h + p.age * 0.35) % 1;
            const radius = size * (0.18 + 0.42 * h) * (1 + scatter * 2.5);
            setAttrs(smoke.node, {
              cx: c.x + Math.cos(a) * radius, cy: c.y + lift - h * size * 1.25 * grow + Math.sin(a) * radius * 0.18,
              r: size * (0.07 + 0.08 * h), opacity: grow * (1 - scatter) * 0.85,
            });
          }
          once(p, "dizzy", 2.2, () => react("dizzy"));
        },
      },
    };

    function stepPlay(dt) {
      if (!play) return;
      if (press || parked || !bubble.hidden || body.mode !== "stand") {
        endPlay();
        return;
      }
      const act = ACTS[play.name];
      if (play.phase === "walk") {
        if (body.targetX !== null) return;
        play.phase = "act";
        play.age = 0;
        play.duration = act.duration || 0;
        try {
          if (act.begin) act.begin(play);
        } catch (_) {
          endPlay();
          return;
        }
      } else {
        play.age += dt;
      }
      const r = play.element.isConnected ? play.element.getBoundingClientRect() : null;
      if (!r || !r.width || r.bottom <= 0 || r.top >= height || r.right <= 0 || r.left >= width || play.age >= play.duration) {
        endPlay();
        return;
      }
      play.rect = r;
      try {
        act.tick(play);
        if (play.data.parts) stepParts(play);
      } catch (_) {
        endPlay();
        return;
      }
    }

    function endPlay() {
      if (!play) return;
      play = null;
      s.point = 0;
      s.magic = 0;
      lift = 0;
      Object.assign(spell, { scale: 1, spin: 0, turn: 0, fade: 1 });
      if (s.mood === "thinking") s.mood = "idle";
      if (fxLayer) {
        fxLayer.remove();
        fxLayer = null;
      }
      nextWander = performance.now() + 5000 + Math.random() * 8000;
      place();
      wake();
    }

    function react(name) {
      s.reaction = name;
      s.reactionAge = 0;
      wake();
    }

    function place() {
      const sink = size * (1 - GROUND / 100);
      const charm = spell.scale !== 1 || spell.spin || spell.turn
        ? ` rotate(${spell.turn.toFixed(1)}deg) rotateY(${spell.spin.toFixed(1)}deg) scale(${spell.scale.toFixed(3)})` : "";
      host.style.transform = `translate3d(${Math.round(body.x)}px,${Math.round(body.y - lift + sink)}px,0)${charm}`;
      host.style.opacity = spell.fade < 1 ? spell.fade.toFixed(2) : "";
      if (!bubble.hidden) placeBubble();
    }

    function placeBubble() {
      const rect = bubble.getBoundingClientRect();
      const bw = rect.width || 300;
      const bh = rect.height || 120;
      let left = clamp(body.x + size / 2 - bw / 2, 8, Math.max(8, width - bw - 8));
      let top = body.y - bh - 4;
      if (top < 8) {
        top = clamp(body.y, 8, Math.max(8, height - bh - 8));
        left = body.x > width / 2 ? body.x - bw - 6 : body.x + size + 6;
        left = clamp(left, 8, Math.max(8, width - bw - 8));
      }
      bubble.style.transform = `translate3d(${Math.round(left)}px,${Math.round(top)}px,0)`;
    }

    function land(impact) {
      body.vy = 0;
      s.land = 1;
      if (impact > 1400) react("giggle");
      if (Math.abs(body.vx) > 20) {
        body.mode = "skid";
      } else {
        body.vx = 0;
        settle();
      }
    }

    function settle() {
      body.mode = "stand";
      body.vx = 0;
      if (!parked) {
        const spot = restSpot(body.x);
        home = clamp(spot / Math.max(width - size, 1), 0, 1);
        write(localStorage, STORAGE.spot, home.toFixed(4));
        if (Math.abs(spot - body.x) > 1) body.targetX = spot;
      }
    }

    function physics(dt) {
      if (body.mode === "fly") {
        const prevBottom = body.y + size;
        Object.assign(body, flightStep(body, dt));
        if (body.x < 0) {
          body.x = 0;
          body.vx = -body.vx * FLIGHT.wallBounce;
          body.vy *= FLIGHT.floorGrip;
        } else if (body.x > width - size) {
          body.x = width - size;
          body.vx = -body.vx * FLIGHT.wallBounce;
          body.vy *= FLIGHT.floorGrip;
        }
        if (body.y < 0) {
          body.y = 0;
          body.vy = -body.vy * FLIGHT.wallBounce;
          body.vx *= FLIGHT.floorGrip;
        }
        const floor = furnitureTop(body.x, prevBottom) - size;
        if (body.y >= floor) {
          body.y = floor;
          const impact = body.vy;
          const rebound = landingVelocity(impact);
          if (rebound === 0) {
            land(impact);
          } else {
            body.vy = rebound;
            body.vx *= FLIGHT.floorGrip;
          }
        }
        return;
      }
      if (body.mode === "drag") return;
      const floor = furnitureTop(body.x) - size;
      if (body.y < floor - 1) {
        body.mode = "fly";
        return;
      }
      if (body.y > floor + 1) {
        body.y = Math.max(floor, body.y - 220 * dt);
        return;
      }
      body.y = floor;
      if (body.mode === "skid") {
        const slow = 2400 * dt;
        body.vx = Math.abs(body.vx) <= slow ? 0 : body.vx - Math.sign(body.vx) * slow;
        body.x += body.vx * dt;
        if (body.x < 0 || body.x > width - size) {
          body.x = clamp(body.x, 0, width - size);
          body.vx = -body.vx * 0.55;
        }
        if (Math.abs(body.vx) < 20) settle();
        return;
      }
      if (body.targetX !== null) {
        const dx = body.targetX - body.x;
        const speed = parked ? 160 : 70;
        if (Math.abs(dx) < 1.5) {
          body.x = body.targetX;
          body.targetX = null;
        } else {
          body.x += Math.sign(dx) * Math.min(Math.abs(dx), speed * dt);
          s.facing = Math.sign(dx);
        }
      }
    }

    function idle(now) {
      if (play || press || parked || !bubble.hidden || body.mode !== "stand" || body.targetX !== null) return;
      if (now < nextWander) return;
      nextWander = now + 5000 + Math.random() * 8000;
      const stay = reducedMotion || still;
      if (Math.random() < (stay ? 0.35 : 0.5) && startPlay(stay)) return;
      if (stay) return;
      const spot = restSpot(clamp(homeX() + (Math.random() * 2 - 1) * 260, 0, Math.max(width - size, 0)));
      if (Math.abs(spot - body.x) > 1.5) body.targetX = spot;
    }

    function guard(now) {
      if (play || press || body.mode !== "stand" || body.targetX !== null || now - lastGuard < 1000) return;
      lastGuard = now;
      if (coveredHere() && !keepClear()) lastGuard = now + 4000;
    }

    function frame(now) {
      raf = 0;
      if (document.hidden || gone) return;
      const dt = clamp((now - (lastFrame || now)) / 1000, 0, 0.1);
      lastFrame = now;
      if (!reducedMotion) s.t += dt;
      if (s.reaction) {
        s.reactionAge += dt;
        if (s.reactionAge >= (DURATIONS[s.reaction] || 0)) s.reaction = "";
      }
      idle(now);
      accumulator += dt;
      while (accumulator >= STEP) {
        physics(STEP);
        accumulator -= STEP;
      }
      const moving = body.targetX !== null || body.mode === "skid";
      s.walking += ((moving ? 1 : 0) - s.walking) * Math.min(dt * 8, 1);
      if (moving) s.stridePhase += dt * 11;
      s.air += ((body.mode === "fly" ? 1 : 0) - s.air) * Math.min(dt * 10, 1);
      s.land = Math.max(0, s.land - dt / 0.3);
      if (s.walking < 0.02 && !moving) s.facing *= Math.max(0, 1 - dt * 3);
      stepPlay(dt);
      guard(now);
      s.gaze = null;
      if (pointer) {
        const cx = body.x + size / 2;
        const cy = body.y + size * 0.62;
        const dx = pointer.x - cx;
        const dy = pointer.y - cy;
        if (Math.hypot(dx, dy) < 480) s.gaze = { x: dx / 240, y: dy / 240 };
      }
      if (play && play.gaze) s.gaze = play.gaze;
      const busy = body.mode === "fly" || body.mode === "drag";
      const lively = busy || moving || play || s.reaction || s.gaze || s.land > 0;
      const interval = busy ? 0 : lively ? 1000 / 30 : reducedMotion ? 1000 : 1000 / 12;
      if (now - lastDraw >= interval) {
        lastDraw = now;
        place();
        draw();
      }
      if (lively || !bubble.hidden) {
        raf = requestAnimationFrame(frame);
      } else if (!reducedMotion) {
        raf = setTimeout(() => {
          raf = requestAnimationFrame(frame);
        }, 1000 / 12);
      }
    }

    function wake() {
      if (!raf && !document.hidden && !gone) {
        lastFrame = performance.now();
        raf = requestAnimationFrame(frame);
      }
    }

    function draw() {
      const ratio = canvas.width / 100;
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
      painter(ctx, s);
    }

    function hideBubble() {
      bubble.hidden = true;
      bubbleHovered = false;
      clearTimeout(bubbleTimer);
      s.mood = "idle";
      wake();
    }

    function scheduleClose(ms) {
      clearTimeout(bubbleTimer);
      bubbleTimer = setTimeout(() => {
        if (!bubbleHovered) hideBubble();
      }, ms);
    }

    function button(label, action) {
      const element = document.createElement("button");
      element.type = "button";
      element.textContent = label;
      element.addEventListener("click", (event) => {
        event.stopPropagation();
        action();
      });
      return element;
    }

    function showBubble(line, options = {}) {
      endPlay();
      bubble.replaceChildren();
      const text = document.createElement("p");
      text.textContent = line;
      bubble.appendChild(text);
      if (options.actions) {
        const row = document.createElement("div");
        row.className = "fm-genie-row";
        for (const [label, action] of options.actions) row.appendChild(button(label, action));
        bubble.appendChild(row);
      }
      bubble.hidden = false;
      s.mood = "speaking";
      setTimeout(() => {
        if (s.mood === "speaking") s.mood = "idle";
      }, 1600);
      placeBubble();
      scheduleClose(options.stay || 5000);
      wake();
    }

    function nextTip() {
      if (parked) {
        unpark();
        return;
      }
      const list = tips(context);
      if (!list.length) return;
      react("wave");
      showBubble(list[tipIndex++ % list.length]);
    }

    function menu() {
      react("nod");
      showBubble("What would you like?", {
        stay: 20000,
        actions: [
          ["Next tip", nextTip],
          [still ? "Wander again" : "Stay put", () => {
            still = !still;
            write(localStorage, STORAGE.still, still ? "1" : null);
            body.targetX = null;
            hideBubble();
          }],
          ["Tuck away", park],
          ["Hide for this visit", () => {
            setKey("sessionStorage", AWAY_KEY, "1");
            teardown();
            announce();
          }],
          ["Hide for good", api.hide],
        ],
      });
    }

    function park() {
      endPlay();
      parked = true;
      write(localStorage, STORAGE.parked, "1");
      hideBubble();
      chooseParkSide();
      body.targetX = homeX();
      if (body.mode !== "stand") body.mode = "fly";
      wake();
    }

    function unpark() {
      parked = false;
      write(localStorage, STORAGE.parked, null);
      body.targetX = restSpot(clamp(homeX(), 0, Math.max(width - size, 0)));
      react("wave");
    }

    host.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      endPlay();
      try {
        host.setPointerCapture(event.pointerId);
      } catch (_) {}
      press = {
        id: event.pointerId,
        startX: event.clientX,
        startY: event.clientY,
        offsetX: event.clientX - body.x,
        offsetY: event.clientY - body.y,
        dragging: false,
        held: false,
        samples: [[performance.now(), event.clientX, event.clientY]],
        timer: setTimeout(() => {
          if (press && !press.dragging) {
            press.held = true;
            menu();
          }
        }, 550),
      };
      react("nod");
    });

    host.addEventListener("pointermove", (event) => {
      pointer = { x: event.clientX, y: event.clientY };
      if (!press || press.id !== event.pointerId) return;
      if (!press.dragging && Math.hypot(event.clientX - press.startX, event.clientY - press.startY) > 6) {
        press.dragging = true;
        clearTimeout(press.timer);
        host.classList.add("is-dragging");
        body.mode = "drag";
        body.targetX = null;
        hideBubble();
        react("surprise");
      }
      if (press.dragging) {
        const now = performance.now();
        press.samples.push([now, event.clientX, event.clientY]);
        while (press.samples.length > 2 && now - press.samples[0][0] > 80) press.samples.shift();
        body.x = clamp(event.clientX - press.offsetX, 0, width - size);
        body.y = clamp(event.clientY - press.offsetY, 0, height - size);
        place();
        wake();
      }
    });

    function release(event, cancelled) {
      if (!press || press.id !== event.pointerId) return;
      clearTimeout(press.timer);
      host.classList.remove("is-dragging");
      const current = press;
      press = null;
      if (current.dragging) {
        const now = performance.now();
        current.samples.push([now, event.clientX, event.clientY]);
        while (current.samples.length > 2 && now - current.samples[0][0] > 80) current.samples.shift();
        const first = current.samples[0];
        const last = current.samples[current.samples.length - 1];
        const span = Math.max((last[0] - first[0]) / 1000, 0.016);
        body.vx = cancelled ? 0 : (last[1] - first[1]) / span;
        body.vy = cancelled ? 0 : (last[2] - first[2]) / span;
        body.mode = "fly";
        if (parked) {
          parked = false;
          write(localStorage, STORAGE.parked, null);
        }
        wake();
        return;
      }
      if (!current.held && !cancelled) nextTip();
    }

    host.addEventListener("pointerup", (event) => release(event, false));
    host.addEventListener("pointercancel", (event) => release(event, true));
    host.addEventListener("click", (event) => {
      if (event.detail === 0) nextTip();
    });
    host.addEventListener("contextmenu", (event) => {
      event.preventDefault();
      menu();
    });
    bubble.addEventListener("pointerenter", () => {
      bubbleHovered = true;
      clearTimeout(bubbleTimer);
    });
    bubble.addEventListener("pointerleave", () => {
      bubbleHovered = false;
      scheduleClose(4000);
    });
    document.addEventListener("pointermove", (event) => {
      pointer = { x: event.clientX, y: event.clientY };
      const dx = pointer.x - (body.x + size / 2);
      const dy = pointer.y - (body.y + size / 2);
      if (Math.hypot(dx, dy) < 480) wake();
    }, { passive: true, ...live });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape" && !bubble.hidden) hideBubble();
    }, live);
    document.addEventListener("visibilitychange", () => {
      if (document.hidden) endPlay();
      else wake();
    }, live);
    document.addEventListener("scroll", () => {
      clearTimeout(scrollTimer);
      scrollTimer = setTimeout(() => {
        keep.at = -Infinity;
        furniture.at = -Infinity;
        if (play && play.phase === "act") {
          if (!coveredHere()) return;
          endPlay();
        }
        keepClear();
      }, 200);
    }, { passive: true, capture: true, ...live });
    window.addEventListener("resize", () => {
      measure();
      keep.at = -Infinity;
      endPlay();
      body.x = parked ? homeX() : clamp(body.x, 0, Math.max(width - size, 0));
      body.y = Math.min(body.y, height - size);
      if (body.mode === "stand") body.y = furnitureTop(clamp(body.x, 0, Math.max(width - size, 0))) - size;
      place();
      draw();
      keepClear();
      wake();
    }, live);

    measure();
    body.x = clamp(homeX(), 0, Math.max(width - size, 0));
    body.y = furnitureTop(body.x) - size;
    if (parked) {
      chooseParkSide();
      body.x = homeX();
    } else {
      body.x = restSpot(body.x);
      body.y = furnitureTop(body.x) - size;
    }
    place();
    draw();
    wake();

    const heartbeat = setInterval(() => wake(), 1500);

    refreshContext();

    window.addEventListener("forkmesh:session-restored", () => {
      refreshContext();
    }, live);

    async function refreshContext() {
      const next = { path: location.pathname, account: "", unread: null, chatNew: 0 };
      let session = null;
      try {
        session = JSON.parse(read(localStorage, "forkmesh.session") || "null");
      } catch (_) {}
      if (session && session.nodeName) {
        next.account = String(session.nodeName);
        next.unread = await unreadCount(next.account, String(session.sessionToken || ""));
      }
      try {
        const activity = JSON.parse(read(sessionStorage, "forkmesh.chatActivity") || "null");
        const seen = JSON.parse(read(localStorage, "forkmesh.chat.activitySeen") || "null");
        if (activity && seen && normalizePath(location.pathname) !== "/chat") {
          next.chatNew = Math.max(0, (Number(activity.messageCount) || 0) - (Number(seen.messageCount) || 0));
        }
      } catch (_) {}
      context = next;
      return context;
    }

    async function unreadCount(account, token) {
      try {
        const cached = JSON.parse(read(sessionStorage, STORAGE.inbox) || "null");
        if (cached && cached.account === account && cached.expiresAt > Date.now()) return cached.unread;
      } catch (_) {}
      if (location.protocol === "file:") return null;
      try {
        const response = await fetch(`/api/notifications?node=${encodeURIComponent(account)}&limit=1`, {
          headers: {
            accept: "application/json",
            ...(token && token !== "cookie" ? { authorization: `Bearer ${token}` } : {}),
          },
          credentials: "include",
          cache: "no-store",
        });
        if (!response.ok) return null;
        const data = await response.json();
        const unread = Number(data && data.unread);
        if (!Number.isFinite(unread)) return null;
        write(sessionStorage, STORAGE.inbox, JSON.stringify({ account, unread, expiresAt: Date.now() + 5 * 60 * 1000 }));
        return unread;
      } catch (_) {
        return null;
      }
    }

    api.next = nextTip;
    api.tuck = park;
    api.play = (name) => (play || parked || !bubble.hidden || body.mode !== "stand" ? "" : startPlay(reducedMotion || still, name));
    api.playing = () => (play ? `${play.name}:${play.phase}` : "");

    function teardown() {
      if (gone) return;
      gone = true;
      abort.abort();
      clearInterval(heartbeat);
      clearTimeout(bubbleTimer);
      clearTimeout(scrollTimer);
      if (press) clearTimeout(press.timer);
      press = null;
      endPlay();
      host.remove();
      bubble.remove();
      style.remove();
      if (stop === teardown) stop = null;
    }

    return teardown;
  }

  boot();
})();
