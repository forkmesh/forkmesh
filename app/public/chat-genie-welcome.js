export const GENIE_WELCOME_SECONDS = 4;
export const GENIE_WELCOME_SCRIPT = "/genie-spider.js";

const W = 280;
const H = 150;
const FLOOR = 134;
const SIZE = 84;
const D = GENIE_WELCOME_SECONDS;

const REACTION_SECONDS = {
  wave: 1.8, cheer: 1.6, surprise: 1.3, nod: 0.9, dizzy: 1.7, eureka: 1.4, giggle: 1.1,
};

const TEAL = [64, 242, 219];
const GOLD = [255, 206, 84];
const LILAC = [190, 160, 235];
const PINK = [255, 122, 186];
const EMBER = [250, 122, 24];
const SILVER = [204, 209, 224];
const CONFETTI = [TEAL, GOLD, LILAC, PINK, EMBER];

const clamp01 = (v) => Math.min(Math.max(v, 0), 1);
const lerp = (a, b, k) => a + (b - a) * k;
const easeOut = (k) => 1 - (1 - clamp01(k)) ** 3;
const easeIn = (k) => clamp01(k) ** 2;
const easeInOut = (k) => {
  const v = clamp01(k);
  return v < 0.5 ? 4 * v * v * v : 1 - (-2 * v + 2) ** 3 / 2;
};
const span = (t, from, to) => clamp01((t - from) / (to - from));
const rgba = (c, a) => `rgba(${c[0]},${c[1]},${c[2]},${clamp01(a)})`;

function seeded(seed) {
  let x = seed >>> 0;
  return () => {
    x = (Math.imul(x, 1664525) + 1013904223) >>> 0;
    return x / 4294967296;
  };
}

function reaction(t, cues) {
  let picked = null;
  for (const [name, at] of cues) {
    if (t >= at && t - at < REACTION_SECONDS[name]) picked = { reaction: name, reactionAge: t - at };
  }
  return picked || { reaction: "", reactionAge: 0 };
}

function landing(t, ...times) {
  let land = 0;
  for (const at of times) {
    if (t >= at && t < at + 0.28) land = Math.max(land, 1 - (t - at) / 0.28);
  }
  return land;
}

function pose(t, extra = {}) {
  return {
    t, mood: "idle", reaction: "", reactionAge: 0, vitality: 0.95, condition: "happy",
    stridePhase: 0, walking: 0, facing: 0, air: 0, land: 0, gaze: { x: 0, y: 0.35 }, magic: 0,
    ...extra,
  };
}

function glowDot(ctx, x, y, r, color, a) {
  if (r <= 0 || a <= 0) return;
  const g = ctx.createRadialGradient(x, y, 0, x, y, r);
  g.addColorStop(0, rgba([255, 255, 255], a));
  g.addColorStop(0.35, rgba(color, a));
  g.addColorStop(1, rgba(color, 0));
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(x, y, r, 0, Math.PI * 2);
  ctx.fill();
}

function star(ctx, x, y, r, color, a, turn = 0) {
  if (a <= 0 || r <= 0) return;
  ctx.save();
  ctx.translate(x, y);
  ctx.rotate(turn);
  ctx.fillStyle = rgba(color, a);
  ctx.beginPath();
  for (let i = 0; i < 8; i += 1) {
    const rr = i % 2 === 0 ? r : r * 0.32;
    const ang = (i / 8) * Math.PI * 2;
    if (i === 0) ctx.moveTo(Math.cos(ang) * rr, Math.sin(ang) * rr);
    else ctx.lineTo(Math.cos(ang) * rr, Math.sin(ang) * rr);
  }
  ctx.closePath();
  ctx.fill();
  ctx.restore();
}

function puff(ctx, x, y, r, color, a) {
  if (r <= 0 || a <= 0) return;
  const g = ctx.createRadialGradient(x, y, 0, x, y, r);
  g.addColorStop(0, rgba(color, a * 0.8));
  g.addColorStop(0.6, rgba(color, a * 0.4));
  g.addColorStop(1, rgba(color, 0));
  ctx.fillStyle = g;
  ctx.beginPath();
  ctx.arc(x, y, r, 0, Math.PI * 2);
  ctx.fill();
}

