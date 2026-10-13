import assert from "node:assert/strict";
import fs from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";


const source = await fs.readFile(new URL("../public/genie-spider.js", import.meta.url), "utf8");
const sandbox = { window: {} };
vm.runInNewContext(source, sandbox);
const genie = sandbox.window.ForkMeshGenie;

const guest = (path) => ({ path, account: "", unread: null, chatNew: 0 });
const member = (path, extra = {}) => ({
  path, account: "ada", unread: 0, chatNew: 0, ...extra,
});


test("paths normalize the way the site serves them", () => {
  assert.equal(genie.normalizePath("/"), "/");
  assert.equal(genie.normalizePath("/index.html"), "/");
  assert.equal(genie.normalizePath("/features.html"), "/features");
  assert.equal(genie.normalizePath("/blog/coves/"), "/blog/coves");
  assert.equal(genie.normalizePath("/docs/getting-started/?q=1#top"), "/docs/getting-started");
});


test("a new visitor hears the page line first, then the onboarding lines", () => {
  const tips = genie.tips(guest("/desktop.html"));
  assert.equal(tips[0], "The desktop app hosts, mirrors and runs coding agents.");
  assert.ok(tips.some((tip) => /free account/.test(tip)));
});


test("a signed-in member hears unread news first and is never told to sign up", () => {
  const tips = genie.tips(member("/features", { unread: 2 }));
  assert.equal(tips[0], "You have 2 unread notifications.");
  assert.ok(tips.every((tip) => !/account/.test(tip)));
  assert.equal(genie.tips(member("/features"))[0], genie.pageTip("/features", true));
});


test("chat news is the briefing when the inbox is empty", () => {
  assert.equal(genie.briefing(member("/", { unread: 0, chatNew: 1 })), "1 new chat message since you last looked.");
  assert.equal(genie.briefing(member("/")), "");
});


test("member-only and guest-only page lines stay with their audience", () => {
  assert.equal(genie.pageTip("/dashboard/agents", false), "");
  assert.match(genie.pageTip("/dashboard/agents", true), /agent/);
  assert.equal(genie.pageTip("/signup", true), "");
  assert.match(genie.pageTip("/signup", false), /free account/);
  assert.match(genie.pageTip("/blog/coves/", false), /More posts/);
  assert.match(genie.pageTip("/blog", false), /RSS/);
});


test("every line is one sentence of five to ten words", () => {
  const paths = ["/", "/docs", "/dashboard", "/dashboard/agents", "/dashboard/repos", "/dashboard/repos/new",
    "/dashboard/settings", "/dashboard/apps", "/chat", "/referral", "/login", "/signup", "/features", "/blog",
    "/blog/coves/", "/desktop", "/mobile", "/market", "/ecosystem", "/hackathon", "/status", "/network"];
  const contexts = paths.flatMap((path) => [guest(path), member(path, { unread: 12, chatNew: 3 }), member(path, { chatNew: 1 })]);
  for (const context of contexts) {
    for (const tip of genie.tips(context)) {
      const words = genie.wordCount(tip);
      assert.ok(words >= 5 && words <= 10, `${words} words: ${tip}`);
      assert.ok(!tip.includes("\n"));
    }
  }
});


const button = (left, right, top = 560, bottom = 600) => ({ left, right, top, bottom });
const spot = (extra) => ({ min: 0, max: 1200, size: 78, floor: 522, rects: [], ...extra });


test("the spider stays where it wants to be when nothing is under it", () => {
  assert.equal(genie.clearX(spot({ preferred: 500 })), 500);
  assert.equal(genie.clearX(spot({ preferred: 500, rects: [button(800, 900)] })), 500);
  assert.equal(genie.clearX(spot({ preferred: 1500 })), 1200);
});


test("flight gains speed from gravity without accelerating forever", () => {
  let state = { x: 0, y: 0, vx: 0, vy: 0 };
  state = genie.flightStep(state, 1 / 120);
  assert.ok(state.y > 0);
  assert.ok(state.vy > 0);
  for (let step = 0; step < 1200; step += 1) state = genie.flightStep(state, 1 / 120);
  assert.ok(state.vy > 1800);
  assert.ok(state.vy < 2200);
  assert.ok(Math.hypot(state.vx, state.vy) <= genie.flight.maxSpeed);
  const capped = genie.flightStep({ x: 0, y: 0, vx: 0, vy: -20000 }, 1 / 120);
  assert.ok(Math.hypot(capped.vx, capped.vy) <= genie.flight.maxSpeed);
  assert.ok(Math.abs(capped.y) <= genie.flight.maxSpeed / 120);
});


