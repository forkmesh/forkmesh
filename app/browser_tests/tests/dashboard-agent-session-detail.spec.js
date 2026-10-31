// Clicking an agent session on a repository's Agents tab opens a detail pane
// modelled on the desktop app's streaming session view: the meta table, the
// whole transcript, and the follow-up composer (adhoc #2117). The list used to
// print all of that inline for every session at once, so there was nothing to
// click and no way to read one run. The page is now the desktop's own two-pane
// shape (adhoc #2120): sessions on the left, the open one on the right, and the
// prompt box along the bottom of that pane - so opening a session no longer
// takes the list off screen.
const { test, expect } = require("@playwright/test");
const path = require("node:path");

const REPO_PAGE_PATH = path.resolve(
  __dirname, "..", "..", "public", "dashboard", "repo.html",
);

const REPOSITORIES = [{
  owner: "alpha",
  name: "engine",
  description: "Physics core",
  visibility: "public",
  source: "remote-clone",
  cloneUrl: "https://forkmesh.test/alpha/engine",
  rootCommit: "root-0",
  stateHash: "state-0",
  updatedAt: "1",
}];

const SESSIONS = [
  {
    id: "session-running",
    status: "running",
    provider: "claude-code",
    title: "Rebuild the solver",
    targetNode: "mirror35",
    createdBy: "alpha",
    createdAt: 1757000000000,
    updatedAt: 1757000600000,
    completedAt: 0,
    taskKey: "task:codex-world",
    createPr: true,
    security: { state: "waived", model: "", reason: "" },
    diagnostic: {
      code: "running",
      level: "active",
      message: "The mirror is running this agent session.",
    },
    agentInfo: {
      model: "opus",
      mode: "Auto",
      strength: "high",
      branchName: "agent/rebuild-solver",
      baseBranch: "main",
      numTurns: 7,
      durationMs: 94000,
      costUsd: 1.25,
      promptTokens: 12000,
      completionTokens: 3000,
      totalTokens: 15000,
      contextTokens: 48000,
      contextWindow: 200000,
      prNumber: 0,
    },
    history: [
      {
        role: "user",
        author: "alpha",
        text: "Rebuild the solver and keep the old API.",
        at: 1757000000000,
        state: "queued",
        attachments: [
          { id: "a".repeat(32), name: "solver-sketch.png", mediaType: "image/png", size: 68 },
          { id: "b".repeat(32), name: "old-plot.png", mediaType: "image/png", size: 68 },
        ],
      },
      {
        role: "assistant",
        author: "claude-code",
        text: "Owner-queued prompt dispatched.",
        at: 1757000300000,
        state: "running",
      },
    ],
  },
  {
    id: "session-failed",
    status: "failed",
    provider: "codex",
    title: "Document the map tiles",
    targetNode: "mirror7",
    createdBy: "alpha",
    createdAt: 1756900000000,
    updatedAt: 1756900900000,
    completedAt: 1756900900000,
    diagnostic: {
      code: "agent_failed",
      level: "attention",
      message: "The mirror reported that this session needs attention.",
    },
    agentInfo: { model: "sol", lastError: "The provider binary is missing." },
    history: [{
      role: "user",
      author: "alpha",
      text: "Write the tile docs.",
      at: 1756900000000,
      state: "queued",
    }],
  },
  {
    id: "session-done",
    status: "completed",
    provider: "claude-code",
    title: "add 123 to the readme",
    targetNode: "mirror35",
    createdBy: "alpha",
    createdAt: 1756800000000,
    updatedAt: 1756800011000,
    completedAt: 1756800011000,
    localAgentId: 39,
    security: { state: "waived", model: "", reason: "" },
    diagnostic: {
      code: "completed",
      level: "ok",
      message: "The mirror completed this agent session.",
    },
    agentInfo: {
      model: "",
      mode: "headless",
      branchName: "agent/web-39-add-123-to-the-readme",
      baseBranch: "main",
      numTurns: 4,
      durationMs: 11000,
      prNumber: 0,
    },
    history: [
      {
        role: "user",
        author: "alpha",
        text: "add 123 to the readme",
        at: 1756800000000,
        state: "queued",
      },
      {
        role: "assistant",
        author: "claude-code",
        text: "Appending 123 to README.md.\n\n\u25b8 Bash: echo 123 >> README.md\n\nAdded \"123\" to the end of README.md.",
        at: 1756800011000,
        state: "completed",
      },
    ],
  },
];

