const { test, expect } = require("@playwright/test");
const path = require("node:path");

const REPO_PAGE_PATH = path.resolve(__dirname, "..", "..", "public", "dashboard", "repo.html");
const ACTIVE_REF = "feature/docs";
const COMMIT = "0123456789abcdef0123456789abcdef01234567";
// engine.js, render.js, guide.md, README.md, plus util.js once "… more" expands.
const LOADED_TEXT_FILES = 5;
const SOURCE = [
  "// engine entry point",
  "export function start(options) {",
  '  const label = "engine";',
  "  return boot(label, options, 42);",
  "}",
  "",
].join("\n");

// A 2x2 opaque red PNG: the image tile's preview, fetched raw.
const RED_PNG = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8AARAwQCgAf7gP9i18U1AAAAABJRU5ErkJggg==",
  "base64");
// A 2 s, 32x24 solid green VP8 video: the video tile's thumbnail source.
const GREEN_WEBM = Buffer.from("GkXfo59ChoEBQveBAULygQRC84EIQoKEd2VibUKHgQJChYECGFOAZwEAAAAAAAO3EU2bdLpNu4tTq4QVSalmU6yBoU27i1OrhBZUrmtTrIHWTbuMU6uEElTDZ1OsggEjTbuMU6uEHFO7a1OsggOh7AEAAAAAAABZAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAVSalmsCrXsYMPQkBNgIxMYXZmNjEuNy4xMDNXQYxMYXZmNjEuNy4xMDNEiYhAn0AAAAAAABZUrmvIrgEAAAAAAAA/14EBc8WI21RTMCYhB4qcgQAitZyDdW5kiIEAhoVWX1ZQOIOBASPjg4QF9eEA4JCwgSC6gRiagQJVsIRVuYEBElTDZ/tzc59jwIBnyJlFo4dFTkNPREVSRIeMTGF2ZjYxLjcuMTAzc3PWY8CLY8WI21RTMCYhB4pnyKFFo4dFTkNPREVSRIeUTGF2YzYxLjE5LjEwMSBsaWJ2cHhnyKFFo4hEVVJBVElPTkSHkzAwOjAwOjAyLjAwMDAwMDAwMAAfQ7Z1QfjngQCjvoEAAIAQAwCdASogABgAAEcIhYWIhYSIAgICdaoD+AP6AghZDL0A/vFyD/8kHryefn/khH/9mebf/Jl/9lsAo5WBAGQAsQEAARAQABgAGFgv9AAIcACjlYEAyACxAQABEBAAGAAYWC/0AAhwAKOVgQEsALEBAAEQEAAYABhYL/QACHAAo5WBAZAAsQEAARAQABgAGFgv9AAIcACjlYEB9ACxAQABEBAAGAAYWC/0AAhwAKOVgQJYALEBAAEQEAAYABhYL/QACHAAo5WBArwAsQEAARAQFGAAYWC/0AAhwACjlYEDIACxAQABEBAAGAAYWC/0AAhwAKOVgQOEALEBAAEQEAAYABhYL/QACHAAo5WBA+gAsQEAARAQABgAGFgv9AAIcACjlYEETACxAQABEBAAGAAYWC/0AAhwAKOVgQSwALEBAAEQEAAYABhYL/QACHAAo5WBBRQAsQEAARAQABgAGFgv9AAIcACjlYEFeACxAQABEBAAGAAYWC/0AAhwAKOVgQXcALEBAAEQEAAYABhYL/QACHAAo5WBBkAAsQEAARAQABgAGFgv9AAIcACjlYEGpACxAQABEBAAGAAYWC/0AAhwAKOVgQcIALEBAAEQEBRgAGFgv9AAIcAAo5WBB2wAsQEAARAQABgAGFgv9AAIcAAcU7trkbuPs4EAt4r3gQHxggGj8IED", "base64");

function sizesFixture() {
  const file = (dir, name, size) => ({ name, path: `${dir}/${name}`, type: "file", size });
  return {
    ok: true,
    commit: COMMIT,
    size: 10000,
    fileCount: 8,
    children: [
      {
        name: "src",
        type: "directory",
        size: 6000,
        children: [
          file("src", "engine.js", 3000),
          file("src", "render.js", 2000),
          { name: "…", type: "summary", size: 1000 },
        ],
      },
      {
        name: "docs",
        type: "directory",
        size: 3500,
        children: [file("docs", "guide.md", 1500), file("docs", "logo.png", 1000), file("docs", "clip.webm", 1000)],
      },
      { name: "README.md", path: "README.md", type: "file", size: 500 },
    ],
  };
}