function backdrop(ctx) {
  const sky = ctx.createLinearGradient(0, 0, 0, H);
  sky.addColorStop(0, "rgb(22,16,46)");
  sky.addColorStop(1, "rgb(10,12,28)");
  ctx.fillStyle = sky;
  ctx.fillRect(0, 0, W, H);
  const twinkle = seeded(7);
  for (let i = 0; i < 18; i += 1) {
    ctx.fillStyle = rgba([255, 255, 255], 0.18 + twinkle() * 0.3);
    ctx.fillRect(twinkle() * W, twinkle() * (FLOOR - 40), 1.2, 1.2);
  }
  const ground = ctx.createLinearGradient(0, FLOOR, 0, H);
  ground.addColorStop(0, "rgba(64,242,219,0.18)");
  ground.addColorStop(0.15, "rgba(36,30,72,0.9)");
  ground.addColorStop(1, "rgba(16,12,34,1)");
  ctx.fillStyle = ground;
  ctx.fillRect(0, FLOOR, W, H - FLOOR);
}

function dust(ctx, x, t, at) {
  const k = span(t, at, at + 0.5);
  if (k <= 0 || k >= 1) return;
  for (const side of [-1, 1]) {
    puff(ctx, x + side * (14 + k * 22), FLOOR - 3 - k * 4, 5 + k * 6, SILVER, 0.5 * (1 - k));
  }
}

function silkDrop(t) {
  const x = W / 2;
  const hang = FLOOR - 16;
  let y;
  if (t < 1.1) y = lerp(-24, hang, easeInOut(t / 1.1));
  else if (t < 1.5) y = hang + Math.sin((t - 1.1) * 18) * 5 * (1 - span(t, 1.1, 1.5));
  else y = lerp(hang, FLOOR, easeIn(span(t, 1.5, 1.65)));
  const anchor = t < 1.5 ? y - SIZE * 0.8 : lerp(hang - SIZE * 0.8, 0, easeOut(span(t, 1.55, 2)));
  return {
    x, y,
    pose: pose(t, {
      air: t < 1.5 ? 0.3 : 0,
      land: landing(t, 1.65),
      ...reaction(t, [["wave", 1.8]]),
    }),
    back(ctx) {
      if (anchor <= 0) return;
      ctx.strokeStyle = rgba(SILVER, 0.75);
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, anchor);
      ctx.stroke();
    },
  };
}

function magicLamp(t) {
  const lx = 84;
  const wobble = t < 0.8 ? Math.sin(t * 28) * 0.09 * (1 - t / 0.8) : 0;
  const rise = easeOut(span(t, 0.9, 1.8));
  const fall = easeIn(span(t, 1.8, 2.15));
  const x = lerp(lx + 44, 196, rise);
  const y = t < 1.8 ? lerp(FLOOR - 22, FLOOR - 48, rise) : lerp(FLOOR - 48, FLOOR, fall);
  return {
    x, y, scale: lerp(0.12, 1, rise), hidden: t < 0.9,
    pose: pose(t, {
      air: t < 2.15 ? 0.45 : 0,
      magic: 1 - span(t, 2.1, 3.2),
      land: landing(t, 2.15),
      ...reaction(t, [["cheer", 2.25]]),
    }),
    back(ctx) {
      for (let i = 0; i < 10; i += 1) {
        const k = span(t, 0.5 + i * 0.08, 1.9 + i * 0.08);
        if (k <= 0 || k >= 1) continue;
        puff(ctx, lx + 42 + k * (40 + i * 6), FLOOR - 26 - k * (34 + (i % 3) * 8), 4 + k * 16, LILAC, 0.75 * (1 - k));
      }
      ctx.save();
      ctx.translate(lx, FLOOR);
      ctx.rotate(wobble);
      const gold = ctx.createLinearGradient(0, -30, 0, 0);
      gold.addColorStop(0, "rgb(255,240,180)");
      gold.addColorStop(0.45, "rgb(255,206,84)");
      gold.addColorStop(1, "rgb(176,120,30)");
      ctx.fillStyle = gold;
      ctx.beginPath();
      ctx.ellipse(0, -2, 13, 3, 0, 0, Math.PI * 2);
      ctx.fill();
      ctx.beginPath();
      ctx.moveTo(-28, -12);
      ctx.quadraticCurveTo(-26, -26, 0, -25);
      ctx.quadraticCurveTo(22, -25, 30, -18);
      ctx.lineTo(44, -26);
      ctx.quadraticCurveTo(42, -18, 30, -8);
      ctx.quadraticCurveTo(18, -3, 0, -3);
      ctx.quadraticCurveTo(-26, -3, -28, -12);
      ctx.fill();
      ctx.beginPath();
      ctx.ellipse(-2, -27, 8, 4, 0, Math.PI, 0);
      ctx.fill();
      ctx.strokeStyle = gold;
      ctx.lineWidth = 3;
      ctx.beginPath();
      ctx.arc(-30, -15, 7, Math.PI * 0.5, Math.PI * 1.5);
      ctx.stroke();
      ctx.restore();
      if (t > 0.5 && t < 1.9) glowDot(ctx, lx + 44, FLOOR - 26, 10, GOLD, 0.7 * (1 - span(t, 1.2, 1.9)));
    },
  };
}