const COMPARE_PATCH = [
  "diff --git a/README.md b/README.md",
  "index 1111111..2222222 100644",
  "--- a/README.md",
  "+++ b/README.md",
  "@@ -1,2 +1,3 @@",
  " # Engine",
  " Physics core",
  "+123",
  "",
].join("\n");

const COMPARE_COMMITS = [
  "From 3333333333333333333333333333333333333333 Mon Sep 17 00:00:00 2001",
  "From: mirror35 <mirror35@users.noreply.forkmesh.com>",
  "Date: Sat, 12 Sep 2026 10:00:00 +0000",
  "Subject: [PATCH] add 123 to the readme",
  "",
  "",
].join("\n");

const PIXEL_PNG = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==",
  "base64",
);

async function openAgentsTab(page, { onFollowUp, onImage } = {}) {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alpha",
      email: "alpha@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
    }));
  });
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route(
    "https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (route) => route.fulfill({
      contentType: "text/javascript",
      body: "window.lucide = { createIcons() {} };",
    }));
  await page.route("**/alpha/engine/agents", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }));
  await page.route("**/alpha/engine/compare/**", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }));
  await page.route("**/api/**", async (route) => {
    const url = new URL(route.request().url());
    let body = { ok: true };
    const image = url.pathname.match(/\/agent-(?:bots|sessions)\/[^/]+\/attachments\/([0-9a-f]{32})$/);
    if (image) {
      onImage?.(url.pathname, route.request().headers());
      return image[1] === "a".repeat(32)
        ? route.fulfill({ status: 200, contentType: "application/octet-stream", body: PIXEL_PNG })
        : route.fulfill({
          status: 410,
          contentType: "application/json; charset=utf-8",
          body: JSON.stringify({ ok: false, error: "attachment_expired" }),
        });
    }
    if (url.pathname === "/api/repositories") {
      body = { ok: true, repositories: REPOSITORIES };
    } else if (url.pathname === "/api/repo/alpha/engine/compare") {
      // The mirror's bounded compare read: the same shape a web pull signs.
      body = {
        ok: true,
        baseOid: "1111111111111111111111111111111111111111",
        headOid: "3333333333333333333333333333333333333333",
        mergeBaseOid: "1111111111111111111111111111111111111111",
        commitCount: 1,
        patch: url.searchParams.get("head") === "agent/web-39-add-123-to-the-readme"
          ? COMPARE_PATCH : "",
        commits: url.searchParams.get("head") === "agent/web-39-add-123-to-the-readme"
          ? COMPARE_COMMITS : "",
      };
    } else if (/\/agent-(bots|sessions)$/.test(url.pathname)) {
      // Owner-owned repositories read /agent-sessions and organization ones
      // read /agent-bots; this fixture is the same panel either way.
      body = {
        ok: true,
        memberRole: "owner",
        scope: "owner",
        canQueueAgent: true,
        canViewAgentSessions: true,
        canDeleteAgentSessions: true,
        sessions: SESSIONS,
      };
    } else if (/\/agent-(bots|sessions)\/[^/]+$/.test(url.pathname)) {
      onFollowUp?.(JSON.parse(route.request().postData() || "{}"));
      body = { ok: true, message: "Saved for mirror35." };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto("/alpha/engine/agents");
  await expect(page.locator("[data-org-agent-session]")).toHaveCount(3);
}

test("an agent row opens the session detail and returns to the list", async ({ page }) => {
  await openAgentsTab(page);

  // The list is summaries only - no session prints its transcript inline.
  const list = page.locator("[data-org-agent-sessions]");
  await expect(list).not.toContainText("Rebuild the solver and keep the old API.");

  await page.locator('[data-org-agent-open="session-running"]').click();
  const detail = page.locator('[data-org-agent-detail="session-running"]');
  await expect(detail).toHaveCount(1);
  // The list stays beside the open session, which keeps a marked row.
  await expect(page.locator("[data-org-agent-session]")).toHaveCount(3);
  await expect(page.locator("[data-org-agent-session-selected]"))
    .toHaveAttribute("data-org-agent-session", "session-running");
  // Left column holds the list; the right pane holds the open session.
  await expect(page.locator("[data-org-agent-sessions] [data-org-agent-session]"))
    .toHaveCount(3);
  await expect(page.locator("[data-org-agent-detail-pane] [data-org-agent-detail]"))
    .toHaveCount(1);

  // The meta table carries what the desktop's own session table shows.
  await expect(detail).toContainText("Claude Code");
  await expect(detail).toContainText("opus");
  await expect(detail).toContainText("Auto");
  await expect(detail).toContainText("mirror35");
  await expect(detail).toContainText("agent/rebuild-solver");
  await expect(detail).toContainText("task:codex-world");
  await expect(detail).toContainText("$1.25");
  await expect(detail).toContainText("15,000 total");
  await expect(detail).toContainText("48,000 / 200,000 (24%)");
  await expect(detail).toContainText("Opens on finish");
  await expect(detail).not.toContainText("waived");
  await expect(detail).toContainText("The mirror is running this agent session.");
  await expect(detail.locator("[data-org-agent-transcript] > li")).toHaveCount(2);
  await expect(detail).toContainText("Rebuild the solver and keep the old API.");
  await expect(detail).toContainText("Owner-queued prompt dispatched.");
  // A running session is still promptable, so the composer rides under it -
  // pinned to the bottom of the pane, below the scrolling transcript.
  const composer = detail.locator('[data-org-agent-followup="session-running"]');
  await expect(composer).toHaveCount(1);
  await expect(composer.locator("textarea[name='prompt']")).toHaveCount(1);
  const paneBox = await detail.boundingBox();
  const composerBox = await composer.boundingBox();
  const transcriptBox = await detail.locator("[data-org-agent-transcript]").boundingBox();
  expect(composerBox.y).toBeGreaterThan(transcriptBox.y);
  expect(composerBox.y + composerBox.height)
    .toBeLessThanOrEqual(paneBox.y + paneBox.height + 1);

  await detail.locator("[data-org-agent-back]").click();
  await expect(page.locator("[data-org-agent-detail]")).toHaveCount(0);
  await expect(page.locator("[data-org-agent-session]")).toHaveCount(3);
  // With no session open the same corner carries the start composer.
  await expect(page.locator("[data-org-agent-detail-pane] [data-org-agent-start]"))
    .toHaveCount(1);
});

test("images attached to a prompt show on the transcript and open full size", async ({ page }) => {
  const reads = [];
  await openAgentsTab(page, { onImage: (pathname, headers) => reads.push({ pathname, headers }) });
  await page.locator('[data-org-agent-open="session-running"]').click();
  const detail = page.locator('[data-org-agent-detail="session-running"]');
  const prompt = detail.locator("[data-org-agent-transcript] > li").first();
  const images = prompt.locator("[data-org-agent-attachment]");
  await expect(images).toHaveCount(2);
  await expect(images.first()).toContainText("solver-sketch.png");
  await expect(images.first().locator("img")).toHaveAttribute("src", /^blob:/);
  await expect(images.nth(1)).toContainText("Kept for a week; no longer stored.");
  await expect(images.nth(1)).toBeDisabled();
  expect(reads.map((read) => read.pathname).sort()).toEqual([
    `/api/repo/alpha/engine/agent-sessions/session-running/attachments/${"a".repeat(32)}`,
    `/api/repo/alpha/engine/agent-sessions/session-running/attachments/${"b".repeat(32)}`,
  ]);
  expect(reads[0].headers.authorization).toBe("Bearer test-session");
  await expect(detail.locator("[data-org-agent-transcript] > li").nth(1).locator("[data-org-agent-attachment]"))
    .toHaveCount(0);

  await images.first().click();
  const viewer = page.locator("[data-org-agent-image-viewer]");
  await expect(viewer).toBeVisible();
  await expect(viewer).toContainText("solver-sketch.png");
  await expect(viewer.locator("img")).toHaveAttribute("src", /^blob:/);
  await viewer.locator("[data-org-agent-image-viewer-close]").click();
  await expect(viewer).toBeHidden();
});

test("the agents page is a list beside one session, with the prompt box at its foot", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 900 });
  await openAgentsTab(page);

  // Nothing is open yet: the pane beside the list is the start composer, and
  // its prompt box sits at the bottom of that pane (adhoc #2120).
  const pane = page.locator("[data-org-agent-detail-pane]");
  const startForm = pane.locator("[data-org-agent-start]");
  await expect(startForm).toHaveCount(1);
  // The node and model pickers came from the panel read, not a later one.
  await expect(startForm.locator("[data-org-agent-target]")).toHaveCount(1);

  const listBox = await page.locator("[data-org-agent-sessions]").boundingBox();
  const paneBox = await pane.boundingBox();
  expect(listBox.x + listBox.width).toBeLessThanOrEqual(paneBox.x + 1);

  const startBox = await startForm.boundingBox();
  expect(startBox.y).toBeGreaterThan(paneBox.y);
  expect(startBox.y + startBox.height)
    .toBeLessThanOrEqual(paneBox.y + paneBox.height + 1);
});

