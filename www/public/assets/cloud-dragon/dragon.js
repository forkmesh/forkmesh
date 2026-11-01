(function () {
  const TAU = Math.PI * 2;
  const GROUND = 96.2;
  const clamp = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
  const clamp01 = (v) => clamp(v, 0, 1);
  const smooth = (v) => {
    const u = clamp01(v);
    return u * u * (3 - 2 * u);
  };
  const back = (v) => {
    const u = clamp01(v) - 1;
    return 1 + 2.7 * u * u * u + 1.7 * u * u;
  };
  const arc = (v) => Math.sin(Math.PI * clamp01(v));
  const seed = (n) => {
    const x = Math.sin(n * 127.1 + 311.7) * 43758.5453;
    return x - Math.floor(x);
  };
  const span = (u, from, to) => clamp01((u - from) / (to - from));
  const wave = (age, speed, phase = 0) => Math.sin(age * speed + phase);
  const bump = (u, from, to) => arc(span(u, from, to));
  const mixed = (a, b, k) => a + (b - a) * k;
  const INK = {
    cream: "#f3e6cc", creamDeep: "#e6d3ae", mint: "#0ebe60", leaf: "#2fcf7c", soft: "#a6d8c4", gold: "#ffc94d",
    ember: "#ff9a2e", pink: "#f47f86", night: "#111827", white: "#ffffff", lilac: "#c9cbe6", steel: "#aab2bf",
  };
  const ALL = ["heading", "short", "button", "link", "nav", "image", "logo", "input", "code", "card"];

  function tick(ctx, x, y, angle, length, width, color) {
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(angle);
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.moveTo(-length / 2, 0);
    ctx.quadraticCurveTo(0, -width, length / 2, 0);
    ctx.quadraticCurveTo(0, width, -length / 2, 0);
    ctx.fill();
    ctx.restore();
  }

  function ticks(ctx, x, y, u, color) {
    const pop = back(span(u, 0, 0.5));
    const fade = 1 - span(u, 0.72, 1);
    ctx.globalAlpha = fade;
    for (let i = 0; i < 3; i += 1) {
      const angle = -1.25 + i * 0.5;
      const r = 8 + pop * 7;
      tick(ctx, x + Math.cos(angle) * r, y + Math.sin(angle) * r, angle, 9 * pop, 1.9, color);
    }
    ctx.globalAlpha = 1;
  }

  function sparkle(ctx, x, y, r, alpha, color) {
    ctx.globalAlpha = alpha;
    ctx.fillStyle = color || INK.gold;
    ctx.beginPath();
    for (let i = 0; i < 8; i += 1) {
      const a = (i * Math.PI) / 4;
      const q = i % 2 ? r * 0.28 : r;
      ctx.lineTo(x + Math.cos(a) * q, y + Math.sin(a) * q);
    }
    ctx.closePath();
    ctx.fill();
    ctx.globalAlpha = 1;
  }

  function puff(ctx, x, y, r, alpha, color) {
    const g = ctx.createRadialGradient(x, y - r * 0.2, 0, x, y, r);
    g.addColorStop(0, color || "rgba(255,255,255,.95)");
    g.addColorStop(0.72, color || "rgba(246,240,232,.8)");
    g.addColorStop(1, "rgba(240,232,220,0)");
    ctx.globalAlpha = alpha;
    ctx.fillStyle = g;
    ctx.beginPath();
    ctx.arc(x, y, r, 0, TAU);
    ctx.fill();
    ctx.globalAlpha = 1;
  }

  function heart(ctx, x, y, r, color, alpha) {
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.translate(x, y);
    ctx.scale(r / 10, r / 10);
    ctx.fillStyle = color || INK.pink;
    ctx.beginPath();
    ctx.moveTo(0, 8);
    ctx.bezierCurveTo(-13, -1, -7, -10, 0, -4);
    ctx.bezierCurveTo(7, -10, 13, -1, 0, 8);
    ctx.fill();
    ctx.restore();
  }

  function note(ctx, x, y, r, alpha) {
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.translate(x, y);
    ctx.fillStyle = INK.leaf;
    ctx.strokeStyle = INK.leaf;
    ctx.lineWidth = r * 0.28;
    ctx.beginPath();
    ctx.ellipse(0, 0, r * 0.55, r * 0.42, -0.3, 0, TAU);
    ctx.fill();
    ctx.beginPath();
    ctx.moveTo(r * 0.45, 0);
    ctx.lineTo(r * 0.45, -r * 1.6);
    ctx.quadraticCurveTo(r * 1.1, -r * 1.3, r * 1.1, -r * 0.8);
    ctx.stroke();
    ctx.restore();
  }

  function letter(ctx, text, x, y, size, alpha, color) {
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.fillStyle = color || "#8f93e0";
    ctx.font = `800 ${size}px ui-rounded, "SF Pro Rounded", -apple-system, system-ui, sans-serif`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(text, x, y);
    ctx.restore();
  }

  function dust(ctx, x, y, u, spread, count) {
    for (let i = 0; i < count; i += 1) {
      const side = i % 2 ? 1 : -1;
      const r = (3 + seed(i) * 4) * (0.6 + u);
      puff(ctx, x + side * spread * (0.2 + 0.8 * seed(i + 4)) * smooth(u), y - seed(i + 9) * 3 - u * 4, r, 0.75 * (1 - u));
    }
  }

  const hold = (c, from = 0.08, to = 0.92) => smooth(span(c.u, 0, from)) * (1 - smooth(span(c.u, to, 1)));

  const ACTIVITIES = {
    "breathe-fire": {
      title: "Breathes a little fire", duration: 3, fits: ["heading", "logo", "card", "image"],
      pose(c) {
        const inhale = smooth(span(c.u, 0.02, 0.2)) * (1 - smooth(span(c.u, 0.2, 0.28)));
        const blow = smooth(span(c.u, 0.22, 0.3)) * (1 - smooth(span(c.u, 0.82, 0.94)));
        return {
          head: -0.26 * inhale - 0.05 * blow, lean: -0.1 * inhale + 0.1 * blow, x: -22 * blow, chest: 0.05 * inhale,
          open: 0.25 * inhale + blow, wing: 0.25 * inhale - 0.1 * blow, lid: blow, happy: blow, jitter: blow * 3,
          pawN: [-30 * blow, -40 * blow], pawF: [-20 * blow, -30 * blow],
        };
      },
      head(ctx, c) {
        const blow = smooth(span(c.u, 0.24, 0.32)) * (1 - smooth(span(c.u, 0.84, 0.96)));
        if (blow <= 0) return;
        const flicker = 1 + wave(c.age, 31) * 0.05;
        c.flame(blow * flicker, 1 + wave(c.age, 23) * 0.1);
      },
      over(ctx, c) {
        const blow = smooth(span(c.u, 0.3, 0.4)) * (1 - smooth(span(c.u, 0.8, 0.92)));
        for (let i = 0; i < 9; i += 1) {
          const life = (c.age * 1.5 + seed(i)) % 1;
          sparkle(ctx, 58 + life * 36, -48 + (seed(i + 3) - 0.5) * 22 - life * 12, 1.8 * (1 - life), blow * (1 - life), i % 2 ? INK.gold : INK.ember);
        }
      },
    },
    land: {
      title: "Drops in with a dusty landing", duration: 2.4, fits: ALL,
      pose(c) {
        const rise = span(c.age, 0, 0.5);
        const fall = span(c.age, 0.5, 0.9);
        const hit = span(c.age, 0.9, 1.5);
        const up = rise < 1 ? smooth(rise) : 1 - fall * fall;
        return {
          y: -620 * up, flap: up * (1 - fall), lean: 0.3 * up, tail: up * 0.8, crouch: (hit > 0 && hit < 1 ? 1 - hit : 0) + 0.3 * bump(c.age, 0, 0.14),
          pawN: [30 * up, -130 * up], pawF: [30 * up, -130 * up], footN: [0, -40 * up], footF: [0, -40 * up],
          lid: hit > 0 && hit < 0.6 ? 1 : 0, happy: hit > 0 && hit < 0.6 ? 1 : 0, wing: 0.5 * (hit > 0 ? 1 - hit : 0),
        };
      },
      over(ctx, c) {
        if (c.age > 0.9) dust(ctx, 0, -2, clamp01((c.age - 0.9) / 0.9), 48, 14);
      },
    },
    terminal: {
      title: "Taps away at a little terminal", duration: 3.6, fits: ["code", "input", "card"],
      pose(c) {
        const on = hold(c);
        return {
          crouch: 0.22 * on, lean: 0.14 * on, head: 0.1 * on + 0.015 * wave(c.age, 2.2), gx: 0.4, gy: 0.5,
          pawN: [120 * on, (40 + Math.max(wave(c.age, 19), 0) * -22) * on], pawF: [85 * on, (70 + Math.max(wave(c.age, 19, 3.1), 0) * -22) * on],
        };
      },
      gear: [{ prop: "laptop", at: "ground", x: 330, y: 1135, scale: 1 }],
      over(ctx, c) {
        const on = hold(c);
        for (let i = 0; i < 5; i += 1) {
          const life = (c.age * 0.55 + i / 5) % 1;
          letter(ctx, i % 2 ? "</>" : "{ }", 42 + seed(i) * 14 + life * 5, -40 - life * 34, 6.4, arc(life) * 0.95 * on, INK.leaf);
        }
      },
    },
    coffee: {
      title: "Sips a warm cup of coffee", duration: 4, fits: ALL,
      pose(c) {
        const on = hold(c);
        const sip = bump(c.u, 0.25, 0.48) + bump(c.u, 0.6, 0.82);
        return {
          head: 0.06 * on - 0.2 * sip, lean: -0.05 * sip, lid: sip, happy: sip, wing: 0.03 * sip, reach: on,
          pawN: [150 + 30 * sip, -30 - 150 * sip], pawF: [255 + 20 * sip, -10 - 150 * sip],
        };
      },
      gear: [{ prop: "mug", at: "pawN", x: 88, y: -18, scale: 1.05, order: "hug" }],
      over(ctx, c) {
        const on = hold(c);
        const p = c.at("pawN", 88, -130);
        ctx.lineCap = "round";
        ctx.lineWidth = 1.4;
        for (let i = 0; i < 3; i += 1) {
          const life = (c.age * 0.42 + i / 3) % 1;
          ctx.strokeStyle = `rgba(190,196,210,${0.75 * arc(life) * on})`;
          ctx.beginPath();
          for (let k = 0; k <= 8; k += 1) {
            const v = k / 8;
            const x = p.x + (i - 1) * 3 + Math.sin(v * 6 + c.age * 3 + i) * 2.4;
            const y = p.y - (life * 14 + v * 12);
            if (k) ctx.lineTo(x, y);
            else ctx.moveTo(x, y);
          }
          ctx.stroke();
        }
      },
    },
    book: {
      title: "Reads a page or two", duration: 4, fits: ["heading", "code", "card"],
      pose(c) {
        const on = hold(c);
        return {
          head: 0.14 * on + wave(c.age, 1.1) * 0.02, gx: wave(c.age, 2.1) * 0.6, gy: 0.75 * on, gh: 0.2, gv: 0.6, reach: on,
          pawN: [75, 140], pawF: [335, 115],
        };
      },
      gear: [{ prop: "book", at: "pawN", x: 118, y: -20, scale: 1, order: "hug" }],
    },
    magnify: {
      title: "Inspects it through a magnifying glass", duration: 3.6, fits: ["code", "image", "heading", "card"],
      pose(c) {
        const on = hold(c);
        const sweep = wave(c.age, 1.6);
        return {
          y: -64 * on, lean: 0.72 * on, crouch: on, head: 0.05 * on + sweep * 0.035, hy: 44 * on, x: sweep * 14, gx: 0.9, gy: 0.35, gh: 0.1, reach: on,
          wing: 0.14, pawN: [205, 235], pawF: [80, 250], footN: [-40, 64], footF: [-30, 64],
        };
      },
      head(ctx, c) {
        const on = hold(c);
        if (on > 0) c.prop("magnifier", 1190, 700, on * 1.04);
      },
    },
    celebrate: {
      title: "Throws confetti", duration: 3.2, fits: ["button", "heading", "card", "logo"],
      pose(c) {
        const h = Math.abs(wave(c.age, 5.2)) * hold(c);
        return {
          y: -110 * h, crouch: 0.2 * (1 - h), open: 1, lid: 1, happy: 1, head: wave(c.age, 2.6) * 0.1, wing: 0.25, flap: 0.3,
          pawN: [-210, -340], pawF: [210, -340], footN: [0, -26 * h], footF: [0, -26 * h], front: 1,
        };
      },
      over(ctx, c) {
        const colors = [INK.mint, INK.gold, INK.pink, "#6aa4ff", "#b58cff"];
        for (let i = 0; i < 34; i += 1) {
          const age = c.age - seed(i) * 0.7;
          if (age < 0) continue;
          const side = seed(i + 40) - 0.5;
          ctx.save();
          ctx.globalAlpha = 1 - span(age, 1.8, 2.3);
          ctx.translate(side * (72 + age * 14) + Math.sin(age * 5 + i) * 3, -104 - Math.sin(clamp01(age / 0.5) * 1.57) * 26 + age * age * 22);
          ctx.rotate(age * (3 + seed(i + 7) * 6));
          ctx.scale(1, Math.cos(age * 7 + i));
          ctx.fillStyle = colors[i % 5];
          ctx.beginPath();
          ctx.roundRect(-1.7, -0.9, 3.4, 1.8, 0.6);
          ctx.fill();
          ctx.restore();
        }
      },
    },
    headphones: {
      title: "Vibes to a tune on headphones", duration: 4.2, fits: ALL,
      pose(c) {
        const on = hold(c);
        const beat = wave(c.age, 7.5);
        return {
          head: wave(c.age, 3.75) * 0.1 * on, y: -Math.abs(beat) * 16 * on, lid: on, lean: wave(c.age, 3.75) * 0.03,
          footN: [0, -Math.max(beat, 0) * 34 * on], pawN: [10, -30 + wave(c.age, 7.5) * 18], pawF: [10, -30 - wave(c.age, 7.5) * 18], tail: 0.2, sway: 2,
        };
      },
      head(ctx, c) {
        const on = hold(c);
        if (on <= 0) return;
        ctx.save();
        ctx.globalAlpha *= on;
        ctx.strokeStyle = "#2c3138";
        ctx.lineWidth = 34;
        ctx.lineCap = "round";
        ctx.beginPath();
        ctx.arc(860, 470, 300, -2.5, -0.2);
        ctx.stroke();
        ctx.strokeStyle = "#51585f";
        ctx.lineWidth = 7;
        ctx.stroke();
        ctx.restore();
        c.prop("cup", 615, 460, on * 1.05);
      },
      over(ctx, c) {
        for (let i = 0; i < 4; i += 1) {
          const life = (c.age * 0.5 + i / 4) % 1;
          note(ctx, (i % 2 ? 42 : -36) + Math.sin(life * 7 + i) * 3, -66 - life * 32, 3.2, arc(life) * hold(c));
        }
      },
    },
    "code-fast": {
      title: "Codes at full speed", duration: 2.8, fits: ["code", "input"],
      pose(c) {
        const on = hold(c);
        return {
          crouch: 0.25 * on, lean: 0.24 * on, head: 0.08 * on, jitter: 5 * on, gx: 0.4, gy: 0.5, tail: 0.7, sway: 3, open: 0.3 * on, wing: -0.1,
          pawN: [125 * on, (40 + Math.max(wave(c.age, 34), 0) * -26) * on], pawF: [88 * on, (70 + Math.max(wave(c.age, 34, 3.1), 0) * -26) * on],
        };
      },
      gear: [{ prop: "laptop", at: "ground", x: 340, y: 1135, scale: 1 }],
      under(ctx, c) {
        const on = hold(c);
        ctx.lineCap = "round";
        for (let i = 0; i < 7; i += 1) {
          const life = (c.age * 2.3 + seed(i)) % 1;
          const y = -66 + i * 9;
          ctx.strokeStyle = `rgba(14,190,96,${0.8 * (1 - life) * on})`;
          ctx.lineWidth = 1.3;
          ctx.beginPath();
          ctx.moveTo(-46 - life * 26, y);
          ctx.lineTo(-46 - life * 26 - 12 - seed(i + 5) * 12, y);
          ctx.stroke();
        }
      },
    },
    upload: {
      title: "Hugs a cloud while it syncs", duration: 3.8, fits: ALL,
      pose(c) {
        const on = hold(c);
        return {
          y: wave(c.age, 2.4) * 10, head: 0.06 * on + wave(c.age, 1.3) * 0.03, lid: on, happy: on, lean: 0.05 * on, front: 1,
          pawN: [150 * on, -60 * on], pawF: [40 * on, -80 * on],
        };
      },
      gear: [{ prop: "upcloud", at: "ground", x: 190, y: 1040, scale: 0.86, order: "hug" }],
      over(ctx, c) {
        const life = (c.age * 0.8) % 1;
        c.propAt("arrow", 14, -15 - life * 12, 0.72, { alpha: arc(life) * hold(c) });
      },
    },
    protect: {
      title: "Stands guard with a shield", duration: 3.4, fits: ["button", "input", "logo", "card"],
      pose(c) {
        const on = hold(c);
        const brace = bump(c.u, 0.08, 0.3);
        const nuzzle = wave(c.age, 1.5);
        return {
          lean: (0.14 + 0.05 * brace) * on, crouch: 0.2 * on, head: (0.2 + nuzzle * 0.02) * on, hx: 18 * on, hy: 56 * on, x: 22 * brace, gx: 0.7, gy: -0.2, gh: 0.1,
          wing: 0.16 * brace, reach: on, pawN: [150, 215], pawF: [330, 230],
        };
      },
      gear: [{ prop: "shield", at: "ground", x: 285, y: 925, scale: 0.6, order: "hug" }],
      over(ctx, c) {
        const band = (c.age % 1.6) / 1.6;
        sparkle(ctx, 6 + band * 28, -40 + band * 8, 3.4, arc(band) * hold(c), INK.white);
      },
    },
    build: {
      title: "Stacks up some blocks", duration: 3.2, fits: ["card", "button", "code"],
      pose(c) {
        const on = hold(c);
        const tap = Math.max(0, wave(c.age, 6));
        return {
          lean: 0.14 * on, crouch: 0.14 * on, head: 0.1 * on + tap * 0.03, gx: 0.8, gy: 0.5,
          pawF: [230 * on, (-30 - tap * 70) * on], pawN: [140 * on, (40 - Math.max(0, wave(c.age, 6, 3.1)) * 60) * on],
        };
      },
      gear: [{ prop: "blocks", at: "ground", x: 400, y: 1115, scale: 1 }],
      over(ctx, c) {
        const cycle = (c.age * 6) / TAU;
        const u = cycle - Math.floor(cycle);
        if (u > 0.5) {
          const v = (u - 0.5) * 2;
          sparkle(ctx, 42 + seed(Math.floor(cycle)) * 18, -46 - v * 5, 2.6 * (1 - v), (1 - v) * hold(c), INK.mint);
        }
      },
    },
    share: {
      title: "Sends off a paper plane", duration: 3.4, fits: ["link", "button", "nav"],
      pose(c) {
        const wind = bump(c.u, 0, 0.24);
        const throwIt = smooth(span(c.u, 0.2, 0.28));
        const rest = 1 - smooth(span(c.u, 0.6, 0.86));
        return {
          lean: -0.1 * wind + 0.14 * throwIt * rest, head: -0.1 * wind - 0.1 * rest * throwIt, gx: 1, gy: -0.7, open: 0.6 * throwIt * rest, x: -16 * wind + 16 * throwIt * rest,
          pawF: [250 * wind + 250 * throwIt * rest, -350 * wind - 200 * throwIt * rest], front: 1, footN: [0, -24 * bump(c.u, 0.2, 0.34)],
        };
      },
      over(ctx, c) {
        const u = span(c.u, 0.24, 0.94);
        if (u <= 0) {
          if (c.u > 0.04) c.propAt("plane", 24 - 16 * bump(c.u, 0, 0.24), -74, 0.42, { rot: -0.1 });
          return;
        }
        const path = (v) => ({ x: 34 + v * 104 - Math.sin(v * 7) * 11, y: -52 - v * 58 + Math.cos(v * 7) * 11 - 11 });
        ctx.strokeStyle = "rgba(92,126,180,.85)";
        ctx.setLineDash([2.2, 3]);
        ctx.lineWidth = 1;
        ctx.lineCap = "round";
        ctx.beginPath();
        for (let k = 0; k <= 34; k += 1) {
          const p = path((k / 34) * u);
          if (k) ctx.lineTo(p.x, p.y);
          else ctx.moveTo(p.x, p.y);
        }
        ctx.globalAlpha = 1 - span(c.u, 0.82, 1);
        ctx.stroke();
        ctx.setLineDash([]);
        const p = path(u);
        const q = path(Math.min(u + 0.01, 1.01));
        c.propAt("plane", p.x, p.y, 0.6, { rot: Math.atan2(q.y - p.y, q.x - p.x) + 0.42 });
        ctx.globalAlpha = 1;
      },
    },
    binoculars: {
      title: "Scans the page through binoculars", duration: 3.6, fits: ALL,
      pose(c) {
        const on = hold(c);
        const pan = wave(c.age, 1.3);
        return {
          y: -64 * on, lean: 0.72 * on, crouch: on, head: -0.04 * on + pan * 0.07, hy: 30 * on, x: pan * 18, gx: 1, gh: 0.1, reach: on,
          wing: 0.16, pawN: [120, -40], pawF: [440, -20], footN: [-40, 64], footF: [-30, 64],
        };
      },
      head(ctx, c) {
        const on = hold(c);
        if (on > 0) c.prop("binoculars", 1150, 500, on);
      },
      over(ctx, c) {
        const pulse = arc((c.age % 1.7) / 1.7);
        sparkle(ctx, 62, -50, 3.6 * pulse, pulse * hold(c), INK.white);
      },
    },
    travel: {
      title: "Sets off with a backpack", duration: 3.4, fits: ALL, stride: (c) => c.age * 9,
      pose(c) {
        const on = hold(c);
        return { ground: on, lid: on, happy: on, tail: 0.86 * on, tailBase: 0.03 * on, sway: 2.2 };
      },
      gear: [{ prop: "backpack", at: "torso", x: 545, y: 830, scale: 1, order: "under" }],
      under(ctx, c) {
        const u = (c.age * 1.43) % 1;
        puff(ctx, -30 - u * 14, -3 - u * 3, 3 + u * 4, 0.7 * (1 - u) * hold(c));
      },
    },
    sleep: {
      title: "Naps on a cloud", duration: 6, fits: ALL,
      pose(c) {
        const on = smooth(span(c.age, 0, 0.8));
        return {
          y: -150 * on + wave(c.age, 1.2) * 12, crouch: 1 * on, lean: 0.62 * on, head: 0.7 * on, hy: 150 * on, hx: 40 * on, lid: on, wing: -0.25 * on, chest: wave(c.age, 1.6) * 0.035,
          pawN: [70 * on, 90 * on], pawF: [10 * on, 90 * on], tail: -0.25 * on, sway: 0.25,
        };
      },
      under(ctx, c) {
        c.propAt("bed", 0, -19 + wave(c.age, 1.2) * 1.2, 0.86 * back(span(c.age, 0, 0.5)));
      },
      over(ctx, c) {
        for (let i = 0; i < 3; i += 1) {
          const life = (c.age * 0.33 + i / 3) % 1;
          c.propAt("zee", 46 + life * 18 + Math.sin(life * 6) * 2, -44 - life * 32, 0.4 + life * 0.6, { alpha: arc(life) * span(c.age, 0.6, 1) });
        }
      },
    },
    stretch: {
      title: "Wakes up with a big stretch", duration: 2.4, fits: ALL,
      pose(c) {
        const up = bump(c.u, 0, 0.82);
        return {
          y: -40 * up, chest: 0.05 * up, lean: -0.1 * up, head: -0.2 * up, open: up, oval: 0.4, lid: up, happy: up, wing: 0.42 * up, tail: 0.5 * up,
          pawN: [-230 * up, -420 * up], pawF: [230 * up, -420 * up], front: 1,
        };
      },
      over(ctx, c) {
        ticks(ctx, 42, -88, span(c.u, 0.2, 0.9), INK.gold);
      },
    },
    star: {
      title: "Cuddles a shiny star", duration: 3.2, fits: ["button", "heading", "logo"],
      pose(c) {
        const on = hold(c);
        return {
          head: 0.14 * on + wave(c.age, 2.4) * 0.05, lean: wave(c.age, 2.4) * 0.05, lid: on, happy: on, gx: 0.3,
          pawN: [150 * on, -110 * on], pawF: [40 * on, -120 * on], x: wave(c.age, 2.4) * 12,
        };
      },
      gear: [{ prop: "star", at: "chest", x: 190, y: 860, scale: 1 }],
      over(ctx, c) {
        for (let i = 0; i < 5; i += 1) {
          const life = (c.age * 0.7 + i / 5) % 1;
          c.propAt("sparkle", 30 + Math.cos(i * 2.4) * 34, -46 + Math.sin(i * 2.4) * 30, 0.5 * arc(life) + 0.1, { alpha: arc(life) * hold(c) });
        }
      },
    },
    globe: {
      title: "Surfs the network on a globe", duration: 4.2, fits: ["link", "nav", "image", "logo"],
      pose(c) {
        const on = hold(c, 0.16, 0.86);
        const rock = wave(c.age, 2.2);
        return {
          y: -610 * on + wave(c.age, 4.4) * 8 * on, x: (-90 + rock * 26) * on, lean: (0.55 + rock * 0.05) * on, crouch: 0.8 * on, head: (-0.3 - rock * 0.05) * on, hy: 40 * on,
          open: 0.35, wing: 0.22 + rock * 0.08, tail: 0.25 * on, sway: 2, reach: on, front: on, gx: 0.8, gy: -0.7,
          pawN: [150, 210], pawF: [330, 200],
        };
      },
      gear: [{ prop: "globe", at: "ground", x: 0, y: 760, scale: 1, order: "hug", rock: true }],
      over(ctx, c) {
        for (let i = 0; i < 4; i += 1) {
          const a = c.age * 2.3 + (i * TAU) / 4;
          const front = Math.sin(a);
          puff(ctx, Math.cos(a) * 58, -34 + front * 11, 1.8 + front * 0.6, (0.55 + front * 0.45) * hold(c, 0.16, 0.86), "rgba(170,255,235,.95)");
        }
      },
    },
    heart: {
      title: "Hugs a big heart", duration: 3.2, fits: ["image", "heading", "card", "logo"],
      pose(c) {
        const on = hold(c);
        return {
          head: 0.12 * on + wave(c.age, 2) * 0.04, lean: wave(c.age, 2) * 0.04, lid: on,
          pawN: [150 * on, -100 * on], pawF: [30 * on, -110 * on],
        };
      },
      gear: [{ prop: "heart", at: "chest", x: 180, y: 860, scale: 1, beat: true }],
      over(ctx, c) {
        for (let i = 0; i < 4; i += 1) {
          const life = (c.age * 0.55 + i / 4) % 1;
          c.propAt("loveheart", 34 + i * 8 + Math.sin(life * 6 + i) * 3, -48 - life * 40, 0.3 + i * 0.1, { alpha: arc(life) * hold(c), rot: Math.sin(life * 6 + i) * 0.2 });
        }
      },
    },
  };

  const AIMED = ["button", "card", "image", "logo", "input", "link", "code", "heading"];
  const TOPS = ["button", "card", "image", "logo", "heading", "input", "code"];
  const SLABS = ["card", "image", "code", "logo", "heading"];
  const roundBox = (ctx, r, pad = 0) => {
    ctx.beginPath();
    ctx.roundRect(r.left - pad, r.top - pad, r.width + pad * 2, r.height + pad * 2, Math.min(12, r.height / 2));
  };
  const puffing = (c, from, to) => smooth(span(c.u, from, from + 0.05)) * (1 - smooth(span(c.u, to, to + 0.05)));
  const edge = (c) => ({ x: c.face > 0 ? c.r.left + 4 * c.S : c.r.right - 4 * c.S, y: c.cy });
  const blowing = (c, from, to, extra = {}) => {
    const on = puffing(c, from, to);
    return { open: on, lean: 0.1 * on, gx: 0.9, gh: 0.2, wing: 0.1 * on, ...extra };
  };

  function tongue(ctx, x, y, h, w, alpha, age, i) {
    const sway = Math.sin(age * 12 + i * 1.7) * w * 0.35;
    const g = ctx.createLinearGradient(x, y, x, y - h);
    g.addColorStop(0, `rgba(255,120,30,${0.95 * alpha})`);
    g.addColorStop(0.55, `rgba(255,190,60,${0.9 * alpha})`);
    g.addColorStop(1, `rgba(255,240,170,${0.1 * alpha})`);
    ctx.fillStyle = g;
    ctx.beginPath();
    ctx.moveTo(x - w / 2, y);
    ctx.quadraticCurveTo(x - w * 0.55 + sway * 0.3, y - h * 0.5, x + sway, y - h);
    ctx.quadraticCurveTo(x + w * 0.55 + sway * 0.3, y - h * 0.5, x + w / 2, y);
    ctx.closePath();
    ctx.fill();
  }

  function jet(ctx, c, to, power) {
    if (power <= 0.02) return;
    const m = c.mouth();
    const length = Math.hypot(to.x - m.x, to.y - m.y) * power;
    ctx.save();
    ctx.translate(m.x, m.y);
    ctx.rotate(Math.atan2(to.y - m.y, to.x - m.x));
    c.image("flame", length / 2, 0, length, (26 + length * 0.14) * c.S * (1 + wave(c.age, 23) * 0.09));
    ctx.restore();
  }

  function frostJet(ctx, c, to, power) {
    const m = c.mouth();
    for (let i = 0; i < 18; i += 1) {
      const life = (c.age * 2.2 + seed(i)) % 1;
      const x = mixed(m.x, to.x, life * power);
      const y = mixed(m.y, to.y, life * power) + (seed(i + 8) - 0.5) * 16 * life * c.S;
      puff(ctx, x, y, (3 + life * 8) * c.S, power * (1 - life * 0.6), "rgba(190,235,255,.9)");
    }
  }

  function ring(ctx, x, y, r, alpha, color, width) {
    ctx.strokeStyle = color;
    ctx.globalAlpha = alpha;
    ctx.lineWidth = width;
    ctx.beginPath();
    ctx.arc(x, y, r, 0, TAU);
    ctx.stroke();
    ctx.globalAlpha = 1;
  }

  function bolt(ctx, x0, y0, x1, y1, age, i, alpha) {
    ctx.strokeStyle = `rgba(255,240,150,${alpha})`;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    for (let k = 1; k < 6; k += 1) {
      const u = k / 6;
      const jitter = (seed(i + k + Math.floor(age * 8)) - 0.5) * 16;
      ctx.lineTo(mixed(x0, x1, u) + jitter, mixed(y0, y1, u) + jitter * 0.5);
    }
    ctx.lineTo(x1, y1);
    ctx.stroke();
  }

  function coin(ctx, x, y, r, spin, alpha) {
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.translate(x, y);
    ctx.scale(Math.max(Math.abs(Math.cos(spin)), 0.25), 1);
    const g = ctx.createRadialGradient(-r * 0.3, -r * 0.3, 0, 0, 0, r);
    g.addColorStop(0, "#fff0b0");
    g.addColorStop(0.6, "#ffc94d");
    g.addColorStop(1, "#c98a1d");
    ctx.fillStyle = g;
    ctx.beginPath();
    ctx.arc(0, 0, r, 0, TAU);
    ctx.fill();
    ctx.restore();
  }

  function egg(ctx, x, y, w, h, tilt, crack, alpha) {
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.translate(x, y);
    ctx.rotate(tilt);
    const g = ctx.createRadialGradient(-w * 0.2, -h * 0.25, 0, 0, 0, h);
    g.addColorStop(0, "#fffaf0");
    g.addColorStop(0.7, "#f0e3c8");
    g.addColorStop(1, "#cdbb95");
    ctx.fillStyle = g;
    ctx.beginPath();
    ctx.ellipse(0, 0, w, h, 0, 0, TAU);
    ctx.fill();
    ctx.fillStyle = "#0ebe60";
    for (const [dx, dy, size] of [[-0.3, 0.1, 0.2], [0.35, -0.2, 0.16], [0.1, 0.5, 0.15]]) {
      ctx.beginPath();
      ctx.ellipse(dx * w, dy * h, size * w, size * h * 0.7, 0.4, 0, TAU);
      ctx.fill();
    }
    if (crack > 0) {
      ctx.strokeStyle = "#6b5332";
      ctx.lineWidth = 1.4;
      ctx.beginPath();
      ctx.moveTo(-w, -h * 0.1);
      for (let i = 1; i <= 6; i += 1) ctx.lineTo(-w + (2 * w * i * crack) / 6, -h * 0.1 + (i % 2 ? -1 : 1) * h * 0.12);
      ctx.stroke();
    }
    ctx.restore();
  }

  function paw(ctx, x, y, r, alpha) {
    ctx.fillStyle = `rgba(14,150,84,${alpha})`;
    ctx.beginPath();
    ctx.ellipse(x, y + r * 0.2, r * 0.62, r * 0.5, 0, 0, TAU);
    ctx.fill();
    for (const [dx, dy] of [[-0.62, -0.42], [-0.22, -0.72], [0.22, -0.72], [0.62, -0.42]]) {
      ctx.beginPath();
      ctx.arc(x + dx * r, y + dy * r, r * 0.22, 0, TAU);
      ctx.fill();
    }
  }

  function leaf(ctx, x, y, r, spin, alpha) {
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.translate(x, y);
    ctx.rotate(spin);
    ctx.fillStyle = "#2fcf7c";
    ctx.beginPath();
    ctx.moveTo(-r, 0);
    ctx.quadraticCurveTo(0, -r * 0.8, r, 0);
    ctx.quadraticCurveTo(0, r * 0.8, -r, 0);
    ctx.fill();
    ctx.restore();
  }

  const TRICKS = {
    scorch: {
      title: "Burns it to a crisp, then it grows back", duration: 7.5, fits: AIMED, at: "side",
      pose: (c) => blowing(c, 0.1, 0.42, { lid: span(c.u, 0.46, 0.5) * (1 - span(c.u, 0.9, 0.94)), happy: 1, head: -0.05 }),
      scene(ctx, c) {
        const r = c.r;
        const burn = smooth(span(c.u, 0.14, 0.44));
        const heal = smooth(span(c.u, 0.66, 0.92));
        jet(ctx, c, edge(c), puffing(c, 0.1, 0.42));
        const from = c.face > 0 ? r.left : r.right;
        const reach = (r.width + 30 * c.S) * burn;
        const back = (r.width + 30 * c.S) * heal;
        ctx.save();
        roundBox(ctx, r, 1);
        ctx.clip();
        const front = from + c.face * reach;
        const rear = from + c.face * back;
        ctx.fillStyle = "rgba(20,12,9,.93)";
        ctx.beginPath();
        ctx.moveTo(rear, r.top - 2);
        for (let i = 0; i <= 8; i += 1) ctx.lineTo(front + Math.sin(i * 1.9 + c.age * 2) * 7 * c.S, r.top - 2 + ((r.height + 4) * i) / 8);
        ctx.lineTo(rear, r.bottom + 2);
        ctx.closePath();
        ctx.fill();
        for (let i = 0; i < 26; i += 1) {
          const x = r.left + seed(i) * r.width;
          const inside = c.face > 0 ? x < front && x > rear : x > front && x < rear;
          if (inside) puff(ctx, x, r.top + seed(i + 30) * r.height, 2.4 * c.S, 0.5 + wave(c.age, 5, i) * 0.4, "rgba(255,120,30,.95)");
        }
        ctx.restore();
        const flames = puffing(c, 0.2, 0.68);
        const count = Math.max(3, Math.round(r.width / (16 * c.S)));
        for (let i = 0; i < count; i += 1) {
          const x = r.left + ((i + 0.5) * r.width) / count;
          if (c.face > 0 ? x < front : x > front) tongue(ctx, x, r.top + 3, (10 + seed(i) * 16) * c.S * (1 + wave(c.age, 9, i) * 0.25), 12 * c.S, flames, c.age, i);
        }
        for (let i = 0; i < 12; i += 1) {
          const life = (c.age * 0.5 + seed(i + 60)) % 1;
          puff(ctx, r.left + seed(i) * r.width, r.top - life * 60 * c.S, (5 + life * 11) * c.S, 0.45 * (1 - life) * flames, "rgba(120,120,130,.8)");
        }
        for (let i = 0; i < 14; i += 1) sparkle(ctx, rear + c.face * (seed(i) - 0.5) * 22 * c.S, r.top + seed(i + 5) * r.height, 3.4 * c.S, puffing(c, 0.66, 0.9) * (0.5 + wave(c.age, 9, i) * 0.5), i % 2 ? INK.mint : INK.white);
      },
    },
    roast: {
      title: "Warms it until it glows", duration: 4.6, fits: ["heading", "short", "button", "logo"], at: "side",
      pose: (c) => blowing(c, 0.12, 0.6),
      scene(ctx, c) {
        const r = c.r;
        const on = puffing(c, 0.12, 0.8);
        jet(ctx, c, edge(c), puffing(c, 0.12, 0.6) * 0.6);
        ctx.save();
        roundBox(ctx, r, 4);
        ctx.clip();
        const g = ctx.createLinearGradient(r.left, 0, r.right, 0);
        const sweep = span(c.u, 0.1, 0.7);
        g.addColorStop(0, "rgba(255,120,30,0)");
        g.addColorStop(clamp01(sweep), `rgba(255,150,40,${0.5 * on})`);
        g.addColorStop(1, "rgba(255,120,30,0)");
        ctx.fillStyle = g;
        ctx.fillRect(r.left - 4, r.top - 4, r.width + 8, r.height + 8);
        ctx.restore();
        ctx.lineCap = "round";
        for (let i = 0; i < 7; i += 1) {
          const life = (c.age * 0.7 + seed(i)) % 1;
          ctx.strokeStyle = `rgba(255,190,120,${0.5 * arc(life) * on})`;
          ctx.lineWidth = 1.4;
          const x = r.left + seed(i + 9) * r.width;
          ctx.beginPath();
          for (let k = 0; k <= 6; k += 1) {
            const y = r.top - (life * 24 + k * 3) * c.S;
            const wobble = Math.sin(k + c.age * 5 + i) * 3;
            if (k) ctx.lineTo(x + wobble, y);
            else ctx.moveTo(x + wobble, y);
          }
          ctx.stroke();
        }
      },
    },
    candle: {
      title: "Lights a birthday candle on it", duration: 5.4, fits: TOPS, at: "side",
      pose: (c) => ({ ...blowing(c, 0.2, 0.3), open: Math.max(puffing(c, 0.2, 0.3), puffing(c, 0.72, 0.8) * 0.5), oval: 1, lid: puffing(c, 0.34, 0.68), happy: 1 }),
      scene(ctx, c) {
        const x = c.cx;
        const y = c.r.top;
        const S = c.S;
        const show = back(span(c.u, 0, 0.14)) * (1 - span(c.u, 0.92, 1));
        ctx.save();
        ctx.translate(x, y);
        ctx.scale(show, show);
        ctx.fillStyle = "#f47f86";
        ctx.beginPath();
        ctx.roundRect(-4 * S, -30 * S, 8 * S, 30 * S, 2 * S);
        ctx.fill();
        ctx.fillStyle = "#fff";
        for (let i = 0; i < 3; i += 1) ctx.fillRect(-4 * S, (-26 + i * 9) * S, 8 * S, 3 * S);
        const lit = c.u > 0.26 && c.u < 0.76;
        if (lit) {
          puff(ctx, 0, -40 * S, 18 * S, 0.5, "rgba(255,220,120,.7)");
          tongue(ctx, 0, -30 * S, 18 * S * (1 + wave(c.age, 16) * 0.12), 9 * S, 1, c.age, 1);
        }
        ctx.restore();
        if (c.u < 0.28) jet(ctx, c, { x, y: y - 34 * S }, puffing(c, 0.2, 0.27) * 0.9);
        if (c.u > 0.76) for (let i = 0; i < 4; i += 1) puff(ctx, x + i * 2 * S, y - (34 + span(c.u, 0.76, 1) * 40 + i * 8) * S, (2 + i) * S, 1 - span(c.u, 0.76, 1), "rgba(190,196,210,.9)");
        if (c.u > 0.34 && c.u < 0.68) note(ctx, x + 24 * S + wave(c.age, 3) * 8, y - 40 * S - ((c.age * 26) % 34) * S, 4 * S, 0.9);
      },
    },
    fireballs: {
      title: "Spits fireballs that bounce off it", duration: 4.4, fits: AIMED, at: "side",
      pose: (c) => {
        const beat = Math.max(wave(c.age, 8.6), 0);
        return { open: beat, lean: 0.08 * beat, gx: 0.9, y: -12 * beat };
      },
      scene(ctx, c) {
        const m = c.mouth();
        const hitAt = edge(c);
        for (let i = 0; i < 4; i += 1) {
          const u = (c.age - 0.4 - i * 0.73) / 1;
          if (u < 0 || u > 1) continue;
          const out = span(u, 0, 0.42);
          const away = span(u, 0.42, 1);
          const x = out < 1 ? mixed(m.x, hitAt.x, out) : hitAt.x - c.face * away * 90 * c.S;
          const y = out < 1 ? mixed(m.y, hitAt.y, out) : hitAt.y + away * away * 100 * c.S - arc(away) * 44 * c.S;
          puff(ctx, x, y, 12 * c.S, 0.9 * (1 - away * 0.6), "rgba(255,150,40,.9)");
          puff(ctx, x, y, 6 * c.S, 1 - away * 0.6, "rgba(255,240,170,1)");
          if (u > 0.4 && u < 0.56) ticks(ctx, hitAt.x, hitAt.y, span(u, 0.4, 0.56), INK.ember);
        }
      },
    },
    fireworks: {
      title: "Shoots fireworks over it", duration: 4.4, fits: AIMED, at: "side",
      pose: (c) => ({ open: puffing(c, 0.08, 0.22) + puffing(c, 0.4, 0.5), head: -0.3 * hold(c), gy: -1, gx: 0.5, lid: span(c.u, 0.6, 0.66), happy: 1 }),
      scene(ctx, c) {
        const colors = [INK.gold, INK.mint, INK.pink, "#6aa4ff", INK.ember];
        for (let b = 0; b < 5; b += 1) {
          const start = 0.1 + b * 0.14;
          const x = c.r.left + ((b + 1) * c.r.width) / 6;
          const top = c.r.top - (34 + seed(b) * 50) * c.S;
          const up = span(c.u, start, start + 0.12);
          const blast = span(c.u, start + 0.12, start + 0.4);
          if (up > 0 && up < 1) puff(ctx, mixed(c.mouth().x, x, up), mixed(c.mouth().y, top, up), 4 * c.S, 1, "rgba(255,220,120,1)");
          if (blast <= 0 || blast >= 1) continue;
          for (let i = 0; i < 12; i += 1) {
            const a = (i / 12) * TAU;
            const r = smooth(blast) * 34 * c.S;
            sparkle(ctx, x + Math.cos(a) * r, top + Math.sin(a) * r + blast * blast * 12 * c.S, 3.6 * c.S * (1 - blast * 0.6), 1 - blast, colors[(b + i) % 5]);
          }
        }
      },
    },
    smokerings: {
      title: "Blows smoke rings onto it", duration: 4.6, fits: AIMED, at: "side",
      pose: (c) => {
        const beat = (c.age * 1.4) % 1;
        return { open: arc(span(beat, 0, 0.3)) * 0.6, oval: 1, gx: 0.9, lean: 0.05 * arc(span(beat, 0, 0.3)) };
      },
      scene(ctx, c) {
        const m = c.mouth();
        const to = edge(c);
        for (let i = 0; i < 5; i += 1) {
          const life = (c.age - i * 0.72) / 1.6;
          if (life < 0 || life > 1) continue;
          const x = mixed(m.x, to.x, life) + Math.sin(life * 5) * 3;
          const y = mixed(m.y, to.y, life) - arc(life) * 14 * c.S;
          ctx.save();
          ctx.translate(x, y);
          ctx.scale(0.5, 1);
          ring(ctx, 0, 0, (5 + life * 15) * c.S, 0.8 * (1 - life * life), "rgba(205,210,225,.95)", (4 - life * 2) * c.S);
          ctx.restore();
        }
      },
    },
    frost: {
      title: "Freezes it solid, then it thaws", duration: 6.6, fits: AIMED, at: "side",
      pose: (c) => {
        const shiver = puffing(c, 0.46, 0.8);
        return { ...blowing(c, 0.1, 0.42, { oval: 0.7 }), x: Math.sin(c.age * 42) * 3 * shiver };
      },
      scene(ctx, c) {
        const r = c.r;
        const cover = smooth(span(c.u, 0.14, 0.42)) * (1 - smooth(span(c.u, 0.66, 0.92)));
        const b = puffing(c, 0.1, 0.42);
        if (b > 0) frostJet(ctx, c, edge(c), b);
        ctx.save();
        roundBox(ctx, r, 2);
        ctx.clip();
        ctx.fillStyle = `rgba(190,232,255,${0.62 * cover})`;
        ctx.fillRect(r.left - 2, r.top - 2, r.width + 4, r.height + 4);
        ctx.strokeStyle = `rgba(255,255,255,${0.9 * cover})`;
        ctx.lineWidth = 1.2;
        const count = Math.round(clamp(r.width * r.height / (1300 * c.S * c.S), 6, 26));
        for (let i = 0; i < count; i += 1) {
          const x = r.left + seed(i) * r.width;
          const y = r.top + seed(i + 40) * r.height;
          const size = (6 + seed(i + 80) * 11) * c.S * cover;
          for (let k = 0; k < 3; k += 1) {
            const a = (k * Math.PI) / 3 + seed(i);
            ctx.beginPath();
            ctx.moveTo(x - Math.cos(a) * size, y - Math.sin(a) * size);
            ctx.lineTo(x + Math.cos(a) * size, y + Math.sin(a) * size);
            ctx.stroke();
          }
        }
        ctx.restore();
        for (let i = 0; i < 9; i += 1) {
          const drip = span(c.u, 0.66 + seed(i) * 0.14, 0.94);
          if (drip <= 0 || drip >= 1) continue;
          puff(ctx, r.left + seed(i + 3) * r.width, r.bottom + drip * drip * 40 * c.S, 3 * c.S, 1 - drip, "rgba(160,220,255,.95)");
        }
      },
    },
    raincloud: {
      title: "Parks a little rain cloud over it", duration: 5.4, fits: SLABS.concat("button"), at: "side",
      pose: (c) => ({ gx: 0.7, gy: -0.7, head: -0.12 * hold(c), open: 0.3 * hold(c), oval: 1 }),
      scene(ctx, c) {
        const r = c.r;
        const on = back(span(c.u, 0, 0.16)) * (1 - span(c.u, 0.88, 1));
        const w = Math.min(r.width * 0.9, 170 * c.S);
        const y = r.top - 30 * c.S + wave(c.age, 2) * 3;
        ctx.strokeStyle = `rgba(120,180,255,${0.8 * on})`;
        ctx.lineWidth = 1.6;
        ctx.lineCap = "round";
        for (let i = 0; i < 16; i += 1) {
          const life = (c.age * 1.5 + seed(i)) % 1;
          const x = c.cx + (seed(i + 7) - 0.5) * w * 0.8;
          const top = y + 12 * c.S + life * (r.height + 26 * c.S);
          ctx.beginPath();
          ctx.moveTo(x, top);
          ctx.lineTo(x - 2, top + 8 * c.S);
          ctx.stroke();
        }
        c.image("upcloud", c.cx, y, w, undefined, { alpha: on });
      },
    },
    zap: {
      title: "Sparks it with static", duration: 4.2, fits: AIMED, at: "side",
      pose: (c) => ({ pawF: [150 * hold(c), -60], open: 0.4 * hold(c), gx: 1, wing: 0.14, jitter: hold(c) * 2 }),
      scene(ctx, c) {
        const r = c.r;
        const on = hold(c, 0.12, 0.86);
        const m = c.paw();
        bolt(ctx, m.x, m.y, edge(c).x, edge(c).y, c.age, 1, on);
        for (let i = 0; i < 7; i += 1) {
          const a = c.age * 3 + (i * TAU) / 7;
          const w = r.width / 2;
          const h = r.height / 2;
          const x0 = c.cx + clamp(Math.cos(a) * 2, -1, 1) * w;
          const y0 = c.cy + clamp(Math.sin(a) * 2, -1, 1) * h;
          const x1 = c.cx + clamp(Math.cos(a + 0.5) * 2, -1, 1) * w;
          const y1 = c.cy + clamp(Math.sin(a + 0.5) * 2, -1, 1) * h;
          bolt(ctx, x0, y0, x1, y1, c.age, i * 9, 0.85 * on);
        }
      },
    },
    rainbow: {
      title: "Breathes a rainbow over it", duration: 4.8, fits: AIMED, at: "side",
      pose: (c) => blowing(c, 0.1, 0.56, { head: -0.1, lid: span(c.u, 0.1, 0.16), happy: 1 }),
      scene(ctx, c) {
        const m = c.mouth();
        const on = hold(c, 0.1, 0.86);
        const grow = smooth(span(c.u, 0.1, 0.56));
        const tx = c.face > 0 ? c.r.right : c.r.left;
        const span0 = Math.abs(tx - m.x) / 2;
        const mid = (tx + m.x) / 2;
        const colors = ["#ff6b6b", "#ffb04d", "#ffe45e", "#57d68a", "#5aa9ff", "#a97cff"];
        const y = Math.max(m.y, c.r.top + 4);
        ctx.lineCap = "round";
        colors.forEach((color, i) => {
          ctx.strokeStyle = color;
          ctx.globalAlpha = 0.72 * on;
          ctx.lineWidth = 4 * c.S;
          ctx.beginPath();
          const rx = span0 - i * 4 * c.S;
          ctx.ellipse(mid, y, Math.max(rx, 1), Math.max(rx, 1) * 0.5 + 20 * c.S, 0, c.face > 0 ? Math.PI : TAU, c.face > 0 ? Math.PI + Math.PI * grow : TAU - Math.PI * grow, c.face < 0);
          ctx.stroke();
        });
        ctx.globalAlpha = 1;
      },
    },
    perch: {
      title: "Flies up and sits on top of it", duration: 5, fits: TOPS, at: "top",
      pose: (c) => ({ gx: Math.sin(c.age * 1.1) * 0.9, gy: 0.4, tail: 0.4, head: 0.06 * Math.sin(c.age * 1.6), wing: 0.1 * bump(c.u, 0, 0.14) }),
      scene() {},
    },
    topnap: {
      title: "Curls up and naps on top of it", duration: 6.4, fits: TOPS, at: "top",
      pose: (c) => {
        const on = hold(c, 0.14, 0.92);
        return { crouch: on, lean: 0.62 * on, head: 0.7 * on, hy: 150 * on, hx: 40 * on, lid: on, wing: -0.25 * on, chest: wave(c.age, 1.6) * 0.035, pawN: [70 * on, 90 * on], pawF: [10 * on, 90 * on], tail: 0.2 * on, sway: 0.3 };
      },
      scene(ctx, c) {
        for (let i = 0; i < 3; i += 1) {
          const life = (c.age * 0.33 + i / 3) % 1;
          const p = c.mouth();
          c.image("zee", p.x + c.face * (16 + life * 26) * c.S, p.y - (24 + life * 46) * c.S, (6 + life * 10) * c.S, undefined, { alpha: arc(life) * hold(c, 0.2, 0.9) });
        }
      },
    },
    hang: {
      title: "Hangs off the edge by its paws, feet kicking", duration: 5.2, fits: SLABS.concat("button"), at: "edge", pivot: "50% 47%", turn: (c) => wave(c.age, 2.4) * 9,
      pose: (c) => ({ reach: 1, front: 1, pawN: [150, 40], pawF: [330, 30], footN: [wave(c.age, 4.8) * 42, 36 - Math.max(wave(c.age, 4.8), 0) * 40], footF: [-wave(c.age, 4.8) * 42, 36 - Math.max(-wave(c.age, 4.8), 0) * 40], tail: 0.75, sway: 2.5, open: 0.3, gy: 0.3, gx: Math.sin(c.age * 1.2) * 0.7 }),
      scene(ctx, c) {
        if (c.u > 0.8) ticks(ctx, c.cx, c.r.top - 8 * c.S, span(c.u, 0.8, 1), INK.gold);
      },
    },
    bat: {
      title: "Hangs upside down from it like a bat", duration: 5, fits: SLABS.concat("button"), at: "ceiling", turn: (c) => 180 + wave(c.age, 2) * 8,
      pose: (c) => ({ wing: -0.28, crouch: 0.2, tail: 0.5, pawN: [40, 60], pawF: [0, 60], gy: 0.3, gx: Math.sin(c.age * 1.3), lid: span(c.u, 0.45, 0.5) * (1 - span(c.u, 0.7, 0.74)), happy: 1, open: 0.4 * bump(c.u, 0.72, 0.9), sway: 2 }),
      scene() {},
    },
    flyby: {
      title: "Zooms across the top of it on a cloud trail", duration: 3.2, fits: SLABS, at: "over",
      path: (c) => ({ x: c.face * mixed(-(c.r.width / 2 + 60 * c.S), c.r.width / 2 + 60 * c.S, smooth(c.u)), y: wave(c.age, 7) * 6 * c.S }),
      pose: () => ({ lean: 0.9, head: -0.3, flap: 1, tail: 0.8, footN: [-110, -60], footF: [-140, -80], pawN: [70, -60], pawF: [70, -60], open: 0.5 }),
      scene(ctx, c) {
        const p = c.at();
        for (let i = 0; i < 10; i += 1) {
          const life = (c.age * 1.7 + i / 10) % 1;
          puff(ctx, p.x - c.face * (32 + life * 90) * c.S, p.y + (seed(i) - 0.5) * 12 * c.S, (5 + life * 9) * c.S, 0.8 * (1 - life), "rgba(240,242,250,.95)");
        }
      },
    },
    orbit: {
      title: "Flies a lap around it", duration: 4.2, fits: SLABS.concat("button"), at: "centre",
      path: (c) => {
        const a = smooth(c.u) * TAU * 1.5 - Math.PI / 2;
        return { x: Math.cos(a) * (c.r.width / 2 + 56 * c.S), y: Math.sin(a) * (c.r.height / 2 + 44 * c.S) };
      },
      faceAt: (c) => (Math.sin(smooth(c.u) * TAU * 1.5 - Math.PI / 2) >= 0 ? -1 : 1),
      pose: () => ({ lean: 0.5, flap: 1, tail: 0.7, footN: [-70, -40], footF: [-80, -50], open: 0.5, pawN: [40, -40], pawF: [40, -40] }),
      scene(ctx, c) {
        const a = smooth(c.u) * TAU * 1.5 - Math.PI / 2;
        for (let i = 1; i < 40; i += 1) {
          const t = a - i * 0.045;
          puff(ctx, c.cx + Math.cos(t) * (c.r.width / 2 + 56 * c.S), c.cy + Math.sin(t) * (c.r.height / 2 + 44 * c.S), (8 - i * 0.15) * c.S, 0.5 * (1 - i / 40), "rgba(240,242,250,.95)");
        }
      },
    },
    divebomb: {
      title: "Dive-bombs it and bounces off", duration: 3, fits: AIMED, at: "over",
      path: (c) => {
        const dive = span(c.u, 0, 0.36);
        const off = span(c.u, 0.36, 0.8);
        return { x: c.face * (-90 * c.S * (1 - dive) - 70 * c.S * off), y: (-(1 - dive * dive) * 100 + 74 * dive) * c.S - arc(off) * 120 * c.S };
      },
      pose: (c) => (c.u < 0.36 ? { lean: 1.1, flap: 0.3, tail: 0.9, open: 0.8, footN: [-100, -60], footF: [-120, -70] } : { lean: -0.2, flap: 1, tail: 0.6, lid: 1, open: 0.5, oval: 1, gx: Math.sin(c.age * 20) }),
      scene(ctx, c) {
        const u = span(c.u, 0.34, 0.6);
        if (u <= 0 || u >= 1) return;
        ticks(ctx, c.cx, c.r.top, u, INK.gold);
        for (let i = 0; i < 8; i += 1) {
          const a = (i / 8) * TAU;
          sparkle(ctx, c.cx + Math.cos(a) * 30 * u * c.S, c.r.top + Math.sin(a) * 18 * u * c.S, 4 * c.S, 1 - u, i % 2 ? INK.gold : INK.white);
        }
      },
    },
    climb: {
      title: "Climbs up the side of it", duration: 4.6, fits: SLABS, at: "wall", stride: (c) => c.age * 8,
      turn: (c) => -c.face * 90,
      path: (c) => ({ x: 0, y: mixed(c.r.height / 2, -c.r.height / 2, smooth(span(c.u, 0.04, 0.8))) }),
      pose: (c) => ({ ground: 1 - smooth(span(c.u, 0.8, 0.86)), tail: 0.5, gx: 0.6, open: 0.2 + 0.5 * span(c.u, 0.82, 0.9), wing: 0.1 }),
      scene() {},
    },
    peek: {
      title: "Peeks out from behind it", duration: 4.6, fits: SLABS.concat("button", "input"), at: "behind",
      path: (c) => ({ x: Math.sin(c.age * 0.9) * Math.min(c.r.width * 0.3, 70 * c.S), y: (1 - hold(c, 0.16, 0.86)) * 62 * c.S }),
      pose: (c) => ({ gx: Math.sin(c.age * 1.7) * 0.9, gy: 0.2, ear: 0.08, head: 0.05 * Math.sin(c.age * 0.9) }),
      scene() {},
    },
    nibble: {
      title: "Takes a bite out of the corner, which grows back", duration: 5.6, fits: ["button", "card", "image", "logo", "input"], at: "side",
      pose: (c) => {
        const chew = puffing(c, 0.2, 0.42);
        return { open: chew * Math.abs(wave(c.age, 12)), x: 44 * chew, lean: 0.14 * chew, lid: span(c.u, 0.44, 0.48) * (1 - span(c.u, 0.62, 0.66)), happy: 1, gy: 0.5, y: -6 * Math.abs(wave(c.age, 12)) * puffing(c, 0.44, 0.62) };
      },
      scene(ctx, c) {
        const r = c.r;
        const x = c.face > 0 ? r.left : r.right;
        const y = r.top;
        const bite = span(c.u, 0.2, 0.42);
        const heal = smooth(span(c.u, 0.72, 0.94));
        const R = Math.min(r.height * 0.5, 30 * c.S) * (1 - heal);
        if (bite > 0 && R > 0.5) {
          ctx.save();
          roundBox(ctx, r, 2);
          ctx.clip();
          ctx.fillStyle = c.paper;
          ctx.beginPath();
          for (let i = 0; i < 3; i += 1) {
            if (bite < i / 3) continue;
            const a = 0.3 + i * 0.5;
            ctx.moveTo(x, y);
            ctx.arc(x + c.face * Math.cos(a) * R, y + Math.sin(a) * R, R * 0.68, 0, TAU);
          }
          ctx.fill();
          ctx.restore();
        }
        for (let i = 0; i < 9; i += 1) {
          const u = span(c.u, 0.22 + i * 0.024, 0.62);
          if (u <= 0 || u >= 1) continue;
          puff(ctx, x + c.face * (seed(i) * 34 - 10) * c.S, y + u * u * 120 * c.S, 2.4 * c.S, 1 - u, "rgba(200,190,170,.95)");
        }
        for (let i = 0; i < 6; i += 1) sparkle(ctx, x + c.face * seed(i) * 26 * c.S, y + seed(i + 4) * 22 * c.S, 3 * c.S, puffing(c, 0.72, 0.92), INK.mint);
      },
    },
    sneeze: {
      title: "Sniffs it and sneezes sparks all over it", duration: 3.8, fits: AIMED, at: "side",
      pose: (c) => {
        const load = span(c.u, 0.1, 0.56);
        const blast = bump(c.u, 0.56, 0.8);
        return { head: -0.2 * load * (c.u < 0.58 ? 1 : 0) + 0.26 * blast, open: 0.4 * load * (c.u < 0.58 ? 1 : 0) + 0.7 * blast, oval: 1, lid: c.u > 0.46 && c.u < 0.7 ? 1 : 0, y: 26 * blast, x: c.u < 0.5 ? 24 * Math.sin(c.age * 7) * 0.2 : -24 * blast, wing: 0.24 * blast, ear: 0.14 * blast };
      },
      scene(ctx, c) {
        const u = span(c.u, 0.56, 0.96);
        if (u <= 0 || u >= 1) return;
        const m = c.mouth();
        const to = edge(c);
        for (let i = 0; i < 18; i += 1) {
          const k = clamp01(u * 2.2 - seed(i) * 0.4);
          const x = mixed(m.x, to.x + c.face * seed(i + 3) * c.r.width * 0.7, k);
          const y = mixed(m.y, c.r.top + seed(i + 6) * c.r.height, k) + (seed(i + 9) - 0.5) * 30 * c.S * k;
          sparkle(ctx, x, y, 3 * c.S, 1 - u, i % 3 ? INK.gold : INK.ember);
        }
      },
    },
    polish: {
      title: "Polishes it until it shines", duration: 4.6, fits: ["button", "logo", "image", "card"], at: "side",
      pose: (c) => ({ pawF: [190 * hold(c) + wave(c.age, 11) * 34, -90 + wave(c.age, 11, 1) * 34], gx: 0.9, gy: 0.3, lean: 0.1, lid: span(c.u, 0.74, 0.78), happy: 1 }),
      scene(ctx, c) {
        const r = c.r;
        const u = span(c.u, 0.3, 0.74);
        ctx.save();
        roundBox(ctx, r, 1);
        ctx.clip();
        const x = mixed(r.left - 50 * c.S, r.right + 50 * c.S, u);
        const g = ctx.createLinearGradient(x - 40 * c.S, 0, x + 40 * c.S, 0);
        g.addColorStop(0, "rgba(255,255,255,0)");
        g.addColorStop(0.5, "rgba(255,255,255,.75)");
        g.addColorStop(1, "rgba(255,255,255,0)");
        ctx.fillStyle = g;
        ctx.transform(1, 0, -0.3, 1, 0, 0);
        ctx.fillRect(r.left - 200, r.top, r.width + 500, r.height);
        ctx.restore();
        for (let i = 0; i < 6; i += 1) {
          const life = (c.age * 0.9 + i / 6) % 1;
          sparkle(ctx, r.left + seed(i) * r.width, r.top + seed(i + 5) * r.height, 5 * c.S * arc(life), arc(life) * span(c.u, 0.5, 0.74) * (1 - span(c.u, 0.9, 1)), INK.white);
        }
      },
    },
    scratch: {
      title: "Leaves glowing claw marks on it, which fade", duration: 4.6, fits: SLABS.concat("button"), at: "side",
      pose: (c) => {
        const swipe = bump(c.u, 0.16, 0.3);
        return { pawF: [200, -220 + 240 * span(c.u, 0.16, 0.3)], open: 0.8 * swipe, lean: 0.2 * swipe, x: 40 * swipe, gx: 1, lid: span(c.u, 0.34, 0.38) * (1 - span(c.u, 0.6, 0.64)), happy: 1 };
      },
      scene(ctx, c) {
        const r = c.r;
        const draw = span(c.u, 0.16, 0.3);
        const fade = 1 - span(c.u, 0.6, 0.96);
        ctx.save();
        roundBox(ctx, r, 1);
        ctx.clip();
        ctx.lineCap = "round";
        const w = Math.min(r.width * 0.4, 90 * c.S);
        for (let i = 0; i < 3; i += 1) {
          const x = c.cx + (i - 1) * Math.min(26 * c.S, r.width * 0.16);
          for (const [line, color] of [[7, "rgba(14,190,96,.35)"], [2.6, "rgba(190,255,220,.95)"]]) {
            ctx.strokeStyle = color;
            ctx.globalAlpha = fade;
            ctx.lineWidth = line * c.S;
            ctx.beginPath();
            ctx.moveTo(x + w * 0.2, r.top + r.height * 0.1);
            ctx.lineTo(x + w * 0.2 - w * 0.4 * draw, r.top + r.height * (0.1 + 0.8 * draw));
            ctx.stroke();
          }
        }
        ctx.restore();
        ctx.globalAlpha = 1;
      },
    },
    pawprints: {
      title: "Walks along the top leaving paw prints", duration: 5.4, fits: ["card", "image", "heading", "button", "code"], at: "top", stride: (c) => c.age * 9,
      path: (c) => ({ x: c.face * mixed(-c.r.width / 2 + 10, c.r.width / 2 - 10, smooth(span(c.u, 0.04, 0.8))), y: 0 }),
      pose: (c) => ({ ground: 1 - smooth(span(c.u, 0.78, 0.84)), tail: 0.5, sway: 2, lid: span(c.u, 0.84, 0.88), happy: 1 }),
      scene(ctx, c) {
        const r = c.r;
        const steps = Math.max(3, Math.round(r.width / (26 * c.S)));
        const u = span(c.u, 0.04, 0.8);
        const fade = 1 - span(c.u, 0.84, 1);
        ctx.save();
        roundBox(ctx, r, 1);
        ctx.clip();
        for (let i = 0; i < steps; i += 1) {
          if ((i + 1) / steps > smooth(u)) continue;
          const x = c.face > 0 ? r.left + ((i + 0.5) * r.width) / steps : r.right - ((i + 0.5) * r.width) / steps;
          paw(ctx, x, r.top + (i % 2 ? 9 : 19) * c.S, 6 * c.S, 0.7 * fade);
        }
        ctx.restore();
      },
    },
    fogheart: {
      title: "Fogs it up and draws a heart with a claw", duration: 5.4, fits: ["input", "button", "code", "image", "card"], at: "side",
      pose: (c) => ({ open: 0.6 * puffing(c, 0.08, 0.3), oval: 1, pawF: [190, -70 + Math.sin(c.u * 40) * 30 * puffing(c, 0.34, 0.6)], gx: 1, gy: 0.3, lid: span(c.u, 0.62, 0.66), happy: 1 }),
      scene(ctx, c) {
        const r = c.r;
        const fog = smooth(span(c.u, 0.08, 0.3)) * (1 - smooth(span(c.u, 0.72, 0.96)));
        ctx.save();
        roundBox(ctx, r, 2);
        ctx.clip();
        ctx.fillStyle = `rgba(235,240,246,${0.82 * fog})`;
        ctx.fillRect(r.left - 2, r.top - 2, r.width + 4, r.height + 4);
        const R = Math.min(r.height * 0.3, 16 * c.S) * (span(c.u, 0.34, 0.6) > 0 ? 1 : 0);
        if (R > 0) heart(ctx, c.cx, c.cy, R * 1.6 * smooth(span(c.u, 0.34, 0.6)), "#f47f86", fog);
        ctx.restore();
      },
    },
    roar: {
      title: "Roars at it", duration: 3.6, fits: AIMED, at: "side",
      pose: (c) => {
        const load = bump(c.u, 0, 0.3);
        const on = puffing(c, 0.26, 0.72);
        return { lean: -0.16 * load + 0.14 * on, head: -0.16 * load, open: Math.max(0.3 * load, on), wing: 0.1 * load + 0.42 * on, ear: 0.16 * on, jitter: 3 * on, y: -20 * on, flap: 0.2 * on };
      },
      scene(ctx, c) {
        const on = puffing(c, 0.26, 0.72);
        const m = c.mouth();
        for (let i = 0; i < 4; i += 1) {
          const life = (c.age * 1.5 + i / 4) % 1;
          ctx.save();
          ctx.translate(m.x + c.face * life * 120 * c.S, m.y);
          ctx.scale(0.5, 1);
          ring(ctx, 0, 0, (10 + life * 52) * c.S, on * (1 - life), INK.ember, 2.4 * c.S);
          ctx.restore();
        }
        letter(ctx, "RAWR!", c.cx, c.r.top - 18 * c.S + Math.sin(c.age * 40) * 2, 22 * c.S * back(span(c.u, 0.26, 0.4)), on, INK.ember);
      },
    },
    hoard: {
      title: "Piles treasure on top and sits on it", duration: 5.6, fits: ["card", "image", "button", "logo"], at: "top",
      pose: (c) => {
        const happy = span(c.u, 0.5, 0.56);
        return { lid: happy, happy: 1, pawN: [150 * happy, -100 * happy], pawF: [30 * happy, -100 * happy], tail: 0.1, y: -Math.abs(wave(c.age, 5)) * 18 * happy, gy: 0.8 * (1 - happy), gx: 0.4 };
      },
      scene(ctx, c) {
        const r = c.r;
        const out = 1 - span(c.u, 0.9, 1);
        for (let i = 0; i < 26; i += 1) {
          const start = seed(i) * 0.5;
          const fall = span(c.u, start, start + 0.14);
          if (fall <= 0) continue;
          const x = c.at().x + (seed(i + 11) - 0.5) * 136 * c.S;
          const rest = r.top - 3 * c.S - seed(i + 22) * 7 * c.S;
          const y = mixed(rest - 150 * c.S, rest, fall * fall);
          coin(ctx, x, y, 5.4 * c.S, fall < 1 ? c.age * 12 + i : 1 + i, out);
        }
      },
    },
    egg: {
      title: "Lays an egg on it, and it hatches", duration: 6.4, fits: ["card", "image", "button", "logo", "heading"], at: "side",
      pose: (c) => ({ gx: 0.9, gy: 0.4 - span(c.u, 0.6, 0.64), open: 0.6 * bump(c.u, 0.62, 0.9), lid: span(c.u, 0.66, 0.7), happy: 1, wing: 0.2 * bump(c.u, 0.62, 0.9) }),
      scene(ctx, c) {
        const S = c.S;
        const x = c.cx;
        const y = c.r.top - 15 * S;
        const hatch = span(c.u, 0.6, 0.66);
        const wobble = span(c.u, 0.2, 0.6) * Math.sin(c.age * 15) * 0.2;
        if (hatch < 1) egg(ctx, x, y, 12 * S, 15 * S, wobble, span(c.u, 0.38, 0.6), back(span(c.u, 0.02, 0.14)));
        if (hatch <= 0) return;
        const fade = 1 - span(c.u, 0.9, 1);
        for (let i = 0; i < 10; i += 1) {
          const a = (i / 10) * TAU;
          const u = span(c.u, 0.6, 0.8);
          puff(ctx, x + Math.cos(a) * 26 * u * S, y + Math.sin(a) * 18 * u * S, 5 * S, 1 - u, "rgba(255,250,235,.95)");
        }
        const baby = back(span(c.u, 0.62, 0.72));
        ctx.save();
        ctx.translate(x, y + 8 * S - Math.abs(wave(c.age, 8)) * 5 * S);
        ctx.scale(baby, baby);
        ctx.globalAlpha = fade;
        ctx.fillStyle = "#f3e6cc";
        ctx.beginPath();
        ctx.ellipse(0, -9 * S, 12 * S, 11 * S, 0, 0, TAU);
        ctx.fill();
        ctx.fillStyle = "#17120f";
        for (const side of [-1, 1]) {
          ctx.beginPath();
          ctx.ellipse(side * 5 * S, -10 * S, 2.1 * S, 2.6 * S, 0, 0, TAU);
          ctx.fill();
        }
        ctx.fillStyle = "#a6d8c4";
        for (const side of [-1, 1]) {
          ctx.beginPath();
          ctx.moveTo(side * 6 * S, -19 * S);
          ctx.lineTo(side * 9 * S, -27 * S);
          ctx.lineTo(side * 1 * S, -20 * S);
          ctx.fill();
        }
        ctx.restore();
        heart(ctx, x + 16 * S, y - 26 * S - span(c.u, 0.7, 1) * 18 * S, 5 * S, INK.pink, fade);
      },
    },
    guard: {
      title: "Stands guard over it with a shield", duration: 4.6, fits: ["button", "input", "logo", "card"], at: "side",
      pose: (c) => {
        const on = hold(c);
        return { head: 0.14 * on, hy: 40 * on, lean: 0.12 * on, reach: on, pawN: [150, 215], pawF: [330, 230], gx: 0.7 };
      },
      gear: [{ prop: "shield", at: "ground", x: 300, y: 905, scale: 0.74, order: "hug" }],
      scene(ctx, c) {
        const on = hold(c);
        ctx.strokeStyle = `rgba(14,190,96,${0.7 * on})`;
        ctx.lineWidth = 2 * c.S;
        roundBox(ctx, c.r, 6 * c.S + wave(c.age, 4) * 2);
        ctx.stroke();
        for (let i = 0; i < 2; i += 1) {
          const life = (c.age * 0.6 + i / 2) % 1;
          ctx.strokeStyle = `rgba(14,190,96,${0.45 * (1 - life) * on})`;
          roundBox(ctx, c.r, (8 + life * 22) * c.S);
          ctx.stroke();
        }
      },
    },
    gust: {
      title: "Flaps up a gust that blows leaves across it", duration: 4.4, fits: AIMED, at: "side",
      pose: (c) => ({ flap: 1, wing: 0.3, lean: -0.1, gx: 0.8, lid: span(c.u, 0.2, 0.24), happy: 1 }),
      scene(ctx, c) {
        const r = c.r;
        const on = hold(c, 0.12, 0.88);
        ctx.lineCap = "round";
        for (let i = 0; i < 9; i += 1) {
          const life = (c.age * 1.1 + seed(i)) % 1;
          const x = mixed(edge(c).x - c.face * 30 * c.S, c.face > 0 ? r.right + 50 * c.S : r.left - 50 * c.S, life);
          const y = r.top + seed(i + 6) * r.height + Math.sin(life * 6 + i) * 8 * c.S;
          ctx.strokeStyle = `rgba(190,205,225,${0.7 * arc(life) * on})`;
          ctx.lineWidth = 1.6 * c.S;
          ctx.beginPath();
          ctx.moveTo(x, y);
          ctx.lineTo(x - c.face * 26 * c.S, y);
          ctx.stroke();
          if (i % 2) leaf(ctx, x, y, 5.4 * c.S, c.age * 6 + i, arc(life) * on);
        }
      },
    },
  };

  for (const name in TRICKS) ACTIVITIES[name] = TRICKS[name];

  function scene(ctx, s, art, o) {
    const trick = ACTIVITIES[o.name];
    const v = viewOf(art);
    if (!trick || !trick.scene || !v.marks) return;
    const r = o.rect;
    const unit = o.size / 100;
    const world = (m) => ({ x: o.origin.x + (50 + m[0]) * unit, y: o.origin.y + (GROUND + m[1]) * unit });
    const c = {
      age: o.age, duration: trick.duration, u: clamp01(o.age / trick.duration), t: s.t, r, S: o.size / 96, face: o.face, paper: o.paper,
      cx: r.left + r.width / 2, cy: r.top + r.height / 2,
      mouth: () => world(v.marks.mouth), paw: () => world(v.marks.paw), at: () => world([0, -40]),
      image(name, x, y, w, h, opts = {}) {
        const f = art.meta.props[name];
        if (!f || w <= 0) return;
        const height = h || (w * f.h) / f.w;
        ctx.save();
        if (opts.alpha !== undefined) ctx.globalAlpha *= opts.alpha;
        ctx.translate(x, y);
        ctx.drawImage(art.image, f.x, f.y, f.w, f.h, -w / 2, -height / 2, w, height);
        ctx.restore();
      },
    };
    trick.scene(ctx, c);
  }

  function trickMotion(name, o) {
    const trick = ACTIVITIES[name];
    const c = { age: o.age, duration: trick.duration, u: clamp01(o.age / trick.duration), r: o.rect, S: o.size / 96, face: o.face };
    return {
      path: trick.path ? trick.path(c) : { x: 0, y: 0 },
      turn: trick.turn ? trick.turn(c) : 0,
      face: trick.faceAt ? trick.faceAt(c) : o.face,
    };
  }

  const SIT = {
    x: 0, y: 0, lean: 0, crouch: 0, chest: 0, head: 0, hx: 0, hy: 0, wing: 0, flap: 0, tail: 0, tailBase: 0, sway: 1,
    open: 0, oval: 0, lid: 0, lidN: 0, lidF: 0, happy: 0, gx: 0, gy: 0, gh: 0, gv: 0, ear: 0, jitter: 0, pawNa: 0, pawFa: 0,
    reach: 0, pawNx: 0, pawNy: 0, pawFx: 0, pawFy: 0, footNx: 0, footNy: 0, footFx: 0, footFy: 0, ground: 0, front: 0,
  };
  const KEYS = Object.keys(SIT);
  const SNAPPY = new Set(["lid", "lidN", "lidF", "happy", "open", "jitter", "pawNa", "pawFa", "gx", "gy"]);
  const LOOSE = new Set(["tail", "tailBase", "wing", "flap", "sway", "gh", "gv", "ear"]);
  const STIFF = { snappy: [520, 36], loose: [58, 7.5], limb: [250, 20], body: [190, 15.5] };
  const QUIRKS = [
    { name: "yawn", time: 2.1, pose: (u) => ({ open: bump(u, 0.1, 0.85), oval: 0.55, lid: bump(u, 0.1, 0.9), head: -0.2 * bump(u, 0.05, 0.9), wing: 0.14 * bump(u, 0.1, 0.9), chest: 0.04 * bump(u, 0.1, 0.8), ear: -0.1 * bump(u, 0.1, 0.9) }) },
    { name: "sneeze", time: 1.3, sneeze: 0.56, pose: (u) => ({ head: -0.18 * bump(u, 0, 0.56) + 0.24 * bump(u, 0.54, 0.8), open: 0.4 * bump(u, 0, 0.56) + 0.5 * bump(u, 0.54, 0.74), oval: 1, lid: span(u, 0.42, 0.56) * (1 - span(u, 0.72, 0.84)), y: 30 * bump(u, 0.54, 0.8), wing: 0.2 * bump(u, 0.54, 0.8), ear: 0.14 * bump(u, 0.54, 0.8) }) },
    { name: "look", time: 2.6, pose: (u) => ({ gx: u < 0.5 ? -0.9 : 0.8, gy: -0.2, gh: (u < 0.5 ? -0.9 : 0.8) * bump(u, 0, 1) * 1.1, ear: 0.08 }) },
    { name: "tilt", time: 1.7, pose: (u) => ({ head: 0.17 * bump(u, 0, 1), gy: 0.1, ear: 0.13 * bump(u, 0, 1) }) },
    { name: "wings", time: 1.7, pose: (u) => ({ wing: 0.5 * bump(u, 0, 0.7), lean: -0.03 * bump(u, 0, 0.7), flap: 0.32 * bump(u, 0.55, 1) }) },
    { name: "flick", time: 1.2, pose: (u) => ({ tail: 0.55 * bump(u, 0, 1), sway: 2 + 3 * bump(u, 0, 1) }) },
    { name: "lick", time: 2.4, pose: (u) => ({ pawN: [78 * bump(u, 0, 1), -285 * bump(u, 0, 1)], head: 0.1 * bump(u, 0, 1), lid: bump(u, 0.15, 0.9), open: 0.32 * Math.max(wave(u * 2.4, 16), 0) * bump(u, 0.2, 0.85), front: bump(u, 0, 1) }) },
    { name: "shift", time: 1, pose: (u) => ({ footN: [0, -34 * bump(u, 0, 1)], x: 16 * bump(u, 0, 1), lean: 0.03 * bump(u, 0, 1) }) },
    { name: "hop", time: 0.9, pose: (u) => ({ y: -58 * bump(u, 0.15, 0.8), crouch: 0.3 * bump(u, 0, 0.3) + 0.25 * bump(u, 0.75, 1), open: 0.5 * bump(u, 0.15, 0.8), footN: [0, -18 * bump(u, 0.15, 0.8)], footF: [0, -18 * bump(u, 0.15, 0.8)], ear: 0.1 * bump(u, 0.15, 0.8) }) },
  ];

  function quirkAt(t) {
    const n = Math.floor(t / 5.5);
    const start = n * 5.5 + seed(n + 70) * 2;
    const q = QUIRKS[Math.floor(seed(n + 90) * QUIRKS.length)];
    const u = (t - start) / q.time;
    return u > 0 && u < 1 ? { q, u } : null;
  }

  function viewOf(art) {
    if (!art.view) {
      const q = {};
      for (const key of KEYS) q[key] = { x: SIT[key], v: 0 };
      art.view = { q, last: null, flips: { head: 1, body: 1, wing: 1, tail: 1 }, facing: 1, turnAt: -9 };
    }
    return art.view;
  }

  function blinkAt(t) {
    const period = 3.4;
    const n = Math.floor(t / period);
    const start = n * period + seed(n) * 2.2;
    const one = (at) => {
      const u = (t - at) / 0.17;
      return u > 0 && u < 1 ? Math.sin(Math.PI * u) : 0;
    };
    return Math.max(one(start), seed(n + 50) < 0.3 ? one(start + 0.3) : 0);
  }

  function eyelid(ctx, x, y, rx, ry, closed, happy) {
    if (closed <= 0.02) return;
    ctx.save();
    ctx.beginPath();
    ctx.ellipse(x, y, rx * 1.42, ry * 1.42, 0, 0, TAU);
    ctx.clip();
    ctx.translate(x, y);
    ctx.scale(rx, ry);
    const lid = ctx.createRadialGradient(0, 0, 1.1, 0, 0, 1.42);
    lid.addColorStop(0, "#e3ccb1");
    lid.addColorStop(1, "#e3ccb100");
    ctx.fillStyle = lid;
    ctx.fillRect(-1.45, -1.45, 2.9, 2.9 * closed);
    ctx.restore();
    if (closed > 0.55) {
      ctx.save();
      ctx.strokeStyle = "#2b241e";
      ctx.lineWidth = 10;
      ctx.lineCap = "round";
      ctx.globalAlpha *= span(closed, 0.55, 0.8);
      ctx.beginPath();
      if (happy > 0.5) ctx.ellipse(x, y + ry * 0.45, rx * 0.95, ry * 0.75, 0, Math.PI * 1.1, Math.PI * 1.9);
      else ctx.ellipse(x, y - ry * 0.2, rx * 0.95, ry * 0.6, 0, Math.PI * 0.1, Math.PI * 0.9);
      ctx.stroke();
      ctx.restore();
    }
  }

  function mouth(ctx, cx, cy, open, oval) {
    if (open <= 0.02) return;
    const rx = mixed(64, 30, oval) * (0.7 + open * 0.3);
    const ry = 6 + open * mixed(34, 30, oval);
    const y = cy + ry * 0.55;
    ctx.save();
    ctx.fillStyle = "#7c2f35";
    ctx.beginPath();
    ctx.moveTo(cx - rx, cy);
    ctx.quadraticCurveTo(cx, cy - 8 * (1 - oval), cx + rx, cy);
    ctx.quadraticCurveTo(cx + rx * 0.92, y + ry * 0.85, cx, y + ry * 0.75);
    ctx.quadraticCurveTo(cx - rx * 0.92, y + ry * 0.85, cx - rx, cy);
    ctx.fill();
    ctx.clip();
    ctx.fillStyle = "#ee8f94";
    ctx.beginPath();
    ctx.ellipse(cx + rx * 0.05, y + ry * 0.72, rx * 0.56, ry * 0.5, 0, 0, TAU);
    ctx.fill();
    ctx.fillStyle = "#fff8ec";
    for (const side of [-1, 1]) {
      ctx.beginPath();
      ctx.moveTo(cx + side * rx * 0.62, cy - 4);
      ctx.lineTo(cx + side * rx * 0.48, cy + 15);
      ctx.lineTo(cx + side * rx * 0.34, cy - 4);
      ctx.fill();
    }
    ctx.restore();
  }

  function pose(s, v) {
    const target = { ...SIT };
    const assign = (next) => {
      for (const key in next) {
        const value = next[key];
        if (Array.isArray(value)) {
          target[`${key}x`] = value[0];
          target[`${key}y`] = value[1];
        } else {
          target[key] = value;
        }
      }
    };
    const t = s.t;
    const breath = Math.sin(t * 1.7);
    target.chest = breath * 0.012;
    target.head = Math.sin(t * 0.41) * 0.02;
    target.wing = Math.sin(t * 1.05) * 0.03;
    const n = Math.floor(t / 6);
    const flutterAt = n * 6 + seed(n + 3) * 4;
    if (t > flutterAt && t < flutterAt + 0.7) target.wing += Math.sin((t - flutterAt) * 26) * 0.1;
    const glance = Math.floor(t / 1.8);
    target.gx = s.gaze ? clamp(s.gaze.x, -1, 1) * v.facing : (seed(glance + 13) - 0.5) * 1.1;
    target.gy = s.gaze ? clamp(s.gaze.y, -1, 1) : (seed(glance + 41) - 0.5) * 0.5;
    if (s.gaze) {
      target.ear = 0.06;
      target.head += 0.04;
      target.sway = 1.8;
    }
    v.sneeze = null;
    const walk = clamp01(s.walking || 0);
    const act = s.ambient || s.pose;
    if (s.soar) {
      assign({ flap: 1, lean: 0.34, tail: 0.6, tailBase: 0.15, pawN: [50, -40], pawF: [50, -40], footN: [-60, -40], footF: [-70, -50], open: 0.2 });
    } else if (s.held) {
      assign({
        y: -30, head: -0.06, pawN: [40, -90], pawF: [20, -90], footN: [Math.sin(t * 6) * 20 - 20, -60], footF: [Math.sin(t * 6 + 1.5) * 20 + 10, -60],
        open: 0.5, tail: 0.4, tailBase: 0.15, wing: 0.25 + Math.sin(t * 9) * 0.06, lean: clamp((s.vx || 0) / 2200, -0.4, 0.4),
      });
    } else if ((s.air || 0) > 0.3) {
      const pitch = clamp((s.vy || 0) / 1800, -0.5, 0.5);
      assign({
        lean: 0.85 + pitch * 0.4, head: -0.4 - pitch * 0.4, flap: 1, tail: 0.5, tailBase: 0.2, open: 0.55, pawN: [70, -60], pawF: [70, -60],
        footN: [-110, -60], footF: [-140, -80],
      });
    } else if (act && ACTIVITIES[act]) {
      const a = ACTIVITIES[act];
      const age = s.ambient ? s.ambientAge || 0 : s.poseAge || 0;
      assign(a.pose({ age, duration: a.duration, u: clamp01(age / a.duration), t }));
      if (a.scene && s.aloft && (a.at === "side" || a.at === "over")) {
        target.flap = Math.max(target.flap, 0.75);
        target.footNx -= 20;
        target.footNy -= 34;
        target.footFx -= 20;
        target.footFy -= 40;
        target.tail = Math.max(target.tail, 0.4);
      }
    } else if (walk > 0.05) {
      assign({ ground: walk, tail: 0.86 * walk, tailBase: 0.03 * walk, sway: 2.2, wing: 0.1 + Math.sin(s.stridePhase * 2) * 0.1 });
    } else if (s.reaction) {
      const age = s.reactionAge || 0;
      const r = s.reaction;
      if (r === "wave") assign({ pawF: [225 + Math.sin(age * 13) * 40, -310], pawFa: Math.sin(age * 13) * 0.4, open: 0.5, lean: -0.03, gx: 1, front: 1 });
      if (r === "cheer") assign({ pawN: [-210, -320], pawF: [210, -320], y: -90 * Math.abs(Math.sin(age * 8)), open: 1, lid: 1, happy: 1, wing: 0.3, front: 1 });
      if (r === "nod") assign({ head: Math.sin(age * 11) * 0.12 * (1 - clamp01(age)) });
      if (r === "eureka") assign({ pawF: [215, -330], gy: -1, open: 0.7, y: -50 * arc(age / 0.5), front: 1 });
      if (r === "giggle") assign({ lid: 1, happy: 1, open: 0.8, head: Math.sin(age * 22) * 0.05, y: -Math.abs(Math.sin(age * 17)) * 20, pawN: [60, -120] });
      if (r === "surprise") assign({ y: -110 * arc(age / 0.34), open: 0.7, oval: 1, wing: 0.35, pawN: [40, -60], pawF: [40, -60] });
      if (r === "dizzy") assign({ head: Math.sin(age * 9) * 0.14, hx: Math.cos(age * 9) * 16, gx: Math.sin(age * 9), gy: Math.cos(age * 9), lean: Math.sin(age * 5) * 0.05 });
    } else if (!s.reduced) {
      const forced = s.quirk ? QUIRKS.find((entry) => entry.name === s.quirk.name) : null;
      const now = forced ? { q: forced, u: s.quirk.u } : quirkAt(t);
      if (now) {
        assign(now.q.pose(now.u));
        if (now.q.sneeze && now.u > now.q.sneeze) v.sneeze = (now.u - now.q.sneeze) / (1 - now.q.sneeze);
      }
    }
    if (act && ACTIVITIES[act]) {
      const lead = s.ambient ? s.ambientAge || 0 : s.poseAge || 0;
      if (!ACTIVITIES[act].scene) target.crouch += 0.24 * bump(lead, 0, 0.26);
    }
    if (target.gh === 0) target.gh = target.gx * 0.75;
    if (target.gv === 0) target.gv = target.gy;
    if (s.hover) {
      target.chest += Math.sin(t * 38) * 0.006;
      target.ear += 0.05;
    }
    if (s.land > 0) {
      target.crouch = Math.max(target.crouch, s.land * 0.85);
      target.wing += s.land * 0.4;
    }
    return target;
  }

  function settle(v, target, dt, reduced) {
    const q = v.q;
    const steps = Math.max(1, Math.ceil(dt * 120));
    const h = dt / steps;
    for (const key of KEYS) {
      const value = q[key];
      const goal = target[key];
      if (reduced) {
        value.x = goal;
        value.v = 0;
        continue;
      }
      const [k, c] = STIFF[SNAPPY.has(key) ? "snappy" : LOOSE.has(key) ? "loose" : key.startsWith("paw") || key.startsWith("foot") ? "limb" : "body"];
      for (let i = 0; i < steps; i += 1) {
        value.v += (k * (goal - value.x) - c * value.v) * h;
        value.x += value.v * h;
      }
    }
  }

  function turning(v, s, dt) {
    const want = Math.abs(s.facing || 0) > 0.2 ? Math.sign(s.facing) : s.rest || v.facing;
    if (want !== v.facing) {
      v.facing = want;
      v.turnAt = s.t;
    }
    const age = s.t - v.turnAt;
    const delays = { head: 0, body: 0.09, wing: 0.14, tail: 0.2 };
    for (const part in delays) {
      if (age >= delays[part]) {
        const f = v.flips[part];
        v.flips[part] = clamp(f + clamp(v.facing - f, -dt * 8.5, dt * 8.5), -1, 1);
      }
    }
    return age < 0.5 ? arc(age / 0.5) : 0;
  }

  function solveLimb(from, restFrom, restTo, to, opts) {
    const rest = Math.atan2(restTo[1] - restFrom[1], restTo[0] - restFrom[0]);
    const length = Math.hypot(restTo[0] - restFrom[0], restTo[1] - restFrom[1]);
    const dx = to[0] - from[0];
    const dy = to[1] - from[1];
    const angle = Math.atan2(dy, dx);
    const stretch = clamp(Math.hypot(dx, dy) / length, opts.min, opts.max);
    return { angle: angle - rest, rest, stretch };
  }

  function wrapped(angle) {
    return Math.atan2(Math.sin(angle), Math.cos(angle));
  }

  function bendTail(v, tail, root, base, q, t, s) {
    const curve = tail.curve;
    const n = curve.length - 1;
    if (!v.tailRest) {
      v.tailRest = [];
      v.tailLen = [];
      for (let i = 0; i < n; i += 1) {
        v.tailRest.push(Math.atan2(curve[i + 1][1] - curve[i][1], curve[i + 1][0] - curve[i][0]));
        v.tailLen.push(Math.hypot(curve[i + 1][0] - curve[i][0], curve[i + 1][1] - curve[i][1]));
      }
      v.tailAim = v.tailRest.slice();
      v.tailTotal = v.tailLen.reduce((sum, value) => sum + value, 0);
    }
    const aim = v.tailAim;
    const dt = v.dt || 0;
    const points = [root.slice()];
    const curl = 1 - clamp(q.tail, -0.4, 1);
    const reach = mixed(1, tail.length * 1.16 / v.tailTotal, clamp01(q.tail));
    const swing = clamp((s.vx || 0) * -0.00012, -0.03, 0.03);
    for (let i = 0; i < n; i += 1) {
      const k = i / (n - 1);
      const parent = i ? aim[i - 1] : v.tailRest[0] + base - 0.17 * clamp01(q.tail);
      const rel = i ? wrapped(v.tailRest[i] - v.tailRest[i - 1]) : 0;
      const wag = Math.sin(t * 2.4 - i * 0.3) * 0.009 * q.sway * (0.5 + k * 1.8);
      const want = parent + rel * curl + wag + swing;
      const rate = s.reduced || !dt ? 1 : 1 - Math.exp(-(36 - 25 * k) * dt);
      aim[i] += wrapped(want - aim[i]) * rate;
      const p = points[i];
      points.push([p[0] + Math.cos(aim[i]) * v.tailLen[i] * reach, p[1] + Math.sin(aim[i]) * v.tailLen[i] * reach]);
    }
    const lowest = Math.max(...points.map((p) => p[1]));
    if (lowest > 1214) points.forEach((p, i) => { p[1] -= (lowest - 1214) * smooth((i / n) * 1.6); });
    return { points, aim };
  }

  function texture(ctx, image, box, a, b, c, d, e, f) {
    const du1 = c[0] - a[0];
    const dv1 = c[1] - a[1];
    const du2 = e[0] - a[0];
    const dv2 = e[1] - a[1];
    const den = du1 * dv2 - du2 * dv1;
    if (!den) return;
    const dx1 = d[0] - b[0];
    const dy1 = d[1] - b[1];
    const dx2 = f[0] - b[0];
    const dy2 = f[1] - b[1];
    const m = (dx1 * dv2 - dx2 * dv1) / den;
    const n = (dx2 * du1 - dx1 * du2) / den;
    const o = (dy1 * dv2 - dy2 * dv1) / den;
    const p = (dy2 * du1 - dy1 * du2) / den;
    const cx = (b[0] + d[0] + f[0]) / 3;
    const cy = (b[1] + d[1] + f[1]) / 3;
    const out = (q) => {
      const dx = q[0] - cx;
      const dy = q[1] - cy;
      const size = Math.hypot(dx, dy) || 1;
      return [q[0] + (dx / size) * 4, q[1] + (dy / size) * 4];
    };
    ctx.save();
    ctx.beginPath();
    ctx.moveTo(...out(b));
    ctx.lineTo(...out(d));
    ctx.lineTo(...out(f));
    ctx.closePath();
    ctx.clip();
    ctx.transform(m, o, n, p, b[0] - m * a[0] - n * a[1], b[1] - o * a[0] - p * a[1]);
    ctx.drawImage(image, box.x, box.y, box.w, box.h, box.x, box.y, box.w, box.h);
    ctx.restore();
  }

  function drawTail(ctx, art, strip, tail, chain, from, to) {
    const { points, aim } = chain;
    const n = points.length - 1;
    const outer = [];
    const inner = [];
    for (let i = 0; i <= n; i += 1) {
      const before = aim[Math.max(i - 1, 0)];
      const after = aim[Math.min(i, n - 1)];
      const angle = before + wrapped(after - before) / 2;
      const nx = -Math.sin(angle);
      const ny = Math.cos(angle);
      outer.push([points[i][0] + nx * tail.outer, points[i][1] + ny * tail.outer]);
      inner.push([points[i][0] - nx * tail.inner, points[i][1] - ny * tail.inner]);
    }
    for (let i = to - 1; i >= from; i -= 1) {
      const u0 = strip.x + (strip.w * i) / n;
      const u1 = strip.x + (strip.w * (i + 1)) / n;
      const top = strip.y;
      const low = strip.y + strip.h;
      const box = { x: Math.floor(u0) - 1, y: top, w: Math.ceil(u1 - u0) + 3, h: strip.h };
      texture(ctx, art.image, box, [u0, top], outer[i], [u1, top], outer[i + 1], [u1, low], inner[i + 1]);
      texture(ctx, art.image, box, [u0, top], outer[i], [u1, low], inner[i + 1], [u0, low], inner[i]);
    }
  }

  function drawRig(ctx, art, s, v, c, act) {
    const meta = art.meta;
    const rig = meta.rig;
    const J = meta.joints;
    const axis = J.axis;
    const q = {};
    for (const key of KEYS) q[key] = v.q[key].x;
    const t = s.t;
    const shake = q.jitter ? [Math.sin(t * 61) * q.jitter, Math.cos(t * 47) * q.jitter] : [0, 0];
    const seen = (f) => (f < 0 ? -1 : 1) * (0.56 + 0.44 * Math.abs(f));
    const fb = seen(v.flips.body);
    const flipX = (x) => axis[0] + (x - axis[0]) * v.flips.body;
    const layer = (name) => {
      const r = rig[name];
      const k = 1 / meta.rigScale;
      const scale = (r.s || 1) * k;
      ctx.drawImage(art.image, r.x, r.y, r.w, r.h, r.ox, r.oy, r.w * scale, r.h * scale);
    };
    const crouch = q.crouch;
    const hipLow = crouch * 92;
    const hip = [J.hip[0] + q.x + shake[0], J.hip[1] + q.y + hipLow + shake[1]];
    const trot = q.ground;
    const a0 = act ? ACTIVITIES[act] : null;
    const stride = a0 && a0.stride ? a0.stride(c) : s.stridePhase || 0;
    const stepBob = Math.abs(Math.sin(stride)) * trot;
    const leanNow = q.lean + trot * (0.12 + Math.sin(stride) * 0.05);
    const hipW = [hip[0], hip[1] - trot * 16 - stepBob * 13];
    const cos = Math.cos(leanNow);
    const sin = Math.sin(leanNow);
    const squash = 1 - crouch * 0.26 + q.chest;
    const wide = 1 + crouch * 0.1 - q.chest * 0.5;
    const torso = (p) => {
      const px = (p[0] - J.hip[0]) * wide;
      const py = (p[1] - J.hip[1]) * squash;
      return [hipW[0] + px * cos - py * sin, hipW[1] + px * sin + py * cos];
    };
    v.torso = torso;
    const place = (flip, at, angle, name, pivot, sx = 1, sy = 1) => {
      ctx.save();
      ctx.translate(flipX(at[0]), at[1]);
      ctx.scale(flip, 1);
      ctx.rotate(angle);
      ctx.scale(sx, sy);
      ctx.translate(-pivot[0], -pivot[1]);
      layer(name);
      ctx.restore();
    };
    const step = (phase, amount) => {
      const p = (((phase % TAU) + TAU) % TAU) / TAU;
      if (p < 0.5) {
        const lift = Math.sin(p * TAU);
        return [mixed(-118, 118, smooth(p * 2)) * amount, -lift * 64 * amount, lift];
      }
      return [(118 - (p - 0.5) * 472) * amount, 0, 0];
    };
    const footWorld = (rest, ox, oy, phase) => {
      const st = step(phase, trot);
      return [rest[0] + ox + st[0] + q.x, Math.min(rest[1] + oy + st[1] + q.y, 1236), st[2]];
    };
    const footN = footWorld(J.footNear, q.footNx, q.footNy, stride);
    const footF = footWorld(J.footFar, q.footFx, q.footFy, stride + Math.PI);
    const pawReach = (rest, ox, oy, phase) => {
      const swing = Math.cos(phase) * 62 * trot;
      const local = torso([rest[0] + ox * (1 - q.reach) + swing, rest[1] + oy * (1 - q.reach) - 24 * trot - Math.abs(Math.sin(phase)) * 16 * trot]);
      if (q.reach < 0.01) return local;
      const base = torso(J.neck);
      return [mixed(local[0], base[0] + ox, q.reach), mixed(local[1], base[1] + oy, q.reach)];
    };
    const pawN = pawReach(J.pawNear, q.pawNx, q.pawNy, stride);
    const pawF = pawReach(J.pawFar, q.pawFx, q.pawFy, stride + Math.PI);
    const shoulderN = torso(J.shoulderNear);
    const shoulderF = torso(J.shoulderFar);
    const within = (from, to, most) => {
      const dx = to[0] - from[0];
      const dy = to[1] - from[1];
      const gap = Math.hypot(dx, dy);
      if (gap <= most) return;
      to[0] = from[0] + (dx / gap) * most;
      to[1] = from[1] + (dy / gap) * most;
    };
    within(shoulderN, pawN, 345);
    within(shoulderF, pawF, 325);
    const hipN = torso(J.hipNear);
    const hipF = torso(J.hipFar);
    const neck = torso(J.neck);
    const gear = act && ACTIVITIES[act].gear ? ACTIVITIES[act].gear : [];
    v.bones = { pawN, pawF };
    const drawGear = (order) => {
      for (const g of gear) {
        if ((g.order || "mid") !== order) continue;
        c.gear(g, v);
      }
    };
    const wingAt = (p, angle, name, sign, rise = 0) => {
      const beat = q.flap > 0.02 ? Math.sin(t * 24) : 0;
      const a = sign * (angle + beat * 0.42 * q.flap + clamp(v.q.y.v * 0.00045, -0.28, 0.28));
      ctx.save();
      const rear = torso(J.wingFar);
      const own = torso(p);
      const at = [mixed(own[0], rear[0] + 170, rise), mixed(own[1], rear[1] - 40, rise)];
      ctx.translate(flipX(at[0]), at[1]);
      ctx.scale(seen(v.flips.wing), 1);
      ctx.rotate(a + leanNow * (sign > 0 ? 0.85 : -0.1 * rise + 0.85 * (1 - rise)));
      ctx.scale(1, 1 - Math.abs(beat) * 0.1 * q.flap);
      ctx.translate(-p[0], -p[1]);
      layer(name);
      ctx.restore();
    };
    wingAt(J.wingFar, q.wing + q.flap * 0.1, "wingFar", 1);
    wingAt(J.wingNear, q.wing + q.flap * 0.1, "wingNear", -1, Math.min(Math.abs(leanNow) / 0.9, 1));
    drawGear("back");
    const limb = (name, from, restFrom, restTo, to, min, max) => {
      const l = solveLimb(from, restFrom, restTo, to, { min, max });
      ctx.save();
      ctx.translate(flipX(from[0]), from[1]);
      ctx.scale(fb, 1);
      ctx.rotate(l.angle);
      ctx.rotate(l.rest);
      ctx.scale(l.stretch, 1);
      ctx.rotate(-l.rest);
      ctx.translate(-restFrom[0], -restFrom[1]);
      layer(name);
      ctx.restore();
      return l.angle;
    };
    const shin = (from, foot) => {
      const gap = Math.hypot(foot[0] - from[0], foot[1] - from[1]);
      const show = Math.max(clamp01(trot * 4), span(gap, 190, 240));
      if (show <= 0) return;
      ctx.save();
      ctx.globalAlpha *= show;
      limb("armNear", from, J.shoulderNear, J.pawNear, [foot[0], foot[1] - 12], 0.5, 1.8);
      ctx.restore();
    };
    shin(torso([930, 1040]), footF);
    const thighF = solveLimb(hipF, J.hipFar, J.footFar, footF, { min: 0.55, max: 1.4 });
    ctx.save();
    ctx.translate(flipX(hipF[0]), hipF[1]);
    ctx.scale(fb, 1);
    ctx.rotate(thighF.angle);
    ctx.rotate(thighF.rest);
    ctx.scale(thighF.stretch, 1);
    ctx.rotate(-thighF.rest);
    ctx.translate(-J.hipFar[0], -J.hipFar[1]);
    layer("thighFar");
    ctx.restore();
    place(fb, footF, -footF[2] * 0.34 * trot, "footFar", J.footFar);
    const tailFlip = seen(v.flips.tail);
    const root = torso(meta.tail.curve[0]);
    root[1] = Math.min(root[1], meta.tail.curve[0][1] + 12);
    const chain = bendTail(v, meta.tail, root, leanNow * 0.7 + q.tailBase + trot * Math.sin(stride) * 0.06, q, t, s);
    const tuck = Math.round(chain.aim.length * 0.42);
    const tailPart = (from, to) => {
      ctx.save();
      ctx.translate(flipX(root[0]), root[1]);
      ctx.scale(tailFlip, 1);
      ctx.translate(-root[0], -root[1]);
      drawTail(ctx, art, rig.tailStrip, meta.tail, chain, from, to);
      ctx.restore();
    };
    tailPart(0, tuck);
    place(fb, torso(J.hip), leanNow, "body", J.hip, wide, squash);
    shin(torso([650, 1050]), footN);
    limb("thighNear", hipN, J.hipNear, J.footNear, footN, 0.6, 1.3);
    place(fb, footN, -footN[2] * 0.34 * trot, "footNear", J.footNear);
    tailPart(tuck, chain.aim.length);
    drawGear("under");
    const farAngle = solveLimb(shoulderF, J.shoulderFar, J.pawFar, pawF, { min: 0.55, max: 1.5 }).angle;
    const farReach = Math.hypot(pawF[0] - shoulderF[0], pawF[1] - shoulderF[1]) / 215;
    const sleeve = span(Math.abs(farAngle), 0.14, 0.42);
    ctx.save();
    ctx.globalAlpha *= 1 - sleeve;
    limb("armFar", shoulderF, J.shoulderFar, J.pawFar, pawF, 0.55, 1.5);
    ctx.restore();
    const farSleeve = () => {
      if (sleeve <= 0) return;
      ctx.save();
      ctx.globalAlpha *= sleeve;
      ctx.translate(flipX(shoulderF[0]), shoulderF[1]);
      ctx.scale(fb, 1);
      ctx.rotate(farAngle - 0.28);
      ctx.scale(-0.82, 0.82 * 1.22 * clamp(farReach, 0.6, 1.5));
      ctx.translate(-J.shoulderNear[0], -J.shoulderNear[1]);
      layer("armNear");
      ctx.restore();
    };
    const nearAngle = solveLimb(shoulderN, J.shoulderNear, J.pawNear, pawN, { min: 0.55, max: 1.5 }).angle;
    const headTurn = q.head + q.gh * 0.075 - trot * 0.1;
    const headAt = [neck[0] + q.gh * 12 + q.hx, neck[1] + q.gv * 7 + q.hy];
    const toMouth = [J.mouth[0] - J.neck[0], J.mouth[1] + 26 - J.neck[1]];
    const mouthAt = [
      flipX(headAt[0]) + seen(v.flips.head) * (toMouth[0] * Math.cos(headTurn) - toMouth[1] * Math.sin(headTurn)),
      headAt[1] + toMouth[0] * Math.sin(headTurn) + toMouth[1] * Math.cos(headTurn),
    ];
    v.marks = {
      mouth: [(mouthAt[0] - axis[0]) * meta.units, (mouthAt[1] - axis[1]) * meta.units],
      paw: [(flipX(pawF[0]) - axis[0]) * meta.units, (pawF[1] - axis[1]) * meta.units],
    };
    const drawHead = () => {
      const fh = seen(v.flips.head);
      ctx.save();
      const nx = flipX(neck[0] + q.gh * 12 + q.hx);
      ctx.translate(nx, neck[1] + q.gv * 7 + q.hy + stepBob * 8 - clamp(v.q.y.v * 0.02, -24, 24));
      ctx.scale(fh, 1);
      ctx.rotate(q.head + q.gh * 0.075 - trot * 0.1 + trot * Math.sin(stride * 2) * 0.025);
      ctx.translate(-J.neck[0], -J.neck[1]);
      const n = Math.floor(t / 4.3);
      const earAt = n * 4.3 + seed(n + 11) * 3;
      const twitch = t > earAt && t < earAt + 0.28 ? Math.sin((t - earAt) * 34) * 0.1 : 0;
      ctx.save();
      ctx.translate(J.ear[0], J.ear[1]);
      ctx.rotate(twitch + q.ear + Math.sin(t * 1.3) * 0.012 + q.flap * 0.1 + trot * Math.sin(stride * 2) * 0.09 + clamp(v.q.head.v * 0.09 + v.q.y.v * 0.00035, -0.24, 0.24));
      ctx.translate(-J.ear[0], -J.ear[1]);
      layer("ear");
      ctx.restore();
      layer("head");
      const [ex, ey, erx, ery] = meta.eyes.eyeNear;
      const [fx, fy, frx, fry] = meta.eyes.eyeFar;
      const eye = (name, cx, cy, rx, ry, k) => {
        ctx.save();
        ctx.beginPath();
        ctx.ellipse(cx, cy, rx, ry, 0, 0, TAU);
        ctx.clip();
        ctx.translate(q.gx * 12 * k, q.gy * 9);
        layer(name);
        ctx.restore();
      };
      eye("eyeNear", ex, ey, erx, ery, 1);
      eye("eyeFar", fx, fy, frx, fry, 0.45);
      const blink = Math.max(blinkAt(t), arc((t - v.turnAt) / 0.18));
      eyelid(ctx, ex, ey, erx, ery, Math.max(blink, q.lid, q.lidN), q.happy);
      eyelid(ctx, fx, fy, frx, fry, Math.max(blink, q.lid, q.lidF), q.happy);
      mouth(ctx, J.mouth[0], J.mouth[1], q.open, q.oval);
      const pull = s.gaze ? 0.5 + Math.sin(t * 3) * 0.12 : 0.26;
      const g = ctx.createRadialGradient(735, 575, 0, 735, 575, 58);
      g.addColorStop(0, `rgba(247,150,150,${pull})`);
      g.addColorStop(1, "rgba(247,150,150,0)");
      ctx.fillStyle = g;
      ctx.beginPath();
      ctx.arc(735, 575, 58, 0, TAU);
      ctx.fill();
      if (act && ACTIVITIES[act].head) ACTIVITIES[act].head(ctx, c);
      ctx.restore();
    };
    const pawLayer = (name, to, rest, angle, extra) => place(fb, to, angle * 0.55 + extra, name, rest);
    const nearArm = () => limb("armNear", shoulderN, J.shoulderNear, J.pawNear, pawN, 0.55, 1.5);
    if (q.front > 0.5) {
      drawHead();
      farSleeve();
      drawGear("hug");
      pawLayer("pawFar", pawF, J.pawFar, farAngle, q.pawFa);
      drawGear("mid");
      nearArm();
      pawLayer("pawNear", pawN, J.pawNear, nearAngle, q.pawNa);
    } else {
      farSleeve();
      pawLayer("pawFar", pawF, J.pawFar, farAngle, q.pawFa);
      drawHead();
      drawGear("hug");
      drawGear("mid");
      nearArm();
      pawLayer("pawNear", pawN, J.pawNear, nearAngle, q.pawNa);
    }
    drawGear("front");
  }

  const FLAME_TIP = 0.58;

  function kit(ctx, art, s, v, age, duration) {
    const meta = art.meta;
    const unit = meta.units;
    const J = meta.joints;
    const toLocal = (p) => ({ x: (p[0] - J.axis[0]) * unit, y: (p[1] - J.axis[1]) * unit });
    const c = {
      age, duration, u: duration ? clamp01(age / duration) : 0, t: s.t,
      prop(name, x, y, scale, o = {}) {
        const f = meta.props[name];
        if (!f || scale <= 0) return;
        const w = f.w * f.k * scale;
        const h = f.h * f.k * scale;
        const px = (o.pivot ? o.pivot[0] : 0) * w;
        const py = (o.pivot ? o.pivot[1] : 0) * h;
        ctx.save();
        if (o.alpha !== undefined) ctx.globalAlpha *= o.alpha;
        ctx.translate(x + px, y + py);
        if (o.rot) ctx.rotate(o.rot);
        if (o.sy) ctx.scale(1, o.sy);
        ctx.translate(-px, -py);
        ctx.drawImage(art.image, f.x, f.y, f.w, f.h, -w / 2, -h / 2, w, h);
        ctx.restore();
      },
      propAt(name, x, y, scale, o = {}) {
        ctx.save();
        ctx.translate(x, y);
        ctx.scale(unit, unit);
        c.prop(name, 0, 0, scale, o);
        ctx.restore();
      },
      flame(size, tall) {
        const f = meta.props.flame;
        const [mx, my] = J.mouth;
        const w = f.w * f.k * size;
        ctx.save();
        ctx.translate(mx + 6, my + 30);
        ctx.scale(1, tall);
        ctx.drawImage(art.image, f.x, f.y, f.w, f.h, -10, -f.h * f.k * size * FLAME_TIP, w, f.h * f.k * size);
        ctx.restore();
      },
      gear(g, view) {
        const show = back(span(c.u, 0, 0.1)) * (1 - smooth(span(c.u, 0.9, 1)));
        const beat = g.beat ? 1 + Math.pow(Math.max(Math.sin(c.age * 6.2), 0), 3) * 0.1 : 1;
        const flip = view.flips.body;
        const face = (flip < 0 ? -1 : 1) * (0.56 + 0.44 * Math.abs(flip));
        ctx.save();
        let x;
        let y;
        if (g.at === "ground") {
          x = J.axis[0] + g.x;
          y = g.y;
        } else if (g.at === "pawF" || g.at === "pawN") {
          const p = view.bones[g.at];
          x = p[0] + g.x;
          y = p[1] + g.y;
        } else {
          const base = g.at === "chest" ? [J.hip[0] + g.x, g.y] : [g.x, g.y];
          const p = view.torso(base);
          x = p[0];
          y = p[1];
        }
        ctx.translate(J.axis[0] + (x - J.axis[0]) * flip, y);
        ctx.scale(face, 1);
        c.prop(g.prop, 0, 0, g.scale * show * beat);
        ctx.restore();
      },
      at(where, x, y) {
        const bones = v.bones;
        if (where === "torso") return toLocal(v.torso([x, y]));
        const p = bones[where];
        return toLocal([p[0] + x, p[1] + y]);
      },
    };
    return c;
  }

  function shadow(ctx, width, alpha) {
    const g = ctx.createRadialGradient(0, 0, 0, 0, 0, width);
    g.addColorStop(0, `rgba(8,10,20,${0.3 * alpha})`);
    g.addColorStop(1, "rgba(8,10,20,0)");
    ctx.save();
    ctx.scale(1, 0.16);
    ctx.fillStyle = g;
    ctx.beginPath();
    ctx.arc(0, 0, width, 0, TAU);
    ctx.fill();
    ctx.restore();
  }

  function paint(ctx, s, art) {
    const v = viewOf(art);
    const dt = v.last === null ? 0 : clamp(s.t - v.last, 0, 0.1);
    v.last = s.t;
    v.dt = dt;
    const hopping = turning(v, s, dt);
    const target = pose(s, v);
    if (hopping) {
      target.y -= 60 * hopping;
      target.footNy -= 26 * hopping;
      target.footFy -= 26 * hopping;
      target.crouch += 0.12 * hopping;
      target.wing += 0.2 * hopping;
    }
    settle(v, target, dt, s.reduced);
    const act = s.ambient || s.pose;
    const a = act ? ACTIVITIES[act] : null;
    const age = s.ambient ? s.ambientAge || 0 : s.poseAge || 0;
    const unit = art.meta.units;
    const J = art.meta.joints;
    const lift = Math.max(-v.q.y.x * unit, 0);
    ctx.save();
    ctx.translate(50, GROUND);
    if (!s.aloft) shadow(ctx, 44 - clamp01(lift / 60) * 16, 1 - clamp01(lift / 90) * 0.6);
    const c = kit(ctx, art, s, v, age, a ? a.duration : 0);
    const flip = v.flips.body < 0 ? -1 : 1;
    if (a && a.under) {
      ctx.save();
      ctx.scale(flip, 1);
      a.under(ctx, c);
      ctx.restore();
    }
    ctx.save();
    ctx.scale(unit, unit);
    const stretch = v.q.y.x < -8 ? clamp(Math.abs(v.q.y.v) / 2400, 0, 0.15) : 0;
    ctx.scale(1 - stretch * 0.55, 1 + stretch);
    ctx.translate(-J.axis[0], -J.axis[1]);
    drawRig(ctx, art, s, v, c, act);
    ctx.restore();
    if (v.sneeze !== null && v.sneeze !== undefined) {
      ctx.save();
      ctx.scale(flip, 1);
      for (let i = 0; i < 4; i += 1) {
        const u = clamp01(v.sneeze * 1.3 - i * 0.1);
        puff(ctx, 16 + u * 15 + i, -49 - u * 3 + (i - 1.5) * u * 2.4, 1 + u * 2.6, 0.75 * (1 - u), "rgba(214,220,228,.95)");
      }
      ctx.restore();
    }
    ctx.save();
    ctx.scale(flip, 1);
    if (a && a.arms) a.arms(ctx, c);
    if (a && a.over) a.over(ctx, c);
    ctx.restore();
    if (a && a.after) {
      ctx.save();
      a.after(ctx, c);
      ctx.restore();
    }
    ctx.restore();
  }

  function load(version) {
    const base = "/assets/cloud-dragon/";
    const query = version ? `?v=${version}` : "";
    const meta = fetch(`${base}atlas.json${query}`).then((response) => {
      if (!response.ok) throw new Error("atlas");
      return response.json();
    });
    const image = new Promise((resolve, reject) => {
      const picture = new Image();
      picture.onload = () => resolve(picture);
      picture.onerror = reject;
      picture.src = `${base}atlas.webp${query}`;
    });
    return Promise.all([meta, image]).then(([data, picture]) => ({ meta: data, image: picture }));
  }

  window.ForkMeshDragon = { activities: ACTIVITIES, tricks: Object.keys(TRICKS), quirks: QUIRKS, kinds: ALL, load, paint, keys: KEYS, scene, trickMotion };
})();