test("a fast flick loses airspeed and lands with a soft rebound", () => {
  let state = { x: 0, y: 0, vx: 3200, vy: -400 };
  for (let step = 0; step < 120; step += 1) state = genie.flightStep(state, 1 / 120);
  assert.ok(state.x > 1000);
  assert.ok(state.vx > 0 && state.vx < 1800);
  assert.equal(genie.landingVelocity(239), 0);
  assert.equal(genie.landingVelocity(1000), -300);
});


test("the spider steps to the nearest side of a button at its floor", () => {
  const rects = [button(500, 560)];
  const left = genie.clearX(spot({ preferred: 470, rects }));
  const right = genie.clearX(spot({ preferred: 510, rects }));
  assert.ok(left < 470 && genie.footprintCover(left, spot({ rects })) === 0);
  assert.ok(right > 510 && genie.footprintCover(right, spot({ rects })) === 0);
  assert.ok(left + 78 * genie.body.right <= 500);
  assert.ok(right + 78 * genie.body.left >= 560);
});


test("a button in the corner sends it the other way, never off the floor", () => {
  const rects = [button(1180, 1278)];
  const x = genie.clearX(spot({ preferred: 1200, rects }));
  assert.ok(x < 1200 && x >= 0);
  assert.equal(genie.footprintCover(x, spot({ rects })), 0);
});


test("elements above its head or below its feet do not move it", () => {
  const heading = { left: 400, right: 900, top: 300, bottom: 340 };
  const footer = { left: 400, right: 900, top: 600, bottom: 640 };
  assert.equal(genie.clearX(spot({ preferred: 500, rects: [heading, footer] })), 500);
});


test("standing on a fixed bar lifts its body clear of the page below the bar's top", () => {
  const link = button(480, 620, 560, 590);
  const floor = (x) => (x > 300 && x < 700 ? 400 : 522);
  assert.equal(genie.clearX(spot({ preferred: 500, rects: [link], floor })), 500);
  assert.notEqual(genie.clearX(spot({ preferred: 500, rects: [link] })), 500);
});


test("a row of buttons with gaps leaves it in a gap wide enough", () => {
  const rects = [button(0, 300), button(400, 700), button(800, 1278)];
  const x = genie.clearX(spot({ preferred: 600, rects }));
  assert.equal(genie.footprintCover(x, spot({ rects })), 0);
  assert.ok(x >= 300 && x + 78 <= 800);
});


test("when nothing is clear it picks the least covered spot", () => {
  const rects = [button(0, 600), button(640, 1278, 590, 600)];
  const x = genie.clearX(spot({ preferred: 100, rects }));
  const cover = genie.footprintCover(x, spot({ rects }));
  assert.ok(cover > 0);
  for (const other of [0, 100, 300, 560, 700, 1000, 1200]) {
    assert.ok(cover <= genie.footprintCover(other, spot({ rects })) + 0.01);
  }
});


test("element kinds come from the tag, role, type and logo labels", () => {
  const kinds = (info) => [...genie.elementKinds(info)].sort();
  assert.deepEqual(kinds({ tag: "IMG", label: "site-logo" }), ["image", "logo"]);
  assert.deepEqual(kinds({ tag: "div", label: "brand Logo" }), ["logo"]);
  assert.deepEqual(kinds({ tag: "h2", words: 3 }), ["heading", "short"]);
  assert.deepEqual(kinds({ tag: "h1", words: 14 }), ["heading"]);
  assert.deepEqual(kinds({ tag: "a", rowLinks: 4 }), ["link", "nav"]);
  assert.deepEqual(kinds({ tag: "a", rowLinks: 1 }), ["link"]);
  assert.deepEqual(kinds({ tag: "input", type: "submit" }), ["button"]);
  assert.deepEqual(kinds({ tag: "input" }), ["input"]);
  assert.deepEqual(kinds({ tag: "input", type: "hidden" }), []);
  assert.deepEqual(kinds({ tag: "div", role: "button" }), ["button"]);
  assert.deepEqual(kinds({ tag: "pre" }), ["code"]);
  assert.deepEqual(kinds({ tag: "section", label: "pricing-card" }), ["card"]);
});