function confettiShower(t) {
  const rnd = seeded(31);
  const pieces = Array.from({ length: 48 }, () => ({
    x: rnd() * W, delay: rnd() * 1.4, speed: 70 + rnd() * 60, spin: 3 + rnd() * 6,
    phase: rnd() * 6, color: CONFETTI[Math.floor(rnd() * CONFETTI.length)],
  }));
  const draw = (ctx, half) => {
    pieces.forEach((p, i) => {
      if (i % 2 !== half) return;
      const age = t - p.delay;
      if (age < 0) return;
      const y = -8 + age * p.speed;
      if (y > H + 8) return;
      ctx.save();
      ctx.translate(p.x + Math.sin(age * 3 + p.phase) * 10, y);
      ctx.rotate(age * p.spin);
      ctx.scale(Math.cos(age * p.spin * 1.7), 1);
      ctx.fillStyle = rgba(p.color, 0.95);
      ctx.fillRect(-3.5, -1.8, 7, 3.6);
      ctx.restore();
    });
  };
  return {
    x: W / 2, y: FLOOR,
    pose: pose(t, reaction(t, [["cheer", 0.15], ["cheer", 1.85]])),
    back: (ctx) => draw(ctx, 0),
    front: (ctx) => draw(ctx, 1),
  };
}

function magicCarpet(t) {
  const arrive = easeOut(span(t, 0, 1.5));
  const cx = lerp(-70, W / 2, arrive);
  const cy = FLOOR - 18 + Math.sin(t * 4) * 3;
  const moving = t < 1.5;
  const carpet = (ctx) => {
    for (let i = 0; i < 9; i += 1) {
      const k = span(t, i * 0.12, i * 0.12 + 0.9);
      if (k <= 0 || k >= 1 || i * 0.12 > 1.4) continue;
      star(ctx, lerp(-70, W / 2, easeOut(span(i * 0.12, 0, 1.5))) - 48 - k * 10, cy + 4 + (i % 3) * 3, 3, GOLD, 1 - k, k * 4);
    }
    const ripple = (x) => Math.sin(x * 0.12 - t * 8) * 2.4;
    const half = 50;
    const fabric = ctx.createLinearGradient(cx - half, 0, cx + half, 0);
    fabric.addColorStop(0, "rgb(102,61,168)");
    fabric.addColorStop(0.5, "rgb(143,102,209)");
    fabric.addColorStop(1, "rgb(102,61,168)");
    ctx.fillStyle = fabric;
    ctx.beginPath();
    for (let x = -half; x <= half; x += 5) {
      const y = cy + ripple(x);
      if (x === -half) ctx.moveTo(cx + x, y);
      else ctx.lineTo(cx + x, y);
    }
    for (let x = half; x >= -half; x -= 5) ctx.lineTo(cx + x, cy + 7 + ripple(x));
    ctx.closePath();
    ctx.fill();
    ctx.strokeStyle = rgba(GOLD, 0.9);
    ctx.lineWidth = 1.2;
    ctx.beginPath();
    for (let x = -half + 3; x <= half - 3; x += 5) {
      const y = cy + 3.5 + ripple(x);
      if (x === -half + 3) ctx.moveTo(cx + x, y);
      else ctx.lineTo(cx + x, y);
    }
    ctx.stroke();
    for (const side of [-1, 1]) {
      for (let j = 0; j < 3; j += 1) {
        const y = cy + 1 + j * 2.5 + ripple(side * half);
        ctx.beginPath();
        ctx.moveTo(cx + side * half, y);
        ctx.lineTo(cx + side * (half + 5), y + 1.5);
        ctx.stroke();
      }
    }
  };
  return {
    x: cx, y: cy + 1,
    pose: pose(t, {
      facing: moving ? 1 : 0,
      ...reaction(t, [["wave", 1.55]]),
    }),
    back: carpet,
  };
}

