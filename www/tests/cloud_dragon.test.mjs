import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import fs from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";

const asset = (name) => new URL(`../public/assets/cloud-dragon/${name}`, import.meta.url);
const spider = await fs.readFile(new URL("../public/genie-spider.js", import.meta.url), "utf8");
const source = await fs.readFile(asset("dragon.js"), "utf8");
const meta = JSON.parse(await fs.readFile(asset("atlas.json"), "utf8"));
const sandbox = { window: {} };
vm.runInNewContext(spider, sandbox);
vm.runInNewContext(source, sandbox);
const dragon = sandbox.window.ForkMeshDragon;
const genie = sandbox.window.ForkMeshGenie;

const SHEET = [
  "breathe-fire", "land", "terminal", "coffee", "book", "magnify", "celebrate", "headphones", "code-fast", "upload",
  "protect", "build", "share", "binoculars", "travel", "sleep", "stretch", "star", "globe", "heart",
];

function fakeContext() {
  const gradient = { addColorStop() {} };
  return new Proxy({}, {
    get: (target, name) => {
      if (name === "createRadialGradient" || name === "createLinearGradient") return () => gradient;
      if (name in target) return target[name];
      return () => {};
    },
    set: (target, name, value) => {
      target[name] = value;
      return true;
    },
  });
}

const art = () => ({ meta, image: {} });
const finite = (value) => assert.ok(Number.isFinite(value));


test("the chosen activities are all there", () => {
  const solo = Object.keys(dragon.activities).filter((name) => !dragon.activities[name].scene);
  assert.deepEqual(solo.sort(), [...SHEET].sort());
});


test("there are thirty distinct interactions with page elements", () => {
  assert.equal(dragon.tricks.length, 30);
  assert.equal(new Set(dragon.tricks.map((name) => dragon.activities[name].title)).size, 30);
  const spots = new Set(["side", "top", "edge", "ceiling", "over", "centre", "wall", "behind"]);
  for (const name of dragon.tricks) {
    const trick = dragon.activities[name];
    assert.ok(spots.has(trick.at), name);
    assert.equal(typeof trick.scene, "function", name);
    assert.ok(trick.fits.length > 0, name);
  }
});


test("every interaction draws and moves from start to end, facing both ways", () => {
  const rect = { left: 300, top: 200, right: 460, bottom: 244, width: 160, height: 44 };
  for (const name of dragon.tricks) {
    const trick = dragon.activities[name];
    for (const face of [1, -1]) {
      const picture = art();
      const s = { t: 1, walking: 0, air: 0, land: 0, facing: face, rest: face, reaction: "", pose: name, poseAge: 0, gaze: null, stridePhase: 0, aloft: true };
      for (let u = 0; u <= 1; u += 0.1) {
        s.poseAge = trick.duration * u;
        s.t += 0.1;
        dragon.paint(fakeContext(), s, picture);
        const o = { name, age: s.poseAge, rect, size: 96, face, paper: "#fff", origin: { x: 100, y: 120 } };
        dragon.scene(fakeContext(), s, picture, o);
        const motion = dragon.trickMotion(name, o);
        finite(motion.path.x);
        finite(motion.path.y);
        finite(motion.turn);
      }
    }
  }
});


test("the dragon never touches the page: no DOM, style, click or focus calls", () => {
  for (const banned of ["document.", ".style", ".click(", ".focus(", "setAttribute", "classList", "innerHTML", "appendChild"]) {
    assert.ok(!source.includes(banned), banned);
  }
});


test("every activity has a title, page kinds, a duration and a pose for the one body", () => {
  for (const [name, act] of Object.entries(dragon.activities)) {
    assert.ok(act.title.length > 8, name);
    assert.ok(act.fits.length > 0, name);
    assert.ok(act.duration > 1, name);
    assert.equal(typeof act.pose, "function", name);
    assert.equal(act.frame, undefined, name);
  }
});


test("every prop an activity holds is in the atlas", () => {
  for (const [name, act] of Object.entries(dragon.activities)) {
    for (const gear of act.gear || []) assert.ok(meta.props[gear.prop], `${name}: ${gear.prop}`);
  }
  for (const name of ["flame", "magnifier", "binoculars", "globe", "upcloud", "arrow", "bed", "plane", "zee", "loveheart", "sparkle"]) {
    assert.ok(meta.props[name], name);
  }
});


