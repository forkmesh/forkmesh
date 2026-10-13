#!/usr/bin/env node
import { createRequire } from "node:module";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PAGE_LIBRARY = path.join(HERE, "world_bench_page.js");
const config = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const root = config.root || path.resolve(HERE, "..");

function loadPlaywright() {
  const bases = [root, ...(config.playwrightFrom || [])];
  try {
    const common = execFileSync("git", ["-C", root, "rev-parse", "--path-format=absolute", "--git-common-dir"], {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "ignore"],
    }).trim();
    if (common) bases.push(path.dirname(common));
  } catch {}
  const anchors = [];
  if (process.env.FORKMESH_PLAYWRIGHT) anchors.push(path.join(process.env.FORKMESH_PLAYWRIGHT, "package.json"));
  for (const base of bases) {
    anchors.push(path.join(base, "app", "browser_tests", "package.json"));
    anchors.push(path.join(base, "world", "browser_tests", "package.json"));
  }
  for (const anchor of anchors) {
    try {
      return createRequire(anchor)("playwright");
    } catch {}
  }
  throw new Error(
    "playwright not found: run npm ci --prefer-offline in app/browser_tests or set FORKMESH_PLAYWRIGHT",
  );
}

const log = (...parts) => process.stderr.write(`world_bench: ${parts.join(" ")}\n`);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function slug(text) {
  return String(text || "unknown-gpu")
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "") || "unknown-gpu";
}

function hashSeed(text) {
  let value = 0x5eed1234;
  for (const char of String(text)) value = (Math.imul(value, 31) + char.charCodeAt(0)) >>> 0;
  return value;
}

function fixtureRandom(seed) {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function buildFixtures(spec, fixedNow) {
  const random = fixtureRandom(hashSeed("world-bench-fixtures"));
  const pick = (list) => list[Math.floor(random() * list.length)];
  const hex = (length) => Array.from({ length }, () => Math.floor(random() * 16).toString(16)).join("");
  const buckets = ["hour", "5h", "24h", "3d", "older"];
  const countries = ["US", "CA", "DE", "FR", "JP", "BR", "IN", "GB", "AU", "NL"];
  const browsers = ["Firefox", "Chrome", "Safari", "Edge"];
  const systems = ["Linux", "macOS", "Windows", "iOS", "Android"];
  const mirrors = Array.from({ length: spec.mirrorNodes }, (_, index) => `mirror${index + 1}`);
  const users = Array.from({ length: spec.members }, (_, index) => ({
    name: `member-${String(index).padStart(3, "0")}`,
    createdAt: fixedNow - (index + 1) * 86_400_000,
    nodes: index % 9 === 0 ? [mirrors[index % mirrors.length]] : [],
    totalActiveMs: Math.floor(random() * 50) * 3_600_000,
    totalSteps: Math.floor(random() * 90_000),
    officeVisits: Math.floor(random() * 40),
    issues: Math.floor(random() * 30),
    pulls: Math.floor(random() * 30),
    commits: Math.floor(random() * 400),
    discussions: Math.floor(random() * 10),
    countryCode: pick(countries),
    browser: pick(browsers),
    os: pick(systems),
    activityBucket: buckets[index % buckets.length],
    visitCount: 1 + Math.floor(random() * 90),
  }));
  const people = Array.from({ length: spec.avatars }, (_, index) => ({
    id: `idle${String(index).padStart(4, "0")}`,
    name: `idler-${String(index).padStart(2, "0")}`,
    accountStatus: "Registered",
    nodeCount: index % 4,
    availability: "inactive",
    lastActive: "Last active recently",
    activityBucket: buckets[index % buckets.length],
  }));
  const languages = ["Python", "JavaScript", "C++", "Go", "Rust", "Dart", "TypeScript", "Kotlin"];
  const repositories = Array.from({ length: spec.repositories }, (_, index) => ({
    owner: index === 0 ? "forkmesh" : `owner${index % 8}`,
    name: index === 0 ? "forkmesh" : `repo-${index}`,
    description: `Benchmark repository ${index}`,
    source: "forkmesh",
    liveHost: index % 3 !== 0,
    language: languages[index % languages.length],
    commit: hex(40),
    pullCount: Math.floor(random() * 40),
    starCount: Math.floor(random() * 500),
    sizeBytes: 100_000 + Math.floor(random() * 90_000_000),
    updatedAt: fixedNow - index * 3_600_000,
  }));
  const mirrorRecords = mirrors.map((node, index) => ({
    node,
    status: index % 5 === 4 ? "offline" : "online",
    integrity: "ok",
    cloneAvailable: true,
    commit: "a".repeat(40),
    branch: "main",
    sizeBytes: 75_000_000 + index * 1_000_000,
    issueCount: 47,
    commitCount: 8661,
    branchCount: 18,
    pullCount: 42,
    discussionCount: 2,
    artifactCount: 1,
    platform: "linux",
    version: "0.9.0",
    cpuPercent: 10 + index * 5,
    memUsedBytes: 536_870_912,
    memTotalBytes: 1_073_741_824,
    diskUsedBytes: 5_368_709_120,
    diskTotalBytes: 10_737_418_240,
  }));
  return {
    summary: {
      members: users.length,
      avatars: people.length,
      repositories: repositories.length,
      mirrorNodes: mirrors.length,
    },
    api: {
      "/api/world/context": {
        now: fixedNow,
        countryCode: "",
        worldConnections: 64,
        worldMessagesPerSecond: 4,
        chatConnections: 128,
      },
      "/api/accounts/users": { ok: true, users },
      "/api/world/inactive": { ok: true, people },
      "/api/world/visitors": {
        ok: true,
        total: 48_213,
        today: 1_204,
        yesterdaySameTime: 1_117,
        pastHour: 86,
        pastHourYesterday: 71,
      },
      "/api/repositories": { ok: true, repositories },
      "/api/repository-imports": { ok: true, repositories: [] },
      "/api/network/overview": {
        ok: true,
        stats: {
          repos: repositories.length,
          hosts: mirrors.length,
          clients: 0,
          onlineNodes: mirrorRecords.filter((m) => m.status === "online").map((m) => m.node),
        },
        leaderboards: {
          nodes: mirrors.map((name, index) => ({ name, pullCount: 5 + index, issueCount: 7 + index })),
        },
        history: {},
      },
      "/api/repo/forkmesh/forkmesh/mirrors": {
        ok: true,
        owner: "forkmesh",
        repo: "forkmesh",
        mirrors: mirrorRecords,
      },
      "/api/world/ticket": { ok: true, authenticated: false, accountStatus: "Guest", nodeCount: 0, ticket: "" },
      "/api/world/events": { events: [] },
      "/api/notifications": { ok: true, notifications: [], unread: 0 },
    },
  };
}


function writeDataUrl(file, dataUrl) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, Buffer.from(dataUrl.split(",")[1], "base64"));
}