function sparkleTwirl(t) {
  const k = easeOut(span(t, 0, 1.3));
  const sparkle = 1 - span(t, 1.3, 2.1);
  const orbit = (ctx) => {
    for (let i = 0; i < 10; i += 1) {
      const ang = t * 5 + (i / 10) * Math.PI * 2;
      const r = lerp(70, 30, k);
      star(ctx, W / 2 + Math.cos(ang) * r, FLOOR - 36 + Math.sin(ang) * r * 0.55, 4, i % 2 ? GOLD : TEAL, sparkle, ang);
    }
  };
  return {
    x: W / 2, y: lerp(FLOOR - 54, FLOOR, k), scale: lerp(0.2, 1, k), rotate: (1 - k) * Math.PI * 4,
    pose: pose(t, {
      air: t < 1.3 ? 0.4 : 0,
      magic: 1 - span(t, 1.3, 2.6),
      land: landing(t, 1.3),
      ...reaction(t, [["eureka", 1.4]]),
    }),
    front: orbit,
  };
}

function bounceIn(t) {
  const hops = [[-30, 50, 46], [50, 104, 32], [104, W / 2, 20]];
  const each = 0.45;
  const index = Math.min(Math.floor(t / each), hops.length);
  let x = W / 2;
  let y = FLOOR;
  let air = 0;
  if (index < hops.length) {
    const [from, to, height] = hops[index];
    const v = (t - index * each) / each;
    x = lerp(from, to, v);
    y = FLOOR - 4 * height * v * (1 - v);
    air = 0.6 * Math.sin(Math.PI * v);
  }
  return {
    x, y,
    pose: pose(t, {
      air,
      facing: index < hops.length ? 1 : 0,
      stridePhase: t * 10,
      land: landing(t, each, each * 2, each * 3),
      ...reaction(t, [["giggle", 1.45], ["nod", 2.6]]),
    }),
    back(ctx) {
      dust(ctx, 50, t, each);
      dust(ctx, 104, t, each * 2);
      dust(ctx, W / 2, t, each * 3);
    },
  };
}

function balloonFloat(t) {
  const u = easeInOut(span(t, 0, 2));
  const x = W / 2 + Math.sin(t * 2.2) * 14 * (1 - u);
  const y = lerp(-20, FLOOR, u);
  const held = t < 2.05;
  const hand = { x: x - SIZE * 0.4, y: y - SIZE * 0.58 };
  const lift = easeIn(span(t, 2.05, 3.4));
  const colors = [PINK, GOLD, TEAL];
  const offsets = [[-16, -44], [3, -56], [20, -42]];
  const draw = (ctx) => {
    const base = held ? hand : { x: hand.x + lift * 30, y: hand.y - lift * 190 };
    offsets.forEach(([dx, dy], i) => {
      const bx = base.x + dx + Math.sin(t * 2 + i) * 3;
      const by = base.y + dy + Math.cos(t * 1.7 + i) * 2;
      ctx.strokeStyle = rgba(SILVER, 0.7);
      ctx.lineWidth = 0.8;
      ctx.beginPath();
      ctx.moveTo(bx, by + 11);
      ctx.quadraticCurveTo(bx + 4, (by + base.y) / 2 + 6, base.x, base.y);
      ctx.stroke();
      const g = ctx.createRadialGradient(bx - 3, by - 4, 1, bx, by, 12);
      g.addColorStop(0, "rgba(255,255,255,0.9)");
      g.addColorStop(0.3, rgba(colors[i], 0.95));
      g.addColorStop(1, rgba(colors[i].map((c) => c * 0.6), 0.95));
      ctx.fillStyle = g;
      ctx.beginPath();
      ctx.ellipse(bx, by, 9, 11, 0, 0, Math.PI * 2);
      ctx.fill();
      ctx.beginPath();
      ctx.moveTo(bx - 2, by + 12.5);
      ctx.lineTo(bx + 2, by + 12.5);
      ctx.lineTo(bx, by + 10);
      ctx.closePath();
      ctx.fill();
    });
  };
  return {
    x, y,
    pose: pose(t, {
      mood: held ? "greeting" : "idle",
      air: held ? 0.25 * (1 - u) : 0,
      land: landing(t, 2),
      ...reaction(t, [["giggle", 2.15]]),
    }),
    back: draw,
  };
}