test("a session no node ever started shows its error and has nothing to resume", async ({ page }) => {
  await openAgentsTab(page);
  await page.locator('[data-org-agent-open="session-failed"]').click();
  const detail = page.locator('[data-org-agent-detail="session-failed"]');
  await expect(detail).toContainText("The provider binary is missing.");
  await expect(detail).toContainText("needs attention");
  await expect(detail).toContainText("nothing to resume");
  await expect(detail.locator("[data-org-agent-followup]")).toHaveCount(0);
  // No branch was ever named, so there is no diff to view.
  await expect(detail.locator("[data-org-agent-compare]")).toHaveCount(0);
});

test("a completed session shows its transcript first, links its branch diff, and still takes a prompt", async ({ page }) => {
  const sent = [];
  await openAgentsTab(page, { onFollowUp: (body) => sent.push(body) });
  await page.locator('[data-org-agent-open="session-done"]').click();
  const detail = page.locator('[data-org-agent-detail="session-done"]');
  await expect(detail).toContainText("The mirror completed this agent session.");

  // The session is what the page is about: the transcript reads above the
  // details table, and the node's narration of the run is in it.
  await expect(detail.locator("[data-org-agent-transcript] > li")).toHaveCount(2);
  await expect(detail).toContainText("Bash: echo 123 >> README.md");
  const transcriptBox = await detail.locator("[data-org-agent-transcript]").boundingBox();
  const metaBox = await detail.locator("[data-org-agent-meta]").boundingBox();
  expect(transcriptBox.y).toBeLessThan(metaBox.y);

  // The branch is readable before any pull request exists: the toolbar's
  // Diff shows it in the pane, and its own row links the compare page.
  const compareHref = "/alpha/engine/compare/main...agent/web-39-add-123-to-the-readme";
  await expect(detail.locator('[data-org-agent-action="diff"]')).toHaveCount(1);
  await expect(detail.locator("[data-org-agent-compare]")).toHaveCount(0);
  await expect(detail.locator("[data-org-agent-branch-link]"))
    .toHaveAttribute("href", compareHref);

  // Finished is not over: the composer stays, and says it resumes the run.
  const composer = detail.locator('[data-org-agent-followup="session-done"]');
  await expect(composer).toHaveCount(1);
  await expect(composer).toHaveAttribute("data-org-agent-followup-resumes", "");
  await expect(composer).toContainText("resumes this session");
  await expect(detail).not.toContainText("no longer accepting prompts");
  await composer.locator('textarea[name="prompt"]').fill("Now add 456 as well.");
  await composer.locator('button[type="submit"]').click();
  await expect.poll(() => sent.map((body) => body.prompt))
    .toEqual(["Now add 456 as well."]);
});

