// The supported device matrix, measured rather than asserted from the source.
// Each tier boots the real World pinned to its own quality profile, under the
// viewport, pixel density and CPU throttle that stands in for that class of
// hardware, and records what a visitor would actually get: frames per second,
// movement input response, pointer gaps, and whether the world is all still
// there. The last test lets detection be wrong on purpose and watches the
// adaptive ladder step down on its own.
const { test, expect } = require("@playwright/test");

// The performance matrix must not depend on a public CDN, a live World
// socket, or whatever production data happens to be available during a run.
// The staged World now loads its pinned Three.js module from its own origin;
// these fixtures only keep API and socket I/O out of the measurements.
const FIXED_NOW = 1_785_000_000_000;

// CPU throttling is the only knob Chromium gives us that a headless machine
// cannot cheat: it slows the main thread by the given factor, which is what
// separates these classes in practice once resolution is accounted for.
const MATRIX = [
  {
    tier: "desktop-high",
    label: "High-end desktop",
    viewport: { width: 1600, height: 900 },
    deviceScaleFactor: 1,
    hasTouch: false,
    cpuThrottle: 1,
    minFps: 40,
    maxInputMs: 80,
  },
  {
    tier: "laptop-mid",
    label: "Mid-tier laptop",
    viewport: { width: 1440, height: 900 },
    deviceScaleFactor: 2,
    hasTouch: false,
    cpuThrottle: 2,
    minFps: 24,
    maxInputMs: 120,
  },
  {
    tier: "mobile-mid",
    label: "Mid-tier mobile",
    viewport: { width: 412, height: 915 },
    deviceScaleFactor: 2.6,
    hasTouch: true,
    cpuThrottle: 4,
    minFps: 15,
    maxInputMs: 200,
  },
  {
    tier: "mobile-low",
    label: "Low-tier mobile",
    viewport: { width: 360, height: 740 },
    deviceScaleFactor: 3,
    hasTouch: true,
    cpuThrottle: 6,
    minFps: 10,
    maxInputMs: 320,
  },
];

const SAMPLE_MS = 3_000;
const recorded = [];
const SOFTWARE_RENDERER = /swiftshader|llvmpipe|softpipe|software|microsoft basic/i;

// Booting, warming up and sampling four device classes under throttling does
// not fit one 30 s test slot.
// The cross-tier comparison consumes the four readings collected before it.
// A serial suite is retried as one unit, so a Playwright worker restart cannot
// leave that comparison with an empty in-memory matrix.
test.describe.configure({ mode: "serial", timeout: 120_000 });

