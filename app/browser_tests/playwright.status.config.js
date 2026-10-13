const { defineConfig } = require("@playwright/test");
const path = require("node:path");


module.exports = defineConfig({
  testDir: path.join(__dirname, "tests"),
  testMatch: "status-page-reliability.spec.js",
  fullyParallel: false,
  workers: 1,
  reporter: [["list"]],
  timeout: 30_000,
  expect: { timeout: 8_000 },
  use: {
    browserName: "chromium",
    headless: true,
    serviceWorkers: "block",
  },
});