async function openRepoPage(page, requests) {
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route("https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.lucide = { createIcons() {} };" }));
  await page.route("**/acme/engine", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }));
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = {};
    if (url.pathname === "/api/repositories") {
      body = {
        repositories: [{
          owner: "acme", name: "engine", description: "Code map fixture",
          defaultBranch: ACTIVE_REF, branch: ACTIVE_REF, live: true, public: true,
        }],
      };
    } else if (url.pathname === "/api/repo/acme/engine/sizes") {
      requests.sizes.push(url);
      body = sizesFixture();
    } else if (url.pathname === "/api/repo/acme/engine/blobs") {
      requests.blobs.push(url);
      const blobs = {};
      url.searchParams.getAll("path").forEach((item) => {
        blobs[item] = item.endsWith(".png")
          ? { ok: true, path: item, encoding: "base64", content: "AAAA" }
          : { ok: true, path: item, encoding: "utf-8", content: SOURCE };
      });
      body = { ok: true, commit: COMMIT, blobs };
    } else if (url.pathname === "/api/repo/acme/engine/raw") {
      requests.raw.push(url);
      return url.searchParams.get("path").endsWith(".webm")
        ? route.fulfill({ status: 200, contentType: "video/webm", body: GREEN_WEBM })
        : route.fulfill({ status: 200, contentType: "application/octet-stream", body: RED_PNG });
    } else if (url.pathname === "/api/repo/acme/engine/blob") {
      requests.blob.push(url);
      body = { ok: true, path: url.searchParams.get("path"), content: SOURCE, encoding: "utf-8" };
    } else if (url.pathname === "/api/repo/acme/engine/tree") {
      requests.tree.push(url);
      body = url.searchParams.get("path") === "src"
        ? {
          ok: true,
          commit: COMMIT,
          entries: [
            { name: "engine.js", type: "blob", size: 3000 },
            { name: "render.js", type: "blob", size: 2000 },
            { name: "util.js", type: "blob", size: 600 },
            { name: "vendor", type: "tree", size: 0 },
          ],
        }
        : { ok: true, entries: [], counts: {} };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto("/acme/engine");
  const button = page.locator("[data-repo-root-toolbar] [data-repo-codemap-open]");
  await expect(button).toBeVisible();
  return button;
}

const newRequests = () => ({ sizes: [], blobs: [], blob: [], tree: [], raw: [] });
// The viewer script is fetched on the first click, so early polls see nothing.
const mapState = (page) => page.evaluate(() => window.ForkMeshCodeMap?.current?.state() || {});

// Counts canvas pixels that differ from the first (background) pixel, i.e.
// whether anything was actually painted.
const paintedPixels = (page) => page.evaluate(() => {
  const canvas = document.querySelector("[data-codemap-canvas]");
  const data = canvas.getContext("2d").getImageData(0, 0, canvas.width, canvas.height).data;
  let painted = 0;
  for (let i = 4; i < data.length; i += 4) {
    if (data[i] !== data[0] || data[i + 1] !== data[1] || data[i + 2] !== data[2]) painted += 1;
  }
  return painted;
});

test("the Code map button draws the whole tree and loads source at the pinned commit", async ({ page }) => {
  const requests = newRequests();
  const button = await openRepoPage(page, requests);
  await button.click();

  const dialog = page.getByRole("dialog", { name: "Code map of acme/engine" });
  await expect(dialog).toBeVisible();
  // (The About rail's size chart reads /sizes as well, so count is not pinned.)
  expect(requests.sizes.length).toBeGreaterThan(0);
  requests.sizes.forEach((url) => expect(url.searchParams.get("ref")).toBe(ACTIVE_REF));

  // The folded "… more" entry is large on screen, so it expands by itself into
  // the file the size tree left out (util.js) plus a not-yet-listed folder.
  await expect.poll(async () => (await mapState(page)).files).toBe(7);
  expect(requests.tree.some((url) => url.searchParams.get("path") === "src"
    && url.searchParams.get("ref") === COMMIT)).toBe(true);

  // Every text file is large enough on screen to want its source; the batch
  // reads are pinned to the commit the tree came from, and the image is skipped.
  await expect.poll(async () => (await mapState(page)).loadedFiles).toBe(LOADED_TEXT_FILES);
  // The page reads its own metadata through /blobs at the branch; the map's
  // reads are the ones carrying the commit.
  const asked = requests.blobs
    .filter((url) => url.searchParams.get("ref") === COMMIT)
    .flatMap((url) => url.searchParams.getAll("path"));
  expect(asked.sort()).toEqual([
    "README.md", "docs/guide.md", "src/engine.js", "src/render.js", "src/util.js",
  ]);
  expect(await paintedPixels(page)).toBeGreaterThan(5000);

  // The image and the video are no text: each is read raw at the same commit
  // and drawn as a picture (the video as a frame) inside its cell.
  await expect.poll(async () => (await mapState(page)).previews).toBe(2);
  const rawPaths = [...new Set(requests.raw
    .filter((url) => url.searchParams.get("ref") === COMMIT)
    .map((url) => url.searchParams.get("path")))];
  expect(rawPaths.sort()).toEqual(["docs/clip.webm", "docs/logo.png"]);
  const pixelsLike = (test) => page.evaluate((source) => {
    const match = new Function("r", "g", "b", `return ${source};`);
    const canvas = document.querySelector("[data-codemap-canvas]");
    const data = canvas.getContext("2d").getImageData(0, 0, canvas.width, canvas.height).data;
    let count = 0;
    for (let i = 0; i < data.length; i += 4) if (match(data[i], data[i + 1], data[i + 2])) count += 1;
    return count;
  }, test);
  await expect.poll(() => pixelsLike("r > 240 && g < 16 && b < 16")).toBeGreaterThan(100);
  await expect.poll(() => pixelsLike("r < 40 && g > 200 && b < 40")).toBeGreaterThan(100);

  await expect(dialog.getByText("JavaScript")).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(dialog).toBeHidden();
  await expect(button).toBeFocused();
});