test("every frame stays inside the atlas", () => {
  for (const f of [...Object.values(meta.props), ...Object.values(meta.rig)]) {
    assert.ok(f.x >= 0 && f.y >= 0 && f.w > 0 && f.h > 0);
  }
});


test("each activity paints from its first moment to its last", () => {
  for (const [name, act] of Object.entries(dragon.activities)) {
    for (let u = 0; u <= 1; u += 0.1) {
      const s = { t: 2 + u, walking: 0, air: 0, land: 0, facing: 1, reaction: "", pose: name, poseAge: act.duration * u, gaze: null, stridePhase: 0 };
      dragon.paint(fakeContext(), s, art());
    }
  }
});


test("the skeleton has every joint and limb layer", () => {
  for (const name of ["wingFar", "wingNear", "body", "head", "ear", "eyeNear", "eyeFar", "armNear", "pawNear", "armFar",
    "pawFar", "thighNear", "footNear", "thighFar", "footFar"]) assert.ok(meta.rig[name], name);
  assert.ok(meta.rig.tailStrip);
  assert.ok(meta.tail.curve.length > 8);
  for (const name of ["hip", "neck", "shoulderNear", "shoulderFar", "pawNear", "pawFar", "hipNear", "hipFar", "footNear", "footFar"]) {
    assert.ok(meta.joints[name], name);
  }
});


test("poses only name joints the skeleton has, and stay finite", () => {
  const known = new Set(dragon.keys);
  for (const [name, act] of Object.entries(dragon.activities)) {
    for (let u = 0; u <= 1; u += 0.05) {
      const pose = act.pose({ age: act.duration * u, duration: act.duration, u, t: 3 });
      for (const [key, value] of Object.entries(pose)) {
        if (Array.isArray(value)) {
          assert.ok(known.has(`${key}x`) && known.has(`${key}y`), `${name}: ${key}`);
          value.forEach(finite);
        } else {
          assert.ok(known.has(key), `${name}: ${key}`);
          finite(value);
        }
      }
    }
  }
});


test("the body turns round in stages and ends mirrored", () => {
  const picture = art();
  const s = { t: 1, walking: 0, air: 0, land: 0, facing: 1, reaction: "", pose: "", poseAge: 0, gaze: null, stridePhase: 0 };
  dragon.paint(fakeContext(), s, picture);
  s.facing = -1;
  s.t += 0.08;
  dragon.paint(fakeContext(), s, picture);
  const flips = picture.view.flips;
  assert.ok(flips.head < flips.tail);
  for (let i = 0; i < 90; i += 1) {
    s.t += 1 / 60;
    dragon.paint(fakeContext(), s, picture);
  }
  assert.deepEqual({ ...picture.view.flips }, { head: -1, body: -1, wing: -1, tail: -1 });
});


test("the tail bends along its whole length", () => {
  const picture = art();
  const s = { t: 1, walking: 1, air: 0, land: 0, facing: 1, reaction: "", pose: "", poseAge: 0, gaze: null, stridePhase: 0 };
  for (let i = 0; i < 120; i += 1) {
    s.t += 1 / 60;
    s.stridePhase += 0.2;
    dragon.paint(fakeContext(), s, picture);
  }
  const aim = picture.view.tailAim;
  const rest = picture.view.tailRest;
  assert.equal(aim.length, meta.tail.curve.length - 1);
  assert.ok(aim.some((angle, index) => Math.abs(angle - rest[index]) > 0.05));
  aim.forEach(finite);
});


test("every idle quirk has a name and paints from start to end", () => {
  assert.equal(new Set(dragon.quirks.map((quirk) => quirk.name)).size, dragon.quirks.length);
  for (const quirk of dragon.quirks) {
    for (let u = 0; u < 1; u += 0.1) {
      const s = { t: 2 + u, walking: 0, air: 0, land: 0, facing: 1, reaction: "", pose: "", poseAge: 0, gaze: null, stridePhase: 0, quirk: { name: quirk.name, u } };
      dragon.paint(fakeContext(), s, art());
    }
  }
});


