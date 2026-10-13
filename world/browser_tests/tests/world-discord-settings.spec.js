// The Discord bridge is a settings section: it renders inside the settings
// panel, under the trigger that opens it, with settings still open.
const { test, expect } = require("@playwright/test");

async function openWorld(page) {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/world?nointro=1");
  await page.waitForFunction(() =>
    Boolean(document.querySelector("forkmesh-world")?.world?.renderer?.domElement),
  );
  await page.waitForSelector("[data-world-discord-open]", { state: "attached" });
  // The bridge loader is imported on an idle slice after the world reports
  // itself interactive; clicking the trigger before then does nothing.
  await page.waitForFunction(
    () =>
      performance
        .getEntriesByType("resource")
        .some((entry) => entry.name.endsWith("/world/world-discord.js")),
    undefined,
    { timeout: 20_000 },
  );
}

test("the Discord bridge renders inside the settings panel", async ({ page }) => {
  await openWorld(page);

  // Nothing is mounted until the bridge is asked for.
  const before = await page.evaluate(() => {
    const root = document.querySelector("[data-world-discord-root]");
    return {
      exists: Boolean(root),
      inSettings: Boolean(root?.closest("[data-world-settings]")),
      hidden: root?.hidden ?? null,
      children: root?.childElementCount ?? -1,
    };
  });
  expect(before).toEqual({
    exists: true,
    inSettings: true,
    hidden: true,
    children: 0,
  });

  await page.locator("[data-world-settings-open]").first().click();
  await expect(page.locator("[data-world-settings]")).toHaveAttribute(
    "data-open",
    "true",
  );
  await page.locator("[data-world-discord-open]").click();

  // The panel paints in the settings-hosted root, and settings stays open.
  await page.waitForFunction(
    () =>
      Boolean(
        document.querySelector("[data-world-discord-root] .world-discord-panel"),
      ),
    undefined,
    { timeout: 15_000 },
  );
  const after = await page.evaluate(() => {
    const root = document.querySelector("[data-world-discord-root]");
    const panel = root?.querySelector(".world-discord-panel");
    const settings = document.querySelector("[data-world-settings]");
    return {
      inSettings: Boolean(panel?.closest("[data-world-settings]")),
      hidden: root?.hidden ?? null,
      settingsOpen: settings?.dataset.open ?? null,
      settingsInert: Boolean(settings?.inert),
      expanded: document
        .querySelector("[data-world-discord-open]")
        ?.getAttribute("aria-expanded"),
      position: getComputedStyle(root).position,
      bodyRoots: document.querySelectorAll(
        "body > [data-world-discord-root]",
      ).length,
    };
  });
  expect(after).toEqual({
    inSettings: true,
    hidden: false,
    settingsOpen: "true",
    settingsInert: false,
    expanded: "true",
    position: "static",
    bodyRoots: 0,
  });

  // Closing the bridge collapses the section without closing settings.
  await page.locator(".world-discord-close").click();
  await expect(page.locator("[data-world-settings]")).toHaveAttribute(
    "data-open",
    "true",
  );
  expect(
    await page.evaluate(
      () => document.querySelector("[data-world-discord-root]").hidden,
    ),
  ).toBe(true);
});

test("the 3D Discord board opens settings on the bridge", async ({ page }) => {
  await openWorld(page);
  await page.evaluate(() =>
    window.dispatchEvent(new CustomEvent("forkmesh:open-discord")),
  );
  await page.waitForFunction(
    () =>
      Boolean(
        document.querySelector("[data-world-discord-root] .world-discord-panel"),
      ),
    undefined,
    { timeout: 15_000 },
  );
  await expect(page.locator("[data-world-settings]")).toHaveAttribute(
    "data-open",
    "true",
  );
});