function archiveGolden(file) {
  if (!fs.existsSync(file)) return "";
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  const target = path.join(path.dirname(file), "archive", stamp, path.basename(file));
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.copyFileSync(file, target);
  return target;
}

async function routeFixtures(page, origin, fixtures) {
  await page.routeWebSocket(/\/api\/.*\/ws/, () => {});
  const staticRoots = ["world/public", "www/public", "app/public"].map((dir) => path.join(root, dir));
  const blanks = new Set(["/analytics.js"]);
  await page.route("**/*", async (route) => {
    const url = new URL(route.request().url());
    if (url.origin !== origin) return route.abort();
    if (url.pathname.startsWith("/api/")) {
      const body = fixtures.api[url.pathname];
      if (route.request().method() !== "GET" || !body) {
        return route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true }) });
      }
      return route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify(body) });
    }
    if (url.pathname.startsWith("/world/") || url.pathname === "/world" || url.pathname.startsWith("/marketplace/")) {
      return route.continue();
    }
    if (blanks.has(url.pathname)) {
      return route.fulfill({ status: 200, contentType: "text/javascript", body: "" });
    }
    for (const base of staticRoots) {
      const file = path.join(base, decodeURIComponent(url.pathname));
      if (file.startsWith(base + path.sep) && fs.existsSync(file) && fs.statSync(file).isFile()) {
        return route.fulfill({ path: file });
      }
    }
    return route.fulfill({ status: 404, body: "" });
  });
}


async function settleLive(page, { stableSamples, intervalMs, capMs }) {
  const history = [];
  const started = Date.now();
  let stable = 0;
  let last = "";
  while (Date.now() - started < capMs) {
    await sleep(intervalMs);
    const signature = await page.evaluate(() => window.__worldBench.signature());
    history.push(signature);
    stable = signature && signature === last ? stable + 1 : 0;
    last = signature;
    if (stable >= stableSamples) return { ms: Date.now() - started, converged: true, history: history.slice(-4) };
  }
  return { ms: Date.now() - started, converged: false, history: history.slice(-4) };
}

async function loadWorld(page, settings) {
  const started = Date.now();
  await page.goto(`${settings.origin}/world/`, { waitUntil: "domcontentloaded", timeout: settings.loadTimeoutMs });
  await page.waitForFunction(
    () => {
      const world = window.__worldBench.world();
      const boot = window.__forkmeshWorldBoot;
      return Boolean(world?.renderer && boot?.measure?.interactiveFrameMs && boot.settled);
    },
    null,
    { timeout: settings.loadTimeoutMs, polling: 100 },
  );
  const load = await page.evaluate(() => window.__worldBench.load());
  load.wallMs = Date.now() - started;
  return load;
}