test("zooming to a file offers Open file, which loads it in the code tab", async ({ page }) => {
  const requests = newRequests();
  const button = await openRepoPage(page, requests);
  await button.click();
  await expect.poll(async () => (await mapState(page)).loadedFiles).toBe(LOADED_TEXT_FILES);

  const before = (await mapState(page)).scale;
  // README.md is the smallest cell, so fitting it to the viewport must zoom in.
  expect(await page.evaluate(() => window.ForkMeshCodeMap.current.goToPath("README.md"))).toBe(true);
  await expect.poll(async () => (await mapState(page)).scale).toBeGreaterThan(before * 1.5);
  await expect(page.locator(".fmcm-crumbs")).toContainText("README.md");

  await page.getByRole("button", { name: "Open file" }).click();
  await expect(page.locator("[data-codemap]")).toHaveCount(0);
  await expect.poll(() => requests.blob.length).toBeGreaterThan(0);
  expect(requests.blob.some((url) => url.searchParams.get("path") === "README.md")).toBe(true);
  await expect(page.locator("[data-repo-blob]")).toContainText("export function start");
});

test("a pointer click on the canvas zooms toward the clicked folder", async ({ page }) => {
  const requests = newRequests();
  const button = await openRepoPage(page, requests);
  await button.click();
  await expect.poll(async () => (await mapState(page)).loadedFiles).toBe(LOADED_TEXT_FILES);
  const canvas = page.locator("[data-codemap-canvas]");
  const box = await canvas.boundingBox();
  // src is the largest directory, so squarify places it first: top-left.
  await page.mouse.click(box.x + box.width * 0.2, box.y + box.height * 0.3);
  await expect.poll(async () => (await mapState(page)).focus).toMatch(/^src/);
});

test("the 3D toggle builds the city from the same tree", async ({ page }) => {
  const requests = newRequests();
  const button = await openRepoPage(page, requests);
  const webgl = await page.evaluate(() => Boolean(document.createElement("canvas").getContext("webgl2")));
  test.skip(!webgl, "this browser build has no WebGL2");
  await button.click();
  await expect.poll(async () => (await mapState(page)).loadedFiles).toBe(LOADED_TEXT_FILES);

  await page.getByRole("button", { name: "3D", exact: true }).click();
  await expect.poll(async () => (await mapState(page)).mode).toBe("3d");
  await expect(page.locator("[data-codemap-canvas3d]")).toBeVisible();
  const state = await mapState(page);
  // root, src and docs are slabs; every file (and any folded stub) is a tile.
  expect(state.three.dirs).toBe(3);
  expect(state.three.tiles).toBeGreaterThanOrEqual(5);
  await expect.poll(async () => (await mapState(page)).three.planes).toBeGreaterThan(0);
  const planes = (await mapState(page)).three.planes;

  // Planes and their textures outlive zooming far out and back in: nothing
  // is dropped (and so repainted or re-uploaded) on the way.
  const canvas3d = page.locator("[data-codemap-canvas3d]");
  const box = await canvas3d.boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  for (let i = 0; i < 6; i += 1) await page.mouse.wheel(0, 400);
  await page.waitForTimeout(500);
  expect((await mapState(page)).three.planes).toBeGreaterThanOrEqual(planes);
  for (let i = 0; i < 6; i += 1) await page.mouse.wheel(0, -400);
  await page.waitForTimeout(500);
  expect((await mapState(page)).three.planes).toBeGreaterThanOrEqual(planes);

  await page.getByRole("button", { name: "2D", exact: true }).click();
  await expect.poll(async () => (await mapState(page)).mode).toBe("2d");
  await expect(page.locator("[data-codemap-canvas]")).toBeVisible();
});