test("the branch diff link opens the compare page, which feeds the new-pull form", async ({ page }) => {
  await openAgentsTab(page);
  await page.locator('[data-org-agent-open="session-done"]').click();
  await page.locator('[data-org-agent-detail="session-done"] [data-org-agent-branch-link]').click();

  // /owner/repo/compare/base...head is its own page on the Pull requests tab.
  await expect(page).toHaveURL(/\/alpha\/engine\/compare\/main\.\.\.agent\/web-39-add-123-to-the-readme$/);
  const compare = page.locator("[data-repo-compare]");
  await expect(compare).toHaveCount(1);
  await expect(page.locator('[data-dashboard-repo-tab="pulls"]'))
    .toHaveAttribute("aria-selected", "true");
  await expect(compare).toContainText("Comparing");
  await expect(compare).toContainText("agent/web-39-add-123-to-the-readme");
  // The commit series and the patch, through the pull review's diff renderer.
  await expect(compare.locator("[data-repo-compare-commits]")).toContainText("add 123 to the readme");
  const diff = compare.locator("[data-repo-compare-diff]");
  await expect(diff).toContainText("README.md");
  // The added line sits in the renderer's green "add" row (its "+" gutter is a
  // separate cell, so the text is matched on its own).
  await expect(diff.locator(".bg-emerald-500\\/10", { hasText: "123" })).toHaveCount(1);
  await expect(compare.locator("[data-repo-compare-summary]")).toContainText("1 commit · 1 file changed");

  // It is the pull review's Files changed pane (adhoc #2148): the changed-file
  // explorer, the unified/split layout toggle, and viewed folds - but no line
  // gutters to start a review comment, since there is no pull request yet.
  const files = diff.locator("[data-repo-pull-files]");
  await expect(files).toHaveCount(1);
  await expect(files.locator('[data-repo-pull-file="README.md"]')).toHaveCount(1);
  await expect(files.locator("[data-repo-pull-viewed-summary]")).toHaveText("0 of 1 viewed");
  await expect(files.locator("[data-repo-pull-diff-line]")).toHaveCount(0);
  // Split rows pair old and new sides in a four-column grid; unified rows are
  // one code column behind two line-number gutters.
  const splitRows = files.locator(".grid-cols-\\[3\\.5rem_minmax\\(0\\,1fr\\)_3\\.5rem_minmax\\(0\\,1fr\\)\\]");
  await expect(splitRows).toHaveCount(0);
  await files.locator("[data-repo-diff-settings] > summary").click();
  await files.locator('[data-repo-pull-diff-mode="split"]').click();
  await expect(splitRows.first()).toBeVisible();
  await expect(files.locator('[data-repo-pull-diff-mode="split"]')).toHaveAttribute("aria-pressed", "true");
  await files.locator("[data-repo-diff-settings] > summary").click();
  await files.locator('[data-repo-pull-diff-mode="unified"]').click();
  await expect(splitRows).toHaveCount(0);
  await expect(files.locator('[data-repo-pull-diff-mode="unified"]')).toHaveAttribute("aria-pressed", "true");
  await files.locator('[data-repo-pull-viewed="README.md"]').click();
  await expect(files.locator('[data-repo-pull-diff-file="README.md"]')).toHaveAttribute("data-viewed", "true");
  await expect(files.locator("[data-repo-pull-viewed-summary]")).toHaveText("All 1 viewed");
  // The mark is keyed by the compared commits and survives a reload.
  await page.reload();
  await expect(page.locator('[data-repo-compare] [data-repo-pull-diff-file="README.md"]'))
    .toHaveAttribute("data-viewed", "true");
  await page.locator('[data-repo-compare] [data-repo-pull-viewed="README.md"]').click();
  await expect(page.locator("[data-repo-compare] [data-repo-pull-viewed-summary]")).toHaveText("0 of 1 viewed");

  // "Create pull request" opens the form on the compared pair.
  await compare.locator("[data-repo-compare-pull]").click();
  const form = page.locator("[data-repo-pull-new-form]");
  await expect(form).toHaveCount(1);
  await expect(form.locator("[data-repo-pull-base]")).toHaveValue("main");
  await expect(form.locator("[data-repo-pull-head]")).toHaveValue("agent/web-39-add-123-to-the-readme");
  await expect(page).toHaveURL(/\/alpha\/engine\/pulls$/);
});

