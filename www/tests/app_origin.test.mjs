import assert from "node:assert/strict";
import fs from "node:fs/promises";
import test from "node:test";


const source = await fs.readFile(new URL("../worker.js", import.meta.url), "utf8");
const worker = (await import(
  "data:text/javascript;base64," + Buffer.from(source).toString("base64")
)).default;

const SECRET = "edge-secret-for-tests-0123456789abcdef0123";

function environment(overrides = {}) {
  return {
    APP_ORIGIN: "https://app.forkmesh.com",
    WORLD_ORIGIN: "https://world.forkmesh.com",
    DEPLOY_FINGERPRINT: "fingerprint",
    WWW_EDGE_SECRET: SECRET,
    ASSETS: { async fetch() { return new Response("missing", { status: 404 }); } },
    ...overrides,
  };
}

function route(handler) {
  const seen = [];
  globalThis.fetch = async (input, init) => {
    const request = new Request(input, init);
    seen.push(request);
    return handler(request, seen.length);
  };
  return seen;
}

async function mac(message) {
  const key = await crypto.subtle.importKey(
    "raw", new TextEncoder().encode(SECRET), { name: "HMAC", hash: "SHA-256" },
    false, ["sign"]);
  const raw = await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(message));
  return Buffer.from(raw).toString("base64url");
}


test("the App is reached over APP_ORIGIN with a signed client header", async () => {
  const seen = route(() => new Response("ok"));
  const response = await worker.fetch(new Request("https://forkmesh.com/api/stats?x=1", {
    headers: {
      "cf-connecting-ip": "198.51.100.9",
      "cf-ipcountry": "CA",
      "cf-ray": "abc",
      "x-forwarded-for": "10.0.0.1",
      "x-real-ip": "10.0.0.2",
      "true-client-ip": "10.0.0.3",
      // A visitor cannot smuggle in a pre-signed header of their own.
      "x-forkmesh-edge-client": "v1,1.2.3.4,US,1,forged",
      accept: "application/json",
    },
  }), environment());
  assert.equal(response.status, 200);
  assert.equal(seen.length, 1);
  const [request] = seen;
  assert.equal(request.url, "https://app.forkmesh.com/api/stats?x=1");
  assert.equal(request.redirect, "manual");
  for (const name of ["cf-connecting-ip", "cf-ipcountry", "cf-ray", "x-forwarded-for",
    "x-real-ip", "true-client-ip"]) {
    assert.equal(request.headers.get(name), null, name);
  }
  assert.equal(request.headers.get("accept"), "application/json");
  const [version, ip, country, ts, signature] =
    request.headers.get("x-forkmesh-edge-client").split(",");
  assert.deepEqual([version, ip, country], ["v1", "198.51.100.9", "CA"]);
  assert.ok(Math.abs(Number(ts) - Date.now() / 1000) < 60);
  assert.equal(signature, await mac(["v1", ip, country, ts, "GET", "/api/stats"].join("\n")));
});


test("the visitor's place and network from request.cf ride along for the log", async () => {
  const seen = route(() => new Response("ok"));
  const request = new Request("https://forkmesh.com/forkmesh/forkmesh/info/refs", {
    headers: {
      "cf-connecting-ip": "198.51.100.9",
      "x-forkmesh-edge-where": "city=Forged",
    },
  });
  Object.defineProperty(request, "cf", {
    value: { country: "US", city: "Ashburn", region: "Virginia", asn: 7922,
      asOrganization: "Comcast\nCable" },
  });
  await worker.fetch(request, environment());
  const where = new URLSearchParams(seen[0].headers.get("x-forkmesh-edge-where"));
  assert.deepEqual(Object.fromEntries(where), {
    city: "Ashburn", region: "Virginia", asn: "7922", org: "Comcast Cable",
  });
  assert.equal(seen[0].headers.get("x-forkmesh-edge-client").split(",")[1], "198.51.100.9");
});


test("a visitor cannot send their own place header", async () => {
  const seen = route(() => new Response("ok"));
  await worker.fetch(new Request("https://forkmesh.com/api/stats", {
    headers: { "cf-connecting-ip": "198.51.100.9", "x-forkmesh-edge-where": "city=Forged" },
  }), environment());
  assert.equal(seen[0].headers.get("x-forkmesh-edge-where"), null);
});


