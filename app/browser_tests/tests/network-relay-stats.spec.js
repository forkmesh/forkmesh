const { test, expect } = require("@playwright/test");


test("public relay stats explain an empty healthy network", async ({ page }) => {
  await page.route("**/api/network/stats*", (route) =>
    route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify({
        ok: true,
        hosts: 0,
        repos: 0,
        clients: 0,
        payoutNodes: [],
      }),
    }),
  );
  await page.route("**/api/network/online-history", (route) =>
    route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify({ hours: [], nodes: [] }),
    }),
  );
  await page.route("**/api/network/leaderboards", (route) =>
    route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify({}),
    }),
  );

  // The lightweight browser-test server exposes the source filename directly;
  // the production Worker canonicalizes /network to this same asset.
  await page.goto("/network.html");

  await expect(page.locator("[data-network-hosts]")).toHaveText("0");
  await expect(page.locator("[data-network-hosts-description]")).toHaveText(
    "No managed mirror hosts are reachable right now.",
  );
  await expect(page.locator("[data-network-repos]")).toHaveText("0");
  await expect(page.locator("[data-network-repos-description]")).toHaveText(
    "No public repositories have been cataloged yet.",
  );
  await expect(page.getByText("User wallets", { exact: true })).toBeVisible();
  await expect(page.getByText("No user payout wallets are listed yet.")).toBeVisible();
  await expect(page.getByText("Online clients", { exact: true })).toHaveCount(0);
  await expect(page.getByText("Longest hosted", { exact: true })).toHaveCount(0);
  await expect(page.getByText("Cloudflare coordinates the route.")).toHaveCount(0);
});
