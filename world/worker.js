const APP_ASSET_PATHS = new Set([
  "/account-events.js",
  "/api-client.js",
  "/chat-apps.js",
  "/chat-attachments.js",
  "/chat-crypto.js",
  "/chat-frame-auth.js",
  "/chat-issue-filing.js",
  "/chat-mlkem.js",
  "/chat-moderation.js",
  "/chat-retention.js",
  "/chat-room-transport.js",
  "/chat-seal.js",
  "/chat-call-facestream.js",
  "/fmm/cc.js",
  "/fmm/client.js",
  "/fmm/fec.js",
  "/fmm/fmm-playout-worklet.js",
  "/fmm/fmm-seal.js",
  "/fmm/fmm-worker.js",
  "/fmm/frag.js",
  "/fmm/hop.js",
  "/fmm/jitter.js",
  "/fmm/seqnum.js",
  "/fmm/wire.js",
  "/call-avatar-puppet.js",
  "/call-avatar-rig-wire.js",
  "/call-avatar-rig.js",
  "/call-quality-meter.js",
  "/call-speech-meter.js",
  "/chat-call-backgrounds.js",
  "/chat-call-e2ee.js",
  "/chat-call-face-custom.js",
  "/chat-call-face-expression.js",
  "/chat-call-face-geometry.js",
  "/chat-call-face-mesh.js",
  "/chat-call-face-render.js",
  "/chat-call-face-template.js",
  "/chat-call-face-tracker.js",
  "/chat-call-faces.js",
  "/chat-call-hand-tracker.js",
  "/chat-call-outfits.js",
  "/chat-call-segment.js",
  "/chat-call-voice-worklet.js",
  "/chat-call-voice.js",
  "/chat-group-call-crypto.js",
  "/chat-group-calls.js",
  "/fmm/fmm-call.js",
  "/fmm/fmm-e2ee.js",
  "/fmm/fmm-local.js",
  "/dashboard-chat.js",
  "/member-welcome.js",
  "/membership-prompt.js",
  "/analytics.js",
  "/safe-input.js",
  "/analytics-sdk.js",
  "/start",
]);

const MOBILE_WORLD_RUNTIME_ASSETS = Object.freeze([
  {
    path: "/world/index.html",
    contentType: "text/html",
    minimumBytes: 3_000,
    maximumBytes: 64 * 1024,
  },
  {
    path: "/world/world.js",
    contentType: "javascript",
    minimumBytes: 512 * 1024,
    maximumBytes: 2 * 1024 * 1024,
  },
  {
    path: "/world/world-scene.js",
    contentType: "javascript",
    minimumBytes: 1024,
    maximumBytes: 4 * 1024 * 1024,
  },
  {
    path: "/world/vendor/three-0.184.0/three.module.min.js",
    contentType: "javascript",
    minimumBytes: 256 * 1024,
    maximumBytes: 512 * 1024,
  },
  {
    path: "/world/vendor/three-0.184.0/three.core.min.js",
    contentType: "javascript",
    minimumBytes: 256 * 1024,
    maximumBytes: 512 * 1024,
  },
]);

function isAppAsset(pathname) {
  return APP_ASSET_PATHS.has(pathname) ||
    pathname.startsWith("/assets/") || pathname.startsWith("/favicon/");
}

function appAsset(request, env) {
  const source = new URL(request.url);
  const destination = new URL(env.APP_ORIGIN);
  destination.pathname = source.pathname;
  destination.search = source.search;
  return env.APP.fetch(new Request(destination, request));
}

function htmlAttribute(value) {
  return String(value || "").replace(/[&<>"']/g, (character) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;",
  })[character]);
}

async function asset(request, env, pathname) {
  const url = new URL(request.url);
  const assetPath = pathname || url.pathname;
  url.pathname = assetPath;
  const response = await env.ASSETS.fetch(new Request(url, request));
  const headers = new Headers(response.headers);
  headers.set("x-forkmesh-worker", "world");
  headers.set("x-forkmesh-deploy-fingerprint", String(env.DEPLOY_FINGERPRINT || ""));
  const result = new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
  if (assetPath === "/world/index.html" && response.ok) {
    const origin = htmlAttribute(env.APP_ORIGIN);
    const fingerprint = htmlAttribute(env.DEPLOY_FINGERPRINT);
    return new HTMLRewriter().on("head", {
      element(element) {
        // The App and World deploy independently. Give the page the identity
        // of the static World bundle it actually loaded so its update watcher
        // never compares fresh World assets with an older App Worker revision.
        element.prepend(
          `<meta name="forkmesh-api-origin" content="${origin}">` +
            `<meta name="forkmesh-world-deploy" content="${fingerprint}">`,
          { html: true },
        );
      },
    }).transform(result);
  }
  return result;
}

async function checkMobileWorldAsset(request, env, contract) {
  try {
    const url = new URL(contract.path, request.url);
    const response = await env.ASSETS.fetch(new Request(url, {
      method: "HEAD",
      headers: { "cache-control": "no-cache" },
    }));
    const status = Number(response.status || 0);
    if (status !== 200) {
      return {
        path: contract.path,
        ok: false,
        status,
        reason: `${contract.path} returned HTTP ${status}`,
      };
    }
    const contentType = String(
      response.headers.get("content-type") || "",
    ).toLowerCase();
    if (!contentType.includes(contract.contentType)) {
      return {
        path: contract.path,
        ok: false,
        status,
        reason: `${contract.path} returned an invalid content type`,
      };
    }
    const announcedBytes = Number(
      response.headers.get("content-length") || 0,
    );
    if (
      announcedBytes > 0 &&
      (
        announcedBytes < contract.minimumBytes ||
        announcedBytes > contract.maximumBytes
      )
    ) {
      return {
        path: contract.path,
        ok: false,
        status,
        bytes: announcedBytes,
        reason: `${contract.path} returned an unexpected byte length`,
      };
    }
    return {
      path: contract.path,
      ok: true,
      status,
      bytes: announcedBytes || null,
    };
  } catch (error) {
    return {
      path: contract.path,
      ok: false,
      status: 0,
      reason: `${contract.path} could not be loaded: ${String(error).slice(0, 120)}`,
    };
  }
}

async function mobileWorldHealth(request, env) {
  if (request.method !== "GET" && request.method !== "HEAD") {
    return new Response("Method not allowed", {
      status: 405,
      headers: { allow: "GET, HEAD" },
    });
  }
  const assets = await Promise.all(
    MOBILE_WORLD_RUNTIME_ASSETS.map((contract) =>
      checkMobileWorldAsset(request, env, contract)
    ),
  );
  const failure = assets.find((item) => !item.ok);
  const payload = {
    ok: !failure,
    check: "mobile-world-startup",
    profile: "mobile",
    sameOriginRuntime: true,
    threeRevision: "184",
    assets,
    reason: failure?.reason || null,
  };
  const headers = new Headers({
    "cache-control": "no-store",
    "content-type": "application/json; charset=utf-8",
    "x-forkmesh-worker": "world",
    "x-forkmesh-deploy-fingerprint": String(env.DEPLOY_FINGERPRINT || ""),
  });
  return new Response(
    request.method === "HEAD" ? null : JSON.stringify(payload),
    { status: failure ? 503 : 200, headers },
  );
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;

    if (path === "/health/mobile") {
      return mobileWorldHealth(request, env);
    }
    if (path === "/" || path === "/world" || path === "/world/") {
      return asset(request, env, "/world/index.html");
    }
    if (isAppAsset(path)) {
      return appAsset(request, env);
    }
    return env.APP.fetch(request);
  },
};