async function prepareTierPage(page, identity) {
  await page.addInitScript(({ now, guestId }) => {
    Date.now = () => now;
    sessionStorage.setItem("forkmesh.world.guestId.v1", guestId);
  }, { now: FIXED_NOW, guestId: `device-tier-${identity}` });
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    const body =
      url.pathname === "/api/world/context"
        ? {
            now: FIXED_NOW,
            countryCode: "",
            worldConnections: 0,
            worldMessagesPerSecond: 0,
            chatConnections: 0,
          }
        : url.pathname === "/api/network/overview"
          ? {
              ok: true,
              stats: { repos: 0, hosts: 0, clients: 0, onlineNodes: [] },
              leaderboards: { nodes: [] },
              history: {},
            }
          : url.pathname === "/api/repo/forkmesh/forkmesh/mirrors"
            ? { ok: true, owner: "forkmesh", repo: "forkmesh", mirrors: [] }
            : url.pathname === "/api/world/instances"
              ? { instances: [] }
              : url.pathname === "/api/world/events"
                ? { events: [] }
                : url.pathname === "/api/world/fediverse"
                  ? { mastodon: [], lemmy: [], x: [], reddit: [] }
                  : url.pathname === "/api/world/media/spaces"
                    ? { spaces: [] }
                    : url.pathname === "/api/accounts/users"
                      ? { ok: true, users: [] }
                      : url.pathname === "/api/repositories"
                        ? { repositories: [] }
                        : url.pathname === "/api/version"
                          ? { ok: true, version: "test", rev: "0".repeat(40), now: FIXED_NOW }
                          : { ok: true };
    return route.fulfill({
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.routeWebSocket("**/api/world/ws*", (socket) => {
    socket.send(JSON.stringify({ type: "welcome", id: identity, peers: [] }));
  });
}

async function bootPinnedWorld(browser, entry) {
  const context = await browser.newContext({
    viewport: entry.viewport,
    deviceScaleFactor: entry.deviceScaleFactor,
    hasTouch: entry.hasTouch,
    isMobile: false,
  });
  const page = await context.newPage();
  await prepareTierPage(page, entry.tier);
  const cdp = await context.newCDPSession(page);
  if (entry.cpuThrottle > 1) {
    await cdp.send("Emulation.setCPUThrottlingRate", {
      rate: entry.cpuThrottle,
    });
  }
  // adaptive=0 pins the tier so the reading describes that tier rather than
  // wherever the ladder would have wandered on this machine.
  await page.goto(`/world/?nointro=1&quality=${entry.tier}&adaptive=0`);
  await page.waitForFunction(
    () => {
      const shell = document.querySelector("forkmesh-world");
      return (
        Boolean(shell?.world?.renderer?.domElement) &&
        Number(shell.world.renderer.info?.render?.frame || 0) > 0
      );
    },
    undefined,
    { timeout: 60_000 },
  );
  return { context, page, cdp };
}

async function measure(page) {
  // Warm up first: shader compilation and the first plate uploads happen once
  // and are not what a visitor experiences while walking around.
  await page.waitForTimeout(1_500);
  await page.evaluate(() => {
    const shell = document.querySelector("forkmesh-world");
    const { world } = shell;
    // The shell's one-second dashboard sampler also consumes renderer
    // diagnostics. Stop it for this three-second measurement so FPS and input
    // readings describe one complete driven window instead of its final
    // fraction.
    window.clearInterval(shell.diagnosticsTimer);
    shell.diagnosticsTimer = 0;
    // Reading the diagnostics clears the window, so this call starts a fresh
    // one for the sample below.
    world.getDiagnostics();
  });
  // Hold a movement key and drag the camera for the whole sample window, so
  // the frame rate is measured while the world is being used, not idling.
  await page.keyboard.down("KeyW");
  const box = await page.locator("canvas.world-canvas").boundingBox();
  if (!box) throw new Error("World canvas did not receive a layout box");
  const center = { x: box.x + box.width / 2, y: box.y + box.height / 2 };
  await page.mouse.move(center.x, center.y);
  await page.mouse.down();
  const started = Date.now();
  let step = 0;
  while (Date.now() - started < SAMPLE_MS) {
    step += 1;
    await page.mouse.move(
      center.x + Math.sin(step / 3) * (box.width / 6),
      center.y + Math.cos(step / 5) * (box.height / 8),
    );
    await page.waitForTimeout(60);
  }
  await page.mouse.up();
  await page.keyboard.up("KeyW");
  return page.evaluate(() => {
    const { world } = document.querySelector("forkmesh-world");
    const diagnostics = world.getDiagnostics();
    const quality = world.getQualityState();
    const canvas = world.renderer.domElement;
    const rect = canvas.getBoundingClientRect();
    const elements = world.listWorldElements();
    return {
      fps: diagnostics.fps,
      frameTimeMs: diagnostics.frameTimeMs,
      frameTimeP95Ms: diagnostics.frameTimeP95Ms,
      cpuFrameMs: diagnostics.cpuFrameMs,
      longestFrameMs: diagnostics.longestFrameMs,
      inputResponseMs: diagnostics.inputResponseMs,
      worstInputResponseMs: diagnostics.worstInputResponseMs,
      pointerWorstGapMs: diagnostics.pointerWorstGapMs,
      pointerMoves: diagnostics.pointerMoves,
      drawCalls: diagnostics.rendererCalls,
      triangles: diagnostics.rendererTriangles,
      pixelRatio: diagnostics.pixelRatio,
      shadowsEnabled: diagnostics.shadowsEnabled,
      drawingBuffer: [
        diagnostics.output.drawingBufferWidth,
        diagnostics.output.drawingBufferHeight,
      ],
      cssSize: [diagnostics.output.cssWidth, diagnostics.output.cssHeight],
      antialias: diagnostics.output.antialias,
      gpu: diagnostics.output.gpu || "withheld",
      canvasTextureScale: diagnostics.output.canvasTextureScale,
      quality,
      // "Fully visible" has to mean the world is really there, not that a
      // canvas exists: the element registry, the drawn geometry and the
      // canvas box all have to agree.
      visible: {
        canvasWidth: Math.round(rect.width),
        canvasHeight: Math.round(rect.height),
        canvasVisible: getComputedStyle(canvas).visibility !== "hidden",
        elements: elements.length,
        enabledElements: elements.filter((element) => element.enabled).length,
        drawnElements: elements.filter((element) => element.drawables > 0).length,
      },
    };
  });
}

for (const entry of MATRIX) {
  test(`${entry.label} keeps the whole world visible and responsive`, async ({
    browser,
  }, testInfo) => {
    const { context, page } = await bootPinnedWorld(browser, entry);
    try {
      const reading = await measure(page);
      recorded.push({ ...entry, reading });
      await testInfo.attach(`device-tier-${entry.tier}.json`, {
        body: JSON.stringify({ device: entry, reading }, null, 2),
        contentType: "application/json",
      });
      console.log(
        `[device-tier] ${entry.tier} ` +
          `${reading.fps.toFixed(1)} fps · ` +
          `frame ${reading.frameTimeMs.toFixed(1)} ms (p95 ${reading.frameTimeP95Ms.toFixed(1)}) · ` +
          `input ${reading.worstInputResponseMs.toFixed(1)} ms worst · ` +
          `pointer gap ${reading.pointerWorstGapMs.toFixed(1)} ms · ` +
          `buffer ${reading.drawingBuffer.join("x")} @ DPR ${reading.pixelRatio.toFixed(2)} · ` +
          `${reading.drawCalls} draws / ${reading.triangles} triangles · ` +
          `shadows ${reading.shadowsEnabled ? "on" : "off"} · ` +
          `GPU ${reading.gpu}`,
      );

      // The tier the URL pinned is the tier that ran.
      expect(reading.quality.tier).toBe(entry.tier);
      expect(reading.quality.adaptive).toBe(false);
      expect(reading.quality.changes).toHaveLength(0);

      // Nothing was removed from the world to reach that frame rate.
      expect(reading.visible.canvasWidth).toBeGreaterThan(
        entry.viewport.width * 0.5,
      );
      expect(reading.visible.canvasHeight).toBeGreaterThan(200);
      expect(reading.visible.canvasVisible).toBe(true);
      expect(reading.visible.drawnElements).toBeGreaterThan(20);
      expect(reading.drawCalls).toBeGreaterThan(20);
      expect(reading.triangles).toBeGreaterThan(5_000);
      expect(reading.drawingBuffer[0]).toBeGreaterThan(0);
      expect(reading.drawingBuffer[1]).toBeGreaterThan(0);

      // Resolution is capped by the tier, not by the panel the device has.
      expect(reading.pixelRatio).toBeLessThanOrEqual(
        reading.quality.pixelRatioCap + 0.001,
      );
      expect(
        reading.drawingBuffer[0] * reading.drawingBuffer[1],
      ).toBeLessThanOrEqual(reading.quality.pixelBudget * 1.05);

      // Frame rate and input response while the world is being driven.
      // Headless CI normally rasterises with SwiftShader, which is not a
      // high-end desktop substitute. Keep its reading in the artifact, but
      // reserve the calibrated per-tier floors for a hardware-backed run;
      // otherwise a host GPU change would turn evidence collection into a
      // false product regression.
      const softwareFallback = SOFTWARE_RENDERER.test(reading.gpu);
      const minFps = softwareFallback ? 4 : entry.minFps;
      const maxInputMs = softwareFallback
        ? Math.max(1_000, entry.maxInputMs)
        : entry.maxInputMs;
      expect(reading.fps).toBeGreaterThanOrEqual(minFps);
      expect(reading.worstInputResponseMs).toBeLessThanOrEqual(
        maxInputMs,
      );
      expect(reading.pointerMoves).toBeGreaterThan(0);
    } finally {
      await context.close();
    }
  });
}

test("the weaker tiers really do cost less than the stronger ones", async () => {
  // Runs after the four cases above (workers: 1, fullyParallel: false), so it
  // compares what they measured rather than re-booting anything.
  expect(recorded).toHaveLength(MATRIX.length);
  const by = (tier) => recorded.find((entry) => entry.tier === tier).reading;
  const desktop = by("desktop-high");
  const laptop = by("laptop-mid");
  const mid = by("mobile-mid");
  const low = by("mobile-low");

  // Shadows and multisampling are the two whole-frame costs the phone tiers
  // give up; both survive on the pointer-fine tiers.
  expect(desktop.shadowsEnabled).toBe(true);
  expect(laptop.shadowsEnabled).toBe(true);
  expect(mid.shadowsEnabled).toBe(false);
  expect(low.shadowsEnabled).toBe(false);
  expect(desktop.antialias).toBe(true);

  // Plate resolution, decorative populations and the local light budget fall
  // monotonically down the table.
  for (const knob of [
    "canvasTextureScale",
    "pointLightBudget",
    "weatherParticles",
    "starCount",
    "maxSatellites",
  ]) {
    const values = [desktop, laptop, mid, low].map((reading) =>
      knob === "canvasTextureScale"
        ? reading.canvasTextureScale
        : reading.quality[knob],
    );
    expect(values, knob).toEqual([...values].sort((a, b) => b - a));
    expect(values[3], knob).toBeLessThan(values[0]);
  }

  // The low tier shades well under one device pixel per CSS pixel; that is the
  // single largest lever available on that hardware.
  expect(low.pixelRatio).toBeLessThan(1);
  expect(low.drawingBuffer[0] * low.drawingBuffer[1]).toBeLessThan(
    desktop.drawingBuffer[0] * desktop.drawingBuffer[1],
  );
});

test("a device slower than detection expected is stepped down automatically", async ({
  browser,
}, testInfo) => {
  // Detection is deliberately overruled to the strongest tier on a main thread
  // throttled far below what it describes: exactly the case the ladder exists
  // for, and the one QA cannot reproduce by asking for a weaker tier.
  const context = await browser.newContext({
    viewport: { width: 900, height: 620 },
    deviceScaleFactor: 2,
    hasTouch: false,
  });
  const page = await context.newPage();
  await prepareTierPage(page, "adaptive-stepdown");
  const cdp = await context.newCDPSession(page);
  await cdp.send("Emulation.setCPUThrottlingRate", { rate: 12 });
  try {
    await page.goto("/world/?nointro=1&quality=desktop-high");
    await page.waitForFunction(
      () => {
        const shell = document.querySelector("forkmesh-world");
        return Number(shell?.world?.renderer?.info?.render?.frame || 0) > 0;
      },
      undefined,
      { timeout: 60_000 },
    );
    const booted = await page.evaluate(() =>
      document.querySelector("forkmesh-world").world.getQualityState(),
    );
    expect(booted.tier).toBe("desktop-high");
    expect(booted.adaptive).toBe(true);

    // Two sample windows plus the settle delay is the earliest a step can
    // land; allow generously more than that before calling it a failure.
    await page.waitForFunction(
      () =>
        document.querySelector("forkmesh-world").world.getQualityState().steps >
        0,
      undefined,
      { timeout: 60_000 },
    );
    const stepped = await page.evaluate(() => {
      const { world } = document.querySelector("forkmesh-world");
      const quality = world.getQualityState();
      const diagnostics = world.getDiagnostics();
      return {
        quality,
        pixelRatio: diagnostics.pixelRatio,
        drawingBuffer: [
          diagnostics.output.drawingBufferWidth,
          diagnostics.output.drawingBufferHeight,
        ],
        shadowsEnabled: diagnostics.shadowsEnabled,
        drawnElements: world
          .listWorldElements()
          .filter((element) => element.drawables > 0).length,
      };
    });
    await testInfo.attach("device-tier-adaptive-stepdown.json", {
      body: JSON.stringify(stepped, null, 2),
      contentType: "application/json",
    });
    console.log(
      `[device-tier] adaptive step ${stepped.quality.changes
        .map((change) => `${change.from}→${change.to} (${change.reason} @ ${change.fps} fps)`)
        .join(", ")} · buffer ${stepped.drawingBuffer.join("x")} @ DPR ${stepped.pixelRatio.toFixed(2)}`,
    );
    expect(stepped.quality.tier).not.toBe("desktop-high");
    expect(stepped.quality.bootTier).toBe("desktop-high");
    expect(stepped.quality.changes.length).toBeGreaterThan(0);
    expect(stepped.quality.changes[0].reason).toBe("frame-rate");
    // Stepping down lowers cost without emptying the world.
    expect(stepped.drawnElements).toBeGreaterThan(20);
    expect(stepped.pixelRatio).toBeLessThanOrEqual(
      stepped.quality.pixelRatioCap + 0.001,
    );
  } finally {
    await context.close();
  }
});