function welcomeWeb(t) {
  const cx = W / 2;
  const cy = 60;
  const spokes = 12;
  const radius = 76;
  const walk = span(t, 0, 0.9);
  const web = (ctx) => {
    ctx.strokeStyle = rgba(SILVER, 0.5);
    ctx.lineWidth = 0.8;
    const grow = easeOut(span(t, 0.2, 0.9));
    for (let i = 0; i < spokes; i += 1) {
      const ang = (i / spokes) * Math.PI * 2;
      ctx.beginPath();
      ctx.moveTo(cx, cy);
      ctx.lineTo(cx + Math.cos(ang) * radius * grow, cy + Math.sin(ang) * radius * grow * 0.8);
      ctx.stroke();
    }
    const rings = 6;
    const ringsDone = span(t, 0.9, 1.9) * rings;
    for (let r = 0; r < rings; r += 1) {
      const part = clamp01(ringsDone - r);
      if (part <= 0) break;
      const rr = radius * (0.18 + (r / rings) * 0.8);
      ctx.beginPath();
      const steps = Math.ceil(spokes * part);
      for (let i = 0; i <= steps; i += 1) {
        const ang = (Math.min(i, spokes * part) / spokes) * Math.PI * 2;
        const px = cx + Math.cos(ang) * rr;
        const py = cy + Math.sin(ang) * rr * 0.8;
        if (i === 0) ctx.moveTo(px, py);
        else ctx.lineTo(px, py);
      }
      ctx.stroke();
    }
    const word = "WELCOME!";
    const shown = Math.floor(span(t, 1.9, 2.6) * word.length + 0.0001);
    if (shown > 0) {
      ctx.save();
      ctx.font = "800 22px -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif";
      ctx.textAlign = "left";
      ctx.textBaseline = "middle";
      const left = cx - ctx.measureText(word).width / 2;
      ctx.shadowColor = rgba(TEAL, 0.9);
      ctx.shadowBlur = 10;
      ctx.fillStyle = "rgb(230,255,251)";
      ctx.fillText(word.slice(0, shown), left, cy - 22);
      ctx.restore();
    }
  };
  return {
    x: lerp(W + 40, cx + 70, easeOut(walk)), y: FLOOR,
    pose: pose(t, {
      walking: t < 0.9 ? 1 : 0,
      facing: t < 0.9 ? -1 : 0,
      stridePhase: t * 9,
      magic: t > 0.8 && t < 2 ? 0.8 : 0,
      ...reaction(t, [["nod", 2.7]]),
    }),
    back: web,
  };
}

function fireworks(t) {
  const bursts = [
    { x: 64, y: 40, at: 0.35, color: TEAL },
    { x: 214, y: 34, at: 0.95, color: GOLD },
    { x: 140, y: 26, at: 1.6, color: LILAC },
    { x: 92, y: 54, at: 2.3, color: PINK },
  ];
  const sky = (ctx) => {
    for (const b of bursts) {
      const rocket = span(t, b.at - 0.35, b.at);
      if (rocket > 0 && rocket < 1) {
        const ry = lerp(FLOOR, b.y, easeOut(rocket));
        glowDot(ctx, b.x, ry, 4, b.color, 0.9);
      }
      const k = span(t, b.at, b.at + 1.2);
      if (k <= 0 || k >= 1) continue;
      for (let i = 0; i < 18; i += 1) {
        const ang = (i / 18) * Math.PI * 2;
        const dist = easeOut(k) * 46;
        const px = b.x + Math.cos(ang) * dist;
        const py = b.y + Math.sin(ang) * dist + k * k * 22;
        glowDot(ctx, px, py, 3.2, b.color, 1 - k);
      }
    }
  };
  return {
    x: W / 2, y: FLOOR,
    pose: pose(t, {
      gaze: { x: 0, y: -0.8 },
      ...reaction(t, [["cheer", 0.5], ["eureka", 2.2]]),
    }),
    back: sky,
  };
}