test("without WWW_EDGE_SECRET no client identity is forwarded at all", async () => {
  const seen = route(() => new Response("ok"));
  await worker.fetch(new Request("https://forkmesh.com/api/stats", {
    headers: { "cf-connecting-ip": "198.51.100.9", "x-forkmesh-edge-client": "v1,x,y,1,z" },
  }), environment({ WWW_EDGE_SECRET: "" }));
  assert.equal(seen[0].headers.get("x-forkmesh-edge-client"), null);
  assert.equal(seen[0].headers.get("x-forkmesh-edge-where"), null);
  assert.equal(seen[0].headers.get("cf-connecting-ip"), null);
});


test("reads fall back to the node origins when APP_ORIGIN is unreachable", async () => {
  const seen = route((request, count) => {
    if (count === 1) throw new TypeError("connect ECONNREFUSED");
    if (count === 2) return new Response("bad gateway", { status: 502 });
    return new Response("from n3");
  });
  const response = await worker.fetch(
    new Request("https://forkmesh.com/api/repo/alice/x?ref=main", {
      headers: { "cf-connecting-ip": "198.51.100.9" },
    }),
    environment(),
  );
  assert.equal(await response.text(), "from n3");
  assert.deepEqual(seen.map((request) => request.url), [
    "https://app.forkmesh.com/api/repo/alice/x?ref=main",
    "https://n1.next.forkmesh.com/_app/api/repo/alice/x?ref=main",
    "https://n2.next.forkmesh.com/_app/api/repo/alice/x?ref=main",
  ]);
  // Every attempt carries the same signed identity.
  const edge = seen.map((request) => request.headers.get("x-forkmesh-edge-client"));
  assert.ok(edge[0] && edge.every((value) => value === edge[0]));
});


test("reads that time out move on to the next node", async () => {
  const seen = route((request, count) => {
    if (count === 1) {
      return new Promise((_, reject) => {
        request.signal.addEventListener("abort", () => reject(request.signal.reason));
      });
    }
    return new Response("n1 answered");
  });
  const response = await worker.fetch(new Request("https://forkmesh.com/api/stats"),
    environment({ APP_FETCH_TIMEOUT_MS: "50" }));
  assert.equal(await response.text(), "n1 answered");
  assert.equal(seen.length, 2);
});


test("writes are never retried against another node", async () => {
  const seen = route(() => { throw new TypeError("connect ECONNREFUSED"); });
  await assert.rejects(worker.fetch(new Request("https://forkmesh.com/api/login", {
    method: "POST", body: "{}",
  }), environment()));
  assert.equal(seen.length, 1);
  assert.equal(seen[0].url, "https://app.forkmesh.com/api/login");
});


test("when every origin fails the visitor gets a 502 from www", async () => {
  route(() => { throw new TypeError("down"); });
  const response = await worker.fetch(new Request("https://forkmesh.com/api/stats"),
    environment({ APP_FALLBACK_ORIGINS: "https://n9.next.forkmesh.com" }));
  assert.equal(response.status, 502);
  assert.equal(response.headers.get("x-forkmesh-worker"), "www");
  assert.deepEqual(await response.json(), { error: "app_unavailable" });
});


test("App redirects pass through instead of being followed", async () => {
  route(() => new Response(null, { status: 302, headers: { location: "/login" } }));
  const response = await worker.fetch(new Request("https://forkmesh.com/api/oauth/start"),
    environment());
  assert.equal(response.status, 302);
  assert.equal(response.headers.get("location"), "/login");
});


test("cross-check vectors", { skip: !process.env.FORKMESH_EDGE_VECTORS_OUT }, async () => {
  const cases = [
    ["GET", "https://forkmesh.com/api/stats?x=1", "198.51.100.9", "CA"],
    ["POST", "https://forkmesh.com/alice/repo/git-upload-pack", "2001:db8::7", ""],
    ["GET", "https://forkmesh.com/@jett", "203.0.113.5", "DE"],
  ];
  const vectors = [];
  for (const [method, url, ip, country] of cases) {
    const seen = route(() => new Response("ok"));
    const headers = { "cf-connecting-ip": ip };
    if (country) headers["cf-ipcountry"] = country;
    await worker.fetch(new Request(url, {
      method, headers, body: method === "POST" ? "x" : undefined,
    }), environment());
    assert.equal(seen.length, 1, url);
    vectors.push({
      method: seen[0].method,
      path: new URL(seen[0].url).pathname,
      header: seen[0].headers.get("x-forkmesh-edge-client"),
      secret: SECRET,
      ip,
      country,
    });
  }
  await fs.writeFile(process.env.FORKMESH_EDGE_VECTORS_OUT, JSON.stringify(vectors));
});
