import assert from "node:assert/strict";
import fs from "node:fs/promises";
import test from "node:test";


const source = await fs.readFile(new URL("../worker.js", import.meta.url), "utf8");
const worker = (await import(
  "data:text/javascript;base64," + Buffer.from(source).toString("base64")
)).default;


// Next has no App service binding: worker.js fetches APP_ORIGIN over the
// network. Tests describe the App as `APP: { fetch }` and environment()
// routes the global fetch to it; the Worker itself never sees env.APP.
function environment(overrides = {}) {
  const { APP = { async fetch() { return new Response("app"); } }, ...rest } = overrides;
  globalThis.fetch = async (input, init) => APP.fetch(new Request(input, init));
  return {
    APP_ORIGIN: "https://app.forkmesh.com",
    WORLD_ORIGIN: "https://world.forkmesh.com",
    DEPLOY_FINGERPRINT: "fingerprint",
    ASSETS: { async fetch() { return new Response("missing", { status: 404 }); } },
    ...rest,
  };
}


test("repository shortcuts reach App", async () => {
  const response = await worker.fetch(
    new Request("https://forkmesh.com/alice/repo?tab=issues"),
    environment(),
  );
  assert.equal(response.status, 308);
  assert.equal(response.headers.get("location"), "https://app.forkmesh.com/alice/repo?tab=issues");
});


test("ecosystem project pages are served by the ecosystem document", async () => {
  const requested = [];
  const env = environment({
    ASSETS: {
      async fetch(request) {
        requested.push(new URL(request.url).pathname + new URL(request.url).search);
        return new Response("<h1>ForkMesh Ecosystem</h1>", {
          headers: { "content-type": "text/html; charset=utf-8" },
        });
      },
    },
  });
  for (const path of ["/ecosystem/git-bug", "/ecosystem/git-bug/?ref=home"]) {
    const response = await worker.fetch(
      new Request("https://forkmesh.com" + path, { headers: { dnt: "1" } }),
      env,
    );
    assert.equal(response.status, 200);
    assert.equal(await response.text(), "<h1>ForkMesh Ecosystem</h1>");
    assert.equal(response.headers.get("x-forkmesh-worker"), "www");
  }
  assert.deepEqual(requested, ["/ecosystem", "/ecosystem"]);

  for (const path of ["/ecosystem/Git_Bug", "/ecosystem/git-bug/issues"]) {
    const response = await worker.fetch(new Request("https://forkmesh.com" + path), env);
    assert.equal(response.status, 308);
    assert.equal(response.headers.get("location"), "https://app.forkmesh.com" + path);
  }
});


test("public profiles and the assets their markup names stay on this origin", async () => {
  const proxied = [];
  const profileProxyHeaders = [];
  const env = environment({
    APP: {
      async fetch(request) {
        proxied.push(new URL(request.url).pathname);
        profileProxyHeaders.push(request.headers.get("x-forkmesh-public-profile-proxy"));
        return new Response("app");
      },
    },
    ASSETS: {
      async fetch() {
        throw new Error("profile assets must not be served from WWW's own dist");
      },
    },
  });
  // The document, plus the files app/public/dashboard/profile/index.html and
  // its bundle load by absolute path. A redirect for any of them breaks the
  // page: the script would 404 and the stylesheet would land on another
  // origin, which `script-src 'self'`/`style-src 'self'` refuse. /qr.js is
  // fetched at runtime to draw a profile's tip address, and only App stages
  // it, so it has to reach App from this origin too.
  for (const path of [
    "/@jett",
    "/@jett/repositories",
    "/dashboard.js",
    "/dashboard/tailwind.css",
    "/qr.js",
  ]) {
    const response = await worker.fetch(new Request("https://forkmesh.com" + path), env);
    assert.equal(response.status, 200, path);
    assert.equal(response.headers.get("location"), null, path);
  }
  assert.deepEqual(proxied, [
    "/@jett",
    "/@jett/repositories",
    "/dashboard.js",
    "/dashboard/tailwind.css",
    "/qr.js",
  ]);
  assert.deepEqual(profileProxyHeaders, [
    "forkmesh.com",
    "forkmesh.com",
    null,
    null,
    null,
  ]);
  // Query strings survive, so the ?v= cache-busting stamp reaches App.
  const stamped = await worker.fetch(
    new Request("https://forkmesh.com/dashboard.js?v=a59ef978c5a9"),
    environment({
      APP: {
        async fetch(request) {
          assert.equal(request.url, "https://app.forkmesh.com/dashboard.js?v=a59ef978c5a9");
          return new Response("bundle");
        },
      },
    }),
  );
  assert.equal(stamped.status, 200);
});


test("www profile aliases redirect to the one canonical profile origin", async () => {
  const env = environment({
    APP: {
      async fetch() {
        throw new Error("www profile aliases must redirect before App");
      },
    },
  });
  const response = await worker.fetch(
    new Request("https://www.next.forkmesh.com/@Jett?view=profile"),
    env,
  );
  assert.equal(response.status, 308);
  assert.equal(
    response.headers.get("location"),
    "https://forkmesh.com/@Jett?view=profile",
  );
});