function peekaboo(t) {
  const x = W / 2;
  const peek = FLOOR + 18;
  let y;
  let air = 0;
  if (t < 0.7) y = lerp(FLOOR + 90, peek, easeOut(t / 0.7));
  else if (t < 1.6) y = peek;
  else if (t < 2.1) {
    const v = span(t, 1.6, 2.1);
    y = lerp(peek, FLOOR, v) - 4 * 46 * v * (1 - v);
    air = 0.6 * Math.sin(Math.PI * v);
  } else y = FLOOR;
  const glance = t > 0.75 && t < 1.55 ? Math.sin((t - 0.75) * 8) : 0;
  const hole = 1 - span(t, 2.1, 2.6);
  return {
    x, y, clip: y > FLOOR,
    pose: pose(t, {
      air,
      gaze: { x: glance, y: 0.2 },
      land: landing(t, 2.1),
      ...reaction(t, [["surprise", 0.85], ["wave", 2.15]]),
    }),
    back(ctx) {
      if (hole <= 0) return;
      ctx.fillStyle = `rgba(4,4,12,${0.85 * hole})`;
      ctx.beginPath();
      ctx.ellipse(x, FLOOR + 2, 30, 5, 0, 0, Math.PI * 2);
      ctx.fill();
    },
  };
}

export const GENIE_WELCOMES = Object.freeze([
  { id: "silk", text: "🕷️ A genie spider drops in on a silk thread to say welcome!", scene: silkDrop },
  { id: "lamp", text: "🪔 A genie spider pops out of a magic lamp to say welcome!", scene: magicLamp },
  { id: "confetti", text: "🎉 A genie spider cheers under a shower of confetti. Welcome!", scene: confettiShower },
  { id: "carpet", text: "✨ A genie spider flies in on a magic carpet to say welcome!", scene: magicCarpet },
  { id: "twirl", text: "💫 A genie spider twirls in on a cloud of sparkles. Welcome!", scene: sparkleTwirl },
  { id: "bounce", text: "🕷️ A genie spider comes bouncing in to say welcome!", scene: bounceIn },
  { id: "balloons", text: "🎈 A genie spider floats down on balloons. Welcome!", scene: balloonFloat },
  { id: "web", text: "🕸️ A genie spider spins a WELCOME web!", scene: welcomeWeb },
  { id: "fireworks", text: "🎆 A genie spider lights up the sky with fireworks. Welcome!", scene: fireworks },
  { id: "peekaboo", text: "👀 A genie spider peeks out of a hole, then hops up to say welcome!", scene: peekaboo },
]);

export function genieWelcomeIndex(value) {
  return Number.isInteger(value) && value >= 0 && value < GENIE_WELCOMES.length ? value : null;
}

export function pickGenieWelcome(previous = null, random = Math.random) {
  const count = GENIE_WELCOMES.length;
  const last = genieWelcomeIndex(previous);
  const pool = last === null ? count : count - 1;
  const pick = Math.min(Math.floor(random() * pool), pool - 1);
  return last !== null && pick >= last ? pick + 1 : pick;
}