test("there are at least twelve interactions and every kind has some", () => {
  const names = Object.keys(genie.interactions);
  assert.ok(names.length >= 12);
  const kinds = ["heading", "short", "button", "link", "nav", "image", "logo", "input", "code", "card"];
  for (const name of names) {
    assert.ok(genie.interactions[name].fits.length > 0);
    for (const kind of genie.interactions[name].fits) assert.ok(kinds.includes(kind), `${name} fits ${kind}`);
  }
  for (const kind of kinds) {
    assert.ok(names.filter((name) => genie.interactions[name].fits.includes(kind)).length >= 2, kind);
  }
  assert.ok(names.some((name) => genie.interactions[name].still));
});


test("thirty genie effects join the interactions, none of them still", () => {
  const effects = genie.genieEffects;
  assert.equal(effects.length, 30);
  assert.equal(new Set(effects).size, 30);
  for (const name of effects) {
    const play = genie.interactions[name];
    assert.ok(play && play.genie, name);
    assert.ok(play.fits.length > 0, name);
    assert.ok(!play.still, name);
  }
  const kinds = ["heading", "short", "button", "link", "nav", "image", "logo", "input", "code", "card"];
  for (const kind of kinds) assert.ok(effects.some((name) => genie.interactions[name].fits.includes(kind)), kind);
  assert.equal(effects.filter((name) => genie.interactions[name].teleport).join(), "poof");
});


test("a page with only a link still gets genie effects from the bag", () => {
  const bag = [];
  const seen = new Set();
  for (let i = 0; i < 200; i += 1) seen.add(genie.drawInteraction(bag, ["link"]));
  const effects = genie.genieEffects.filter((name) => genie.interactions[name].fits.includes("link"));
  assert.ok(effects.length >= 10);
  for (const name of effects) assert.ok(seen.has(name), name);
});


test("the bag uses every interaction before one repeats", () => {
  const all = ["heading", "short", "button", "link", "nav", "image", "logo", "input", "code", "card"];
  const names = Object.keys(genie.interactions);
  let seed = 7;
  const rand = () => {
    seed = (seed * 16807) % 2147483647;
    return seed / 2147483647;
  };
  const bag = [];
  for (let round = 0; round < 3; round += 1) {
    const drawn = names.map(() => genie.drawInteraction(bag, all, { rand }));
    assert.deepEqual([...drawn].sort(), [...names].sort());
  }
});


test("the bag only hands out interactions that fit what is on the page", () => {
  const bag = [];
  for (let i = 0; i < 30; i += 1) {
    const name = genie.drawInteraction(bag, ["link"]);
    assert.ok(genie.interactions[name].fits.includes("link"), name);
  }
  assert.equal(genie.drawInteraction([], []), "");
});


test("with reduced motion or Stay put only still interactions are drawn", () => {
  const bag = [];
  for (let i = 0; i < 20; i += 1) {
    const name = genie.drawInteraction(bag, ["button", "heading"], { still: true });
    assert.ok(genie.interactions[name].still, name);
  }
});


test("the shared cookie decides whether it is off, then this origin's flag", () => {
  assert.equal(genie.offFrom("", null), false);
  assert.equal(genie.offFrom("", "1"), true);
  assert.equal(genie.offFrom("a=1; forkmesh_genie=off", null), true);
  assert.equal(genie.offFrom("forkmesh_genie=on; b=2", "1"), false);
  assert.equal(genie.offFrom("xforkmesh_genie=off", null), false);
});


test("the cookie is shared across forkmesh.com hosts and host-only elsewhere", () => {
  assert.equal(
    genie.offCookie(true, "app.forkmesh.com", true),
    "forkmesh_genie=off; Path=/; Max-Age=31536000; SameSite=Lax; Domain=.forkmesh.com; Secure",
  );
  assert.match(genie.offCookie(false, "forkmesh.com", true), /^forkmesh_genie=on;.*Domain=\.forkmesh\.com/);
  assert.doesNotMatch(genie.offCookie(true, "localhost", false), /Domain|Secure/);
});


