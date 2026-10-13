import assert from "node:assert/strict";
import fs from "node:fs/promises";
import test from "node:test";


const source = await fs.readFile(new URL("../worker.js", import.meta.url), "utf8");
const worker = (await import(
  "data:text/javascript;base64," + Buffer.from(source).toString("base64")
)).default;

const EDGE_SECRET = "test-edge-secret-0123456789abcdef0123456789";

async function expectedEdgeMac(ip, country, ts, method, path) {
  const key = await crypto.subtle.importKey(
    "raw", new TextEncoder().encode(EDGE_SECRET), { name: "HMAC", hash: "SHA-256" },
    false, ["sign"]);
  const mac = await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(
    ["v1", ip, country, String(ts), method, path].join("\n")));
  return Buffer.from(mac).toString("base64url");
}

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;

// Next has no App service binding: worker.js fetches APP_ORIGIN, so the
// global fetch is routed to the stand-in App described by `APP`.
function environment(overrides = {}) {
  const captured = [];
  const env = {
    APP_ORIGIN: "https://app.forkmesh.com",
    WORLD_ORIGIN: "https://world.forkmesh.com",
    DEPLOY_FINGERPRINT: "fingerprint",
    ATTRIBUTION_COOKIE_DOMAIN: "forkmesh.com",
    WWW_EDGE_SECRET: EDGE_SECRET,
    APP: {
      async fetch(request) {
        captured.push({
          url: request.url,
          method: request.method,
          headers: Object.fromEntries(request.headers.entries()),
          body: request.method === "POST" ? JSON.parse(await request.text()) : null,
        });
        return new Response("app");
      },
    },
    ASSETS: {
      async fetch() {
        return new Response("<!doctype html><title>ForkMesh</title>", {
          headers: { "content-type": "text/html; charset=utf-8" },
        });
      },
    },
    ...overrides,
  };
  const app = env.APP;
  delete env.APP;
  globalThis.fetch = async (input, init) => app.fetch(new Request(input, init));
  return { env, captured };
}

function navigation(url, headers = {}) {
  return new Request(url, {
    headers: {
      accept: "text/html,application/xhtml+xml",
      "sec-fetch-dest": "document",
      "sec-fetch-mode": "navigate",
      "user-agent": "Mozilla/5.0 Firefox/130",
      "cf-connecting-ip": "203.0.113.7",
      "cf-ipcountry": "US",
      ...headers,
    },
  });
}

function waited() {
  const promises = [];
  return {
    ctx: { waitUntil(promise) { promises.push(promise); } },
    async settle() { await Promise.all(promises); },
  };
}


test("a landing page gets an opaque cookie and one forwarded observation", async () => {
  const { env, captured } = environment();
  const { ctx, settle } = waited();
  const response = await worker.fetch(
    navigation("https://forkmesh.com/?utm_source=reddit&utm_campaign=forever_launch", {
      referer: "https://old.reddit.com/r/git/",
    }),
    env,
    ctx,
  );
  await settle();
  assert.equal(response.status, 200);
  assert.equal(await response.text(), "<!doctype html><title>ForkMesh</title>");
  const cookie = response.headers.get("set-cookie");
  assert.ok(cookie, "cookie expected");
  const [pair, ...attributes] = cookie.split("; ");
  const [name, sessionId] = pair.split("=");
  assert.equal(name, "fm_attr");
  assert.match(sessionId, UUID);
  assert.deepEqual(attributes.sort(), [
    "Domain=forkmesh.com", "HttpOnly", "Max-Age=7776000", "Path=/", "SameSite=Lax", "Secure",
  ].sort());
  // The cookie is the only thing the browser holds: no UTM values in it.
  assert.ok(!cookie.includes("reddit") && !cookie.includes("forever_launch"));

  assert.equal(captured.length, 1);
  const [observation] = captured;
  assert.equal(observation.url, "https://app.forkmesh.com/api/attribution/capture");
  assert.equal(observation.method, "POST");
  assert.deepEqual(observation.body, {
    sessionId,
    minted: true,
    path: "/",
    search: "?utm_source=reddit&utm_campaign=forever_launch",
    host: "forkmesh.com",
  });
  assert.equal(observation.headers["x-forkmesh-attribution-relay"], "www");
  assert.equal(observation.headers["x-forkmesh-attribution-country"], "US");
  // The visitor's address is never forwarded as a Cf-* header; it travels
  // in the HMAC-signed x-forkmesh-edge-client header instead.
  assert.equal(observation.headers["cf-connecting-ip"], undefined);
  assert.equal(observation.headers["cf-ipcountry"], undefined);
  const [version, ip, country, ts, mac] = observation.headers["x-forkmesh-edge-client"].split(",");
  assert.equal(version, "v1");
  assert.equal(ip, "203.0.113.7");
  assert.equal(country, "US");
  assert.ok(Math.abs(Number(ts) - Date.now() / 1000) < 60);
  assert.equal(mac, await expectedEdgeMac(ip, country, ts, "POST", "/api/attribution/capture"));
  assert.equal(observation.headers.referer, "https://old.reddit.com/r/git/");
  assert.equal(observation.headers["user-agent"], "Mozilla/5.0 Firefox/130");
  assert.equal(observation.headers.cookie, undefined);
});