export function drawGenieWelcome(ctx, paint, index, time) {
  const welcome = GENIE_WELCOMES[genieWelcomeIndex(index) ?? 0];
  const t = Math.min(Math.max(Number(time) || 0, 0), D);
  const frame = welcome.scene(t);
  backdrop(ctx);
  ctx.save();
  frame.back?.(ctx);
  ctx.restore();
  if (paint && !frame.hidden) {
    ctx.save();
    if (frame.clip) {
      ctx.beginPath();
      ctx.rect(0, 0, W, FLOOR + 1);
      ctx.clip();
    }
    const k = (frame.scale ?? 1) * (SIZE / 100);
    ctx.translate(frame.x, frame.y);
    if (frame.rotate) {
      ctx.translate(0, -40 * k);
      ctx.rotate(frame.rotate);
      ctx.translate(0, 40 * k);
    }
    ctx.scale(k, k);
    ctx.translate(-50, -96.2);
    paint(ctx, frame.pose);
    ctx.restore();
  }
  ctx.save();
  frame.front?.(ctx);
  ctx.restore();
  return frame;
}

let painterRequest = null;

export function loadGeniePainter(win = globalThis) {
  if (typeof win.ForkMeshGenie?.paint === "function") return Promise.resolve(win.ForkMeshGenie.paint);
  if (painterRequest) return painterRequest;
  const doc = win.document;
  if (!doc?.createElement) return Promise.resolve(null);
  painterRequest = new Promise((resolve) => {
    if (!doc.documentElement.dataset.genie) doc.documentElement.dataset.genie = "off";
    const script = doc.createElement("script");
    script.src = GENIE_WELCOME_SCRIPT;
    script.async = true;
    script.addEventListener("load", () => resolve(typeof win.ForkMeshGenie?.paint === "function" ? win.ForkMeshGenie.paint : null));
    script.addEventListener("error", () => {
      painterRequest = null;
      resolve(null);
    });
    (doc.head || doc.documentElement).append(script);
  });
  return painterRequest;
}

export function mountGenieWelcome(index, options = {}) {
  const which = genieWelcomeIndex(index) ?? 0;
  const win = options.window || globalThis;
  const doc = win.document;
  const stage = doc.createElement("button");
  stage.type = "button";
  stage.className = "chat-genie-welcome";
  stage.setAttribute("data-instance-plugin", "genie");
  stage.dataset.genieWelcome = GENIE_WELCOMES[which].id;
  stage.title = "Play the genie spider again";
  stage.setAttribute("aria-label", `Genie spider welcome: ${GENIE_WELCOMES[which].id}. Play again`);
  const canvas = doc.createElement("canvas");
  canvas.width = W;
  canvas.height = H;
  canvas.setAttribute("aria-hidden", "true");
  stage.append(canvas);
  const ctx = canvas.getContext?.("2d");
  const reduced = Boolean(win.matchMedia?.("(prefers-reduced-motion: reduce)").matches);
  let paint = null;
  let raf = 0;
  let started = 0;

  const resize = () => {
    const ratio = Math.min(Math.max(win.devicePixelRatio || 1, 1), 3);
    const width = Math.round(W * ratio);
    if (canvas.width !== width) {
      canvas.width = width;
      canvas.height = Math.round(H * ratio);
    }
    return ratio;
  };
  const render = (t) => {
    if (!ctx) return;
    const ratio = resize();
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    drawGenieWelcome(ctx, paint, which, t);
  };
  const tick = (now) => {
    if (!stage.isConnected) {
      raf = 0;
      return;
    }
    const t = (now - started) / 1000;
    render(t);
    if (t < D) raf = win.requestAnimationFrame(tick);
    else {
      raf = 0;
      stage.classList.remove("is-playing");
    }
  };
  const play = () => {
    if (reduced || !paint || !win.requestAnimationFrame) {
      render(D);
      return;
    }
    if (raf) win.cancelAnimationFrame(raf);
    started = win.performance?.now?.() ?? Date.now();
    stage.classList.add("is-playing");
    options.onPlay?.();
    raf = win.requestAnimationFrame(tick);
  };

  render(D);
  const ready = loadGeniePainter(win).then((loaded) => {
    paint = loaded;
    render(D);
    return Boolean(loaded);
  });
  stage.addEventListener("click", () => void ready.then(play));
  if (options.autoplay && !reduced) {
    const start = () => void ready.then((ok) => ok && play());
    if (typeof win.IntersectionObserver === "function") {
      const watch = new win.IntersectionObserver((entries) => {
        if (!entries.some((entry) => entry.isIntersecting)) return;
        watch.disconnect();
        start();
      });
      watch.observe(stage);
    } else {
      start();
    }
  }
  return stage;
}