test("the tail curls when sitting and trails out straight when walking", () => {
  const spread = (walking) => {
    const picture = art();
    const s = { t: 1, walking, air: 0, land: 0, facing: 1, reaction: "", pose: "", poseAge: 0, gaze: null, stridePhase: 0 };
    for (let i = 0; i < 180; i += 1) {
      s.t += 1 / 60;
      dragon.paint(fakeContext(), s, picture);
    }
    const aim = picture.view.tailAim;
    return Math.max(...aim.map((angle) => Math.abs(Math.atan2(Math.sin(angle - aim[0]), Math.cos(angle - aim[0])))));
  };
  assert.ok(spread(0) > 2);
  assert.ok(spread(1) < 1.2);
});


test("springs carry the body smoothly between poses", () => {
  const picture = art();
  const s = { t: 1, walking: 0, air: 0, land: 0, facing: 1, reaction: "", pose: "", poseAge: 0, gaze: null, stridePhase: 0 };
  dragon.paint(fakeContext(), s, picture);
  s.pose = "stretch";
  s.poseAge = 1;
  s.t += 1 / 30;
  dragon.paint(fakeContext(), s, picture);
  const early = picture.view.q.pawNy.x;
  assert.ok(early < 0 && early > -200);
});


test("idle, walking, flying, held and reactions all paint", () => {
  const base = { t: 3, walking: 0, air: 0, land: 0, facing: 0, reaction: "", pose: "", poseAge: 0, gaze: null, stridePhase: 1 };
  const shapes = [
    {}, { walking: 1 }, { air: 1, vy: 900 }, { held: true, vx: 400 }, { land: 0.8 }, { gaze: { x: 0.5, y: -0.2 } },
    ...["wave", "cheer", "nod", "eureka", "giggle", "surprise", "dizzy"].map((reaction) => ({ reaction, reactionAge: 0.3 })),
  ];
  for (const shape of shapes) dragon.paint(fakeContext(), { ...base, ...shape }, art());
});


test("a drawn activity respects the kinds on the page", () => {
  const bag = [];
  const seen = new Set();
  for (let i = 0; i < 90; i += 1) {
    const name = genie.drawInteraction(bag, ["code"], { table: dragon.activities });
    assert.ok(dragon.activities[name].fits.includes("code"), name);
    seen.add(name);
  }
  assert.ok(seen.size > 8);
});


test("the bag plays everything before any repeats", () => {
  const bag = [];
  const seen = new Set();
  const total = Object.keys(dragon.activities).length;
  for (let i = 0; i < total; i += 1) seen.add(genie.drawInteraction(bag, dragon.kinds, { table: dragon.activities }));
  assert.equal(seen.size, total);
});


test("with nowhere to go it only draws solo activities", () => {
  const bag = [];
  for (let i = 0; i < 60; i += 1) {
    const name = genie.drawInteraction(bag, dragon.kinds, { table: dragon.activities, skip: (entry) => entry.scene });
    assert.ok(!dragon.activities[name].scene, name);
  }
});


test("the chosen mascot is read from the cookie first", () => {
  assert.equal(genie.mascotFrom("", null), "spider");
  assert.equal(genie.mascotFrom("", "dragon"), "dragon");
  assert.equal(genie.mascotFrom("a=1; forkmesh_mascot=dragon", null), "dragon");
  assert.equal(genie.mascotFrom("forkmesh_mascot=spider", "dragon"), "spider");
  assert.equal(genie.mascotFrom("forkmesh_mascot=kraken", null), "spider");
});


test("the mascot cookie is shared across forkmesh.com only", () => {
  assert.match(genie.mascotCookie("dragon", "app.forkmesh.com", true), /Domain=\.forkmesh\.com; Secure$/);
  assert.doesNotMatch(genie.mascotCookie("dragon", "instance.example", false), /Domain/);
});


test("the version in genie-spider.js matches the dragon's files", async () => {
  const digest = createHash("sha256");
  for (const name of ["dragon.js", "atlas.json", "atlas.webp"]) digest.update(await fs.readFile(asset(name)));
  assert.equal(genie.dragonVersion, digest.digest("hex").slice(0, 10));
});