test("a returning visitor keeps their cookie and is observed under it", async () => {
  const { env, captured } = environment();
  const existing = "4b0f2f3e-8c2d-4f3a-9d2e-1f2a3b4c5d6e";
  const response = await worker.fetch(
    navigation("https://forkmesh.com/features", { cookie: "theme=dark; fm_attr=" + existing }),
    env,
  );
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("set-cookie"), null);
  assert.equal(captured.length, 1);
  assert.equal(captured[0].body.sessionId, existing);
  assert.equal(captured[0].body.minted, false);
  assert.equal(captured[0].body.path, "/features");
});


test("a tampered cookie value is replaced, never forwarded", async () => {
  const { env, captured } = environment();
  const response = await worker.fetch(
    navigation("https://forkmesh.com/", { cookie: "fm_attr=<script>utm_source=evil" }),
    env,
  );
  const cookie = response.headers.get("set-cookie");
  assert.ok(cookie);
  assert.match(cookie.split("; ")[0].split("=")[1], UUID);
  assert.equal(captured[0].body.minted, true);
  assert.ok(!JSON.stringify(captured[0].body).includes("evil"));
});


for (const [label, headers] of [
  ["DNT: 1", { dnt: "1" }],
  ["Sec-GPC: 1", { "sec-gpc": "1" }],
  ["an EU visitor (no consent banner yet)", { "cf-ipcountry": "DE" }],
]) {
  test(`${label} serves the page with no cookie and no observation`, async () => {
    const { env, captured } = environment();
    const response = await worker.fetch(
      navigation("https://forkmesh.com/?utm_source=reddit", headers),
      env,
    );
    assert.equal(response.status, 200);
    assert.equal(await response.text(), "<!doctype html><title>ForkMesh</title>");
    assert.equal(response.headers.get("set-cookie"), null);
    assert.equal(captured.length, 0);
  });
}


test("the skip-country list is configuration", async () => {
  const { env, captured } = environment({ ATTRIBUTION_SKIP_COUNTRIES: "XK" });
  const german = await worker.fetch(
    navigation("https://forkmesh.com/", { "cf-ipcountry": "DE" }), env);
  assert.ok(german.headers.get("set-cookie"));
  assert.equal(captured.length, 1);
  const skipped = await worker.fetch(
    navigation("https://forkmesh.com/", { "cf-ipcountry": "XK" }), env);
  assert.equal(skipped.headers.get("set-cookie"), null);
  assert.equal(captured.length, 1);
});


test("git protocol and repository paths are untouched by attribution", async () => {
  const { env, captured } = environment({
    APP: {
      async fetch(request) {
        captured.push({ url: request.url, method: request.method });
        return new Response("pack", {
          headers: { "content-type": "application/x-git-upload-pack-advertisement" },
        });
      },
    },
  });
  const refs = await worker.fetch(
    new Request("https://forkmesh.com/alice/repo/info/refs?service=git-upload-pack", {
      headers: { accept: "*/*", "user-agent": "git/2.45.0" },
    }),
    env,
  );
  assert.equal(refs.status, 200);
  assert.equal(refs.headers.get("set-cookie"), null);
  const pack = await worker.fetch(
    new Request("https://forkmesh.com/alice/repo.git/git-upload-pack", {
      method: "POST",
      headers: { accept: "*/*", "user-agent": "git/2.45.0",
        "content-type": "application/x-git-upload-pack-request" },
      body: "0000",
    }),
    env,
  );
  assert.equal(pack.status, 200);
  assert.equal(pack.headers.get("set-cookie"), null);
  // Exactly the two git requests were proxied; no capture was sent.
  assert.deepEqual(captured.map((c) => c.url), [
    "https://app.forkmesh.com/alice/repo/info/refs?service=git-upload-pack",
    "https://app.forkmesh.com/alice/repo.git/git-upload-pack",
  ]);
  // Repository shortcuts still redirect exactly as before, cookie-free.
  const shortcut = await worker.fetch(
    navigation("https://forkmesh.com/alice/repo?utm_source=reddit"), env);
  assert.equal(shortcut.status, 308);
  assert.equal(shortcut.headers.get("location"),
    "https://app.forkmesh.com/alice/repo?utm_source=reddit");
  assert.equal(shortcut.headers.get("set-cookie"), null);
  assert.equal(captured.length, 2);
});


test("only top-level HTML navigations are observed", async () => {
  const { env, captured } = environment({
    ASSETS: {
      async fetch() {
        return new Response("body{}", { headers: { "content-type": "text/css" } });
      },
    },
  });
  const stylesheet = await worker.fetch(
    new Request("https://forkmesh.com/styles.css", {
      headers: { accept: "text/css,*/*;q=0.1", "sec-fetch-dest": "style" },
    }),
    env,
  );
  assert.equal(stylesheet.headers.get("set-cookie"), null);
  const subresource = await worker.fetch(
    new Request("https://forkmesh.com/", {
      headers: { accept: "text/html", "sec-fetch-dest": "iframe", "sec-fetch-mode": "navigate" },
    }),
    env,
  );
  assert.equal(subresource.headers.get("set-cookie"), null);
  const head = await worker.fetch(
    new Request("https://forkmesh.com/", { method: "HEAD", headers: { accept: "text/html" } }),
    env,
  );
  assert.equal(head.headers.get("set-cookie"), null);
  assert.equal(captured.length, 0);
});


test("a failing App relay never changes what the visitor gets", async () => {
  const { env } = environment({
    APP: { async fetch() { throw new Error("app down"); } },
  });
  const response = await worker.fetch(navigation("https://forkmesh.com/about"), env);
  assert.equal(response.status, 200);
  assert.equal(await response.text(), "<!doctype html><title>ForkMesh</title>");
  assert.ok(response.headers.get("set-cookie"));
});
