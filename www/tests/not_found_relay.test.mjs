import assert from "node:assert/strict";
import fs from "node:fs/promises";
import test from "node:test";


const source = await fs.readFile(new URL("../worker.js", import.meta.url), "utf8");
const worker = (await import(
  "data:text/javascript;base64," + Buffer.from(source).toString("base64")
)).default;

function environment(assets) {
  const relayed = [];
  globalThis.fetch = async (input, init) => {
    const request = new Request(input, init);
    relayed.push({
      url: request.url,
      method: request.method,
      body: request.method === "POST" ? JSON.parse(await request.text()) : null,
    });
    return new Response('{"ok":true}');
  };
  return {
    relayed,
    env: {
      APP_ORIGIN: "https://app.forkmesh.com",
      WORLD_ORIGIN: "https://world.forkmesh.com",
      DEPLOY_FINGERPRINT: "fingerprint",
      ASSETS: { fetch: assets },
    },
  };
}

function waited() {
  const promises = [];
  return {
    ctx: { waitUntil(promise) { promises.push(promise); } },
    async settle() { await Promise.all(promises); },
  };
}

test("a missing marketing page is relayed to the App's 404 log without its query", async () => {
  const { env, relayed } = environment(async (request) => {
    if (new URL(request.url).pathname === "/404") {
      return new Response("<h1>Page not found</h1>", { headers: { "content-type": "text/html" } });
    }
    return new Response(null, { status: 404 });
  });
  const { ctx, settle } = waited();
  const response = await worker.fetch(
    new Request("https://forkmesh.com/blog/old-post?utm_source=x", {
      headers: { referer: "https://news.example/item?id=1", "user-agent": "Mozilla/5.0 Test" },
    }),
    env,
    ctx,
  );
  await settle();
  assert.equal(response.status, 404);
  assert.equal(await response.text(), "<h1>Page not found</h1>");
  assert.deepEqual(relayed, [{
    url: "https://app.forkmesh.com/api/not-found",
    method: "POST",
    body: {
      path: "/blog/old-post",
      referrer: "https://news.example/item?id=1",
      userAgent: "Mozilla/5.0 Test",
    },
  }]);
});

test("pages that exist and non-read requests are not relayed", async () => {
  const { env, relayed } = environment(async () => new Response("ok"));
  const { ctx, settle } = waited();
  const response = await worker.fetch(new Request("https://forkmesh.com/pricing"), env, ctx);
  await settle();
  assert.equal(response.status, 200);
  assert.deepEqual(relayed, []);
});

test("a failed relay never changes the 404 the visitor gets", async () => {
  const { env } = environment(async () => new Response(null, { status: 404 }));
  globalThis.fetch = async () => { throw new Error("app down"); };
  const { ctx, settle } = waited();
  const response = await worker.fetch(new Request("https://forkmesh.com/gone"), env, ctx);
  await settle();
  assert.equal(response.status, 404);
});
