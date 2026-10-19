import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";

const ROOT = new URL("../../", import.meta.url);
const SOURCE = readFileSync(new URL("marketplace/packages/usage/content/web/page.js", ROOT), "utf8");

function loadPage() {
  const registrations = [];
  const context = vm.createContext({
    Intl,
    Date,
    Number,
    Math,
    window: { ForkMeshDashboard: { registerPage: (id, init) => registrations.push({ id, init }) } },
  });
  const resetClock = vm.runInContext(`${SOURCE}\n;usagePageResetClock`, context);
  return { resetClock, registrations };
}

const ZONE = "UTC";
const NOW = Date.UTC(2026, 9, 19, 10, 5);
const at = (days, hour, minute) => Date.UTC(2026, 9, 19 + days, hour, minute);

test("usage page registers itself", () => {
  const { registrations } = loadPage();
  assert.deepEqual(registrations.map((entry) => entry.id), ["usage"]);
});

test("reset clock uses 12-hour time", () => {
  const { resetClock } = loadPage();
  assert.equal(resetClock(at(0, 15, 45), NOW, ZONE), "Resets 3:45 PM");
  assert.equal(resetClock(at(0, 23, 59), NOW, ZONE), "Resets 11:59 PM");
});

test("reset clock names the weekday when it is not today", () => {
  const { resetClock } = loadPage();
  assert.equal(resetClock(at(1, 0, 0), NOW, ZONE), "Resets Tue 12:00 AM");
  assert.equal(resetClock(at(1, 9, 0), NOW, ZONE), "Resets Tue 9:00 AM");
  assert.equal(resetClock(at(6, 12, 30), NOW, ZONE), "Resets Sun 12:30 PM");
});

test("reset clock names the date a week or more out", () => {
  const { resetClock } = loadPage();
  assert.equal(resetClock(at(7, 10, 0), NOW, ZONE), "Resets Oct 26, 10:00 AM");
});

test("reset clock is empty without an upcoming reset", () => {
  const { resetClock } = loadPage();
  assert.equal(resetClock(at(0, 9, 0), NOW, ZONE), "");
  assert.equal(resetClock(0, NOW, ZONE), "");
});

test("reset clock follows the viewer's time zone", () => {
  const { resetClock } = loadPage();
  const now = Date.UTC(2026, 9, 19, 17, 30);
  assert.equal(resetClock(now + 2 * 3600 * 1000, now, "Asia/Kolkata"), "Resets Tue 1:00 AM");
});