function page({ cookie = "", local = {}, session = {}, genieAttr, instance } = {}) {
  const store = (seed) => {
    const map = new Map(Object.entries(seed));
    return {
      map,
      getItem: (key) => (map.has(key) ? map.get(key) : null),
      setItem: (key, value) => map.set(key, String(value)),
      removeItem: (key) => map.delete(key),
    };
  };
  const touched = [];
  const cookies = [];
  const events = [];
  const jar = new Map(cookie ? cookie.split("; ").map((part) => part.split("=")) : []);
  const document = {
    readyState: "complete",
    documentElement: { dataset: genieAttr ? { genie: genieAttr } : {} },
    get cookie() {
      return [...jar].map(([key, value]) => `${key}=${value}`).join("; ");
    },
    set cookie(text) {
      cookies.push(text);
      const [pair] = text.split(";");
      const [key, value] = pair.split("=");
      jar.set(key, value);
    },
    querySelector: (selector) => {
      if (selector === "script[data-instance-brand]") return instance ? {} : null;
      return touched.push("querySelector");
    },
    addEventListener: (name) => touched.push(`document:${name}`),
  };
  const window = {
    localStorage: store(local),
    sessionStorage: store(session),
    addEventListener: (name) => touched.push(`window:${name}`),
    dispatchEvent: (event) => events.push(event.detail),
  };
  class CustomEvent {
    constructor(type, init) {
      this.type = type;
      this.detail = init.detail;
    }
  }
  const sandbox = {
    window, document, CustomEvent,
    location: { hostname: "app.forkmesh.com", protocol: "https:", pathname: "/" },
    fetch: (url) => {
      touched.push(`fetch:${url}`);
      return Promise.resolve({ ok: true, json: () => Promise.resolve(instance) });
    },
    setInterval: () => touched.push("setInterval"),
    setTimeout: () => touched.push("setTimeout"),
  };
  vm.runInNewContext(source, sandbox);
  return { api: window.ForkMeshGenie, window, touched, cookies, events };
}


test("hidden for good, it builds nothing: no element, listener, timer or fetch", () => {
  for (const options of [{ cookie: "forkmesh_genie=off" }, { local: { "forkmesh.genie.off": "1" } }]) {
    const view = page(options);
    assert.equal(view.api.isOff(), true);
    assert.equal(view.api.shown(), false);
    assert.deepEqual(view.touched, []);
  }
  const back = page({ cookie: "forkmesh_genie=on", local: { "forkmesh.genie.off": "1" } });
  assert.equal(back.api.isOff(), false);
  assert.deepEqual(back.touched, ["querySelector"]);
});


test("on a white-label instance it builds only when the genie plugin is on", async () => {
  const off = page({ instance: { plugins: [{ id: "genie", enabled: false }] } });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(off.api.shown(), false);
  assert.deepEqual(off.touched, ["fetch:/api/instance"]);
  const missing = page({ instance: { plugins: [] } });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(missing.touched, ["fetch:/api/instance"]);
  const on = page({ instance: { plugins: [{ id: "genie", enabled: true }] } });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(on.touched, ["fetch:/api/instance", "querySelector"]);
});


test("hide and show write this origin's flag and the shared cookie", () => {
  const view = page({ session: { "forkmesh.genie.away": "1" }, genieAttr: "off" });
  assert.equal(view.api.hidden(), true);
  assert.equal(view.api.isOff(), false);
  view.api.hide();
  assert.equal(view.window.localStorage.getItem("forkmesh.genie.off"), "1");
  assert.match(view.cookies.at(-1), /^forkmesh_genie=off;.*Domain=\.forkmesh\.com/);
  assert.equal(view.events.at(-1).off, true);
  view.api.show();
  assert.equal(view.window.localStorage.getItem("forkmesh.genie.off"), null);
  assert.equal(view.window.sessionStorage.getItem("forkmesh.genie.away"), null);
  assert.match(view.cookies.at(-1), /^forkmesh_genie=on;/);
  assert.deepEqual({ ...view.events.at(-1) }, { off: false, hidden: false });
  assert.deepEqual(view.touched, []);
});