test("dashboard pages still redirect to App", async () => {
  for (const path of ["/dashboard", "/dashboard/profile", "/dashboard/chat"]) {
    const response = await worker.fetch(
      new Request("https://forkmesh.com" + path),
      environment(),
    );
    assert.equal(response.status, 308, path);
    assert.equal(response.headers.get("location"), "https://app.forkmesh.com" + path);
  }
});


test("RSS aliases serve the built WWW asset without touching App", async () => {
  const assetRequests = [];
  const env = environment({
    APP: {
      async fetch() {
        throw new Error("RSS must not reach App");
      },
    },
    ASSETS: {
      async fetch(request) {
        assetRequests.push(request.url);
        return new Response("<rss version=\"2.0\"/>", {
          headers: { "content-type": "application/xml" },
        });
      },
    },
  });
  for (const path of ["/rss.xml", "/feed.xml", "/blog/rss.xml", "/blog/feed.xml"]) {
    const response = await worker.fetch(new Request("https://forkmesh.com" + path), env);
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("location"), null);
    assert.equal(response.headers.get("content-type"), "application/rss+xml; charset=utf-8");
    assert.equal(response.headers.get("cache-control"), "public, max-age=1800");
    assert.equal(response.headers.get("x-forkmesh-worker"), "www");
    assert.equal(await response.text(), "<rss version=\"2.0\"/>");
  }
  assert.deepEqual(assetRequests, Array(4).fill("https://forkmesh.com/rss.xml"));
  const head = await worker.fetch(new Request("https://forkmesh.com/feed.xml", {
    method: "HEAD",
  }), env);
  assert.equal(head.status, 200);
  assert.equal(head.headers.get("content-type"), "application/rss+xml; charset=utf-8");
  const rejected = await worker.fetch(new Request("https://forkmesh.com/rss.xml", {
    method: "POST",
  }), env);
  assert.equal(rejected.status, 405);
  assert.equal(rejected.headers.get("allow"), "GET, HEAD");
});


test("non-read requests that reach the Worker are proxied", async () => {
  const captured = [];
  const env = environment({
    APP: {
      async fetch(request) {
        captured.push([request.url, request.method, await request.text()]);
        return new Response("proxied");
      },
    },
  });
  const response = await worker.fetch(new Request("https://forkmesh.com/pricing", {
    method: "POST",
    body: "payload",
  }), env);
  assert.equal(response.status, 200);
  assert.deepEqual(captured, [[
    "https://app.forkmesh.com/pricing",
    "POST",
    "payload",
  ]]);
});


test("missing marketing pages retain the custom 404", async () => {
  const requested = [];
  const env = environment({
    ASSETS: {
      async fetch(request) {
        const path = new URL(request.url).pathname;
        requested.push(path);
        if (path === "/404") {
          return new Response("<h1>Page not found</h1>", {
            headers: { "content-type": "text/html" },
          });
        }
        return new Response(null, { status: 404 });
      },
    },
  });
  const response = await worker.fetch(
    new Request("https://forkmesh.com/missing"),
    env,
  );
  assert.equal(response.status, 404);
  assert.equal(await response.text(), "<h1>Page not found</h1>");
  assert.equal(response.headers.get("x-forkmesh-worker"), "www");
  assert.deepEqual(requested, ["/missing", "/404"]);
});


test("homepage availability is cached and lists only healthy canonical mirrors", async () => {
  const originalFetch = globalThis.fetch;
  const probes = [];
  const probe = async (request) => {
    const url = request.url;
    probes.push(url);
    if (url === "https://app.forkmesh.com/health") {
      return new Response("rate limited", { status: 429 });
    }
    if (url === "https://n2.next.forkmesh.com/health") {
      return new Response("unavailable", { status: 503 });
    }
    return new Response('{"ok":true}', {
      status: 200,
      headers: { "content-type": "application/json" },
    });
  };
  const env = () => environment({ APP: { fetch: probe } });
  try {
    const rejected = await worker.fetch(new Request(
      "https://forkmesh.com/api/home-availability", { method: "POST" },
    ), env());
    assert.equal(rejected.status, 405);
    assert.equal(probes.length, 0);

    const response = await worker.fetch(new Request(
      "https://forkmesh.com/api/home-availability",
    ), env());
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-forkmesh-worker"), "www");
    const payload = await response.json();
    assert.equal(payload.appOnline, false);
    assert.equal(payload.appStatus, 429);
    assert.ok(payload.expectedReturnAt > payload.checkedAt);
    assert.deepEqual(payload.mirrors, [
      {
        name: "n1",
        url: "https://n1.next.forkmesh.com/forkmesh/forkmesh",
      },
      {
        name: "n3",
        url: "https://n3.next.forkmesh.com/forkmesh/forkmesh",
      },
    ]);
    assert.equal(probes.length, 4);

    const cached = await worker.fetch(new Request(
      "https://forkmesh.com/api/home-availability",
    ), env());
    assert.equal((await cached.json()).appOnline, false);
    assert.equal(probes.length, 4, "the second browser cannot fan out probes");
  } finally {
    globalThis.fetch = originalFetch;
  }
});