test("a compare address survives a refresh and says when a branch has nothing new", async ({ page }) => {
  await openAgentsTab(page);
  await page.goto("/alpha/engine/compare/main...agent/web-39-add-123-to-the-readme");
  const compare = page.locator("[data-repo-compare]");
  await expect(compare.locator("[data-repo-compare-diff] .bg-emerald-500\\/10", { hasText: "123" }))
    .toHaveCount(1);

  await page.goto("/alpha/engine/compare/main...feature/empty");
  await expect(page.locator("[data-repo-compare-empty]"))
    .toContainText("has nothing that is not already on");
  // Back to the list leaves the comparison behind in the address bar too.
  await page.locator("[data-repo-compare-back]").click();
  await expect(page).toHaveURL(/\/alpha\/engine\/pulls$/);
  await expect(page.locator("[data-repo-compare]")).toHaveCount(0);
});

test("a follow-up sent from the detail page stays on that page", async ({ page }) => {
  const sent = [];
  await openAgentsTab(page, { onFollowUp: (body) => sent.push(body) });
  await page.locator('[data-org-agent-open="session-running"]').click();
  const detail = page.locator('[data-org-agent-detail="session-running"]');
  await detail.locator('textarea[name="prompt"]').fill("Also update the docs.");
  await detail.locator('button[type="submit"]').click();

  await expect.poll(() => sent.map((body) => body.prompt))
    .toEqual(["Also update the docs."]);
  // The re-read redraws the same session, not the list behind it.
  await expect(page.locator('[data-org-agent-detail="session-running"]'))
    .toHaveCount(1);
});