async function goldenResult(page, pose, settings, report, shot) {
  const goldenFile = path.join(settings.goldenRoot, slug(report.gpu), `${pose.name}.png`);
  if (settings.bless) {
    const archived = archiveGolden(goldenFile);
    writeDataUrl(goldenFile, shot.golden);
    return { baseline: goldenFile, blessed: true, archived };
  }
  if (!fs.existsSync(goldenFile)) return { baseline: goldenFile, missing: true };
  const baseline = `data:image/png;base64,${fs.readFileSync(goldenFile).toString("base64")}`;
  const compared = await page.evaluate(
    ([actual, base, tolerance]) => window.__worldBench.compare(actual, base, tolerance),
    [shot.golden, baseline, settings.golden.tolerance],
  );
  const diffFile = path.join(settings.shotsDir, `${pose.name}.diff.png`);
  if (compared.diff) writeDataUrl(diffFile, compared.diff);
  return {
    baseline: goldenFile,
    changedPct: compared.changedPct,
    maxDelta: compared.maxDelta,
    sameSize: compared.sameSize,
    diff: compared.diff ? diffFile : "",
  };
}

async function runPose(browser, pose, settings, fixtures, report) {
  const context = await browser.newContext({
    viewport: settings.viewport,
    deviceScaleFactor: 1,
    timezoneId: "UTC",
    locale: "en-US",
    serviceWorkers: "block",
  });
  const page = await context.newPage();
  const errors = [];
  page.on("pageerror", (error) => errors.push(String(error.message || error).slice(0, 300)));
  await page.addInitScript((value) => {
    window.__worldBenchConfig = value;
  }, {
    fixedNow: settings.fixedNow,
    seed: hashSeed(`world-bench:${pose.name}`),
    guestId: "world-bench-visitor",
  });
  await page.addInitScript({ path: PAGE_LIBRARY });
  await routeFixtures(page, settings.origin, fixtures);
  const result = { errors };
  try {
    result.load = await loadWorld(page, settings);
    report.gpu = report.gpu || result.load.gpu;
    result.placed = await page.evaluate((pose) => window.__worldBench.place(pose), pose);
    result.settle = await settleLive(page, settings.settle);
    const shot = await page.evaluate(
      ([pose, settings]) => window.__worldBench.capture(pose, settings),
      [pose, settings],
    );
    Object.assign(result, {
      masked: shot.masked,
      settledFrames: shot.settledFrames,
      position: shot.position,
      counts: shot.counts,
      audit: shot.audit,
    });
    const shotFile = path.join(settings.shotsDir, `${pose.name}.png`);
    writeDataUrl(shotFile, shot.golden);
    if (shot.full) writeDataUrl(path.join(settings.shotsDir, `${pose.name}.full.png`), shot.full);
    result.shot = shotFile;
    result.golden = await goldenResult(page, pose, settings, report, shot);
    if (settings.probe) {
      const source = fs.readFileSync(settings.probe, "utf8");
      result.probe = await page.evaluate((source) => new Function(`return (async () => {${source}})()`)(), source);
    }
    const startedAt = await page.evaluate(() => window.__worldBench.startRecording());
    await sleep(settings.measureSeconds * 1000);
    result.live = await page.evaluate((at) => window.__worldBench.stopRecording(at), startedAt);
  } catch (error) {
    result.failed = String(error?.stack || error).slice(0, 800);
  } finally {
    await context.close();
  }
  return result;
}

async function main() {
  const { chromium } = loadPlaywright();
  const started = Date.now();
  const fixtures = buildFixtures(config.fixtures, config.fixedNow);
  const browser = await chromium.launch({
    headless: true,
    args: config.software
      ? ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"]
      : ["--use-gl=angle", "--use-angle=gl-egl", "--ignore-gpu-blocklist"],
  });
  const report = {
    tool: "world_bench",
    version: 1,
    startedAt: new Date(started).toISOString(),
    commit: config.commit || "",
    viewport: config.viewport,
    golden: config.golden,
    fixtures: fixtures.summary,
    software: Boolean(config.software),
    gpu: "",
    goldenKey: "",
    poses: {},
  };
  try {
    for (const pose of config.poses) {
      const poseStarted = Date.now();
      const result = await runPose(browser, pose, config, fixtures, report);
      report.poses[pose.name] = result;
      const counts = result.counts || {};
      log(
        `${pose.name}: ${result.failed ? "FAILED" : "ok"} in ${((Date.now() - poseStarted) / 1000).toFixed(1)}s`,
        `draws=${counts.draws ?? "-"} tris=${counts.triangles ?? "-"}`,
        `render p50/p90=${result.live?.renderMsP50 ?? "-"}/${result.live?.renderMsP90 ?? "-"}ms`,
        result.golden?.changedPct !== undefined ? `golden=${result.golden.changedPct}%` : "",
      );
      if (result.failed) log(result.failed.split("\n")[0]);
    }
  } finally {
    await browser.close();
  }
  report.software = report.software || /swiftshader|llvmpipe|softpipe|software/i.test(report.gpu);
  report.goldenKey = slug(report.gpu);
  report.wallSeconds = Math.round((Date.now() - started) / 100) / 10;
  fs.mkdirSync(path.dirname(config.out), { recursive: true });
  fs.writeFileSync(config.out, JSON.stringify(report, null, 1));
}

main().catch((error) => {
  process.stderr.write(`world_bench: ${error?.stack || error}\n`);
  process.exit(2);
});
