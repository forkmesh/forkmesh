const { test, expect } = require("@playwright/test");
const path = require("node:path");
const { createCipheriv, createHash, pbkdf2Sync } = require("node:crypto");

const CHAT_HTML_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "..",
  "app",
  "public",
  "chat.html",
);
const OFFICE_ENTRANCE_POSITION = [0, 0.38, -168.8];
const OFFICE_ENTRY_TICKET = "playwright-office-entry-ticket";
const WORLD_INPUT_RESPONSE_BUDGET_MS = 100;
const WORLD_RENDER_DRAW_CALL_BUDGET = 450;
const WORLD_RENDER_TRIANGLE_BUDGET = 750_000;
const WORLD_INPUT_RESPONSE_SAMPLES = 5;

const PRIVATE_SETTINGS = {
  theme: "world",
  availability: "inactive",
  activityCategory: "viewing-repository",
  publicDoor: "closed",
  displayName: "Never expose this name",
  privacy: {
    name: false,
    country: false,
    browser: false,
    os: false,
    activity: false,
    inactivity: false,
    localTime: false,
    nodes: false,
  },
};

const FIXED_NOW = 1_785_000_000_000;
const AQUARIUM_SCHOOL_USERS = [
  {
    name: "reef-us-firefox-one",
    countryCode: "US",
    browser: "Firefox",
    os: "Linux",
    activityBucket: "hour",
  },
  {
    name: "reef-us-firefox-two",
    countryCode: "US",
    browser: "Firefox",
    os: "Linux",
    activityBucket: "5h",
  },
  {
    name: "reef-ca-safari-one",
    countryCode: "CA",
    browser: "Safari",
    os: "macOS",
    activityBucket: "24h",
  },
  {
    name: "reef-ca-safari-two",
    countryCode: "CA",
    browser: "Safari",
    os: "macOS",
    activityBucket: "3d",
  },
];

function reefDirectoryUsers(count = 20) {
  return Array.from({ length: count }, (_, index) => ({
    name: `reef-member-${index}`,
    activityBucket: index % 2 ? "hour" : "older",
  }));
}

function worldChatKey(passphrase) {
  const salt = createHash("sha256")
    .update("ForkMesh room:world-general", "utf8")
    .digest()
    .subarray(0, 16);
  return pbkdf2Sync(passphrase, salt, 210000, 32, "sha256");
}

function encryptWorldChatEnvelope(message, key, nonceByte) {
  const nonce = Buffer.alloc(12, nonceByte);
  const cipher = createCipheriv("aes-256-gcm", key, nonce);
  const body = Buffer.concat([
    cipher.update(JSON.stringify(message), "utf8"),
    cipher.final(),
  ]);
  return {
    kind: "cipher",
    v: 1,
    nonce: nonce.toString("base64"),
    tag: cipher.getAuthTag().toString("base64"),
    body: body.toString("base64"),
    historyReplay: true,
  };
}

async function prepareWorldPage(
  page,
  socketId,
  {
    includeThread = false,
    session = null,
    notifications = [],
    events = [],
    chatPassphrase = "",
    worldSocketHandler = null,
    repositoryFixture = null,
    repositoryStarFixture = null,
    repositoryFollowerFixture = null,
    accountFixture = null,
    unavailablePaths = [],
    chatChannels = [],
    chatChannelStatus = 200,
    ticketAuthenticated = true,
    ticketActivity = null,
    directoryUsers = [],
    systemCapacityTables = [],
    systemCapacityD1Storage = null,
    officeEntryRequests = [],
    officeFloorRequests = [],
    officeAttendanceRequests = [],
    officeAttendanceFixture = null,
    officeFloorAccess = null,
    officeFloorDelayMs = 0,
    officeTaskFixture = null,
    accountSessionFixture = null,
    organizationViewerRole = "",
  } = {},
) {
  let mentionState = "review";
  let notificationsRead = false;
  let activeSession = session;
  await page.addInitScript(({ now, identity, accountSession }) => {
    Date.now = () => now;
    let seed = [...identity].reduce(
      (value, char) => (Math.imul(value, 31) + char.charCodeAt(0)) >>> 0,
      0x5eed1234,
    );
    Math.random = () => {
      seed = (seed * 1664525 + 1013904223) >>> 0;
      return seed / 0x1_0000_0000;
    };
    sessionStorage.setItem(
      "forkmesh.world.guestId.v1",
      `playwright-${identity}`,
    );
    if (accountSession) {
      localStorage.setItem("forkmesh.session", JSON.stringify(accountSession));
    }
  }, { now: FIXED_NOW, identity: socketId, accountSession: session });
  await page.emulateMedia({ reducedMotion: "reduce" });
  page.on("pageerror", (error) => {
    process.stderr.write(`World page error: ${error.message}\n`);
  });
  page.on("console", (message) => {
    if (message.type() === "error") {
      process.stderr.write(`World console error: ${message.text()}\n`);
    }
  });
  await page.route("**/chat?embed=office", (route) =>
    route.fulfill({
      path: CHAT_HTML_PATH,
      contentType: "text/html; charset=utf-8",
    }),
  );
  await page.route("**/api/**", async (route) => {
    const url = new URL(route.request().url());
    if (unavailablePaths.includes(url.pathname)) {
      return route.fulfill({
        status: 503,
        contentType: "application/json; charset=utf-8",
        body: JSON.stringify({ ok: false, error: "fixture_unavailable" }),
      });
    }
    let metadataCapacityRelease = null;
    if (
      repositoryFixture &&
      Number(repositoryFixture.singleCoreDelayMs) > 0 &&
      url.pathname.startsWith("/api/repo/forkmesh/forkmesh/") &&
      ["/branches", "/tree", "/blobs", "/sizes", "/stats"].some((suffix) =>
        url.pathname.endsWith(suffix),
      )
    ) {
      const previous =
        repositoryFixture.metadataCapacityTail || Promise.resolve();
      metadataCapacityRelease = null;
      repositoryFixture.metadataCapacityTail = new Promise((resolve) => {
        metadataCapacityRelease = resolve;
      });
      await previous;
      repositoryFixture.metadataActive =
        Number(repositoryFixture.metadataActive || 0) + 1;
      repositoryFixture.maxMetadataActive = Math.max(
        Number(repositoryFixture.maxMetadataActive || 0),
        repositoryFixture.metadataActive,
      );
      await new Promise((resolve) =>
        setTimeout(
          resolve,
          Math.max(
            1,
            Math.min(
              1000,
              Number(repositoryFixture.singleCoreDelayMs) || 0,
            ),
          ),
        ),
      );
    }
    let status = 200;
    let body =
      url.pathname === "/api/world/context"
        ? {
            now: FIXED_NOW,
            countryCode: "",
            worldConnections: 64,
            worldMessagesPerSecond: 4,
            chatConnections: 128,
          }
        : url.pathname === "/api/network/overview"
          ? {
              ok: true,
              stats: {
                repos: 1,
                hosts: 2,
                clients: 0,
                onlineNodes: ["mirror2", "mirror3"],
              },
              leaderboards: {
                nodes: [
                  { name: "mirror2", pullCount: 5, issueCount: 7 },
                  { name: "mirror3", pullCount: 5, issueCount: 7 },
                ],
              },
              history: {},
            }
        : url.pathname === "/api/repo/forkmesh/forkmesh/mirrors"
          ? {
              ok: true,
              owner: "forkmesh",
              repo: "forkmesh",
              mirrors: [
                {
                  node: "mirror2",
                  status: "online",
                  integrity: "ok",
                  cloneAvailable: true,
                  commit: "a".repeat(40),
                  branch: "main",
                  sizeBytes: 75_139_176,
                  issueCount: 47,
                  commitCount: 8661,
                  branchCount: 18,
                  pullCount: 42,
                  discussionCount: 2,
                  artifactCount: 1,
                  platform: "linux",
                  version: "0.7.0",
                  cpuPercent: 25,
                  memUsedBytes: 536_870_912,
                  memTotalBytes: 1_073_741_824,
                  diskUsedBytes: 5_368_709_120,
                  diskTotalBytes: 10_737_418_240,
                },
                {
                  node: "mirror3",
                  status: "online",
                  integrity: "ok",
                  cloneAvailable: true,
                  commit: "a".repeat(40),
                  branch: "main",
                  sizeBytes: 74_944_512,
                  issueCount: 47,
                  commitCount: 8661,
                  branchCount: 18,
                  pullCount: 42,
                  discussionCount: 2,
                  artifactCount: 1,
                  platform: "linux",
                  version: "0.7.0",
                },
              ],
            }
        : url.pathname === "/api/world/instances"
          ? {
              instances: [
                {
                  id: "0123456789abcdef01234567",
                  label: "North relay",
                  origin: "https://north.example/",
                  approved: true,
                  health: "online",
                  online: true,
                  healthEvidence: "fresh-verified-node-health",
                  federationPublicKey: "must-not-render",
                  token: "must-not-render",
                },
                {
                  id: "89abcdef0123456789abcdef",
                  label: "Garden relay",
                  origin: "https://garden.example/",
                  approved: true,
                  health: "awaiting_verified_health",
                  online: false,
                  healthEvidence: "no-fresh-verified-node-health",
                },
              ],
            }
        : url.pathname === "/api/chat/room-key"
          ? url.searchParams.get("room") === "world-general" &&
            chatPassphrase
            ? {
                ok: true,
                scope: "mainnode/forkmesh",
                room: "world-general",
                access: "public-world-general",
                passphrase: chatPassphrase,
              }
            : ((status = 401), { error: "unauthorized" })
        : url.pathname === "/api/chat/channels"
          ? chatChannelStatus === 200
            ? { ok: true, channels: chatChannels }
            : ((status = chatChannelStatus), { error: "invalid_session" })
        : url.pathname === "/api/world/events"
          ? { events }
          : url.pathname === "/api/notifications"
            ? route.request().method() === "POST"
              ? ((notificationsRead = true), { ok: true })
              : {
                  ok: true,
                  notifications: notifications.map((item) => ({
                    ...item,
                    readAt: notificationsRead
                      ? item.readAt || FIXED_NOW
                      : item.readAt || 0,
                  })),
                  unread: notificationsRead
                    ? 0
                    : notifications.filter((item) => !item.readAt).length,
                }
            : url.pathname === "/api/world/ticket" && activeSession
              ? ticketAuthenticated
                ? {
                    authenticated: true,
                    accountStatus: "Registered",
                    name: activeSession.nodeName,
                    isAdmin:
                      systemCapacityTables.length > 0 ||
                      Boolean(systemCapacityD1Storage),
                    ticket: "playwright-world-ticket",
                    expiresAt: FIXED_NOW + 300_000,
                    totalActiveMs: Math.max(
                      0,
                      Number(ticketActivity?.totalActiveMs) || 0,
                    ),
                    activityObservedAt:
                      Number(ticketActivity?.activityObservedAt) || FIXED_NOW,
                    ...(systemCapacityTables.length || systemCapacityD1Storage
                      ? {
                          systemCapacity: {
                            tables: systemCapacityTables,
                            ...(systemCapacityD1Storage
                              ? { d1Storage: systemCapacityD1Storage }
                              : {}),
                          },
                        }
                      : {}),
                  }
                : {
                    ok: true,
                    authenticated: false,
                    accountStatus: "Guest",
                    nodeCount: 0,
                    ticket: "",
                  }
          : url.pathname === "/api/orgs/forkmesh"
            ? {
                ok: true,
                organization: "forkmesh",
                viewerRole: organizationViewerRole,
              }
          : url.pathname === "/api/accounts/users"
            ? { ok: true, users: directoryUsers }
          : url.pathname === "/api/world/community-ads/placements"
            ? {
                ok: true,
                label: "Community-reviewed placement",
                context: "town-square",
                tracking: "none",
                behavioralTargeting: false,
                sensitiveTargeting: false,
                personalDataUsed: false,
                placements: [
                  {
                    proposalId: "a".repeat(32),
                    label: "Community-reviewed placement",
                    sponsor: "Open Tools Cooperative",
                    copy: "Auditable build tools for open-source teams.",
                    destinationUrl:
                      "https://sponsor.example.org/open-builds",
                    whyShown:
                      "Enabled for the town-square page context.",
                    tracking: "none",
                  },
                ],
              }
          : url.pathname === "/api/world/fediverse-mentions" &&
              route.request().method() === "GET"
            ? {
                ok: true,
                automaticIssueCreation: false,
                items: [
                  {
                    id: "b".repeat(32),
                    kind: "mention",
                    state: mentionState,
                    repository: "acme/project",
                    author: "alice@social.example.org",
                    authorName: "Alice",
                    excerpt: "The save button crashes on large projects.",
                    remoteUrl:
                      "https://social.example.org/@alice/42",
                    issueUrl: "",
                    issueNumber: 0,
                    progress:
                      mentionState === "pending"
                        ? "Pending owner-node materialization."
                        : "Awaiting authorized manual review.",
                    verifiedPublicActivity: true,
                    automaticIssueCreation: false,
                  },
                  ...(includeThread
                    ? [
                        {
                          id: "c".repeat(32),
                          kind: "reply",
                          state: "linked",
                          repository: "acme/project",
                          author: "bob@lemmy.example.org",
                          authorName: "Bob",
                          excerpt: "Nested reply from a public Lemmy thread.",
                          remoteUrl:
                            "https://lemmy.example.org/comment/77",
                          issueUrl:
                            "https://forkmesh.example.org/acme/project/issues/17",
                          issueNumber: 17,
                          progress:
                            "Verified reply linked to a tracked public issue.",
                          verifiedPublicActivity: true,
                          automaticIssueCreation: false,
                        },
                      ]
                    : []),
                ],
              }
          : url.pathname ===
              "/api/repo/acme/project/fedi-comments"
            ? {
                ok: true,
                comments: [
                  {
                    remoteId:
                      "https://lemmy.example.org/comment/77",
                    parentRemoteId: "",
                    author: "bob",
                    authorName: "Bob",
                    body: "Nested reply from a public Lemmy thread.",
                    backlink:
                      "https://lemmy.example.org/comment/77",
                    sourceInstance: "lemmy.example.org",
                    sourceSoftware: "lemmy",
                    lifecycle: "active",
                    depth: 1,
                    nativeEvent: false,
                    provenance: {
                      source: "activitypub",
                      instance: "lemmy.example.org",
                      software: "lemmy",
                      backlink:
                        "https://lemmy.example.org/comment/77",
                      nativeEvent: false,
                    },
                  },
                ],
              }
          : url.pathname.endsWith("/preview")
            ? {
                ok: true,
                mentionId: "b".repeat(32),
                repository: "acme/project",
                author: "alice@social.example.org",
                remoteUrl: "https://social.example.org/@alice/42",
                draft: {
                  title: "Save button crashes",
                  body: "The save button crashes on large projects.",
                },
                state: "review",
                requiresExplicitConfirmation: true,
                followupDefault: false,
              }
          : url.pathname.endsWith("/create")
            ? ((mentionState = "pending"),
              {
                ok: true,
                state: "pending",
                issueNumber: 0,
                proposedIssueNumber: 17,
                created: false,
              })
          : url.pathname.endsWith("/moderate")
            ? { ok: true, state: "dismissed" }
          : url.pathname === "/api/world/fediverse"
            ? { mastodon: [], lemmy: [], x: [], reddit: [] }
          : url.pathname === "/api/world/media/spaces"
            ? { spaces: [] }
            : url.pathname === "/api/version"
              ? {
                  ok: true,
                  version: "0.7.0",
                  rev: "d".repeat(40),
                  now: FIXED_NOW,
                }
            : url.pathname === "/api/repositories"
              ? { repositories: [] }
              : {};
    const officeTaskBasePath = url.pathname.startsWith("/api/tasks")
      ? "/api/tasks"
      : "/api/world/office/marketing-tasks";
    if (
      officeTaskFixture &&
      (
        url.pathname.startsWith("/api/tasks") ||
        url.pathname.startsWith("/api/world/office/marketing-tasks")
      )
    ) {
      const method = route.request().method();
      const suffix = url.pathname
        .slice(officeTaskBasePath.length)
        .split("/")
        .filter(Boolean);
      let requestBody = {};
      try {
        requestBody = route.request().postDataJSON() || {};
      } catch (_) {}
      officeTaskFixture.requests ||= [];
      officeTaskFixture.tasks ||= [];
      officeTaskFixture.requests.push({
        method,
        path: url.pathname,
        body: requestBody,
      });
      if (!session?.sessionToken) {
        status = 401;
        body = { error: "invalid_session" };
      } else if (method === "GET" && suffix.length === 0) {
        body = {
          ok: true,
          actor: String(session.nodeName || "").toLowerCase(),
          canManage: officeTaskFixture.canManage === true,
          serverNow: FIXED_NOW,
          tasks: officeTaskFixture.tasks,
          ...(officeTaskFixture.canManage
            ? { members: officeTaskFixture.members || [session.nodeName] }
            : {}),
        };
      } else if (method === "POST" && suffix.length === 0) {
        const task = {
          id: String(officeTaskFixture.nextId || "e".repeat(32)),
          title: requestBody.title,
          department: String(requestBody.department || "marketing")
            .toLowerCase(),
          assignee: String(requestBody.assignee || "").toLowerCase(),
          status: "idle",
          elapsedMs: 0,
          startedAt: 0,
          nextCheckinAt: 0,
          createdAt: FIXED_NOW,
          updatedAt: FIXED_NOW,
          lastCheckin: null,
        };
        officeTaskFixture.tasks.unshift(task);
        status = 201;
        body = { ok: true, task };
      } else if (
        method === "POST" &&
        suffix.length === 1 &&
        suffix[0] === "stop-active"
      ) {
        const actor = String(session.nodeName || "").toLowerCase();
        const task = officeTaskFixture.tasks.find(
          (item) => item.assignee === actor && item.status === "active",
        );
        if (task) {
          Object.assign(task, {
            status: "idle",
            elapsedMs: Number(task.elapsedMs || 0) + 1500,
            startedAt: 0,
            nextCheckinAt: 0,
          });
        }
        body = { ok: true, stopped: Boolean(task) };
      } else if (method === "POST" && suffix.length === 2) {
        const [taskId, action] = suffix;
        const task = officeTaskFixture.tasks.find((item) => item.id === taskId);
        if (!task) {
          status = 404;
          body = { error: "task_not_found" };
        } else if (action === "start") {
          Object.assign(task, {
            status: "active",
            startedAt: FIXED_NOW,
            nextCheckinAt: FIXED_NOW + 4 * 60 * 1000,
          });
          body = { ok: true, task };
        } else if (action === "stop") {
          Object.assign(task, {
            status: "idle",
            elapsedMs: Number(task.elapsedMs || 0) + 1500,
            startedAt: 0,
            nextCheckinAt: 0,
          });
          body = { ok: true, task };
        } else if (action === "checkin") {
          task.lastCheckin = {
            state: requestBody.state,
            at: FIXED_NOW,
          };
          task.nextCheckinAt = FIXED_NOW + 6 * 60 * 1000;
          body = { ok: true, task };
        } else {
          status = 404;
          body = { error: "not_found" };
        }
      } else {
        status = 405;
        body = { error: "method_not_allowed" };
      }
    } else if (
      accountSessionFixture &&
      url.pathname.startsWith("/api/accounts/sessions")
    ) {
      const method = route.request().method();
      const target =
        url.pathname
          .slice("/api/accounts/sessions".length)
          .split("/")
          .filter(Boolean)[0] || "";
      accountSessionFixture.requests ||= [];
      accountSessionFixture.sessions ||= [];
      accountSessionFixture.requests.push({ method, target });
      if (!session?.sessionToken) {
        status = 401;
        body = { error: "invalid_session" };
      } else if (method === "GET") {
        body = {
          ok: true,
          sessions: accountSessionFixture.sessions,
          account: {
            nodeName: String(session.nodeName || "").toLowerCase(),
            status: "active",
            emailVerified: session.emailVerified === true,
            kind: "user",
            lastSeenAt: FIXED_NOW,
            lastEmailAt: 0,
            lastEmailStatus: "",
            lastEmailKind: "",
          },
          privacyNotice: "Only you can see this list.",
        };
      } else if (method === "DELETE" && target === "others") {
        accountSessionFixture.sessions =
          accountSessionFixture.sessions.filter((item) => item.current);
        body = { ok: true, currentRevoked: false };
      } else if (method === "DELETE" && target === "all") {
        accountSessionFixture.sessions = [];
        body = { ok: true, currentRevoked: true };
      } else if (method === "DELETE") {
        const revoked = accountSessionFixture.sessions.find(
          (item) => item.id === target,
        );
        if (!revoked) {
          status = 404;
          body = { error: "session_not_found" };
        } else {
          accountSessionFixture.sessions =
            accountSessionFixture.sessions.filter(
              (item) => item.id !== target,
            );
          body = { ok: true, currentRevoked: revoked.current === true };
        }
      } else {
        status = 405;
        body = { error: "method_not_allowed" };
      }
    } else if (url.pathname === "/api/world/office/attendance") {
      let requestBody = {};
      try {
        requestBody = route.request().postDataJSON() || {};
      } catch (_) {}
      const method = route.request().method();
      officeAttendanceRequests.push({
        method,
        headers: route.request().headers(),
        body: requestBody,
      });
      if (method === "GET") {
        body = {
          ok: true,
          visits: officeAttendanceFixture?.getVisits || [],
        };
      } else if (method === "POST" && session?.sessionToken) {
        const visits =
          requestBody.action === "out"
            ? officeAttendanceFixture?.outVisits
            : officeAttendanceFixture?.inVisits;
        body = { ok: true, visits: visits || [] };
      } else if (method === "POST") {
        status = 401;
        body = { error: "login_required" };
      } else {
        status = 405;
        body = { error: "method_not_allowed" };
      }
    } else if (url.pathname === "/api/world/office/floors") {
      officeFloorRequests.push({
        method: route.request().method(),
        url: route.request().url(),
        headers: route.request().headers(),
      });
      if (route.request().method() !== "GET") {
        status = 405;
        body = { error: "method_not_allowed" };
      } else if (!session?.sessionToken) {
        status = 401;
        body = { error: "login_required" };
      } else {
        body = {
          ok: true,
          authenticated: true,
          account: String(session.nodeName || "").toLowerCase(),
          allowedFloorIds: ["lobby", "marketing", "rooftop"],
          teams: [],
          ...(officeFloorAccess || {}),
        };
      }
      if (Number(officeFloorDelayMs) > 0) {
        await new Promise((resolve) =>
          setTimeout(resolve, Number(officeFloorDelayMs)),
        );
      }
    } else if (url.pathname === "/api/world/office/general/entry") {
      let requestBody = {};
      try {
        requestBody = route.request().postDataJSON();
      } catch (_) {}
      officeEntryRequests.push({
        method: route.request().method(),
        url: route.request().url(),
        headers: route.request().headers(),
        body: requestBody,
      });
      if (route.request().method() !== "POST") {
        status = 405;
        body = {
          ok: false,
          error: "method_not_allowed",
        };
      } else if (!session?.sessionToken) {
        status = 401;
        body = { ok: false, error: "login_required" };
      } else {
        body = {
          ok: true,
          entryTicket: OFFICE_ENTRY_TICKET,
          expiresAt: FIXED_NOW + 60_000,
        };
      }
    } else if (accountFixture && url.pathname === "/api/accounts/signup") {
      body = {
        ok: true,
        nodeName: "world-user",
        email: "world-user@example.test",
        emailVerified: false,
      };
    } else if (accountFixture && url.pathname === "/api/accounts/login") {
      activeSession = {
        kind: "user",
        ok: true,
        nodeName: "world-user",
        email: "world-user@example.test",
        status: "registered",
        emailVerified: true,
        isAdmin: false,
        sessionToken: "world-session-token",
        nodes: [],
      };
      body = activeSession;
    }
    if (repositoryFixture) {
      const codeOid = (
        repositoryFixture.mergePublished ? "f" : "a"
      ).repeat(40);
      const pullOid = (
        repositoryFixture.mergePublished ? "d" : "b"
      ).repeat(40);
      const stateHash = "c".repeat(64);
      const pullMarkdown = (number, title, head) => `---
schema: forkmesh-pull-v1
number: ${number}
title: "${title}"
status: ${
  repositoryFixture.mergePublished && number === 44 ? "merged" : "open"
}
authorName: Alice
base: main
head: ${head}
derive: branch
creationBaseOid: ${"a".repeat(40)}
creationHeadOid: ${"e".repeat(40)}
---
This description came from the exact pull metadata commit.`;
      const longContext = Array.from(
        { length: 90 },
        (_, index) => ` line ${index + 1}`,
      ).join("\n");
      const patch = `diff --git a/src/alpha.js b/src/alpha.js
index 1111111..2222222 100644
--- a/src/alpha.js
+++ b/src/alpha.js
@@ -1,90 +1,91 @@
${longContext}
+const worldReviewMarker = "world-pr-diff-visible";
diff --git a/src/beta.js b/src/beta.js
index 3333333..4444444 100644
--- a/src/beta.js
+++ b/src/beta.js
@@ -1,90 +1,90 @@
${longContext}
-const oldValue = false;
+const oldValue = true;`;
      const repoBase = "/api/repo/forkmesh/forkmesh";
      const pullMergeMatch = url.pathname.match(
        /^\/api\/repo\/forkmesh\/forkmesh\/pulls\/44\/merge$/,
      );
      if (
        repositoryFollowerFixture &&
        url.pathname === `${repoBase}/about`
      ) {
        if (!Array.isArray(repositoryFollowerFixture.requests)) {
          repositoryFollowerFixture.requests = [];
        }
        repositoryFollowerFixture.requests.push({
          path: url.pathname,
          method: route.request().method(),
        });
        body = repositoryFollowerFixture.unavailable
          ? { ok: false, error: "fixture_unavailable" }
          : {
              ok: true,
              description: "Peer-to-peer code hosting mesh",
              fediverse: {
                enabled: true,
                handle: "@forkmesh.forkmesh@forkmesh.com",
                followers: Number(repositoryFollowerFixture.followers ?? 0),
                followersList: repositoryFollowerFixture.followersList || [],
              },
            };
      } else if (
        repositoryStarFixture &&
        url.pathname === `${repoBase}/star`
      ) {
        const method = route.request().method();
        const authorization =
          route.request().headers().authorization || "";
        let requestBody = null;
        if (route.request().postData()) {
          try {
            requestBody = route.request().postDataJSON();
          } catch (_) {
            requestBody = "invalid-json";
          }
        }
        if (!Array.isArray(repositoryStarFixture.requests)) {
          repositoryStarFixture.requests = [];
        }
        repositoryStarFixture.requests.push({
          path: url.pathname,
          method,
          authorization,
          body: requestBody,
        });
        repositoryStarFixture.count = Math.max(
          0,
          Number(repositoryStarFixture.count) || 0,
        );
        repositoryStarFixture.starred =
          repositoryStarFixture.starred === true;
        if (method === "GET") {
          body = {
            ok: true,
            count: repositoryStarFixture.count,
            starred: Boolean(session && repositoryStarFixture.starred),
          };
        } else if (!["POST", "DELETE"].includes(method)) {
          status = 405;
          body = { error: "method_not_allowed" };
        } else if (!session?.sessionToken || !authorization) {
          status = 401;
          body = { error: "invalid_session" };
        } else if (repositoryStarFixture.failMutation) {
          status = 503;
          body = { error: "fixture_unavailable" };
        } else {
          const nextStarred = method === "POST";
          if (nextStarred !== repositoryStarFixture.starred) {
            repositoryStarFixture.count = Math.max(
              0,
              repositoryStarFixture.count + (nextStarred ? 1 : -1),
            );
          }
          repositoryStarFixture.starred = nextStarred;
          body = {
            ok: true,
            count: repositoryStarFixture.count,
            starred: repositoryStarFixture.starred,
          };
        }
      } else if (pullMergeMatch && route.request().method() === "POST") {
        let request = {};
        try {
          request = route.request().postDataJSON();
        } catch (_) {}
        const expectedRequest =
          request?.schemaVersion === 1 &&
          request?.type === "forkmesh.pull-merge-v1" &&
          request?.pullNumber === 44 &&
          /^[A-Za-z0-9_-]{12,80}$/.test(String(request?.requestId || "")) &&
          request?.expectedBaseOid === "a".repeat(40) &&
          request?.expectedHeadOid === "e".repeat(40) &&
          request?.expectedPullsOid === "b".repeat(40);
        if (!expectedRequest) {
          status = 400;
          body = { error: "invalid_request" };
        } else if (repositoryFixture.mergeOutcome === "forbidden") {
          status = 403;
          body = { error: "forbidden" };
        } else if (repositoryFixture.mergeOutcome === "conflict") {
          status = 409;
          body = {
            ok: false,
            status: "failed",
            requestId: request.requestId,
            error: "merge_conflict",
          };
        } else if (repositoryFixture.mergeOutcome === "stale") {
          status = 409;
          body = {
            ok: false,
            status: "failed",
            requestId: request.requestId,
            error: "stale_base",
          };
        } else if (repositoryFixture.mergeOutcome === "retry") {
          const attempt = Number(repositoryFixture.mergeRequestCount || 0);
          repositoryFixture.mergeRequestCount = attempt + 1;
          if (attempt < 8) {
            status = 202;
            body = {
              ok: true,
              status: "processing",
              requestId: request.requestId,
            };
          } else {
            status = 409;
            body = {
              ok: false,
              status: "failed",
              requestId: request.requestId,
              error: "merge_conflict",
            };
          }
        } else {
          const attempt = Number(repositoryFixture.mergeRequestCount || 0);
          repositoryFixture.mergeRequestCount = attempt + 1;
          if (attempt === 0) {
            status = 202;
            body = {
              ok: true,
              status: "processing",
              requestId: request.requestId,
            };
          } else {
            status = 200;
            body = {
              ok: true,
              status: "merged",
              requestId: request.requestId,
              published: true,
              baseBefore: "a".repeat(40),
              head: "e".repeat(40),
              pullsBefore: "b".repeat(40),
              baseAfter: "f".repeat(40),
              pullsAfter: "d".repeat(40),
            };
            repositoryFixture.mergePublished = true;
          }
        }
      } else if (url.pathname === "/api/orgs/forkmesh/repos") {
        body = {
          ok: true,
          repos: [{ node: "mirror2", repo: "forkmesh" }],
        };
      } else if (url.pathname === "/api/repositories") {
        const repositoryOwners =
          repositoryFixture.staleOfflineAlias ||
          repositoryFixture.sourceUserOwner
          ? ["jett", "mirror2", "mirror3"]
          : ["mirror2", "mirror3"];
        body = {
          repositories: repositoryOwners.map((owner) => {
            const stale =
              repositoryFixture.staleOfflineAlias && owner === "jett";
            const source = owner === "jett";
            const conflicting =
              repositoryFixture.conflictingHealthyAlias &&
              owner === "mirror3";
            const missingStateHash =
              repositoryFixture.missingHealthyStateHash &&
              owner === "mirror3";
            return {
              owner,
              name: "forkmesh",
              source: source ? "local-node" : "remote-clone",
              nodeId:
                repositoryFixture.sourceUserOwner && source
                  ? "source-node-id"
                  : owner,
              liveHost: !stale,
              commit: stale || conflicting ? "d".repeat(40) : codeOid,
              stateHash: missingStateHash
                ? ""
                : stale || conflicting
                  ? "e".repeat(64)
                  : stateHash,
              pullCount: stale ? 1 : 2,
              updatedAt: stale ? FIXED_NOW + 1000 : FIXED_NOW,
            };
          }),
        };
      } else if (url.pathname === `${repoBase}/mirrors`) {
        const mirrorNodes = ["mirror2", "mirror3"];
        if (repositoryFixture.staleOfflineAlias) mirrorNodes.push("jett");
        if (repositoryFixture.sourceUserOwner) mirrorNodes.unshift("forkmesh");
        // Two offline machines that differ only in when they were last
        // observed: one inside the World's 24h activity window, one well
        // outside it.
        if (repositoryFixture.dormantNodes) {
          mirrorNodes.push("mirror8", "mirror9");
        }
        body = {
          ok: true,
          owner: "forkmesh",
          repo: "forkmesh",
          mirrors: mirrorNodes.map((node) => {
            if (node === "mirror8" || node === "mirror9") {
              return {
                node,
                id: node,
                status: "offline",
                integrity: "unknown",
                cloneAvailable: false,
                commit: "d".repeat(40),
                branch: "main",
                version: "0.7.0",
                lastSync:
                  node === "mirror8"
                    ? FIXED_NOW - 3 * 60 * 60 * 1000
                    : FIXED_NOW - 30 * 60 * 60 * 1000,
              };
            }
            const stale = node === "jett";
            const conflicting =
              repositoryFixture.conflictingHealthyAlias &&
              node === "mirror3";
            return {
              node,
              id: node === "forkmesh" ? "source-node-id" : node,
              status: stale ? "offline" : "online",
              integrity: stale ? "healing" : "ok",
              cloneAvailable: !stale,
              behind: stale,
              commit: stale || conflicting ? "d".repeat(40) : codeOid,
              branch: "main",
              pullCount: stale ? 1 : 2,
              version: "0.7.0",
              ...(node === "mirror2"
                ? {
                    lastCommitMessage: "Ship the world-edge portals",
                    lastCommitAuthorName: "Ada Lovelace",
                    syncAgeMs: 2 * 60 * 60 * 1000,
                  }
                : {}),
            };
          }),
        };
      } else if (url.pathname === `${repoBase}/branches`) {
        if (
          repositoryFixture.rejectPullMetadataAfterIssueFanout &&
          repositoryFixture.issueTreeRequestStarted
        ) {
          status = 503;
          body = { ok: false, error: "mirror_capacity_exhausted" };
        } else {
          body = {
            ok: true,
            branches: repositoryFixture.invalidPullBranch
              ? [{ name: "main", commit: codeOid }]
              : [
                  { name: "main", commit: codeOid },
                  { name: "forkmesh/pulls", commit: pullOid },
                ],
          };
        }
      } else if (url.pathname === `${repoBase}/tree`) {
        const treePath = url.searchParams.get("path") || "";
        const ref = url.searchParams.get("ref") || "";
        if (treePath === "") {
          body = {
            ok: true,
            commit: codeOid,
            entries: [
              {
                name: "src",
                path: "src",
                type: "tree",
                size: 4096,
                author: "Alice",
              },
              {
                name: "README.md",
                path: "README.md",
                type: "blob",
                size: 1200,
                author: "Alice",
              },
            ],
          };
        } else if (treePath === "src" && ref === codeOid) {
          body = {
            ok: true,
            commit: codeOid,
            entries: [
              {
                name: "[id]+C++.tsx",
                path: "src/[id]+C++.tsx",
                type: "blob",
                size: 4096,
                author: "Alice",
              },
            ],
          };
        } else if (treePath.startsWith(".forkmesh/issues")) {
          repositoryFixture.issueTreeRequestStarted = true;
          const issueDelay = Math.max(
            0,
            Math.min(
              5000,
              Number(repositoryFixture.issueTreeDelayMs) || 0,
            ),
          );
          if (issueDelay) {
            await new Promise((resolve) => setTimeout(resolve, issueDelay));
          }
          body = { ok: true, commit: codeOid, entries: [] };
        } else if (
          treePath === ".forkmesh/pulls" &&
          !repositoryFixture.invalidPullBranch &&
          ref === pullOid
        ) {
          body = {
            ok: true,
            commit: pullOid,
            entries: [
              { name: "44", path: ".forkmesh/pulls/44", type: "tree" },
              { name: "43", path: ".forkmesh/pulls/43", type: "tree" },
            ],
          };
        } else if (
          treePath === ".forkmesh/pulls/44" &&
          !repositoryFixture.invalidPullBranch &&
          ref === pullOid
        ) {
          body = {
            ok: true,
            commit: pullOid,
            entries: [
              {
                name: "0001-review.md",
                path: ".forkmesh/pulls/44/0001-review.md",
                type: "blob",
              },
            ],
          };
        } else {
          status = 404;
          body = { ok: false, error: "not_found" };
        }
      } else if (url.pathname === `${repoBase}/sizes`) {
        body = {
          ok: true,
          commit: codeOid,
          name: "",
          type: "directory",
          size: 5296,
          fileCount: 2,
          children: [
            {
              name: "src",
              type: "directory",
              size: 4096,
              children: [
                {
                  name: "[id]+C++.tsx",
                  path: "src/[id]+C++.tsx",
                  type: "file",
                  size: 4096,
                },
              ],
            },
            {
              name: "README.md",
              path: "README.md",
              type: "file",
              size: 1200,
            },
          ],
        };
      } else if (url.pathname === `${repoBase}/stats`) {
        body = {
          ok: true,
          commit: codeOid,
          fileCount: 2,
          contributorCount: 1,
          contributors: [{ name: "Alice", commits: 4 }],
        };
      } else if (url.pathname === `${repoBase}/blob`) {
        const path = url.searchParams.get("path") || "";
        const ref = url.searchParams.get("ref") || "";
        if (repositoryFixture.invalidPullBranch || ref !== pullOid) {
          status = 404;
          body = { ok: false, error: "not_found" };
        } else if (path === ".forkmesh/pulls/44/pull.md") {
          body = {
            ok: true,
            commit: pullOid,
            encoding: "utf8",
            content: pullMarkdown(44, "Review inside the World", "review-ui"),
          };
        } else if (path === ".forkmesh/pulls/44/changes.patch") {
          body = {
            ok: true,
            commit: pullOid,
            encoding: "utf8",
            content: patch,
          };
        } else {
          status = 404;
          body = { ok: false, error: "not_found" };
        }
      } else if (url.pathname === `${repoBase}/blobs`) {
        const paths = url.searchParams.getAll("path");
        const ref = url.searchParams.get("ref") || "";
        const exactSourceFile =
          ref === codeOid &&
          paths.length === 1 &&
          paths[0] === "src/[id]+C++.tsx";
        const exactPullMetadata =
          ref === pullOid &&
          paths.length > 0 &&
          paths.every((item) =>
            /^\.forkmesh\/pulls\/(?:43|44)\/(?:pull\.md|changes\.patch|0001-review\.md)$/.test(
              item,
            ),
          );
        if (
          (!exactSourceFile && !exactPullMetadata) ||
          (repositoryFixture.invalidPullBranch && exactPullMetadata)
        ) {
          status = 404;
          body = { ok: false, error: "not_found" };
        } else if (exactSourceFile) {
          body = {
            ok: true,
            commit: codeOid,
            blobs: {
              "src/[id]+C++.tsx": {
                ok: true,
                encoding: "utf8",
                content: 'export const routeName = "[id]+C++";\n',
              },
            },
          };
        } else {
          const blobs = {};
          paths.forEach((item) => {
            if (item === ".forkmesh/pulls/44/pull.md") {
              blobs[item] = {
                ok: true,
                encoding: "utf8",
                content: pullMarkdown(44, "Review inside the World", "review-ui"),
              };
            } else if (item === ".forkmesh/pulls/43/pull.md") {
              blobs[item] = repositoryFixture.missingPullMetadata
                ? null
                : {
                    ok: true,
                    encoding: "utf8",
                    content: pullMarkdown(
                      43,
                      "Earlier exact review",
                      "earlier",
                    ),
                  };
            } else if (item === ".forkmesh/pulls/44/changes.patch") {
              blobs[item] = { ok: true, encoding: "utf8", content: patch };
            } else if (item === ".forkmesh/pulls/44/0001-review.md") {
              blobs[item] = {
                ok: true,
                encoding: "utf8",
                content: `---
type: review
author: peer-reviewer
authorName: Peer Reviewer
state: approved
ts: 2026-07-30T12:00:00Z
---
The exact reviewed commits are approved.`,
              };
            } else {
              blobs[item] = null;
            }
          });
          body = { ok: true, commit: pullOid, blobs };
        }
      }
    }
    if (metadataCapacityRelease) {
      repositoryFixture.metadataActive = Math.max(
        0,
        Number(repositoryFixture.metadataActive || 0) - 1,
      );
      metadataCapacityRelease();
    }
    return route.fulfill({
      status,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.routeWebSocket("**/api/world/ws*", (socket) => {
    if (worldSocketHandler) {
      worldSocketHandler(socket, socketId);
      return;
    }
    socket.send(
      JSON.stringify({
        type: "welcome",
        id: socketId,
        peers: [],
      }),
    );
  });
}

async function waitForWorld(page, url = "/world/") {
  await page.goto(url);
  await waitForWorldReady(page);
}

// Boot requests nothing from the repository district. A character has to stand
// on its ground circle before the catalog, the flagship tree, or any size map
// is fetched, so a spec that inspects repository state walks there first —
// exactly what a visitor does — instead of reaching for the loader directly.
async function enterRepositoryDistrict(page) {
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setSpawn({ x: 130, z: 0, space: "town-square" });
  });
}

test("authorized agent sessions create one status-lit world robot each", async ({
  page,
}) => {
  await prepareWorldPage(page, "session-agent-robots");
  await waitForWorld(page);
  const sessions = [
    {
      id: "agent-session-claude",
      provider: "claude-code",
      status: "running",
      localAgentId: 41,
      title: "Claude working session",
    },
    {
      id: "agent-session-codex",
      provider: "codex",
      status: "queued",
      localAgentId: 42,
      title: "Codex queued session",
    },
  ];
  const spawned = await page
    .locator("forkmesh-world")
    .evaluate((shell, roster) => {
      shell.world.setAgentBotAccess(true);
      shell.world.updateMirrorAgentTasks(roster);
      return roster.map((session) => {
        const provider = session.provider === "codex" ? "codex" : "claude";
        const robot = shell.world.scene.getObjectByName(
          `${provider}-agent-droid-${session.id}`,
        );
        return {
          found: Boolean(robot),
          aboveGround: Number(robot?.position?.y || 0) > 0.38,
          sessionId: robot?.children
            ?.find((child) => child?.isMesh)
            ?.userData?.repositoryAgentSession?.id,
        };
      });
    }, sessions);
  expect(spawned).toEqual([
    { found: true, aboveGround: true, sessionId: "agent-session-claude" },
    { found: true, aboveGround: true, sessionId: "agent-session-codex" },
  ]);

  const remaining = await page.locator("forkmesh-world").evaluate(
    (shell, roster) => {
      shell.world.updateMirrorAgentTasks(roster.slice(1));
      return {
        claude: Boolean(
          shell.world.scene.getObjectByName(
            `claude-agent-droid-${roster[0].id}`,
          ),
        ),
        codex: Boolean(
          shell.world.scene.getObjectByName(
            `codex-agent-droid-${roster[1].id}`,
          ),
        ),
      };
    },
    sessions,
  );
  expect(remaining).toEqual({ claude: false, codex: true });

  const hidden = await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setAgentBotAccess(false);
    return shell.world.scene
      .getObjectByName("codex-agent-droid-agent-session-codex")
      ?.visible;
  });
  expect(hidden).toBe(false);
});

async function waitForWorldReady(page) {
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return (
      Boolean(shell?.world?.renderer?.domElement) &&
      Number(shell.world.renderer.info?.render?.frame || 0) > 0
    );
  });
  await page.waitForFunction(() => {
    const loading = document.querySelector("[data-world-loading]");
    return !loading || loading.dataset.ready === "true";
  });
  await page.evaluate(() => document.fonts?.ready);
}

async function pauseWorld(page) {
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setPaused(true);
  });
}

async function measureWorldInputResponses(page) {
  return page.locator("forkmesh-world").evaluate(
    async (shell, sampleCount) => {
      const world = shell.world;
      const canvas = world.renderer.domElement;
      const island = shell.querySelector("[data-world-hud-island]");
      const chatToggle = island?.querySelector("[data-world-island-toggle]");
      const chatBody = island?.querySelector("#fullChatMessages");
      const thumbstick = shell.querySelector("[data-world-thumbstick]");
      const coarsePointer = matchMedia("(pointer: coarse)").matches;
      if (
        !canvas ||
        !island ||
        !chatToggle ||
        !chatBody ||
        (coarsePointer && !thumbstick)
      ) {
        throw new Error("World interaction surfaces are not mounted");
      }

      const nextFrame = () =>
        new Promise((resolve) => window.requestAnimationFrame(resolve));
      const settleFrames = async (count = 2) => {
        for (let frame = 0; frame < count; frame += 1) await nextFrame();
      };
      const waitForPresentedResponse = (action, responded) =>
        new Promise((resolve, reject) => {
          const startedAt = performance.now();
          const deadline = startedAt + 1_000;
          action();
          const observe = () => {
            if (responded()) {
              // The observed scene/DOM mutation is ready for this paint. The
              // following callback confirms that the browser presented it.
              window.requestAnimationFrame(() => {
                resolve(performance.now() - startedAt);
              });
              return;
            }
            if (performance.now() >= deadline) {
              reject(new Error("World input produced no visible response"));
              return;
            }
            window.requestAnimationFrame(observe);
          };
          window.requestAnimationFrame(observe);
        });
      const pointerEvent = (type, pointerId, x, y, buttons) =>
        new PointerEvent(type, {
          bubbles: true,
          cancelable: true,
          composed: true,
          pointerId,
          pointerType: coarsePointer ? "touch" : "mouse",
          isPrimary: true,
          button: type === "pointerdown" ? 0 : -1,
          buttons,
          clientX: x,
          clientY: y,
        });
      const percentile95 = (values) => {
        const sorted = [...values].sort((left, right) => left - right);
        return sorted[Math.ceil(sorted.length * 0.95) - 1];
      };

      world.setCameraView({
        mode: "third-person",
        yaw: 0.65,
        pitch: 0.5,
        zoom: 1,
      });
      world.setSpawn({ x: 0, y: 0.38, z: 18, heading: 0 });
      await settleFrames(3);

      const camera = [];
      for (let sample = 0; sample < sampleCount; sample += 1) {
        const rect = canvas.getBoundingClientRect();
        const startX = rect.left + rect.width * 0.42;
        const startY = rect.top + rect.height * 0.42;
        const direction = sample % 2 ? -1 : 1;
        const pointerId = 5_490 + sample;
        const before = world.camera.quaternion.clone();
        const beforeFrame = world.renderer.info.render.frame;
        camera.push(
          await waitForPresentedResponse(
            () => {
              canvas.dispatchEvent(
                pointerEvent(
                  "pointerdown",
                  pointerId,
                  startX,
                  startY,
                  1,
                ),
              );
              canvas.dispatchEvent(
                pointerEvent(
                  "pointermove",
                  pointerId,
                  startX + direction * 48,
                  startY + direction * 12,
                  1,
                ),
              );
            },
            () =>
              world.renderer.info.render.frame > beforeFrame &&
              world.camera.quaternion.angleTo(before) > 0.001,
          ),
        );
        window.dispatchEvent(
          pointerEvent(
            "pointercancel",
            pointerId,
            startX + direction * 48,
            startY + direction * 12,
            0,
          ),
        );
        await settleFrames();
      }

      const avatar = [];
      // Synthetic PointerEvents do not enter the browser's active-pointer
      // registry, so capture would throw before the real thumbstick handler
      // reaches its movement update. Capture itself has no visual work; make
      // it a no-op while these mobile samples traverse the rest of the actual
      // handler, including its layout reads and handle paint.
      const setPointerCapture = thumbstick?.setPointerCapture;
      if (coarsePointer) thumbstick.setPointerCapture = () => {};
      for (let sample = 0; sample < sampleCount; sample += 1) {
        const direction = sample % 2 ? 1 : -1;
        const before = world.player.position.clone();
        const beforeFrame = world.renderer.info.render.frame;
        const thumbstickBounds = coarsePointer
          ? thumbstick.getBoundingClientRect()
          : null;
        const pointerId = 5_590 + sample;
        avatar.push(
          await waitForPresentedResponse(
            () => {
              if (coarsePointer) {
                thumbstick.dispatchEvent(
                  pointerEvent(
                    "pointerdown",
                    pointerId,
                    thumbstickBounds.left + thumbstickBounds.width / 2,
                    thumbstickBounds.top +
                      thumbstickBounds.height / 2 +
                      direction * thumbstickBounds.height * 0.35,
                    1,
                  ),
                );
              } else {
                window.dispatchEvent(
                  new KeyboardEvent("keydown", {
                    bubbles: true,
                    cancelable: true,
                    code: direction < 0 ? "KeyW" : "KeyS",
                    key: direction < 0 ? "w" : "s",
                  }),
                );
              }
            },
            () =>
              world.renderer.info.render.frame > beforeFrame &&
              world.player.position.distanceToSquared(before) > 0.000001,
          ),
        );
        if (coarsePointer) {
          thumbstick.dispatchEvent(
            pointerEvent(
              "pointercancel",
              pointerId,
              thumbstickBounds.left + thumbstickBounds.width / 2,
              thumbstickBounds.top + thumbstickBounds.height / 2,
              0,
            ),
          );
        } else {
          window.dispatchEvent(
            new KeyboardEvent("keyup", {
              bubbles: true,
              cancelable: true,
              code: direction < 0 ? "KeyW" : "KeyS",
              key: direction < 0 ? "w" : "s",
            }),
          );
        }
        await settleFrames();
      }
      if (coarsePointer) thumbstick.setPointerCapture = setPointerCapture;

      const chat = [];
      for (let sample = 0; sample < sampleCount; sample += 1) {
        shell.setHudIslandExpanded(false);
        await settleFrames();
        chat.push(
          await waitForPresentedResponse(
            () => chatToggle.click(),
            () => {
              const style = getComputedStyle(chatBody);
              const bounds = chatBody.getBoundingClientRect();
              return (
                island.dataset.expanded === "true" &&
                chatToggle.getAttribute("aria-expanded") === "true" &&
                style.display !== "none" &&
                style.visibility !== "hidden" &&
                Number(style.opacity) > 0 &&
                bounds.width > 100
              );
            },
          ),
        );
        shell.setHudIslandExpanded(false);
        await settleFrames();
      }

      return {
        cameraMs: percentile95(camera),
        avatarMs: percentile95(avatar),
        chatMs: percentile95(chat),
        samples: { camera, avatar, chat },
      };
    },
    WORLD_INPUT_RESPONSE_SAMPLES,
  );
}

async function expandWorldRightRail(page) {
  const launcher = page.locator("[data-world-shirt-badge]");
  await launcher.focus();
  await expect(page.locator("[data-world-hud]")).toHaveAttribute(
    "data-hud-expanded",
    "true",
  );
}

test("@critical a slow World module keeps the one static loader and distant APIs stay deferred", async ({
  page,
}) => {
  const apiPaths = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (url.pathname.startsWith("/api/")) apiPaths.push(url.pathname);
  });
  await prepareWorldPage(page, "progressive-first-paint");
  await page.route("**/world/world.js", async (route) => {
    await new Promise((resolve) => setTimeout(resolve, 900));
    await route.continue();
  });

  await page.goto("/world/", { waitUntil: "commit" });
  await expect(page.locator("[data-world-boot-shell]")).toHaveCount(0);
  await expect(page.locator("[data-world-loading]")).toHaveCount(1);
  await expect(
    page.locator("[data-world-loading] [data-world-loading-steps] li"),
  ).toHaveCount(7);
  await expect(page.locator("[data-world-loading]")).toContainText(
    "Opening ForkMesh World",
  );
  await expect(page.locator("[data-world-root]")).toHaveCount(0);
  await expect(page.locator("forkmesh-world")).toHaveAttribute(
    "data-world-booting",
    "",
  );

  await expect(page.locator("[data-world-root]")).toHaveCount(1);
  await expect(page.locator("[data-world-loading]")).toHaveCount(1);
  await expect(page.locator("[data-world-root] [data-world-loading]")).toHaveCount(0);

  await waitForWorldReady(page);
  await expect(page.locator("[data-world-loading]")).toHaveCount(0);
  await expect(page.locator("[data-world-boot]")).toHaveCount(0);
  await expect(page.locator("forkmesh-world")).not.toHaveAttribute(
    "data-world-booting",
    "",
  );
  expect(
    apiPaths.filter((path) => path === "/api/accounts/sessions"),
  ).toHaveLength(1);
  for (const distantPath of [
    "/api/network/overview",
    "/api/world/instances",
    "/api/world/organizations",
    "/api/world/fediverse",
    "/api/world/events",
    "/api/status/world",
  ]) {
    expect(apiPaths).not.toContain(distantPath);
  }
});

test("an App cookie session that outlasts the startup budget still signs World in", async ({
  page,
}) => {
  await prepareWorldPage(page, "late-cookie-session");
  const sessionRequests = [];
  // Registered after prepareWorldPage, so it wins over the shared API mock:
  // this browser holds only the App's HttpOnly cookie (no local marker), and
  // the owner-only sessions read answers well past the 250 ms budget.
  await page.route("**/api/accounts/sessions", async (route) => {
    sessionRequests.push(route.request().method());
    await new Promise((resolve) => setTimeout(resolve, 900));
    await route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify({
        ok: true,
        sessions: [],
        account: {
          nodeName: "cookie-user",
          status: "active",
          emailVerified: true,
          kind: "user",
          lastSeenAt: FIXED_NOW,
        },
      }),
    });
  });
  let documentLoads = 0;
  page.on("framenavigated", (frame) => {
    if (frame === page.mainFrame()) documentLoads += 1;
  });

  await page.goto("/world/");
  await expect
    .poll(() =>
      page
        .evaluate(
          () => document.querySelector("forkmesh-world")?.identity?.id || "",
        )
        .catch(() => ""),
    )
    .toBe("account:cookie-user");
  await waitForWorldReady(page);

  const session = await page.evaluate(() =>
    JSON.parse(localStorage.getItem("forkmesh.session") || "null"),
  );
  expect(session).toMatchObject({
    nodeName: "cookie-user",
    sessionToken: "cookie",
  });
  // One reload adopts the account; the reloaded page reads the marker
  // within budget and never asks again.
  expect(sessionRequests).toEqual(["GET"]);
  expect(documentLoads).toBe(2);
});

test("a guest HUD launcher wears the same painted face as their avatar", async ({
  page,
}) => {
  await prepareWorldPage(page, "guest-launcher-face");
  await waitForWorld(page);
  const badge = page.locator("[data-world-shirt-avatar]");
  await expect(badge).toBeVisible();
  const painted = await page.evaluate(async () => {
    const shell = document.querySelector("forkmesh-world");
    const scene = await import("/world/world-avatar-face.js");
    const image = document.querySelector("[data-world-shirt-avatar]");
    return {
      accountStatus: shell.identity.accountStatus,
      src: image.getAttribute("src").slice(0, 22),
      decodedWidth: image.naturalWidth,
      decodedHeight: image.naturalHeight,
      matchesAvatarFace:
        image.getAttribute("src") ===
        scene.proceduralAvatarFaceDataURL(shell.identity.id),
    };
  });
  expect(painted).toEqual({
    accountStatus: "Guest",
    src: "data:image/png;base64,",
    decodedWidth: 128,
    decodedHeight: 128,
    matchesAvatarFace: true,
  });
  // A one-second identity tick (the local-time privacy badge) must not blank
  // the portrait back to the empty disc.
  const afterTick = await page.locator("forkmesh-world").evaluate((shell) => {
    shell.updateIdentityUI();
    const image = document.querySelector("[data-world-shirt-avatar]");
    return { hidden: image.hidden, src: image.getAttribute("src").slice(0, 22) };
  });
  expect(afterTick).toEqual({ hidden: false, src: "data:image/png;base64," });
});

test("verified code landings write the publisher and commit context into the chat feed", async ({
  page,
}) => {
  await prepareWorldPage(page, "code-landing-details");
  await waitForWorld(page);
  const lines = await page.locator("forkmesh-world").evaluate(async (shell) => {
    const commit = "b".repeat(40);
    const seen = [];
    shell.notifyChatArea = (message) => {
      seen.push(String(message));
      return true;
    };
    shell.activityNoticesEnabledAt = 0;
    shell.liveMirrorNodes = [
      {
        name: "mirror2",
        ownerUser: "ada",
        repositories: [
          {
            owner: "forkmesh",
            name: "forkmesh",
            commit,
            branch: "main",
            lastCommitMessage: "Show richer landing details",
            lastCommitAuthorName: "Ada Lovelace",
            changedFiles: ["src/world.js", "docs/onboarding.md"],
          },
        ],
      },
    ];
    shell.mirrorPushPublisherProfile = async () => ({
      name: "ada",
      avatarPng:
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScL/2QAAAABJRU5ErkJggg==",
    });
    await shell.announceMirrorPushLanding({
      node: "mirror2",
      owner: "forkmesh",
      repo: "forkmesh",
      commit: commit.slice(0, 12),
    });
    return seen;
  });
  expect(lines).toHaveLength(1);
  const line = lines[0];
  expect(line).toContain("Fresh code landed on “forkmesh/forkmesh”.");
  expect(line).toContain("bbbbbbbbbbbb · main · mirror2");
  expect(line).toContain("Message · Show richer landing details");
  expect(line).toContain("Git author · Ada Lovelace");
  expect(line).toContain("Published by · @ada");
  expect(line).toContain("Changed · src/world.js · docs/onboarding.md");
  await expect(page.locator("[data-world-activity-stream]")).toHaveCount(0);
});

async function moveToOfficeEntrance(page, { unpause = false } = {}) {
  await page.locator("forkmesh-world").evaluate(
    (shell, { position, shouldUnpause }) => {
      if (shouldUnpause) shell.world.setPaused(false);
      shell.world.clearFocus();
      shell.world.player.position.set(...position);
    },
    {
      position: OFFICE_ENTRANCE_POSITION,
      shouldUnpause: unpause,
    },
  );
}

async function walkIntoOffice(page) {
  await moveToOfficeEntrance(page, { unpause: true });
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setControl("forward", true);
  });
  try {
    await waitForOfficeEntry(page);
  } finally {
    await page.locator("forkmesh-world").evaluate((shell) => {
      shell.world.setControl("forward", false);
    });
  }
}

async function officeSceneState(page) {
  return page.locator("forkmesh-world").evaluate((shell) => {
    const slidingDoorCount = [
      "forkmesh-office-sliding-door-left",
      "forkmesh-office-sliding-door-right",
    ].filter((name) => shell.world.scene.getObjectByName(name)).length;
    const campus = shell.world.scene.getObjectByName(
      "forkmesh-office-landscaping",
    );
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior",
    );
    return {
      slidingDoorCount,
      hasHingedDoor: Boolean(
        shell.world.scene.getObjectByName("forkmesh-office-door-pivot"),
      ),
      hasKeypad: Boolean(
        shell.world.scene.getObjectByName("forkmesh-office-keypad"),
      ),
      campusVisible: campus?.visible === true,
      interiorVisible: interior?.visible === true,
      space: shell.world.getPosition().space,
    };
  });
}

async function waitForOfficeEntry(page) {
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) => ({
      active: shell.officeController?.active === true,
      space: shell.world.getPosition().space,
    }))
  ).toEqual({
    active: true,
    space: "office-lobby",
  });
}

async function openWorldPullReview(page, number = 44) {
  await waitForWorld(page);
  await enterRepositoryDistrict(page);
  await pauseWorld(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return shell?.repositoryMapState === "ready" &&
      shell?.activeRepository?.owner === "forkmesh" &&
      shell?.activeRepository?.repo === "forkmesh";
  });
  await page.locator("forkmesh-world").evaluate((shell, pullNumber) => {
    shell.openRepositoryRecordWebWorkbench("pull", {
      owner: "forkmesh",
      name: "forkmesh",
      number: pullNumber,
    });
  }, number);
  const workbench = page.frameLocator(
    "[data-world-repository-web-workbench='pull'] iframe",
  );
  await expect(
    workbench.locator("[data-repo-record-detail='pulls']"),
  ).toBeVisible();
  return workbench;
}

async function openWorldPullList(page) {
  await waitForWorld(page);
  await enterRepositoryDistrict(page);
  await pauseWorld(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return shell?.repositoryMapState === "ready" &&
      shell?.activeRepository?.owner === "forkmesh" &&
      shell?.activeRepository?.repo === "forkmesh";
  });
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.openRepositoryRecordWebWorkbench("pull", {
      owner: "forkmesh",
      name: "forkmesh",
    });
  });
  const workbench = page.frameLocator(
    "[data-world-repository-web-workbench='pull'] iframe",
  );
  await expect(workbench.locator("[data-repo-pulls]")).toBeVisible();
  return workbench;
}

async function openWorldRepositoryExplorer(page) {
  await waitForWorld(page);
  await enterRepositoryDistrict(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return shell?.repositoryMapState === "ready" &&
      shell?.activeRepository?.owner === "forkmesh" &&
      shell?.activeRepository?.repo === "forkmesh";
  });
  await page.locator("forkmesh-world").evaluate((shell) =>
    shell.openLandmark("repositories"),
  );
}

test("World prompt button copies owner deploy and member PR workflows", async ({
  page,
}) => {
  test.setTimeout(60_000);
  await page.addInitScript(() => {
    window.__worldCopiedPrompts = [];
    Object.defineProperty(navigator, "clipboard", {
      configurable: true,
      value: {
        writeText: async (text) => {
          window.__worldCopiedPrompts.push(text);
        },
      },
    });
  });
  await prepareWorldPage(page, "world-mcp-prompt", {
    session: {
      nodeName: "jett",
      sessionToken: "playwright-jett-session",
    },
  });
  await waitForWorld(page);

  const promptButton = page.getByRole("button", {
    name: "Copy MCP task prompt",
  });
  await expect(promptButton).toContainText("Prompt");
  await page.locator("forkmesh-world").evaluate(async (shell) => {
    shell.sessionAuthenticated = true;
    shell.organizations = [{ name: "forkmesh", role: "owner" }];
    shell.postJSON = async (path, body) => {
      window.__worldPromptRequest = { path, body };
      return { token: "fmbot_owner_test" };
    };
    await shell.copyMcpTaskPrompt(
      shell.querySelector("[data-world-mcp-prompt]"),
    );
  });
  await expect.poll(() =>
    page.evaluate(() => window.__worldCopiedPrompts.length)
  ).toBe(1);
  await expect(promptButton).toBeEnabled();

  const owner = await page.evaluate(() => ({
    prompt: window.__worldCopiedPrompts[0],
    request: window.__worldPromptRequest,
  }));
  expect(owner.request.path).toBe("/api/orgs/forkmesh/bot-tokens");
  expect(owner.request.body.scopes).toEqual([
    "organization.tasks.read",
    "organization.tasks.write",
  ]);
  expect(owner.request.body.expiresDays).toBe(1);
  expect(owner.prompt).toContain("merge only when the merge");
  expect(owner.prompt).toContain("then deploy");
  expect(owner.prompt).not.toContain("Do not merge or deploy");

  await page.locator("forkmesh-world").evaluate(async (shell) => {
    shell.organizations = [{ name: "forkmesh", role: "member" }];
    shell.postJSON = async () => ({ token: "fmbot_member_test" });
    await shell.copyMcpTaskPrompt(
      shell.querySelector("[data-world-mcp-prompt]"),
    );
  });
  await expect.poll(() =>
    page.evaluate(() => window.__worldCopiedPrompts.length)
  ).toBe(2);
  const memberPrompt = await page.evaluate(
    () => window.__worldCopiedPrompts[1],
  );
  expect(memberPrompt).toContain("open a focused pull request");
  expect(memberPrompt).toContain("Do not merge or deploy");
  expect(memberPrompt).not.toContain("then deploy");
});

test("signed-in World receives private and global notifications", async ({
  page,
}) => {
  const notificationRequests = [];
  page.on("request", (request) => {
    if (new URL(request.url()).pathname === "/api/notifications") {
      notificationRequests.push({
        method: request.method(),
        authorization: request.headers().authorization || "",
        body: request.postData() ? request.postDataJSON() : null,
      });
    }
  });
  await prepareWorldPage(page, "world-notifications", {
    session: {
      nodeName: "jett",
      sessionToken: "playwright-jett-session",
    },
    notifications: [
      {
        id: "a".repeat(64),
        kind: "Repository",
        title: "Mirror refresh completed",
        body: "forkmesh/forkmesh is healthy on the registered mirror.",
        ts: FIXED_NOW - 1_000,
        readAt: 0,
      },
      {
        id: "c".repeat(64),
        kind: "Issue",
        title: "Issue #17 / forkmesh/forkmesh",
        body: "Literal punctuation stays readable; markup <b>does not run</b>.",
        href: "javascript:alert('must-not-run')",
        ts: FIXED_NOW - 2_000,
        readAt: FIXED_NOW - 1_500,
      },
    ],
    events: [
      {
        id: "b".repeat(32),
        type: "Infrastructure",
        title: "ForkMesh production deployment complete",
        description: "The production route and multiplayer World are ready.",
        destination: "Town Square",
        startsAt: new Date(FIXED_NOW - 60_000).toISOString(),
        endsAt: new Date(FIXED_NOW + 3_600_000).toISOString(),
      },
    ],
  });
  await waitForWorld(page);

  const notificationButton = page.getByRole("button", {
    name: /Open World pings/,
  });
  const badge = notificationButton.locator("[data-world-notification-count]");
  await expect(badge).toBeVisible();
  await expect(badge).toHaveText("2");
  await notificationButton.click();
  const panel = page.locator("[data-world-detail]");
  await expect(panel).toContainText("Mirror refresh completed");
  await expect(panel).toContainText(
    "ForkMesh production deployment complete",
  );
  await expect(panel).toContainText(
    "The production route and multiplayer World are ready.",
  );
  await expect(panel).toContainText("Issue #17 / forkmesh/forkmesh");
  await expect(panel).toContainText(
    "Literal punctuation stays readable; markup <b>does not run</b>.",
  );
  await expect(
    panel.locator(`[data-world-notification-id="${"c".repeat(64)}"] a`),
  ).toHaveCount(0);
  await expect(
    panel.locator('[data-world-notification-id][data-unread="true"]'),
  ).toHaveCount(1);

  const initialGet = notificationRequests.find(
    (request) => request.method === "GET",
  );
  expect(initialGet?.authorization).toBe(
    "Bearer playwright-jett-session",
  );
  const markReadRequest = page.waitForRequest(
    (request) =>
      new URL(request.url()).pathname === "/api/notifications" &&
      request.method() === "POST",
  );
  await panel.getByRole("button", { name: "Mark all read" }).click();
  await markReadRequest;
  await expect(badge).toHaveText("1");
  await expect(
    panel.locator('[data-world-notification-id][data-unread="false"]'),
  ).toHaveCount(2);
  const markRead = notificationRequests.find(
    (request) => request.method === "POST",
  );
  expect(markRead).toEqual({
    method: "POST",
    authorization: "Bearer playwright-jett-session",
    body: { node: "jett", all: true },
  });

  await page.locator("forkmesh-world").evaluate(async (shell) => {
    localStorage.removeItem("forkmesh.session");
    await shell.refreshPersonalNotifications(false);
  });
  await expect(panel).not.toContainText("Mirror refresh completed");
  await expect(panel).not.toContainText("Issue #17 / forkmesh/forkmesh");
  await expect(panel).toContainText("Sign in for private updates");
});

test("account creation uses /signup while login stays inside the World", async ({
  page,
  context,
}) => {
  const testAccountPassword = ["correct-horse", "battery-staple"].join("-");
  const accountRequests = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (url.pathname.startsWith("/api/accounts/")) {
      accountRequests.push({
        path: url.pathname,
        method: request.method(),
        body: request.postDataJSON(),
      });
    }
    expect(request.url()).not.toContain("world-user@example.test");
    expect(request.url()).not.toContain(testAccountPassword);
  });
  await prepareWorldPage(page, "world-account", {
    accountFixture: true,
  });
  await waitForWorld(page);
  const originalPages = context.pages().length;

  const openAccountPanel = async () => {
    await page.getByRole("button", {
      name: /open World and privacy settings/i,
    }).click();
    await page.getByRole("button", { name: "Account", exact: true }).click();
  };
  await openAccountPanel();
  const account = page.locator("[data-world-account]");
  await expect(account).toBeVisible();
  await expect(account).toContainText(
    "form contents never enter multiplayer presence",
  );
  await account.getByRole("tab", { name: "Create account" }).click();
  const signupLink = account.getByRole("link", { name: "Open secure signup" });
  await expect(signupLink).toHaveAttribute("href", "/signup");
  expect(accountRequests.find((item) => item.path.endsWith("/signup"))).toBeUndefined();

  await account.getByRole("tab", { name: "Log in" }).click();
  const login = account.locator("[data-world-login-form]");
  await login.locator("[name='email']").fill("world-user@example.test");
  await login
    .locator("[name='password']")
    .fill(testAccountPassword);
  const reloaded = page.waitForNavigation({ waitUntil: "domcontentloaded" });
  await login.getByRole("button", { name: /Log in inside/ }).click();
  await reloaded;
  await page.waitForFunction(
    () =>
      document
        .querySelector("forkmesh-world")
        ?.getAttribute("data-world-ready") === "true",
  );
  await expect(page.getByRole("button", {
    name: /world-user.*Registered.*open World and privacy settings/i,
  })).toBeVisible();
  const stored = await page.evaluate(() =>
    JSON.parse(localStorage.getItem("forkmesh.session") || "null"),
  );
  expect(stored).toMatchObject({
    nodeName: "world-user",
    email: "world-user@example.test",
    sessionToken: "world-session-token",
  });
  expect(accountRequests.find((item) => item.path.endsWith("/login"))).toEqual({
    path: "/api/accounts/login",
    method: "POST",
    body: {
      email: "world-user@example.test",
      password: testAccountPassword,
      totp: "",
    },
  });
  expect(context.pages()).toHaveLength(originalPages);
  expect(new URL(page.url()).pathname).toBe("/world/");
});

test("World carries an App cookie session across on the first navigation", async ({
  page,
}) => {
  const appSession = {
    nodeName: "cross-origin-user",
    emailVerified: true,
    sessionToken: "cookie",
  };
  await prepareWorldPage(page, "world-cookie-handoff", {
    session: appSession,
    accountSessionFixture: { sessions: [] },
  });
  // Model the real origin split: App has the HttpOnly cookie, while World's
  // localStorage starts empty because it is a different origin.
  await page.addInitScript(() => {
    localStorage.removeItem("forkmesh.session");
  });
  await waitForWorld(page);

  await expect(page.getByRole("button", {
    name: /cross-origin-user.*Registered.*open World and privacy settings/i,
  })).toBeVisible();
  await page.getByRole("button", {
    name: /open World and privacy settings/i,
  }).click();
  await page.getByRole("button", { name: "Account", exact: true }).click();
  await expect(page.locator("[data-world-account]")).toContainText(
    "cross-origin-user",
  );
  await expect(page.locator("[data-world-login-form]")).toHaveCount(0);
  await expect.poll(() => page.evaluate(() =>
    JSON.parse(localStorage.getItem("forkmesh.session") || "null"),
  )).toMatchObject({
    nodeName: "cross-origin-user",
    emailVerified: true,
    kind: "user",
    sessionToken: "cookie",
  });
});

test("collapsed CHAT bar counts unread remote lines but never your own", async ({
  page,
}) => {
  await prepareWorldPage(page, "world-chat-unread", {
    chatPassphrase: "playwright-public-world-general-passphrase",
  });
  await waitForWorld(page);

  const badge = page.locator("[data-world-chat-terminal-unread]");
  await expect(badge).toBeHidden();

  // The embedded /dashboard/chat iframe mirrors every line to the World with
  // postMessage; drive that same path directly.
  const mirror = (line) =>
    page.evaluate((message) => {
      window.postMessage(
        { type: "forkmesh:world-chat", ...message },
        location.origin,
      );
    }, line);

  // Replayed history only refreshes the newest-line label.
  await mirror({ sender: "peer", text: "old news", history: true });
  await expect(badge).toBeHidden();

  await mirror({ sender: "peer", text: "hello there" });
  await expect(badge).toHaveText("1");
  await mirror({ sender: "peer", text: "anyone around?" });
  await expect(badge).toHaveText("2");

  // Your own lines — from this browser (self) or the same account elsewhere
  // (own) — are already read.
  await mirror({ sender: "me", text: "on my way", self: true });
  await mirror({ sender: "me", text: "from my phone", own: true });
  await expect(badge).toHaveText("2");

  // Opening the bar clears it, and an open bar never accumulates.
  await page.locator("[data-world-chat-terminal]").evaluate((element) => {
    element.open = true;
  });
  await expect(badge).toBeHidden();
  await mirror({ sender: "peer", text: "still talking" });
  await expect(badge).toBeHidden();
});

test("chat launcher opens on hover with messages and left-aligned channels", async ({
  page,
}) => {
  test.slow();
  const passphrase = "playwright-world-hover-chat-passphrase";
  const key = worldChatKey(passphrase);
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    (socket) => {
      socket.send(
        JSON.stringify(
          encryptWorldChatEnvelope(
            {
              type: "chat",
              id: "hover-history-1",
              senderId: "hover-guest",
              sender: "Hover Guest",
              accountKind: "guest",
              channel: "#general",
              text: "The latest chats open with the launcher.",
              ts: FIXED_NOW - 1_000,
            },
            key,
            1,
          ),
        ),
      );
      socket.send(JSON.stringify({
        kind: "forkmesh-history-end",
        v: 1,
      }));
    },
  );
  await prepareWorldPage(page, "world-quick-composer", {
    chatPassphrase: passphrase,
  });
  await waitForWorld(page);

  const terminal = page.locator("[data-world-chat-terminal]");
  const summary = terminal.locator("summary");
  const collapsed = await summary.evaluate((element) => {
    const rect = element.getBoundingClientRect();
    const marker = getComputedStyle(
      element.querySelector(".world-chat-terminal-avatar"),
      "::after",
    );
    return {
      width: Math.round(rect.width),
      height: Math.round(rect.height),
      marker: marker.display,
    };
  });
  expect(collapsed).toEqual({ width: 50, height: 50, marker: "none" });

  const summaryBox = await summary.boundingBox();
  expect(summaryBox).not.toBeNull();
  await page.mouse.move(
    summaryBox.x + summaryBox.width / 2,
    summaryBox.y + summaryBox.height / 2,
  );
  await expect(terminal).toHaveAttribute("open", "");
  await expect(page.locator("[data-world-quick-composer-avatar]")).toBeVisible();
  await expect(terminal).toHaveAttribute("data-show-feed", "true");
  await expect(page.locator("[data-world-quick-chat-feed]")).toBeVisible();
  await expect(
    page.locator("#fullChatMessages .chat-message-row"),
  ).toContainText("The latest chats open with the launcher.");

  const channelLayout = await page
    .locator("[data-world-quick-channels]")
    .evaluate((element) => {
      const style = getComputedStyle(element);
      const rect = element.getBoundingClientRect();
      const first = element.querySelector("button")?.getBoundingClientRect();
      const second = element
        .querySelector("button:nth-of-type(2)")
        ?.getBoundingClientRect();
      const surface = element.closest("[data-world-native-chat]");
      const feed = surface
        ?.querySelector("[data-world-quick-chat-feed]")
        ?.getBoundingClientRect();
      const close = surface
        ?.querySelector("[data-world-chat-terminal-close]")
        ?.getBoundingClientRect();
      return {
        display: style.display,
        flexDirection: style.flexDirection,
        channelLeft: Math.round(rect.left),
        firstLeft: Math.round(first?.left || 0),
        firstTop: Math.round(first?.top || 0),
        secondLeft: Math.round(second?.left || 0),
        secondTop: Math.round(second?.top || 0),
        feedLeft: Math.round(feed?.left || 0),
        feedTop: Math.round(feed?.top || 0),
        closeLeft: Math.round(close?.left || 0),
        closeBottom: Math.round(close?.bottom || 0),
        channelTop: Math.round(rect.top),
        channelRight: Math.round(rect.right),
      };
    });
  expect(channelLayout).toMatchObject({
    display: "flex",
    flexDirection: "row",
  });
  expect(channelLayout.firstLeft).toBeLessThanOrEqual(
    channelLayout.channelLeft + 8,
  );
  expect(channelLayout.firstLeft).toBeLessThanOrEqual(
    channelLayout.feedLeft + 8,
  );
  expect(channelLayout.secondLeft).toBeGreaterThan(channelLayout.firstLeft);
  expect(channelLayout.secondTop).toBe(channelLayout.firstTop);
  expect(channelLayout.feedTop).toBeGreaterThan(channelLayout.firstTop);
  expect(channelLayout.closeBottom).toBeLessThanOrEqual(
    channelLayout.channelTop,
  );

  await page.keyboard.press("Escape");
  await expect(terminal).not.toHaveAttribute("open", "");
  await expect(summary).toBeFocused();
  await page.mouse.move(0, 0);
  await terminal.evaluate((element) => {
    element.classList.add("world-chat-terminal--idle");
  });
  await expect(page.locator(".world-chat-terminal-prompt-icon")).toBeVisible();
});

test("World chat opens intentionally and restores keyboard focus on close", async ({
  page,
}) => {
  test.slow();
  await prepareWorldPage(page, "world-chat-keyboard", {
    chatPassphrase: "playwright-public-world-general-passphrase",
  });
  await waitForWorld(page);

  const terminal = page.locator("[data-world-chat-terminal]");
  const summary = terminal.locator(":scope > summary");
  await summary.focus();
  await expect(terminal).not.toHaveAttribute("open", "");

  await page.keyboard.press("Enter");
  await expect(terminal).toHaveAttribute("open", "");
  await expect(page.locator("#fullChatInput")).toBeFocused();
  await expect(
    page.locator("[data-dashboard-chat-composer-toolbar]"),
  ).toBeHidden();

  await page.keyboard.press("Escape");
  await expect(terminal).not.toHaveAttribute("open", "");
  await expect(summary).toBeFocused();

  await page.keyboard.press("Enter");
  await expect(terminal).toHaveAttribute("open", "");
  await page.locator("[data-world-chat-terminal-close]").click();
  await expect(terminal).not.toHaveAttribute("open", "");
  await expect(summary).toBeFocused();
});

test("mobile World chat finishes entering before an explicit composer focus", async ({
  browser,
}) => {
  test.slow();
  const context = await browser.newContext({
    viewport: { width: 390, height: 667 },
    hasTouch: true,
    isMobile: true,
  });
  const page = await context.newPage();
  await prepareWorldPage(page, "mobile-world-chat-focus", {
    chatPassphrase: "playwright-public-world-general-passphrase",
  });
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await waitForWorld(page);

  const terminal = page.locator("[data-world-chat-terminal]");
  const summary = terminal.locator(":scope > summary");
  const input = page.locator("#fullChatInput");
  await summary.click();
  await expect(terminal).toHaveAttribute("open", "");
  await expect(input).not.toBeFocused();
  await expect
    .poll(() =>
      terminal.evaluate((element) => {
        const body = element.querySelector("[data-world-native-chat]");
        const composer = element.querySelector(
          "[data-dashboard-chat-composer]",
        );
        const animations = [body, composer]
          .flatMap((surface) => surface?.getAnimations?.() || []);
        return (
          animations.length >= 2 &&
          animations.every((animation) => animation.playState === "finished")
        );
      }),
    )
    .toBe(true);

  const settled = await input.evaluate((element) => {
    const body = element.closest("[data-world-native-chat]");
    const bodyStyle = getComputedStyle(body);
    return {
      fontSize: getComputedStyle(element).fontSize,
      opacity: bodyStyle.opacity,
      transform: bodyStyle.transform,
      scrollY: window.scrollY,
    };
  });
  expect(settled.fontSize).toBe("16px");
  expect(settled.opacity).toBe("1");
  expect(["none", "matrix(1, 0, 0, 1, 0, 0)"]).toContain(
    settled.transform,
  );
  expect(settled.scrollY).toBe(0);

  await input.click();
  await expect(input).toBeFocused();
  await expect.poll(() => page.evaluate(() => window.scrollY)).toBe(0);
  await context.close();
});

test("World Task creates an unassigned General task instead of dispatching a bot", async ({
  page,
}) => {
  const officeTaskFixture = { canManage: false, requests: [], tasks: [] };
  await prepareWorldPage(page, "world-general-task", {
    session: {
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-token",
    },
    officeTaskFixture,
    chatPassphrase: "playwright-public-world-general-passphrase",
  });
  await waitForWorld(page);

  const terminal = page.locator("[data-world-chat-terminal]");
  await terminal.evaluate((element) => {
    element.open = true;
  });
  const avatar = page.locator("[data-world-quick-composer-avatar]");
  await expect(avatar).toBeVisible();
  await expect
    .poll(() =>
      avatar.evaluate((element) => {
        const style = getComputedStyle(element);
        return { borderTopWidth: style.borderTopWidth, boxShadow: style.boxShadow };
      }),
    )
    .toEqual({ borderTopWidth: "0px", boxShadow: "none" });

  await page.locator("#fullChatInput").fill("Review the onboarding checklist");
  const taskButton = page.locator("#fullChatTaskSend");
  await expect(taskButton).toHaveAttribute(
    "title",
    "Create an unassigned task in General",
  );
  await taskButton.click();

  await expect
    .poll(() =>
      officeTaskFixture.requests.find(
        (request) => request.method === "POST" && request.path === "/api/tasks",
      ),
    )
    .toMatchObject({
      body: {
        title: "Review the onboarding checklist",
        department: "general",
        team: "",
        destination: "department",
        assigneeKind: "unassigned",
        assignee: "",
      },
    });
  await expect(
    page.locator("[data-dashboard-chat-composer-status]"),
  ).toHaveText("Organization task created.");
});

for (const width of [320, 390]) {
  test(`phone status bar keeps every admin count inside it at ${width}px`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: 700 });
    await prepareWorldPage(page, `phone-status-bar-${width}`, {
      session: {
        nodeName: "jett",
        sessionToken: "playwright-jett-session",
      },
    });
    await waitForWorld(page);
    await page.locator("forkmesh-world").evaluate((shell) => {
      shell.identity = { ...(shell.identity || {}), isAdmin: true };
      shell.renderAdminErrors(150);
      shell
        .querySelectorAll(
          ".world-status-actions [data-world-notification-count], .world-status-actions [data-world-task-count]",
        )
        .forEach((badge) => {
          badge.hidden = false;
          badge.textContent = "999+";
        });
      shell.querySelector("[data-world-online-count]").textContent = "128";
    });

    const layout = await page.evaluate(() => {
      const box = (element) => {
        const rect = element.getBoundingClientRect();
        return {
          left: rect.left,
          right: rect.right,
          top: rect.top,
          bottom: rect.bottom,
        };
      };
      const bar = document.querySelector(".world-status-actions");
      return {
        bar: box(bar),
        brand: box(document.querySelector(".world-brand")),
        avatar: box(document.querySelector("[data-world-shirt-badge]")),
        items: [
          ...bar.querySelectorAll(
            ":scope > button:not([hidden]), .world-online-button, .world-tool-count:not([hidden])",
          ),
        ].map(box),
      };
    });

    expect(layout.items.length).toBe(9);
    expect(layout.bar.left).toBeGreaterThanOrEqual(layout.brand.right);
    expect(layout.bar.right).toBeLessThanOrEqual(layout.avatar.left);
    for (const item of layout.items) {
      expect(item.left).toBeGreaterThanOrEqual(layout.bar.left);
      expect(item.right).toBeLessThanOrEqual(layout.bar.right);
      expect(item.top).toBeGreaterThanOrEqual(layout.bar.top - 2);
      expect(item.bottom).toBeLessThanOrEqual(layout.bar.bottom);
    }
  });
}

test("mobile World chat contains its rail, transcript, and prompt", async ({
  page,
}) => {
  test.slow();
  await page.setViewportSize({ width: 390, height: 667 });
  await prepareWorldPage(page, "mobile-world-chat", {
    chatPassphrase: "playwright-public-world-general-passphrase",
  });
  await waitForWorld(page);

  // Reproduce the worst case: DEBUG was expanded before the visitor opened the
  // native full-height chat.
  await page.locator("[data-world-diagnostics]").evaluate((element) => {
    element.open = true;
  });
  const terminal = page.locator("[data-world-chat-terminal]");
  await terminal.evaluate((element) => {
    element.open = true;
  });
  const input = page.locator("#fullChatInput");
  await expect(input).toBeVisible();
  await input.focus();

  const channels = page.locator("[data-world-quick-channels]");
  const lastChannel = page.locator(
    '[data-world-quick-channel="notifications"]',
  );
  await lastChannel.focus();
  const focusedChannel = await channels.evaluate((element) => {
    const rail = element.getBoundingClientRect();
    const last = element.querySelector(
      '[data-world-quick-channel="notifications"]',
    )?.getBoundingClientRect();
    return {
      scrollable: element.scrollWidth > element.clientWidth,
      lastLeft: last?.left || 0,
      lastRight: last?.right || 0,
      railLeft: rail.left,
      railRight: rail.right,
    };
  });
  expect(focusedChannel.lastLeft).toBeGreaterThanOrEqual(
    focusedChannel.railLeft - 1,
  );
  expect(focusedChannel.lastRight).toBeLessThanOrEqual(
    focusedChannel.railRight + 1,
  );
  const normalDockGap = await terminal.evaluate((element) => {
    const terminalRect = element.getBoundingClientRect();
    const dock = document.querySelector(".world-avatar-actions");
    const dockRect = dock?.getBoundingClientRect();
    return (dockRect?.top || window.innerHeight) - terminalRect.bottom;
  });
  expect(normalDockGap).toBeGreaterThanOrEqual(4);
  const stagedAttachments = page.locator(
    "[data-dashboard-chat-attachments]",
  );
  await stagedAttachments.evaluate((element) => {
    const preview = document.createElement("button");
    preview.type = "button";
    preview.textContent = "release-preview.png ×";
    preview.setAttribute("aria-label", "Remove release-preview.png");
    element.append(preview);
  });
  await expect(stagedAttachments).toBeVisible();
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.coarsePointerViewport = true;
    shell.lastStableViewportWidth = 390;
    shell.lastStableViewportHeight = 667;
  });

  await input.fill(
    "A longer prompt must remain readable when the mobile keyboard is open. ".repeat(
      8,
    ),
  );
  await page.setViewportSize({ width: 390, height: 320 });
  await expect(page.locator("forkmesh-world")).toHaveAttribute(
    "data-world-chat-compact",
    "true",
  );

  const metrics = await input.evaluate((element) => {
    const body = element.closest("[data-world-native-chat]");
    const header = body?.querySelector(".world-quick-chat-header");
    const feed = body?.querySelector("[data-world-quick-chat-feed]");
    const transcript = body?.querySelector("#fullChatMessages");
    const composer = element.closest("[data-dashboard-chat-composer]");
    const terminal = element.closest("[data-world-chat-terminal]");
    const send = terminal?.querySelector("#fullChatSend");
    const close = terminal?.querySelector("[data-world-chat-terminal-close]");
    const attachments = terminal?.querySelector(
      "[data-dashboard-chat-attachments]",
    );
    const dock = document.querySelector(".world-avatar-actions");
    const bodyRect = body?.getBoundingClientRect();
    const headerRect = header?.getBoundingClientRect();
    const feedRect = feed?.getBoundingClientRect();
    const transcriptRect = transcript?.getBoundingClientRect();
    const inputRect = element.getBoundingClientRect();
    const composerRect = composer?.getBoundingClientRect();
    const terminalRect = terminal?.getBoundingClientRect();
    const sendRect = send?.getBoundingClientRect();
    const closeRect = close?.getBoundingClientRect();
    const attachmentsRect = attachments?.getBoundingClientRect();
    const dockRect = dock?.getBoundingClientRect();
    const contained = (rect) =>
      Boolean(
        rect &&
          terminalRect &&
          rect.left >= terminalRect.left - 1 &&
          rect.right <= terminalRect.right + 1 &&
          rect.top >= terminalRect.top - 1 &&
          rect.bottom <= terminalRect.bottom + 1,
      );
    return {
      innerHeight: window.innerHeight,
      documentScrollWidth: document.documentElement.scrollWidth,
      bodyFits:
        Boolean(bodyRect && terminalRect) &&
        bodyRect.right <= terminalRect.right + 1 &&
        bodyRect.bottom <= terminalRect.bottom + 1 &&
        body.scrollWidth <= body.clientWidth + 1 &&
        body.scrollHeight <= body.clientHeight + 1,
      ordered:
        Boolean(headerRect && feedRect && composerRect) &&
        headerRect.bottom <= feedRect.top + 1 &&
        feedRect.bottom <= composerRect.top + 1,
      transcriptContained:
        Boolean(feedRect && transcriptRect) &&
        transcriptRect.left >= feedRect.left - 1 &&
        transcriptRect.right <= feedRect.right + 1 &&
        transcriptRect.top >= feedRect.top - 1 &&
        transcriptRect.bottom <= feedRect.bottom + 1,
      feedHeight: feedRect?.height || 0,
      inputHeight: inputRect.height,
      inputContained: contained(inputRect),
      sendContained: contained(sendRect),
      closeContained: contained(closeRect),
      attachmentsContained: contained(attachmentsRect),
      attachmentsClearInput:
        Boolean(attachmentsRect) &&
        attachmentsRect.bottom <= inputRect.top + 1,
      sendWidth: sendRect?.width || 0,
      sendHeight: sendRect?.height || 0,
      dockTop: dockRect?.top || window.innerHeight,
      terminalZ: Number.parseInt(getComputedStyle(terminal).zIndex, 10) || 0,
      dockZ: dock
        ? Number.parseInt(getComputedStyle(dock).zIndex, 10) || 0
        : 0,
      terminalBottom: terminalRect?.bottom || 0,
    };
  });
  expect(metrics).toMatchObject({
    innerHeight: 320,
    documentScrollWidth: 390,
    bodyFits: true,
    ordered: true,
    transcriptContained: true,
    inputContained: true,
    sendContained: true,
    closeContained: true,
    attachmentsContained: true,
    attachmentsClearInput: true,
  });
  expect(metrics.feedHeight).toBeGreaterThanOrEqual(80);
  expect(metrics.inputHeight).toBeGreaterThanOrEqual(52);
  expect(metrics.sendWidth).toBeGreaterThanOrEqual(48);
  expect(metrics.sendHeight).toBeGreaterThanOrEqual(48);
  expect(metrics.terminalBottom).toBeLessThanOrEqual(metrics.innerHeight - 7);
  expect(
    metrics.terminalBottom <= metrics.dockTop - 4 ||
      metrics.terminalZ > metrics.dockZ,
  ).toBe(true);

  await page.setViewportSize({ width: 390, height: 240 });
  const shortest = await terminal.evaluate((element) => {
    const terminalRect = element.getBoundingClientRect();
    const body = element.querySelector("[data-world-native-chat]");
    const feed = element.querySelector("[data-world-quick-chat-feed]");
    const composer = element.querySelector("[data-dashboard-chat-composer]");
    const input = element.querySelector("#fullChatInput");
    const send = element.querySelector("#fullChatSend");
    const attachments = element.querySelector(
      "[data-dashboard-chat-attachments]",
    );
    const bodyRect = body?.getBoundingClientRect();
    const feedRect = feed?.getBoundingClientRect();
    const composerRect = composer?.getBoundingClientRect();
    const inputRect = input?.getBoundingClientRect();
    const sendRect = send?.getBoundingClientRect();
    const attachmentsRect = attachments?.getBoundingClientRect();
    const contained = (rect) =>
      Boolean(
        rect &&
          rect.left >= terminalRect.left - 1 &&
          rect.right <= terminalRect.right + 1 &&
          rect.top >= terminalRect.top - 1 &&
          rect.bottom <= terminalRect.bottom + 1,
      );
    return {
      innerHeight: window.innerHeight,
      terminalTop: terminalRect.top,
      terminalBottom: terminalRect.bottom,
      feedHeight: feedRect?.height || 0,
      bodyFits:
        Boolean(body && bodyRect) &&
        contained(bodyRect) &&
        body.scrollWidth <= body.clientWidth + 1 &&
        body.scrollHeight <= body.clientHeight + 1,
      ordered:
        Boolean(feedRect && composerRect) &&
        feedRect.bottom <= composerRect.top + 1,
      inputContained: contained(inputRect),
      sendContained: contained(sendRect),
      attachmentsContained: contained(attachmentsRect),
      attachmentsClearInput:
        Boolean(attachmentsRect && inputRect) &&
        attachmentsRect.bottom <= inputRect.top + 1,
    };
  });
  expect(shortest).toMatchObject({
    innerHeight: 240,
    bodyFits: true,
    ordered: true,
    inputContained: true,
    sendContained: true,
    attachmentsContained: true,
    attachmentsClearInput: true,
  });
  expect(shortest.terminalTop).toBeGreaterThanOrEqual(7);
  expect(shortest.terminalBottom).toBeLessThanOrEqual(233);
  expect(shortest.feedHeight).toBeGreaterThanOrEqual(56);

  await page.setViewportSize({ width: 390, height: 180 });
  await expect(page.locator("forkmesh-world")).toHaveAttribute(
    "data-world-chat-micro",
    "true",
  );
  const micro = await terminal.evaluate((element) => {
    const terminalRect = element.getBoundingClientRect();
    const body = element.querySelector("[data-world-native-chat]");
    const feedRect = element
      .querySelector("[data-world-quick-chat-feed]")
      ?.getBoundingClientRect();
    const inputRect = element
      .querySelector("#fullChatInput")
      ?.getBoundingClientRect();
    const sendRect = element
      .querySelector("#fullChatSend")
      ?.getBoundingClientRect();
    const attachmentsRect = element
      .querySelector("[data-dashboard-chat-attachments]")
      ?.getBoundingClientRect();
    const contained = (rect) =>
      Boolean(
        rect &&
          rect.left >= terminalRect.left - 1 &&
          rect.right <= terminalRect.right + 1 &&
          rect.top >= terminalRect.top - 1 &&
          rect.bottom <= terminalRect.bottom + 1,
      );
    return {
      innerHeight: window.innerHeight,
      terminalTop: terminalRect.top,
      terminalBottom: terminalRect.bottom,
      feedHeight: feedRect?.height || 0,
      bodyFits:
        Boolean(body) &&
        body.scrollWidth <= body.clientWidth + 1 &&
        body.scrollHeight <= body.clientHeight + 1,
      inputContained: contained(inputRect),
      sendContained: contained(sendRect),
      attachmentsContained: contained(attachmentsRect),
      attachmentsClearInput:
        Boolean(attachmentsRect && inputRect) &&
        attachmentsRect.bottom <= inputRect.top + 1,
    };
  });
  expect(micro).toMatchObject({
    innerHeight: 180,
    bodyFits: true,
    inputContained: true,
    sendContained: true,
    attachmentsContained: true,
    attachmentsClearInput: true,
  });
  expect(micro.terminalTop).toBeGreaterThanOrEqual(7);
  expect(micro.terminalBottom).toBeLessThanOrEqual(173);
  expect(micro.feedHeight).toBeGreaterThanOrEqual(32);
});

test("World chat follows the visual viewport when a software keyboard opens", async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 667 });
  await prepareWorldPage(page, "mobile-world-chat-keyboard", {
    chatPassphrase: "playwright-public-world-general-passphrase",
  });
  await waitForWorld(page);

  const terminal = page.locator("[data-world-chat-terminal]");
  await terminal.evaluate((element) => {
    element.open = true;
  });
  const applied = await page.locator("forkmesh-world").evaluate((shell) => {
    const viewport = window.visualViewport;
    if (!viewport) return false;
    try {
      Object.defineProperty(viewport, "height", {
        configurable: true,
        value: 320,
      });
      Object.defineProperty(viewport, "offsetTop", {
        configurable: true,
        value: 24,
      });
    } catch (_) {
      return false;
    }
    shell.coarsePointerViewport = true;
    shell.lastStableViewportWidth = 390;
    // Legacy mobile WebViews sometimes resize only the layout viewport. The
    // shell must share the same keyboard metric path for a plain resize.
    window.dispatchEvent(new Event("resize"));
    return true;
  });
  expect(applied).toBe(true);
  await expect(page.locator("forkmesh-world")).toHaveAttribute(
    "data-world-chat-compact",
    "true",
  );

  const metrics = await terminal.evaluate((element) => {
    const shell = element.closest("forkmesh-world");
    const terminalRect = element.getBoundingClientRect();
    const feedRect = element
      .querySelector("[data-world-quick-chat-feed]")
      ?.getBoundingClientRect();
    const transcriptRect = element
      .querySelector("#fullChatMessages")
      ?.getBoundingClientRect();
    const composerRect = element
      .querySelector("[data-dashboard-chat-composer]")
      ?.getBoundingClientRect();
    const inputRect = element
      .querySelector("#fullChatInput")
      ?.getBoundingClientRect();
    const sendRect = element
      .querySelector("#fullChatSend")
      ?.getBoundingClientRect();
    const dock = document.querySelector(".world-avatar-actions");
    const dockRect = dock?.getBoundingClientRect();
    return {
      layoutHeight: window.innerHeight,
      chatHeight: shell.style.getPropertyValue(
        "--world-chat-viewport-height",
      ),
      coveredBottom: shell.style.getPropertyValue(
        "--world-chat-covered-bottom",
      ),
      terminalTop: terminalRect.top,
      terminalBottom: terminalRect.bottom,
      feedHeight: feedRect?.height || 0,
      transcriptContained:
        Boolean(feedRect && transcriptRect) &&
        transcriptRect.left >= feedRect.left - 1 &&
        transcriptRect.right <= feedRect.right + 1 &&
        transcriptRect.top >= feedRect.top - 1 &&
        transcriptRect.bottom <= feedRect.bottom + 1,
      composerBottom: composerRect?.bottom || 0,
      inputRight: inputRect?.right || 0,
      sendRight: sendRect?.right || 0,
      terminalRight: terminalRect.right,
      dockTop: dockRect?.top || window.innerHeight,
      terminalZ: Number.parseInt(getComputedStyle(element).zIndex, 10) || 0,
      dockZ: dock
        ? Number.parseInt(getComputedStyle(dock).zIndex, 10) || 0
        : 0,
    };
  });
  expect(metrics).toMatchObject({
    layoutHeight: 667,
    chatHeight: "320px",
    coveredBottom: "323px",
  });
  expect(metrics.terminalTop).toBeGreaterThanOrEqual(31);
  expect(metrics.terminalBottom).toBeLessThanOrEqual(337);
  expect(
    metrics.terminalBottom <= metrics.dockTop - 4 ||
      metrics.terminalZ > metrics.dockZ,
  ).toBe(true);
  expect(metrics.feedHeight).toBeGreaterThanOrEqual(80);
  expect(metrics.transcriptContained).toBe(true);
  expect(metrics.composerBottom).toBeLessThanOrEqual(
    metrics.terminalBottom + 1,
  );
  expect(metrics.inputRight).toBeLessThanOrEqual(metrics.terminalRight + 1);
  expect(metrics.sendRight).toBeLessThanOrEqual(metrics.terminalRight + 1);

  await page.locator("forkmesh-world").evaluate(() => {
    const viewport = window.visualViewport;
    Object.defineProperty(viewport, "height", {
      configurable: true,
      value: 667,
    });
    Object.defineProperty(viewport, "offsetTop", {
      configurable: true,
      value: 0,
    });
    window.dispatchEvent(new Event("resize"));
  });
  await expect(page.locator("forkmesh-world")).toHaveAttribute(
    "data-world-chat-compact",
    "false",
  );
  const restored = await terminal.evaluate((element) => {
    const shell = element.closest("forkmesh-world");
    const rect = element.getBoundingClientRect();
    return {
      chatHeight: shell.style.getPropertyValue(
        "--world-chat-viewport-height",
      ),
      coveredBottom: shell.style.getPropertyValue(
        "--world-chat-covered-bottom",
      ),
      top: rect.top,
      bottom: rect.bottom,
    };
  });
  expect(restored).toMatchObject({
    chatHeight: "667px",
    coveredBottom: "0px",
  });
  expect(restored.top).toBeGreaterThanOrEqual(0);
  expect(restored.bottom).toBeLessThanOrEqual(667);
});

test("World chat prompt fits on a direct micro-height load", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 180 });
  await prepareWorldPage(page, "mobile-world-chat-direct-micro", {
    chatPassphrase: "playwright-public-world-general-passphrase",
  });
  await waitForWorld(page);

  const shell = page.locator("forkmesh-world");
  const terminal = page.locator("[data-world-chat-terminal]");
  await terminal.evaluate((element) => {
    element.open = true;
  });
  await expect(shell).toHaveAttribute("data-world-chat-micro", "true");
  await expect(page.locator("#fullChatInput")).toBeVisible();

  const metrics = await terminal.evaluate((element) => {
    const terminalRect = element.getBoundingClientRect();
    const inputRect = element
      .querySelector("#fullChatInput")
      ?.getBoundingClientRect();
    const sendRect = element
      .querySelector("#fullChatSend")
      ?.getBoundingClientRect();
    const feedRect = element
      .querySelector("[data-world-quick-chat-feed]")
      ?.getBoundingClientRect();
    return {
      innerHeight: window.innerHeight,
      coveredBottom: getComputedStyle(
        element.closest("forkmesh-world"),
      ).getPropertyValue("--world-chat-covered-bottom"),
      terminalTop: terminalRect.top,
      terminalBottom: terminalRect.bottom,
      inputBottom: inputRect?.bottom || Infinity,
      sendBottom: sendRect?.bottom || Infinity,
      feedHeight: feedRect?.height || 0,
    };
  });

  expect(metrics).toMatchObject({
    innerHeight: 180,
    coveredBottom: "60px",
  });
  expect(metrics.terminalTop).toBeGreaterThanOrEqual(7);
  expect(metrics.terminalBottom).toBeLessThanOrEqual(173);
  expect(metrics.inputBottom).toBeLessThanOrEqual(metrics.terminalBottom);
  expect(metrics.sendBottom).toBeLessThanOrEqual(metrics.terminalBottom);
  expect(metrics.feedHeight).toBeGreaterThanOrEqual(32);
});

test("World chat pre-renders replay history before fast bidirectional scrolling", async ({
  page,
}) => {
  test.slow();
  await page.setViewportSize({ width: 640, height: 900 });
  const passphrase = "playwright-world-history-window-passphrase";
  const key = worldChatKey(passphrase);
  const oversizedImage = Buffer.from(
    '<svg xmlns="http://www.w3.org/2000/svg" width="4096" height="2048">' +
      '<rect width="4096" height="2048" fill="#58a6ff"/></svg>',
    "utf8",
  ).toString("base64");
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    (socket) => {
      // Same-millisecond D1 rows are hash-tied, so mutations may be replayed
      // before their targets. Exercise that ordering explicitly.
      socket.send(JSON.stringify(encryptWorldChatEnvelope({
        type: "edit",
        id: "edit-history-12",
        target: "history-12",
        senderId: "guest-12",
        sender: "Guest 12",
        accountKind: "guest",
        text: "Edited retained message 12.",
        editedAt: FIXED_NOW - 500,
        ts: FIXED_NOW - 500,
      }, key, 40)));
      socket.send(JSON.stringify(encryptWorldChatEnvelope({
        type: "delete",
        id: "delete-history-13",
        target: "history-13",
        senderId: "guest-13",
        sender: "Guest 13",
        accountKind: "guest",
        ts: FIXED_NOW - 250,
      }, key, 41)));
      for (let index = 1; index <= 13; index += 1) {
        socket.send(JSON.stringify(encryptWorldChatEnvelope({
          type: "chat",
          id: `history-${index}`,
          senderId: `guest-${index}`,
          sender: `Guest ${index}`,
          accountKind: "guest",
          channel: "#general",
          text: `Retained message ${index}. ${"Lazy history stays smooth. ".repeat(12)}`,
          ts: FIXED_NOW - (13 - index) * 1_000,
          ...(index === 12
            ? {
                fileName: "oversized-history-image.svg",
                fileMime: "image/svg+xml",
                file: oversizedImage,
              }
            : {}),
        }, key, index)));
      }
      socket.send(JSON.stringify({
        kind: "forkmesh-history-end",
        v: 1,
      }));
    },
  );
  await prepareWorldPage(page, "world-chat-history-window", {
    chatPassphrase: passphrase,
  });
  await waitForWorld(page);

  const terminal = page.locator("[data-world-chat-terminal]");
  await terminal.evaluate((element) => {
    element.open = true;
  });
  await page.locator('[data-world-quick-channel="general"]').click();
  const messages = page.locator("#fullChatMessages .chat-message-row");
  await expect(messages).toHaveCount(5);
  await expect(page.locator(".chat-history-indicator")).toContainText(
    "7 earlier messages",
  );
  // Oldest at the top, newest on the bottom rail, with the "earlier messages"
  // handle above the first row and the feed parked at the latest line.
  await expect
    .poll(() =>
      page.locator("#fullChatMessages").evaluate(
        (element) =>
          element.scrollHeight - element.scrollTop - element.clientHeight <= 2,
      ),
    )
    .toBe(true);
  const feedOrder = await page.locator("#fullChatMessages").evaluate((element) => {
    const rows = Array.from(element.querySelectorAll(".chat-message-row"));
    const indicator = element.querySelector(".chat-history-indicator");
    return {
      texts: rows.map((row) => row.querySelector("p")?.textContent?.trim() || ""),
      indicatorFirst: element.firstElementChild === indicator,
      atBottom:
        element.scrollHeight - element.scrollTop - element.clientHeight <= 2,
    };
  });
  expect(feedOrder.indicatorFirst).toBe(true);
  expect(feedOrder.atBottom).toBe(true);
  expect(feedOrder.texts[0]).toContain("Retained message 8.");
  expect(feedOrder.texts.at(-1)).toContain("Edited retained message 12.");
  await expect(page.locator("#fullChatInput")).toBeInViewport();

  const geometry = await terminal.evaluate((element) => {
    const body = element.querySelector("[data-world-native-chat]");
    const composer = element.querySelector("[data-dashboard-chat-composer]");
    const input = element.querySelector("#fullChatInput");
    const terminalRect = element.getBoundingClientRect();
    const bodyRect = body.getBoundingClientRect();
    const composerRect = composer.getBoundingClientRect();
    const inputRect = input.getBoundingClientRect();
    return {
      viewportHeight: window.innerHeight,
      terminalTop: terminalRect.top,
      terminalHeight: terminalRect.height,
      terminalBottom: terminalRect.bottom,
      bodyBottom: bodyRect.bottom,
      composerBottom: composerRect.bottom,
      inputBottom: inputRect.bottom,
      terminalScrollHeight: element.scrollHeight,
      terminalClientHeight: element.clientHeight,
    };
  });
  expect(geometry.terminalBottom).toBeLessThanOrEqual(
    geometry.viewportHeight + 1,
  );
  expect(geometry.terminalTop).toBeGreaterThanOrEqual(
    geometry.viewportHeight * 0.35,
  );
  expect(geometry.terminalHeight).toBeLessThanOrEqual(
    geometry.viewportHeight * 0.6,
  );
  expect(geometry.bodyBottom).toBeLessThanOrEqual(
    geometry.terminalBottom + 1,
  );
  expect(geometry.composerBottom).toBeLessThanOrEqual(
    geometry.terminalBottom + 1,
  );
  expect(geometry.inputBottom).toBeLessThanOrEqual(
    geometry.terminalBottom + 1,
  );
  expect(geometry.terminalScrollHeight).toBeLessThanOrEqual(
    geometry.terminalClientHeight + 1,
  );
  const attachment = page.locator(
    "#fullChatMessages .chat-message-row",
    { hasText: "Edited retained message 12." },
  );
  const attachmentImage = attachment.locator(".chat-attachment-image");
  await expect(attachmentImage).toBeVisible();
  await expect(attachmentImage).toHaveAttribute("loading", "eager");
  await attachmentImage.evaluate((image) => {
    if (image.complete) return;
    return new Promise((resolve) => {
      image.addEventListener("load", resolve, { once: true });
      image.addEventListener("error", resolve, { once: true });
    });
  });
  const attachmentBounds = await attachment.evaluate((row) => {
    const bubble = row.querySelector(".chat-message-bubble");
    const wrapper = row.querySelector(".chat-attachment-wrapper");
    const image = row.querySelector(".chat-attachment-image");
    const bubbleRect = bubble.getBoundingClientRect();
    const wrapperRect = wrapper.getBoundingClientRect();
    const imageRect = image.getBoundingClientRect();
    return {
      bubbleLeft: bubbleRect.left,
      bubbleRight: bubbleRect.right,
      wrapperLeft: wrapperRect.left,
      wrapperRight: wrapperRect.right,
      imageLeft: imageRect.left,
      imageRight: imageRect.right,
      bubbleClientWidth: bubble.clientWidth,
      bubbleScrollWidth: bubble.scrollWidth,
    };
  });
  expect(attachmentBounds.wrapperLeft).toBeGreaterThanOrEqual(
    attachmentBounds.bubbleLeft - 1,
  );
  expect(attachmentBounds.wrapperRight).toBeLessThanOrEqual(
    attachmentBounds.bubbleRight + 1,
  );
  expect(attachmentBounds.imageLeft).toBeGreaterThanOrEqual(
    attachmentBounds.bubbleLeft - 1,
  );
  expect(attachmentBounds.imageRight).toBeLessThanOrEqual(
    attachmentBounds.bubbleRight + 1,
  );
  expect(attachmentBounds.bubbleScrollWidth).toBeLessThanOrEqual(
    attachmentBounds.bubbleClientWidth + 1,
  );

  await page.locator("#fullChatMessages").evaluate((element) => {
    element.scrollTop = 0;
    element.dispatchEvent(new WheelEvent("wheel", { deltaY: -120 }));
    element.dispatchEvent(new WheelEvent("wheel", { deltaY: -120 }));
  });
  await expect.poll(() => messages.count()).toBeGreaterThan(5);
  const upwardBuffer = await page.locator("#fullChatMessages").evaluate(
    (element) => {
      const indicator = element.querySelector(".chat-history-indicator");
      return {
        complete: indicator?.dataset.complete === "true",
        scrollTop: element.scrollTop,
        clientHeight: element.clientHeight,
        renderedRows: element.querySelectorAll(".chat-message-row").length,
      };
    },
  );
  expect(
    upwardBuffer.complete ||
      upwardBuffer.scrollTop >= upwardBuffer.clientHeight * 1.5 - 2,
  ).toBe(true);

  // Jump both ways in one task, as a scrollbar fling can do. Previously the
  // one-frame reveal lock exposed an empty feed until another input event.
  const bidirectional = await page.locator("#fullChatMessages").evaluate(
    (element) => {
      element.scrollTop = element.scrollHeight;
      element.scrollTop = 0;
      element.dispatchEvent(new WheelEvent("wheel", { deltaY: -240 }));
      element.scrollTop = element.scrollHeight;
      const rect = element.getBoundingClientRect();
      const point = document.elementFromPoint(
        rect.left + rect.width / 2,
        rect.top + Math.min(rect.height - 2, Math.max(2, rect.height / 2)),
      );
      return {
        rowCount: element.querySelectorAll(".chat-message-row").length,
        paintedContent: Boolean(
          point?.closest?.(".chat-message-row, .chat-history-indicator"),
        ),
        atBottom:
          element.scrollHeight - element.scrollTop - element.clientHeight <= 2,
      };
    },
  );
  expect(bidirectional.rowCount).toBeGreaterThanOrEqual(
    upwardBuffer.renderedRows,
  );
  expect(bidirectional.paintedContent).toBe(true);
  expect(bidirectional.atBottom).toBe(true);
});

test("ForkMesh Office walk-in opens chat only through the explicit fallback", async ({
  page,
  context,
}) => {
  const passphrase = "playwright-public-world-general-passphrase";
  const chatSocketURLs = [];
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    (socket) => {
      chatSocketURLs.push(socket.url());
    },
  );
  await prepareWorldPage(page, "world-chat", {
    chatPassphrase: passphrase,
    session: {
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-office-token",
    },
  });
  await waitForWorld(page);

  const worldURL = page.url();
  const pageCount = context.pages().length;
  // The World's always-present embedded global chat connects independently of
  // the Office. Capture that baseline so this journey proves that focusing or
  // approaching the Office does not open an additional Office chat transport.
  await expect.poll(() => chatSocketURLs.length).toBeGreaterThanOrEqual(1);
  const globalChatSocketCount = chatSocketURLs.length;
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.focusLandmark("office");
  });
  expect(chatSocketURLs).toHaveLength(globalChatSocketCount);

  await moveToOfficeEntrance(page);
  await expect(page.locator("[data-world-office-prompt]")).toHaveCount(0);
  expect(chatSocketURLs).toHaveLength(globalChatSocketCount);
  await walkIntoOffice(page);
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.officeController.openFallback();
  });

  const chat = page.locator("[data-world-office-chat]");
  await expect(chat).toBeVisible();
  const officeFrame = page.locator("[data-world-office-frame]");
  await expect(officeFrame).toHaveAttribute("src", "/chat?embed=office");
  const chatFrame = page.frameLocator("[data-world-office-frame]");
  const chatInput = chatFrame.locator("#chat-input");
  await expect(chatInput).toBeVisible();
  await expect(chatFrame.locator("#chat-status")).toHaveText(
    "Connected · public World #general",
  );
  await chat.getByRole("button", {
    name: "Close accessible chat fallback",
  }).click();
  await expect(chat).toBeHidden();
  await expect(officeFrame).not.toHaveAttribute("src", /.+/, {
    timeout: 3500,
  });
  expect(chatSocketURLs).toHaveLength(globalChatSocketCount + 1);
  expect(page.url()).toBe(worldURL);
  expect(context.pages()).toHaveLength(pageCount);
});

test("Office runtime stays off the initial World graph and loads on demand", async ({
  page,
}) => {
  const officeModules = new Set([
    "/world/world-office.js",
    "/world/world-office-meeting.js",
    "/world/world-office-tasks.js",
  ]);
  const requestedOfficeModules = [];
  page.on("request", (request) => {
    const path = new URL(request.url()).pathname;
    if (officeModules.has(path)) requestedOfficeModules.push(path);
  });
  await prepareWorldPage(page, "office-lazy-runtime");
  await waitForWorld(page);
  expect(requestedOfficeModules).toEqual([]);

  const runtime = await page.locator("forkmesh-world").evaluate(async (shell) => {
    const controller = await shell.ensureOfficeRuntime();
    return {
      controller: Boolean(controller),
      meeting: Boolean(shell.officeMeeting),
      tasks: Boolean(shell.officeTasks),
    };
  });
  expect(runtime).toEqual({
    controller: true,
    meeting: true,
    tasks: true,
  });
  expect(new Set(requestedOfficeModules)).toEqual(officeModules);
});

test("a signed-in member walks into the continuous ten-story Office without a gate", async ({
  page,
}) => {
  test.slow();
  const officeEntryRequests = [];
  const officeFloorRequests = [];
  const officeAttendanceRequests = [];
  const signedVisit = {
    id: "signed-in",
    account: "alice",
    inAt: FIXED_NOW,
    outAt: 0,
  };
  await prepareWorldPage(page, "office-member-entry", {
    session: {
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-office-token",
    },
    officeEntryRequests,
    officeFloorRequests,
    officeAttendanceRequests,
    officeAttendanceFixture: {
      getVisits: [{
        id: "stale-get",
        account: "stale",
        inAt: FIXED_NOW - 60_000,
        outAt: FIXED_NOW - 30_000,
      }],
      inVisits: [signedVisit],
    },
  });
  await waitForWorld(page);
  await moveToOfficeEntrance(page);
  await page.locator("forkmesh-world").evaluate((shell) => {
    const setAttendance = shell.world.setOfficeAttendance.bind(shell.world);
    shell.__officeAttendanceSnapshots = [];
    shell.world.setOfficeAttendance = (snapshot) => {
      shell.__officeAttendanceSnapshots.push(
        structuredClone(snapshot),
      );
      return setAttendance(snapshot);
    };
  });

  await expect(page.locator("[data-world-office-prompt]")).toHaveCount(0);
  await expect(page.locator("[data-world-office-enter]")).toHaveCount(0);
  await expect.poll(() => officeSceneState(page)).toEqual({
    slidingDoorCount: 2,
    hasHingedDoor: false,
    hasKeypad: false,
    campusVisible: true,
    interiorVisible: true,
    space: "town-square",
  });

  await walkIntoOffice(page);
  await expect(page.locator("[data-world-office-lobby]")).toBeHidden();
  expect(officeEntryRequests).toHaveLength(0);
  await expect.poll(() => officeFloorRequests.length).toBe(1);
  expect(officeFloorRequests).toEqual([
    expect.objectContaining({
      method: "GET",
      headers: expect.objectContaining({
        authorization: "Bearer alice-office-token",
      }),
    }),
  ]);
  await expect.poll(() => officeAttendanceRequests.length).toBe(1);
  expect(officeAttendanceRequests).toEqual([
    expect.objectContaining({
      method: "POST",
      body: {
        action: "in",
        floor: "lobby",
        visitId: expect.stringMatching(/^[a-f0-9]{32}$/),
      },
      headers: expect.objectContaining({
        authorization: "Bearer alice-office-token",
      }),
    }),
  ]);
  expect(officeAttendanceRequests.some(({ method }) => method === "GET"))
    .toBe(false);
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.__officeAttendanceSnapshots,
    )
  ).toEqual([{ visits: [signedVisit], leaderboard: [] }]);
  await expect.poll(() => officeSceneState(page)).toEqual({
    slidingDoorCount: 2,
    hasHingedDoor: false,
    hasKeypad: false,
    campusVisible: true,
    interiorVisible: true,
    space: "office-lobby",
  });
});

test("guests walk directly into the public Office lobby without network admission", async ({
  page,
}) => {
  test.slow();
  const officeEntryRequests = [];
  const officeFloorRequests = [];
  const officeAttendanceRequests = [];
  const publicVisit = {
    id: "public-last-visit",
    account: "contributor",
    inAt: FIXED_NOW - 120_000,
    outAt: FIXED_NOW - 60_000,
  };
  await prepareWorldPage(page, "office-guest-entry", {
    officeEntryRequests,
    officeFloorRequests,
    officeAttendanceRequests,
    officeAttendanceFixture: {
      getVisits: [publicVisit],
    },
  });
  await waitForWorld(page);
  await page.locator("forkmesh-world").evaluate((shell) => {
    const setAttendance = shell.world.setOfficeAttendance.bind(shell.world);
    shell.__officeAttendanceSnapshots = [];
    shell.world.setOfficeAttendance = (snapshot) => {
      shell.__officeAttendanceSnapshots.push(
        structuredClone(snapshot),
      );
      return setAttendance(snapshot);
    };
  });
  await walkIntoOffice(page);
  await expect(page.locator("[data-world-office-lobby]")).toBeHidden();
  await expect(page.locator("[data-world-office-prompt]")).toHaveCount(0);
  await expect(page.locator("[data-world-login-form]")).toBeHidden();
  expect(officeEntryRequests).toHaveLength(0);
  expect(officeFloorRequests).toHaveLength(0);
  await expect.poll(() => officeAttendanceRequests.length).toBe(1);
  expect(officeAttendanceRequests).toEqual([
    expect.objectContaining({
      method: "GET",
    }),
  ]);
  expect(officeAttendanceRequests.some(({ method }) => method === "POST"))
    .toBe(false);
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.__officeAttendanceSnapshots,
    )
  ).toEqual([{ visits: [publicVisit], leaderboard: [] }]);
  expect(await officeSceneState(page)).toMatchObject({
    slidingDoorCount: 2,
    hasHingedDoor: false,
    hasKeypad: false,
    space: "office-lobby",
  });
});

test("Office entrance doors slide apart on approach and close after departure", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-sliding-doors");
  await waitForWorld(page);
  const positions = () =>
    page.locator("forkmesh-world").evaluate((shell) => {
      const left = shell.world.scene.getObjectByName(
        "forkmesh-office-sliding-door-left"
      );
      const right = shell.world.scene.getObjectByName(
        "forkmesh-office-sliding-door-right"
      );
      return {
        left: Number(left?.position.x || 0),
        right: Number(right?.position.x || 0),
      };
    });
  const closed = await positions();
  expect(closed.left).toBeLessThan(0);
  expect(closed.right).toBeGreaterThan(0);

  await moveToOfficeEntrance(page, { unpause: true });
  await expect.poll(async () => Math.abs((await positions()).right))
    .toBeGreaterThan(Math.abs(closed.right) + 1);

  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.player.position.set(0, 0.38, 0);
  });
  await expect.poll(async () => {
    const next = await positions();
    return Math.abs(next.right - closed.right);
  }).toBeLessThan(0.08);
});

test("Office walkway is one town path running to the lobby", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-level-threshold");
  await waitForWorld(page);
  const surfaces = await page.locator("forkmesh-world").evaluate((shell) => {
    const bounds = (name) => {
      const object = shell.world.scene.getObjectByName(name);
      object.geometry.computeBoundingBox();
      const box = object.geometry.boundingBox;
      const corners = [];
      for (const x of [box.min.x, box.max.x]) {
        for (const y of [box.min.y, box.max.y]) {
          for (const z of [box.min.z, box.max.z]) {
            corners.push(
              object.localToWorld(object.position.clone().set(x, y, z)),
            );
          }
        }
      }
      return {
        top: Math.max(...corners.map((point) => point.y)),
        minZ: Math.min(...corners.map((point) => point.z)),
        maxZ: Math.max(...corners.map((point) => point.z)),
      };
    };
    return {
      path: bounds("forkmesh-town-path-north-office"),
      lobby: bounds("forkmesh-office-floor-slab-lobby"),
      removed: [
        "forkmesh-office-bridge-deck",
        "forkmesh-office-island-approach-deck",
      ].filter((name) => shell.world.scene.getObjectByName(name)),
    };
  });

  expect(surfaces.removed).toEqual([]);
  expect(surfaces.path.top).toBeCloseTo(0.105, 6);
  expect(surfaces.lobby.top).toBeCloseTo(0.38, 6);
  expect(surfaces.path.minZ).toBeCloseTo(surfaces.lobby.maxZ, 6);
  expect(surfaces.path.maxZ).toBeCloseTo(-6, 6);
});

test("Office lobby marine aquarium is visible, ambient, and animated", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-marine-aquarium", {
    directoryUsers: AQUARIUM_SCHOOL_USERS.slice(0, 1),
  });
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await waitForWorld(page);
  await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.whenOfficeAquariumReady().then(() => true),
  );
  const first = await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.enterOfficeLobby({ floorId: "lobby" });
    shell.world.setPaused(false);
    const aquarium = shell.world.scene.getObjectByName(
      "forkmesh-office-marine-aquarium",
    );
    const fish = aquarium.getObjectByName(
      "forkmesh-office-aquarium-user-fish-0",
    );
    const tail = fish.children[1];
    const bubble = aquarium.getObjectByName(
      "forkmesh-office-aquarium-bubble-0",
    );
    const softCoral = aquarium.getObjectByName(
      "forkmesh-office-aquarium-soft-coral-0",
    );
    const surface = aquarium.getObjectByName(
      "forkmesh-office-aquarium-water-surface",
    );
    const caustics = aquarium.getObjectByName(
      "forkmesh-office-aquarium-caustics",
    );
    const light = aquarium.getObjectByName("forkmesh-office-aquarium-light");
    const interactive = [];
    aquarium.traverse((child) => {
      if (child.userData?.interactive) interactive.push(child.name);
    });
    return {
      visible: aquarium.visible,
      fish: fish.position.toArray(),
      tailRotation: tail.rotation.y,
      bubbleY: bubble.position.y,
      coralRotation: softCoral.rotation.z,
      surfaceHeight: surface.geometry.getAttribute("position").getY(0),
      causticOpacity: caustics.children[0].material.opacity,
      lightIntensity: light.intensity,
      interactive,
    };
  });
  await page.waitForTimeout(220);
  const second = await page.locator("forkmesh-world").evaluate((shell) => {
    const aquarium = shell.world.scene.getObjectByName(
      "forkmesh-office-marine-aquarium",
    );
    const fish = aquarium.getObjectByName(
      "forkmesh-office-aquarium-user-fish-0",
    );
    const tail = fish.children[1];
    const bubble = aquarium.getObjectByName(
      "forkmesh-office-aquarium-bubble-0",
    );
    const softCoral = aquarium.getObjectByName(
      "forkmesh-office-aquarium-soft-coral-0",
    );
    const surface = aquarium.getObjectByName(
      "forkmesh-office-aquarium-water-surface",
    );
    const caustics = aquarium.getObjectByName(
      "forkmesh-office-aquarium-caustics",
    );
    const light = aquarium.getObjectByName("forkmesh-office-aquarium-light");
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior",
    );
    const cameraPosition = interior.localToWorld(
      shell.world.camera.position.clone().set(-16.9, 8.2, -0.52),
    );
    const target = interior.localToWorld(
      shell.world.camera.position.clone().set(-29.79, 6.1, -17.2),
    );
    shell.world.setPaused(true);
    shell.world.camera.fov = 52;
    shell.world.camera.updateProjectionMatrix();
    shell.world.camera.position.copy(cameraPosition);
    shell.world.camera.lookAt(target);
    shell.world.renderer.render(shell.world.scene, shell.world.camera);
    return {
      fish: fish.position.toArray(),
      tailRotation: tail.rotation.y,
      bubbleY: bubble.position.y,
      coralRotation: softCoral.rotation.z,
      surfaceHeight: surface.geometry.getAttribute("position").getY(0),
      causticOpacity: caustics.children[0].material.opacity,
      lightIntensity: light.intensity,
    };
  });
  await page.locator("canvas.world-canvas").screenshot({
    path: "/tmp/forkmesh-office-cinematic-reef-close.png",
    animations: "disabled",
  });
  await page.locator("forkmesh-world").evaluate((shell) => {
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior",
    );
    const cameraPosition = interior.localToWorld(
      shell.world.camera.position.clone().set(-9.71, 10.5, -0.99),
    );
    const target = interior.localToWorld(
      shell.world.camera.position.clone().set(-27.73, 5.2, -11.97),
    );
    shell.world.camera.fov = 59;
    shell.world.camera.updateProjectionMatrix();
    shell.world.camera.position.copy(cameraPosition);
    shell.world.camera.lookAt(target);
    shell.world.renderer.render(shell.world.scene, shell.world.camera);
  });
  await page.locator("canvas.world-canvas").screenshot({
    path: "/tmp/forkmesh-office-cinematic-reef-lobby.png",
    animations: "disabled",
  });
  expect(first.visible).toBe(true);
  expect(first.interactive).toEqual([]);
  expect(second.fish).not.toEqual(first.fish);
  expect(second.tailRotation).not.toBe(first.tailRotation);
  expect(second.bubbleY).not.toBe(first.bubbleY);
  expect(second.coralRotation).not.toBe(first.coralRotation);
  expect(second.surfaceHeight).not.toBe(first.surfaceHeight);
  expect(second.causticOpacity).not.toBe(first.causticOpacity);
  expect(second.lightIntensity).not.toBe(first.lightIntensity);
});

test("aquarium fish render only while visiting the Office", async ({ page }) => {
  await prepareWorldPage(page, "office-aquarium-fish-visibility");
  await waitForWorld(page);
  await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.whenOfficeAquariumReady().then(() => true),
  );
  const visibility = await page.locator("forkmesh-world").evaluate((shell) => {
    const aquarium = shell.world.scene.getObjectByName(
      "forkmesh-office-marine-aquarium",
    );
    const batches = () => [
      "forkmesh-office-aquarium-user-fish-bodies",
      "forkmesh-office-aquarium-user-fish-tails",
      "forkmesh-office-aquarium-user-fish-fins",
    ].map((name) => aquarium.getObjectByName(name).visible);
    const outside = batches();
    shell.world.enterOfficeLobby({ floorId: "lobby" });
    const inside = batches();
    return { outside, inside };
  });

  expect(visibility.outside).toEqual([false, false, false]);
  expect(visibility.inside).toEqual([true, true, true]);
});

test("reef controls stay tank-mounted and report a live country school", async ({
  page,
}) => {
  test.slow();
  await prepareWorldPage(page, "office-aquarium-school-control", {
    directoryUsers: AQUARIUM_SCHOOL_USERS,
  });
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await waitForWorld(page);
  await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.whenOfficeAquariumReady().then(() => true),
  );
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.enterOfficeLobby({ floorId: "lobby" });
    shell.world.setCameraView({
      mode: "first-person",
      yaw: Math.PI / 2,
      pitch: -0.08,
      zoom: 0.75,
    });
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior",
    );
    const position = interior.localToWorld(
      shell.world.player.position.clone().set(-6.06, 0.38, -3.5),
    );
    shell.world.player.position.copy(position);
    shell.world.setPaused(false);
  });

  const panel = page.locator("[data-world-aquarium-controls]");
  const schoolStatus = page.locator(
    "[data-world-aquarium-school-status]",
  );
  await expect(panel).toBeVisible();
  await expect(schoolStatus).toHaveText("SCHOOL MODE · STANDBY");
  await expect(schoolStatus).toHaveAttribute("data-active", "false");
  const toggle = panel.locator("[data-world-aquarium-control-toggle]");
  await toggle.click();
  await expect(toggle).toHaveAttribute("aria-expanded", "true");
  await expect(panel.locator(".world-aquarium-control-actions")).toBeVisible();

  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setPaused(true);
  });
  await panel.hover();
  await page.waitForTimeout(260);
  await expect(panel).toBeVisible();
  for (const action of await panel.locator("button").all()) {
    await panel.hover();
    await expect(action).toBeVisible();
    await action.hover();
    await page.waitForTimeout(100);
    await expect(panel).toBeVisible();
  }
  const anchor = await panel.evaluate((element) => {
    const layerBounds = element.parentElement.getBoundingClientRect();
    return {
      x: layerBounds.left + Number.parseFloat(element.style.left),
      y: layerBounds.top + Number.parseFloat(element.style.top),
    };
  });
  const mountedBox = await panel.boundingBox();
  expect(mountedBox).not.toBeNull();
  expect(mountedBox.x + mountedBox.width).toBeCloseTo(anchor.x - 8, 0);
  expect(mountedBox.y).toBeCloseTo(anchor.y + 8, 0);
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setPaused(false);
  });

  await expect
    .poll(
      () => schoolStatus.getAttribute("data-active"),
      { timeout: 20_000 },
    )
    .toBe("true");
  await expect(schoolStatus).toContainText("COUNTRY ACTIVE");
  await page.waitForTimeout(1_900);
  const school = await page.locator("forkmesh-world").evaluate(
    (shell, fishCount) => {
      const aquarium = shell.world.scene.getObjectByName(
        "forkmesh-office-marine-aquarium",
      );
      return Array.from({ length: fishCount }, (_, index) =>
        aquarium
          .getObjectByName(`forkmesh-office-aquarium-user-fish-${index}`)
          .position.toArray(),
      );
    },
    AQUARIUM_SCHOOL_USERS.length,
  );
  const distance = (left, right) =>
    Math.hypot(
      left[0] - right[0],
      left[1] - right[1],
      left[2] - right[2],
    );
  const pairDistances = school.flatMap((left, leftIndex) =>
    school
      .slice(leftIndex + 1)
      .map((right) => distance(left, right))
  ).sort((left, right) => left - right);
  const withinCountry = (pairDistances[0] + pairDistances[1]) / 2;
  const acrossCountries =
    pairDistances.slice(2).reduce((total, value) => total + value, 0) /
    (pairDistances.length - 2);
  await page.locator("forkmesh-world").screenshot({
    path: "/tmp/forkmesh-office-aquarium-school-control.png",
    animations: "disabled",
  });
  expect(
    { withinCountry, acrossCountries, school },
    "matching-country fish should form tighter pods than different countries",
  ).toEqual(
    expect.objectContaining({
      withinCountry: expect.any(Number),
      acrossCountries: expect.any(Number),
    }),
  );
  expect(acrossCountries).toBeGreaterThan(withinCountry + 2);
});

test("aquarium draws one low-poly instanced fish for every public user", async ({
  page,
}) => {
  const users = Array.from({ length: 31 }, (_, index) => ({
    name: `reef-member-${String(index).padStart(2, "0")}`,
    countryCode: index % 2 ? "US" : "CA",
    browser: index % 2 ? "Firefox" : "Safari",
    os: index % 2 ? "Linux" : "macOS",
    activityBucket: index % 3 ? "24h" : "10d",
  }));
  await prepareWorldPage(page, "office-aquarium-all-public-users", {
    directoryUsers: users,
  });
  await waitForWorld(page);
  await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.whenOfficeAquariumReady().then(() => true),
  );
  const school = await page.locator("forkmesh-world").evaluate(
    (shell, userCount) => {
      const aquarium = shell.world.scene.getObjectByName(
        "forkmesh-office-marine-aquarium",
      );
      const batch = (name) =>
        aquarium.getObjectByName(name);
      const triangles = (mesh) =>
        Math.floor(mesh.geometry.index.count / 3) * mesh.count;
      const bodies = batch("forkmesh-office-aquarium-user-fish-bodies");
      const tails = batch("forkmesh-office-aquarium-user-fish-tails");
      const fins = batch("forkmesh-office-aquarium-user-fish-fins");
      return {
        accountPopulation: aquarium.userData.accountPopulation,
        visibleFish: aquarium.userData.visibleFish,
        allAnchorsPresent: Array.from(
          { length: userCount },
          (_, index) =>
            aquarium.getObjectByName(
              `forkmesh-office-aquarium-user-fish-${index}`,
            ) !== undefined,
        ).every(Boolean),
        batches: [bodies, tails, fins].map((mesh) => ({
          instanced: mesh.isInstancedMesh,
          count: mesh.count,
          triangles: triangles(mesh),
        })),
      };
    },
    users.length,
  );

  expect(school.accountPopulation).toBe(users.length);
  expect(school.visibleFish).toBe(users.length);
  expect(school.allAnchorsPresent).toBe(true);
  expect(school.batches).toEqual([
    { instanced: true, count: users.length, triangles: users.length * 36 },
    { instanced: true, count: users.length, triangles: users.length * 2 },
    { instanced: true, count: users.length * 4, triangles: users.length * 8 },
  ]);
});

test("aquarium blocks lobby movement and double-click travel", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-aquarium-collision");
  await waitForWorld(page);
  const placeAtGlass = () =>
    page.locator("forkmesh-world").evaluate((shell) => {
      shell.world.enterOfficeLobby({ floorId: "lobby" });
      shell.world.setCameraView({
        mode: "third-person",
        yaw: Math.PI / 3,
        pitch: 0.35,
        zoom: 0.9,
      });
      const interior = shell.world.scene.getObjectByName(
        "forkmesh-office-interior",
      );
      const position = interior.localToWorld(
        shell.world.player.position.clone().set(-27.45, 0.38, -15.85),
      );
      shell.world.player.position.copy(position);
      shell.world.setPaused(false);
    });

  const readLocalPosition = () =>
    page.locator("forkmesh-world").evaluate((shell) => {
      const interior = shell.world.scene.getObjectByName(
        "forkmesh-office-interior",
      );
      return interior.worldToLocal(
        shell.world.player.getWorldPosition(shell.world.player.position.clone()),
      ).toArray();
    });

  await placeAtGlass();
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setControl("forward", true);
  });
  await page.waitForTimeout(450);
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setControl("forward", false);
  });
  const walked = await readLocalPosition();

  await placeAtGlass();
  await page.locator("forkmesh-world").evaluate((shell) => {
    const canvas = shell.world.renderer.domElement;
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior",
    );
    const target = interior.localToWorld(
      shell.world.player.position.clone().set(-30.22, 0.38, -17.45),
    );
    target.project(shell.world.camera);
    const bounds = canvas.getBoundingClientRect();
    canvas.dispatchEvent(new MouseEvent("dblclick", {
      bubbles: true,
      button: 0,
      clientX: bounds.left + (target.x * 0.5 + 0.5) * bounds.width,
      clientY: bounds.top + (-target.y * 0.5 + 0.5) * bounds.height,
    }));
  });
  await page.waitForTimeout(450);
  const dashed = await readLocalPosition();
  const aquariumFrontLimit = 1.66 + 0.46 - 0.01;
  const tankDepthOf = ([x, , z]) =>
    (x + 30.2243) * Math.cos(-Math.PI / 6) - (z + 17.45) * Math.sin(-Math.PI / 6);

  expect(tankDepthOf(walked)).toBeGreaterThanOrEqual(aquariumFrontLimit);
  expect(tankDepthOf(dashed)).toBeGreaterThanOrEqual(aquariumFrontLimit);
});

test("aquarium feeding appears nearby and expires after one minute", async ({
  page,
}) => {
  test.slow();
  await prepareWorldPage(page, "office-aquarium-feeding", {
    directoryUsers: reefDirectoryUsers(),
  });
  await waitForWorld(page);
  await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.whenOfficeAquariumReady().then(() => true),
  );
  const feedAction = page.locator("[data-world-aquarium-feed]");
  await expect(feedAction).toBeHidden();

  const before = await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.enterOfficeLobby({ floorId: "lobby" });
    shell.world.setCameraView({
      mode: "third-person",
      yaw: Math.PI / 2,
      pitch: 0.3,
      zoom: 0.72,
    });
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior",
    );
    const position = interior.localToWorld(
      shell.world.player.position.clone().set(-26.67, 0.38, -15.4),
    );
    shell.world.player.position.copy(position);
    shell.world.setPaused(false);
    const aquarium = shell.world.scene.getObjectByName(
      "forkmesh-office-marine-aquarium",
    );
    return aquarium.getObjectByName(
      "forkmesh-office-aquarium-user-fish-0",
    ).position.toArray();
  });

  await page.locator("[data-world-aquarium-control-toggle]").click();
  await expect(feedAction).toBeVisible();
  await expect(feedAction).toHaveText("FEED");
  await expect(feedAction).toHaveAccessibleName("Feed the fishes");
  await page.locator("forkmesh-world").screenshot({
    path: "/tmp/forkmesh-office-aquarium-feed-action.png",
    animations: "disabled",
  });
  await feedAction.click();
  await expect(feedAction).toBeDisabled();
  await expect(feedAction).toHaveText("FEEDING…");
  await expect(feedAction).toHaveAccessibleName("Fishes are feeding");
  await page.waitForTimeout(220);
  await page.locator("forkmesh-world").screenshot({
    path: "/tmp/forkmesh-office-aquarium-feeding.png",
    animations: "disabled",
  });

  const active = await page.locator("forkmesh-world").evaluate((shell) => {
    const aquarium = shell.world.scene.getObjectByName(
      "forkmesh-office-marine-aquarium",
    );
    const food = aquarium.getObjectByName(
      "forkmesh-office-aquarium-food",
    );
    const fish = aquarium.getObjectByName(
      "forkmesh-office-aquarium-user-fish-0",
    );
    const state = shell.world.getOfficeAquariumState();
    const fishPositions = aquarium
      .getObjectByName("forkmesh-office-aquarium-user-fish-anchors")
      .children
      .map((child) => child.position.toArray());
    const pelletPositions = food.children.map((child) =>
      child.position.toArray()
    );
    return {
      state,
      foodVisible: food.visible,
      fishPosition: fish.position.toArray(),
      fishPositions,
      pelletPositions,
      repeatStarted: shell.world.feedOfficeAquarium(),
    };
  });
  expect(active.state.active).toBe(true);
  expect(active.state.durationMs).toBe(60_000);
  expect(active.foodVisible).toBe(true);
  expect(active.repeatStarted).toBe(false);
  expect(active.fishPosition).not.toEqual(before);
  const fishZ = active.fishPositions.map((position) => position[2]);
  const pelletZ = active.pelletPositions.map((position) => position[2]);
  expect(Math.max(...fishZ) - Math.min(...fishZ)).toBeGreaterThan(6);
  expect(Math.max(...active.fishPositions.map((position) => position[1])))
    .toBeLessThan(9.6);
  expect(Math.max(...pelletZ) - Math.min(...pelletZ)).toBeGreaterThan(6.5);

  const expired = await page.locator("forkmesh-world").evaluate(
    (shell, endsAt) => {
      const aquarium = shell.world.scene.getObjectByName(
        "forkmesh-office-marine-aquarium",
      );
      const food = aquarium.getObjectByName(
        "forkmesh-office-aquarium-food",
      );
      const fish = aquarium.getObjectByName(
        "forkmesh-office-aquarium-user-fish-0",
      );
      const feedingPosition = fish.position.toArray();
      const state = shell.world.getOfficeAquariumState(endsAt + 1);
      return {
        state,
        foodVisible: food.visible,
        feedingPosition,
      };
    },
    active.state.endsAt,
  );
  expect(expired.state.active).toBe(false);
  expect(expired.foodVisible).toBe(false);
  await expect(feedAction).toBeEnabled();
  await expect(feedAction).toHaveText("FEED");
  await expect(feedAction).toHaveAccessibleName("Feed the fishes");
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) => {
      const aquarium = shell.world.scene.getObjectByName(
        "forkmesh-office-marine-aquarium",
      );
      return aquarium.getObjectByName(
        "forkmesh-office-aquarium-user-fish-0",
      ).position.toArray();
    })
  ).not.toEqual(expired.feedingPosition);
});

test("Office cinematic reef remains composed with reduced motion", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-cinematic-reef-reduced-motion", {
    directoryUsers: AQUARIUM_SCHOOL_USERS.slice(0, 1),
  });
  await page.emulateMedia({ reducedMotion: "reduce" });
  await waitForWorld(page);
  await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.whenOfficeAquariumReady().then(() => true),
  );
  const state = await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.enterOfficeLobby({ floorId: "lobby" });
    const aquarium = shell.world.scene.getObjectByName(
      "forkmesh-office-marine-aquarium",
    );
    const fish = aquarium.getObjectByName(
      "forkmesh-office-aquarium-user-fish-0",
    );
    const bubble = aquarium.getObjectByName(
      "forkmesh-office-aquarium-bubble-0",
    );
    return {
      visible: aquarium.visible,
      fishPosition: fish.position.toArray(),
      bubblePosition: bubble.position.toArray(),
    };
  });
  await page.waitForTimeout(220);
  const settled = await page.locator("forkmesh-world").evaluate((shell) => {
    const aquarium = shell.world.scene.getObjectByName(
      "forkmesh-office-marine-aquarium",
    );
    return {
      fishPosition: aquarium.getObjectByName(
        "forkmesh-office-aquarium-user-fish-0",
      ).position.toArray(),
      bubblePosition: aquarium.getObjectByName(
        "forkmesh-office-aquarium-bubble-0",
      ).position.toArray(),
    };
  });
  const feeding = await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.feedOfficeAquarium();
    const aquarium = shell.world.scene.getObjectByName(
      "forkmesh-office-marine-aquarium",
    );
    const fish = aquarium.getObjectByName(
      "forkmesh-office-aquarium-user-fish-0",
    );
    return {
      state: shell.world.getOfficeAquariumState(),
      fishPosition: fish.position.toArray(),
      tailRotation: fish.children[1].rotation.y,
      pelletPosition: aquarium.getObjectByName(
        "forkmesh-office-aquarium-food-pellet-0",
      ).position.toArray(),
    };
  });
  await page.waitForTimeout(220);
  const feedingSettled = await page.locator("forkmesh-world").evaluate(
    (shell) => {
      const aquarium = shell.world.scene.getObjectByName(
        "forkmesh-office-marine-aquarium",
      );
      const fish = aquarium.getObjectByName(
        "forkmesh-office-aquarium-user-fish-0",
      );
      return {
        fishPosition: fish.position.toArray(),
        tailRotation: fish.children[1].rotation.y,
        pelletPosition: aquarium.getObjectByName(
          "forkmesh-office-aquarium-food-pellet-0",
        ).position.toArray(),
      };
    },
  );
  const expired = await page.locator("forkmesh-world").evaluate(
    (shell, endsAt) => {
      const state = shell.world.getOfficeAquariumState(endsAt + 1);
      const aquarium = shell.world.scene.getObjectByName(
        "forkmesh-office-marine-aquarium",
      );
      return {
        state,
        foodVisible: aquarium.getObjectByName(
          "forkmesh-office-aquarium-food",
        ).visible,
        fishPosition: aquarium.getObjectByName(
          "forkmesh-office-aquarium-user-fish-0",
        ).position.toArray(),
      };
    },
    feeding.state.endsAt,
  );
  expect(state.visible).toBe(true);
  expect(state.fishPosition[1]).toBeGreaterThan(2);
  expect(state.bubblePosition[1]).toBeGreaterThan(0.8);
  expect(settled).toEqual({
    fishPosition: state.fishPosition,
    bubblePosition: state.bubblePosition,
  });
  expect(feeding.state.active).toBe(true);
  expect(feedingSettled).toEqual({
    fishPosition: feeding.fishPosition,
    tailRotation: feeding.tailRotation,
    pelletPosition: feeding.pelletPosition,
  });
  expect(expired.state.active).toBe(false);
  expect(expired.foodVisible).toBe(false);
  expect(Math.hypot(
    ...expired.fishPosition.map(
      (value, index) => value - state.fishPosition[index],
    ),
  )).toBeLessThan(0.05);
});

test("a first-frame doorway crossing enters before proximity catches up", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-first-frame-entry");
  await waitForWorld(page);

  const entry = await page.locator("forkmesh-world").evaluate(async (shell) => {
    // Freeze between the doorway collision and the later proximity ticker,
    // matching a fast dash/double-click arrival in that first frame.
    shell.world.setPaused(true);
    shell.world.player.position.set(0, 0.38, -169.53);
    const controller = await shell.ensureOfficeRuntime({
      userInitiated: true,
    });
    const proximityBefore = controller.proximity;
    const nonDoorwayEntered = await controller.enterOffice({
      source: "door",
    });
    const entered = await controller.enterOffice({
      source: "doorway",
    });
    return {
      proximityBefore,
      nonDoorwayEntered,
      entered,
      active: controller.active,
      space: shell.world.getPosition().space,
    };
  });
  expect(entry).toEqual({
    proximityBefore: "distant",
    nonDoorwayEntered: false,
    entered: true,
    active: true,
    space: "office-lobby",
  });
});

test("walking through the Office doorway hydrates floor access without admission POST", async ({
  page,
}) => {
  const officeEntryRequests = [];
  const officeFloorRequests = [];
  await prepareWorldPage(page, "office-walk-in-entry", {
    session: {
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-office-token",
    },
    officeEntryRequests,
    officeFloorRequests,
  });
  await waitForWorld(page);
  await walkIntoOffice(page);
  await expect.poll(() => officeFloorRequests.length).toBe(1);
  expect(officeEntryRequests).toHaveLength(0);
});

test("Office doorway stays outside at the jamb and never blocks on access hydration", async ({
  page,
}) => {
  test.slow();
  const officeFloorRequests = [];
  const officeAttendanceRequests = [];
  await prepareWorldPage(page, "office-doorway-background-access", {
    session: {
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-office-token",
    },
    officeFloorRequests,
    officeAttendanceRequests,
    officeFloorDelayMs: 5_000,
  });
  await waitForWorld(page);

  const jambState = await page.locator("forkmesh-world").evaluate(async (shell) => {
    shell.world.setPaused(true);
    const controller = await shell.ensureOfficeRuntime({
      userInitiated: true,
    });
    shell.world.setPaused(false);
    const status = shell.world.scene.getObjectByName(
      "forkmesh-office-door-status",
    );
    const doorway = status.getWorldPosition(status.position.clone());
    // The avatar's center is just through the glass, but its full collision
    // body has not cleared the inner jamb yet.
    shell.world.player.position.set(doorway.x, 0.38, doorway.z - 0.26);
    return {
      active: controller.active,
      space: shell.world.getPosition().space,
      status: status.userData.officeDoorStatus,
      visible: status.visible,
    };
  });
  expect(jambState).toEqual({
    active: false,
    space: "town-square",
    status: "open",
    visible: false,
  });
  await page.waitForTimeout(150);
  expect(officeFloorRequests).toHaveLength(0);
  expect(officeAttendanceRequests).toHaveLength(0);

  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setControl("forward", true);
  });
  try {
    await waitForOfficeEntry(page);
    await expect.poll(() => officeFloorRequests.length).toBe(1);
    const whileSyncing = await page.locator("forkmesh-world").evaluate(
      (shell) => {
        const status = shell.world.scene.getObjectByName(
          "forkmesh-office-door-status",
        );
        return {
          position: shell.world.player.getWorldPosition(
            shell.world.player.position.clone(),
          ).z,
          status: status.userData.officeDoorStatus,
          visible: status.visible,
        };
      },
    );
    expect(whileSyncing.status).toBe("syncing");
    expect(whileSyncing.visible).toBe(true);

    await page.waitForTimeout(250);
    const movingPosition = await page.locator("forkmesh-world").evaluate(
      (shell) =>
        shell.world.player.getWorldPosition(
          shell.world.player.position.clone(),
        ).z,
    );
    expect(movingPosition).toBeLessThan(whileSyncing.position - 0.05);
    expect(officeAttendanceRequests).toHaveLength(0);
    await page.locator("forkmesh-world").evaluate((shell) => {
      shell.world.setControl("forward", false);
    });

    await expect.poll(() =>
      page.locator("forkmesh-world").evaluate((shell) =>
        shell.world.scene.getObjectByName(
          "forkmesh-office-door-status",
        ).userData.officeDoorStatus
      )
    ).toBe("ready");
    await expect.poll(() => officeAttendanceRequests.length).toBe(1);

    await page.locator("forkmesh-world").evaluate((shell) => {
      const status = shell.world.scene.getObjectByName(
        "forkmesh-office-door-status",
      );
      const doorway = status.getWorldPosition(status.position.clone());
      shell.world.player.position.set(doorway.x, 0.38, doorway.z - 0.8);
      shell.world.setControl("back", true);
    });
    await expect.poll(() =>
      page.locator("forkmesh-world").evaluate((shell) => ({
        active: shell.officeController.active,
        space: shell.world.getPosition().space,
        status: shell.world.scene.getObjectByName(
          "forkmesh-office-door-status",
        ).userData.officeDoorStatus,
        visible: shell.world.scene.getObjectByName(
          "forkmesh-office-door-status",
        ).visible,
      }))
    ).toEqual({
      active: false,
      space: "town-square",
      status: "open",
      visible: false,
    });
  } finally {
    await page.locator("forkmesh-world").evaluate((shell) => {
      shell.world.setControl("forward", false);
      shell.world.setControl("back", false);
    });
  }
});

test("Office doorway retries a rejected crossing without requiring backward movement", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-doorway-no-twitch");
  await waitForWorld(page);
  await moveToOfficeEntrance(page, { unpause: true });
  await page.locator("forkmesh-world").evaluate((shell) => {
    const original =
      shell.officeController.enterOffice.bind(shell.officeController);
    shell.__officeEntryAttempts = 0;
    shell.officeController.enterOffice = async (entry = {}) => {
      shell.__officeEntryAttempts += 1;
      if (shell.__officeEntryAttempts === 1) {
        // Match a transient rejected/interrupted handoff. The controller's
        // normal finally path clears pending; movement remains inward.
        shell.world.setOfficeDoorwayEntryPending(false);
        return false;
      }
      return original(entry);
    };
    shell.world.setControl("forward", true);
  });
  try {
    await waitForOfficeEntry(page);
    const result = await page.locator("forkmesh-world").evaluate((shell) => ({
      attempts: shell.__officeEntryAttempts,
      space: shell.world.getPosition().space,
    }));
    expect(result).toEqual({ attempts: 2, space: "office-lobby" });
  } finally {
    await page.locator("forkmesh-world").evaluate((shell) => {
      shell.world.setControl("forward", false);
    });
  }
});

test("Office entry preserves the live avatar and keeps zoom inside the tower", async ({
  page,
}) => {
  test.slow();
  await prepareWorldPage(page, "office-seamless-entry-camera", {
    session: {
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-office-token",
    },
  });
  await waitForWorld(page);
  await moveToOfficeEntrance(page, { unpause: true });
  const before = await page.locator("forkmesh-world").evaluate(async (shell) => {
    const controller = await shell.ensureOfficeRuntime();
    shell.world.setCameraZoom(0.9);
    const originalEnterOffice =
      controller.enterOffice.bind(controller);
    const sample = () => {
      const interior = shell.world.scene.getObjectByName(
        "forkmesh-office-interior"
      );
      const playerWorld = shell.world.player.getWorldPosition(
        shell.world.player.position.clone()
      );
      const target = playerWorld.clone();
      target.y += 2.2;
      return {
        uuid: shell.world.player.uuid,
        local: interior.worldToLocal(playerWorld).toArray(),
        cameraDistance: shell.world.camera.position.distanceTo(target),
      };
    };
    shell.__officeThresholdHandoff = null;
    controller.enterOffice = (...args) => {
      const thresholdBefore = sample();
      const result = originalEnterOffice(...args);
      shell.__officeThresholdHandoff = {
        before: thresholdBefore,
        after: sample(),
      };
      return result;
    };
    return {
      uuid: shell.world.player.uuid,
      camera: shell.world.getCameraState(),
      position: shell.world.player.position.toArray(),
    };
  });

  await walkIntoOffice(page);
  await page.waitForTimeout(100);
  const after = await page.locator("forkmesh-world").evaluate((shell) => {
    const clone = shell.world.scene.getObjectByName(
      "forkmesh-office-lobby-player"
    );
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior"
    );
    const target = shell.world.player.getWorldPosition(
      shell.world.player.position.clone()
    );
    target.y += 2.2;
    return {
      uuid: shell.world.player.uuid,
      camera: shell.world.getCameraState(),
      position: shell.world.player.position.toArray(),
      playerVisible: shell.world.player.visible,
      cloneVisible: clone?.visible === true,
      handoff: shell.__officeThresholdHandoff,
      cameraLocal: interior.worldToLocal(
        shell.world.camera.position.clone()
      ).toArray(),
      cameraDistance: shell.world.camera.position.distanceTo(target),
    };
  });
  expect(after.uuid).toBe(before.uuid);
  expect(after.camera.mode).toBe(before.camera.mode);
  expect(after.camera.yaw).toBeCloseTo(before.camera.yaw, 8);
  expect(after.camera.pitch).toBeCloseTo(before.camera.pitch, 8);
  expect(after.camera.zoom).toBeCloseTo(before.camera.zoom, 8);
  expect(after.playerVisible).toBe(true);
  expect(after.cloneVisible).toBe(false);
  expect(after.handoff.before.uuid).toBe(before.uuid);
  expect(after.handoff.after.uuid).toBe(before.uuid);
  expect(after.handoff.before.local[2]).toBeGreaterThan(44);
  expect(after.handoff.before.local[2]).toBeLessThanOrEqual(44.48);
  expect(after.handoff.after.local[0])
    .toBeCloseTo(after.handoff.before.local[0], 7);
  expect(after.handoff.after.local[2])
    .toBeCloseTo(after.handoff.before.local[2], 7);
  expect(after.cameraLocal[2]).toBeGreaterThan(45);
  expect(after.cameraDistance).toBeGreaterThan(10);
  expect(after.cameraDistance)
    .toBeGreaterThan(after.handoff.before.cameraDistance * 0.45);
  expect(
    Math.hypot(
      after.position[0] - before.position[0],
      after.position[2] - before.position[2],
    ),
  ).toBeLessThan(6);

  // From just inside the same doorway, offset the target far enough that the
  // default orbit ray crosses the front plane beside the opening. It must hit
  // the facade clamp rather than inheriting the centered portal exception.
  await page.locator("forkmesh-world").evaluate((shell) => {
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior"
    );
    const point = interior.localToWorld(
      shell.world.player.position.clone().set(4, 0.38, 35.7)
    );
    shell.world.player.parent.worldToLocal(point);
    shell.world.player.position.copy(point);
    shell.world.setCameraZoom(0.9);
  });
  await page.waitForTimeout(100);
  const facadeClamped = await page.locator("forkmesh-world").evaluate(
    (shell) => {
      const interior = shell.world.scene.getObjectByName(
        "forkmesh-office-interior"
      );
      const camera = interior.worldToLocal(
        shell.world.camera.position.clone()
      );
      const player = interior.worldToLocal(
        shell.world.player.getWorldPosition(
          shell.world.player.position.clone()
        )
      );
      player.y += 2.2;
      return {
        camera: camera.toArray(),
        distance: camera.distanceTo(player),
      };
    },
  );
  expect(facadeClamped.camera[2]).toBeLessThan(44.8);
  expect(facadeClamped.distance).toBeLessThan(2);

  await page.locator("forkmesh-world").evaluate((shell) => {
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior"
    );
    const point = interior.localToWorld(
      shell.world.player.position.clone().set(0, 0.38, 0)
    );
    shell.world.player.parent.worldToLocal(point);
    shell.world.player.position.copy(point);
    shell.world.setCameraZoom(28);
  });
  await page.waitForTimeout(700);
  const bounded = await page.locator("forkmesh-world").evaluate((shell) => {
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior"
    );
    const cameraLocal = interior.worldToLocal(
      shell.world.camera.position.clone()
    );
    const playerLocal = interior.worldToLocal(
      shell.world.player.getWorldPosition(shell.world.player.position.clone())
    );
    return {
      camera: cameraLocal.toArray(),
      player: playerLocal.toArray(),
      distance: cameraLocal.distanceTo(playerLocal),
      requestedZoom: shell.world.getCameraState().zoom,
    };
  });
  expect(bounded.requestedZoom).toBe(28);
  expect(bounded.camera[0]).toBeGreaterThan(-84.3);
  expect(bounded.camera[0]).toBeLessThan(84.3);
  expect(bounded.camera[1]).toBeGreaterThan(0.49);
  expect(bounded.camera[1]).toBeLessThan(15.46);
  expect(bounded.camera[2]).toBeGreaterThan(-44.3);
  expect(bounded.camera[2]).toBeLessThan(44.8);
  expect(bounded.distance).toBeGreaterThan(10);
  expect(bounded.distance).toBeLessThan(30);
});

test("Office rooftop restores the full world zoom outside the elevator", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-rooftop-full-zoom");
  await waitForWorld(page);
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setOfficeAccess({
      authenticated: true,
      account: "alice",
      allowedFloorIds: ["lobby", "rooftop"],
    });
    shell.world.enterOfficeLobby();
    shell.world.enterOfficeLobby({ floorId: "rooftop" });
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior"
    );
    const patioWorld = interior.localToWorld(
      shell.world.player.position.clone().set(0, 48.38, 0)
    );
    shell.world.player.parent.worldToLocal(patioWorld);
    shell.world.player.position.copy(patioWorld);
    shell.world.setCameraZoom(28);
    shell.world.setPaused(false);
  });

  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) => {
      const target = shell.world.player
        .getWorldPosition(shell.world.player.position.clone());
      target.y += 2.2;
      return shell.world.camera.position.distanceTo(target);
    })
  ).toBeGreaterThan(700);
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) => ({
      space: shell.world.getPosition().space,
      zoom: shell.world.getCameraState().zoom,
    }))
  ).toEqual({
    space: "office-rooftop",
    zoom: 28,
  });

  // Pull the orbit eye below its target to look sharply upward. It may retain
  // full horizontal zoom, but it must never pass down through the roof slab.
  const canvas = page.locator("[data-world-canvas-wrap] canvas");
  const box = await canvas.boundingBox();
  expect(box).not.toBeNull();
  const x = Math.round(box.x + box.width * 0.5);
  await page.mouse.move(x, Math.round(box.y + box.height * 0.86));
  await page.mouse.down();
  await page.mouse.move(x, Math.round(box.y + box.height * 0.08), {
    steps: 4,
  });
  await page.mouse.up();
  await page.waitForTimeout(120);
  const upward = await page.locator("forkmesh-world").evaluate((shell) => {
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior"
    );
    const camera = interior.worldToLocal(shell.world.camera.position.clone());
    const target = interior.worldToLocal(
      shell.world.player.getWorldPosition(shell.world.player.position.clone())
    );
    return {
      camera: camera.toArray(),
      horizontalDistance: Math.hypot(
        camera.x - target.x,
        camera.z - target.z,
      ),
      zoom: shell.world.getCameraState().zoom,
    };
  });
  expect(upward.camera[1]).toBeGreaterThanOrEqual(64.54);
  expect(upward.horizontalDistance).toBeGreaterThan(400);
  expect(upward.zoom).toBe(28);
});

test("Office rooftop has complete sittable patio furniture and a real source launcher", async ({
  page,
}) => {
  await page.addInitScript(() => {
    window.__officeLaptopOpened = [];
    window.open = (...args) => {
      window.__officeLaptopOpened.push(args);
      return null;
    };
  });
  await prepareWorldPage(page, "office-rooftop-patio");
  await waitForWorld(page);
  const patio = await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setOfficeAccess({
      authenticated: true,
      account: "alice",
      allowedFloorIds: ["lobby", "marketing", "rooftop"],
    });
    shell.world.enterOfficeLobby();
    shell.world.enterOfficeLobby({ floorId: "rooftop" });
    const scene = shell.world.scene;
    const interior = scene.getObjectByName("forkmesh-office-interior");
    const tables = [];
    const chairs = [];
    scene.traverse((object) => {
      if (/^forkmesh-office-rooftop-table-\d+$/.test(object.name)) {
        tables.push({
          id: object.name,
          legs: object.children.filter((child) =>
            child.name.includes("-leg-")
          ).length,
        });
      }
      if (/^forkmesh-office-rooftop-chair-\d+$/.test(object.name)) {
        chairs.push({
          id: object.name,
          legs: object.children.filter((child) =>
            child.name.includes("-leg-")
          ).length,
          backs: object.children.filter((child) =>
            child.name.endsWith("-back")
          ).length,
        });
      }
    });
    const marketingRejected =
      shell.world.sitOnOfficeChair("chair-1");
    const sat = shell.world.sitOnOfficeChair("rooftop-chair-1");
    const chair = scene.getObjectByName(
      "forkmesh-office-rooftop-chair-1"
    );
    const seated = interior.worldToLocal(
      shell.world.player.getWorldPosition(shell.world.player.position.clone())
    );
    const toTable = {
      x: -40 - chair.position.x,
      z: 8 - chair.position.z,
    };
    const length = Math.hypot(toTable.x, toTable.z) || 1;
    const facingDot =
      (-Math.sin(shell.world.player.rotation.y) * (toTable.x / length)) +
      (-Math.cos(shell.world.player.rotation.y) * (toTable.z / length));
    const stood = shell.world.sitOnOfficeChair("rooftop-chair-1");
    shell.world.enterOfficeLobby({ floorId: "marketing" });
    const rooftopRejected =
      shell.world.sitOnOfficeChair("rooftop-chair-1");
    shell.world.enterOfficeLobby({ floorId: "rooftop" });
    return {
      tables,
      chairs,
      marketingRejected,
      rooftopRejected,
      sat,
      stood,
      seated: seated.toArray(),
      standingY: shell.world.player.position.y,
      facingDot,
      laptop: Boolean(scene.getObjectByName(
        "forkmesh-office-rooftop-laptop"
      )),
      laptopBase: Boolean(scene.getObjectByName(
        "forkmesh-office-rooftop-laptop-base"
      )),
      laptopScreen: Boolean(scene.getObjectByName(
        "forkmesh-office-rooftop-laptop-screen"
      )),
      strayTelescope: Boolean(scene.getObjectByName("telescopeTube")),
    };
  });
  expect(patio.tables).toHaveLength(3);
  expect(patio.tables.every((table) => table.legs === 4)).toBe(true);
  expect(patio.chairs).toHaveLength(12);
  expect(patio.chairs.every((chair) =>
    chair.legs === 4 && chair.backs === 1
  )).toBe(true);
  expect(patio).toMatchObject({
    marketingRejected: false,
    rooftopRejected: false,
    sat: true,
    stood: true,
    standingY: 48.38,
    laptop: true,
    laptopBase: true,
    laptopScreen: true,
    strayTelescope: false,
  });
  expect(patio.seated[1]).toBeGreaterThan(64);
  expect(patio.seated[1]).toBeLessThan(48.38);
  expect(patio.facingDot).toBeGreaterThan(0.99);

  const laptopPoint = await page.locator("forkmesh-world").evaluate((shell) => {
    const scene = shell.world.scene;
    const interior = scene.getObjectByName("forkmesh-office-interior");
    const screen = scene.getObjectByName(
      "forkmesh-office-rooftop-laptop-screen"
    );
    const target = screen.getWorldPosition(screen.position.clone());
    const camera = interior.localToWorld(
      screen.position.clone().set(0, 69, 25)
    );
    shell.world.setPaused(true);
    shell.world.camera.position.copy(camera);
    shell.world.camera.lookAt(target);
    shell.world.camera.updateMatrixWorld(true);
    shell.world.renderer.render(scene, shell.world.camera);
    const projected = target.clone().project(shell.world.camera);
    const rect = shell.world.renderer.domElement.getBoundingClientRect();
    return {
      x: rect.left + (projected.x * 0.5 + 0.5) * rect.width,
      y: rect.top + (-projected.y * 0.5 + 0.5) * rect.height,
    };
  });
  await page.mouse.click(laptopPoint.x, laptopPoint.y);
  await expect.poll(() =>
    page.evaluate(() => window.__officeLaptopOpened)
  ).toEqual([[
    "/forkmesh/forkmesh/blob/world/public/world/world-scene.js",
    "_blank",
    "noopener,noreferrer",
  ]]);
});

test("leaving an Office meeting resumes at a walkable first-person pose", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-meeting-walkable-handoff");
  await waitForWorld(page);
  const handoff = await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setOfficeAccess({
      authenticated: true,
      account: "alice",
      allowedFloorIds: ["marketing"],
    });
    shell.world.enterOfficeLobby();
    shell.world.setCameraMode("first-person");
    shell.world.enterOfficeMeeting({
      roomName: "general",
      participantId: "local-me",
      participants: [{
        id: "local-me",
        name: "Alice",
        x: 0,
        z: 0,
        yaw: 0,
        pose: "standing",
      }],
    });
    let localAvatar = null;
    shell.world.scene.traverse((object) => {
      if (object.userData?.id === "local-me") localAvatar = object;
    });
    const localAvatarVisible = localAvatar?.visible === true;
    shell.world.enterOfficeLobby({ floorId: "marketing" });
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior"
    );
    const playerLocal = interior.worldToLocal(
      shell.world.player.getWorldPosition(shell.world.player.position.clone())
    );
    return {
      localAvatarFound: Boolean(localAvatar),
      localAvatarVisible,
      playerLocal: playerLocal.toArray(),
      cameraMode: shell.world.getCameraState().mode,
      playerVisible: shell.world.player.visible,
    };
  });
  expect(handoff.localAvatarFound).toBe(true);
  expect(handoff.localAvatarVisible).toBe(false);
  expect(
    Math.abs(handoff.playerLocal[0]) > 4.36 ||
      Math.abs(handoff.playerLocal[2]) > 2.46,
  ).toBe(true);
  expect(handoff.playerLocal[1]).toBeCloseTo(16.38, 3);
  expect(handoff.cameraMode).toBe("first-person");
  expect(handoff.playerVisible).toBe(false);
});

test("Marketing studio furniture, wall features, reception, and open FM mark align", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-marketing-visual-alignment");
  await waitForWorld(page);
  const state = await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setOfficeAccess({
      authenticated: true,
      account: "alice",
      allowedFloorIds: ["lobby", "marketing"],
    });
    shell.world.enterOfficeLobby();
    shell.world.enterOfficeLobby({ floorId: "marketing" });
    const scene = shell.world.scene;
    const interior = scene.getObjectByName("forkmesh-office-interior");
    const tabletop = scene.getObjectByName(
      "forkmesh-office-marketing-tabletop"
    );
    const legs = [];
    const chairs = [];
    scene.traverse((object) => {
      if (object.name === "forkmesh-office-marketing-table-leg") {
        legs.push(object);
      }
      if (/^forkmesh-office-chair-\d+$/.test(object.name)) {
        const position = object.position;
        const seatedYaw = object.rotation.y + Math.PI;
        const length = Math.hypot(position.x, position.z) || 1;
        chairs.push({
          id: object.name,
          backZ: object.getObjectByName(`${object.name}-back`)?.position.z,
          inwardDot:
            (-Math.sin(seatedYaw) * (-position.x / length)) +
            (-Math.cos(seatedYaw) * (-position.z / length)),
        });
      }
    });
    const tableBottom =
      tabletop.position.y - tabletop.geometry.parameters.height / 2;
    const highestLegTop = Math.max(...legs.map(
      (leg) => leg.position.y + leg.geometry.parameters.height / 2
    ));
    const taskBoard = scene.getObjectByName(
      "forkmesh-office-marketing-task-board"
    );
    const initiativesBoard = scene.getObjectByName(
      "forkmesh-office-marketing-initiatives-board"
    );
    const wallPlacard = scene.getObjectByName(
      "forkmesh-office-wall-placard-marketing"
    );
    const placards = [];
    scene.traverse((object) => {
      if (object.userData?.officeWallMounted === true) {
        placards.push({
          name: object.name,
          mesh: object.isMesh === true,
          sprite: object.isSprite === true,
        });
      }
    });
    const receptionDesk = scene.getObjectByName(
      "forkmesh-office-reception-desk"
    );
    const maya = scene.getObjectByName("avatar:office-greeter-maya");
    const noah = scene.getObjectByName("avatar:office-greeter-noah");
    let noahCount = 0;
    scene.traverse((object) => {
      if (object.name === "avatar:office-greeter-noah") noahCount += 1;
    });
    const chromeCube = scene.getObjectByName("forkmesh-reflective-fm-cube");
    const chromeMark = scene.getObjectByName(
      "forkmesh-reflective-fm-cube-fixed-tilt"
    );
    const support = scene.getObjectByName(
      "forkmesh-reflective-fm-cube-support"
    );
    const contact = scene.getObjectByName(
      "forkmesh-reflective-fm-cube-contact-point"
    );
    const solidBodies = [];
    let fFaces = 0;
    let mFaces = 0;
    let panels = 0;
    const throughCutouts = [];
    chromeCube.traverse((object) => {
      if (object.name.startsWith("forkmesh-reflective-f-face-")) fFaces += 1;
      if (object.name.startsWith("forkmesh-reflective-m-face-")) mFaces += 1;
      if (object.name.startsWith("forkmesh-reflective-fm-panel-")) {
        panels += 1;
        throughCutouts.push(Number(object.userData.logoThroughCutouts) || 0);
      }
      const size = object.geometry?.parameters;
      if (
        object.isMesh &&
        size?.width === 5.6 &&
        size?.height === 5.6 &&
        size?.depth === 5.6
      ) {
        solidBodies.push(object.name || "solid");
      }
    });
    const contactWorld = contact.getWorldPosition(contact.position.clone());
    const supportTop = support.localToWorld(
      support.position.clone().set(
        0,
        support.geometry.parameters.height / 2,
        0,
      )
    );
    const sat = shell.world.sitOnOfficeChair("chair-1");
    const seatedWorld = shell.world.player.getWorldPosition(
      shell.world.player.position.clone()
    );
    const seatedLocal = interior.worldToLocal(seatedWorld).toArray();
    const stood = shell.world.sitOnOfficeChair("chair-1");
    return {
      space: shell.world.getPosition().space,
      tableBottom,
      highestLegTop,
      chairCount: chairs.length,
      chairs,
      boards: {
        task: taskBoard.position.toArray(),
        initiatives: initiativesBoard.position.toArray(),
        wallPlacard: wallPlacard.position.toArray(),
      },
      placards,
      reception: {
        desk: receptionDesk.position.toArray(),
        hasMaya: Boolean(maya),
        noah: noah?.position.toArray(),
        noahCount,
      },
      logo: {
        verticalAxisOnly: {
          x: chromeCube.rotation.x,
          z: chromeCube.rotation.z,
        },
        fixedTiltQuaternion: chromeMark.quaternion.toArray(),
        contactSupportDistance: contactWorld.distanceTo(supportTop),
        fFaces,
        mFaces,
        panels,
        throughCutouts: throughCutouts.sort((left, right) => left - right),
        supportCount: support ? 1 : 0,
        solidBodies,
      },
      seating: {
        sat,
        stood,
        seatedLocal,
        standingY: shell.world.player.position.y,
      },
    };
  });
  await page.locator("forkmesh-world").evaluate((shell) => {
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior"
    );
    const cameraPosition = interior.localToWorld(
      shell.world.camera.position.clone().set(0, 27, 33)
    );
    const target = interior.localToWorld(
      shell.world.camera.position.clone().set(0, 21, -12)
    );
    shell.world.setPaused(true);
    shell.world.camera.position.copy(cameraPosition);
    shell.world.camera.lookAt(target);
    shell.world.renderer.render(shell.world.scene, shell.world.camera);
  });
  await page.screenshot({
    path: "/tmp/forkmesh-office-marketing.png",
    animations: "disabled",
  });

  expect(state.space).toBe("office-marketing");
  expect(state.highestLegTop).toBeGreaterThanOrEqual(state.tableBottom);
  expect(state.highestLegTop - state.tableBottom).toBeLessThanOrEqual(0.2);
  expect(state.chairCount).toBe(8);
  expect(state.chairs.every((chair) => chair.backZ < 0)).toBe(true);
  expect(state.chairs.every((chair) => chair.inwardDot > 0.99)).toBe(true);
  [-31.567, 22.4, -18.225].forEach((value, index) =>
    expect(state.boards.task[index]).toBeCloseTo(value, 3));
  expect(state.boards.initiatives).toEqual([-8, 22.5, -36.34]);
  [-31.627, 7.3, 18.26].forEach((value, index) =>
    expect(state.boards.wallPlacard[index]).toBeCloseTo(value, 3));
  expect(state.placards.length).toBeGreaterThanOrEqual(11);
  expect(state.placards.every((placard) => placard.mesh && !placard.sprite))
    .toBe(true);
  expect(state.reception.desk).toEqual([0, 1.05, -13]);
  expect(state.reception.hasMaya).toBe(false);
  expect(state.reception.noah).toEqual([0, 0.38, -16.5]);
  expect(state.reception.noahCount).toBe(1);
  expect(state.logo.verticalAxisOnly.x).toBeCloseTo(0, 7);
  expect(state.logo.verticalAxisOnly.z).toBeCloseTo(0, 7);
  expect(state.logo.fixedTiltQuaternion.some(
    (value) => Math.abs(value) > 0.1
  )).toBe(true);
  expect(state.logo.contactSupportDistance).toBeLessThan(0.03);
  expect(state.logo).toMatchObject({
    fFaces: 2,
    mFaces: 2,
    panels: 2,
    throughCutouts: [7, 7],
    supportCount: 1,
    solidBodies: [],
  });
  expect(state.seating).toMatchObject({
    sat: true,
    stood: true,
    standingY: 16.38,
  });
  expect(state.seating.seatedLocal[0]).toBeCloseTo(-7.4, 2);
  expect(state.seating.seatedLocal[2]).toBeCloseTo(0, 2);
  // Avatar origins sit below the seat because their hips are modeled above
  // the origin; the rendered hips remain on the chair and the folded feet
  // reach the floor.
  expect(state.seating.seatedLocal[1]).toBeGreaterThan(15.5);
  expect(state.seating.seatedLocal[1]).toBeLessThan(16.38);
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setPaused(false);
    shell.world.enterOfficeLobby({ floorId: "lobby" });
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior"
    );
    const cameraPosition = interior.localToWorld(
      shell.world.camera.position.clone().set(0, 11, 33)
    );
    const target = interior.localToWorld(
      shell.world.camera.position.clone().set(0, 5, -18)
    );
    shell.world.setPaused(true);
    shell.world.camera.position.copy(cameraPosition);
    shell.world.camera.lookAt(target);
    shell.world.renderer.render(shell.world.scene, shell.world.camera);
  });
  await page.screenshot({
    path: "/tmp/forkmesh-office-lobby.png",
    animations: "disabled",
  });
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setPaused(false);
  });
  await page.waitForTimeout(1200);
  await page.locator("forkmesh-world").evaluate((shell) => {
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior"
    );
    const cameraPosition = interior.localToWorld(
      shell.world.camera.position.clone().set(-16, 15, 14)
    );
    const target = interior.localToWorld(
      shell.world.camera.position.clone().set(-19, 7.5, 11)
    );
    shell.world.scene.getObjectByName(
      "forkmesh-reflective-fm-cube"
    ).rotation.y = 0;
    shell.world.setPaused(true);
    shell.world.camera.fov = 65;
    shell.world.camera.updateProjectionMatrix();
    shell.world.camera.position.copy(cameraPosition);
    shell.world.camera.lookAt(target);
    shell.world.renderer.render(shell.world.scene, shell.world.camera);
  });
  await page.locator("canvas.world-canvas").screenshot({
    path: "/tmp/forkmesh-office-logo.png",
    animations: "disabled",
  });
});

test("Noah greets desk approaches locally without per-frame bubble spam", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-noah-reception");
  await waitForWorld(page);
  const placeAtDeskDistance = async (localZ) =>
    page.locator("forkmesh-world").evaluate((shell, z) => {
      const interior = shell.world.scene.getObjectByName(
        "forkmesh-office-interior"
      );
      const point = interior.localToWorld(
        shell.world.player.position.clone().set(0, 0.38, z)
      );
      shell.world.player.parent.worldToLocal(point);
      shell.world.player.position.copy(point);
      shell.world.setPaused(false);
    }, localZ);

  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.enterOfficeLobby();
  });
  await placeAtDeskDistance(-8.5);
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) => {
      let count = 0;
      shell.world.scene.traverse((object) => {
        if (object.userData?.officeReceptionGreeting === true) count += 1;
      });
      return count;
    })
  ).toBe(1);

  const first = await page.locator("forkmesh-world").evaluate((shell) => {
    const scene = shell.world.scene;
    const noah = scene.getObjectByName("avatar:office-greeter-noah");
    let bubble = null;
    let noahCount = 0;
    scene.traverse((object) => {
      if (object.name === "avatar:office-greeter-noah") noahCount += 1;
      if (object.userData?.officeReceptionGreeting === true) bubble = object;
    });
    return {
      noahCount,
      hasMaya: Boolean(scene.getObjectByName("avatar:office-greeter-maya")),
      uuid: bubble?.uuid || "",
      message: bubble?.userData?.message || "",
      bubble: bubble?.position.toArray() || [],
      noah: noah?.getWorldPosition(noah.position.clone()).toArray() || [],
    };
  });
  expect(first.noahCount).toBe(1);
  expect(first.hasMaya).toBe(false);
  expect(first.message).toContain("lobby is open to everyone");
  expect(first.message).toContain("Log in");
  expect(first.message).toContain("restricted team floors");
  expect(Math.abs(first.bubble[0] - first.noah[0])).toBeLessThan(0.05);
  expect(Math.abs(first.bubble[2] - first.noah[2])).toBeLessThan(0.05);
  expect(first.bubble[1]).toBeGreaterThan(first.noah[1]);

  // Cross the hysteresis boundary and return during the cooldown. The existing
  // local bubble remains; no new sprite/event is created on animation frames.
  await placeAtDeskDistance(-3.5);
  await page.waitForTimeout(100);
  await placeAtDeskDistance(-8.5);
  await page.waitForTimeout(180);
  const second = await page.locator("forkmesh-world").evaluate((shell) => {
    const bubbles = [];
    shell.world.scene.traverse((object) => {
      if (object.userData?.officeReceptionGreeting === true) {
        bubbles.push(object.uuid);
      }
    });
    return bubbles;
  });
  expect(second).toEqual([first.uuid]);
});

test("FM sculpture uses mirrored through-cut panels at a deterministic yaw", async ({
  page,
}) => {
  test.setTimeout(60_000);
  await prepareWorldPage(page, "office-fm-sculpture");
  await waitForWorld(page);
  const entryCapture = await page.locator("forkmesh-world").evaluate((shell) => {
    const scene = shell.world.scene;
    const renderer = shell.world.renderer;
    const reflection = shell.world.scene.getObjectByName(
      "forkmesh-office-logo-reflection-camera"
    );
    const chromeCube = scene.getObjectByName(
      "forkmesh-reflective-fm-cube"
    );
    const localPlayer = shell.world.player;
    const originalRender = renderer.render;
    const renderStates = [];
    renderer.render = function renderWithLogoReflectionAudit(
      renderedScene,
      renderedCamera,
    ) {
      if (renderedCamera?.parent === reflection) {
        let visibleAvatarMeshes = 0;
        localPlayer.traverse((object) => {
          if (!object.isMesh || !object.visible) return;
          let ancestor = object.parent;
          while (ancestor && ancestor !== localPlayer) {
            if (!ancestor.visible) return;
            ancestor = ancestor.parent;
          }
          visibleAvatarMeshes += 1;
        });
        renderStates.push({
          playerVisible: localPlayer.visible,
          visibleAvatarMeshes,
          sculptureVisible: chromeCube.visible,
        });
      }
      return originalRender.call(this, renderedScene, renderedCamera);
    };
    reflection.userData.testLogoReflectionRenderStates = renderStates;
    reflection.userData.testLogoReflectionRestore = () => {
      renderer.render = originalRender;
    };
    shell.world.enterOfficeLobby();
    return {
      count: Number(reflection?.userData?.logoCaptureCount) || 0,
      settleMs: Number(reflection?.userData?.logoCaptureSettleMs) || 900,
    };
  });
  expect(entryCapture.count).toBe(0);
  // Motion keeps the six-face cube render deferred. Once movement stops, one
  // settled capture includes the live avatar; continued idling does not loop.
  await page.keyboard.down("w");
  await page.waitForTimeout(250);
  const movingCapture = await page.locator("forkmesh-world").evaluate((shell) =>
    Number(shell.world.scene.getObjectByName(
      "forkmesh-office-logo-reflection-camera"
    )?.userData?.logoCaptureCount) || 0
  );
  await page.keyboard.up("w");
  expect(movingCapture).toBe(0);
  await page.locator("forkmesh-world").evaluate((shell) => {
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-office-interior"
    );
    const reflectedAvatarPoint = interior.localToWorld(
      shell.world.player.position.clone().set(-10, 0.38, 11)
    );
    shell.world.player.parent.worldToLocal(reflectedAvatarPoint);
    shell.world.player.position.copy(reflectedAvatarPoint);
  });
  await page.waitForTimeout(entryCapture.settleMs + 350);
  await expect
    .poll(
      () =>
        page.locator("forkmesh-world").evaluate((shell) =>
          Number(shell.world.scene.getObjectByName(
            "forkmesh-office-logo-reflection-camera"
          )?.userData?.logoCaptureCount) || 0
        ),
      { timeout: 15_000 },
    )
    .toBeGreaterThanOrEqual(1);
  const settledCapture = await page.locator("forkmesh-world").evaluate(
    (shell) => {
      const reflection = shell.world.scene.getObjectByName(
        "forkmesh-office-logo-reflection-camera"
      );
      const states = [
        ...(reflection?.userData?.testLogoReflectionRenderStates || []),
      ];
      reflection?.userData?.testLogoReflectionRestore?.();
      delete reflection?.userData?.testLogoReflectionRenderStates;
      delete reflection?.userData?.testLogoReflectionRestore;
      return {
        count: Number(reflection?.userData?.logoCaptureCount) || 0,
        states,
        playerRestored:
          shell.world.player.visible ===
          (shell.world.getCameraState().mode !== "first-person"),
      };
    }
  );
  expect(settledCapture.count).toBe(1);
  expect(settledCapture.states.length).toBeGreaterThanOrEqual(6);
  expect(settledCapture.states.length % 6).toBe(0);
  expect(settledCapture.states.every((state) =>
    state.playerVisible &&
    state.visibleAvatarMeshes >= 10 &&
    !state.sculptureVisible
  )).toBe(true);
  expect(settledCapture.playerRestored).toBe(true);
  await page.waitForTimeout(entryCapture.settleMs + 200);
  const logo = await page.locator("forkmesh-world").evaluate((shell) => {
    const scene = shell.world.scene;
    const cube = scene.getObjectByName("forkmesh-reflective-fm-cube");
    const mark = scene.getObjectByName(
      "forkmesh-reflective-fm-cube-fixed-tilt"
    );
    const panels = [];
    const letterPieces = [];
    const oldOverlays = [];
    mark.traverse((object) => {
      if (object.name.startsWith("forkmesh-reflective-fm-panel-")) {
        panels.push({
          name: object.name,
          throughCutouts: Number(object.userData.logoThroughCutouts) || 0,
          metalness: object.material.metalness,
          roughness: object.material.roughness,
          hasEnvironment: Boolean(object.material.envMap),
        });
      }
      if (object.isMesh && Array.isArray(object.material)) {
        letterPieces.push({
          materialCount: object.material.length,
          mirroredOuterFace:
            object.material[4]?.metalness >= 0.9 &&
            Boolean(object.material[4]?.envMap),
          matteSides:
            object.material.slice(0, 4).every((material) =>
              material === object.material[5] &&
              material.roughness >= 0.7
            ),
        });
      }
      const geometry = object.geometry?.parameters;
      if (
        geometry?.radiusTop === 0.48 ||
        (
          geometry?.width === 0.3 &&
          geometry?.height === 0.08
        )
      ) {
        oldOverlays.push(object.name || object.geometry.type);
      }
    });
    const support = scene.getObjectByName(
      "forkmesh-reflective-fm-cube-support"
    );
    const contact = scene.getObjectByName(
      "forkmesh-reflective-fm-cube-contact-point"
    );
    const reflection = scene.getObjectByName(
      "forkmesh-office-logo-reflection-camera"
    );
    const contactWorld = contact.getWorldPosition(contact.position.clone());
    const supportTop = support.localToWorld(
      support.position.clone().set(
        0,
        support.geometry.parameters.height / 2,
        0,
      )
    );
    cube.rotation.y = 0;
    const interior = scene.getObjectByName("forkmesh-office-interior");
    shell.world.setPaused(true);
    shell.world.camera.fov = 48;
    shell.world.camera.updateProjectionMatrix();
    shell.world.camera.position.copy(interior.localToWorld(
      shell.world.camera.position.clone().set(-10, 12, 19)
    ));
    shell.world.camera.lookAt(interior.localToWorld(
      shell.world.camera.position.clone().set(-19, 6.3, 11)
    ));
    shell.world.renderer.render(scene, shell.world.camera);
    return {
      panels,
      letterPieces,
      oldOverlays,
      outerTilt: [cube.rotation.x, cube.rotation.z],
      fixedTilt: mark.quaternion.toArray(),
      supportCount: support ? 1 : 0,
      contactSupportDistance: contactWorld.distanceTo(supportTop),
      reflection: {
        captureCount:
          Number(reflection?.userData?.logoCaptureCount) || 0,
        capturePolicy: reflection?.userData?.logoCapturePolicy || "",
        capturedPlayer:
          reflection?.userData?.logoCapturedPlayer === true,
      },
    };
  });
  await page.locator("canvas.world-canvas").screenshot({
    path: "/tmp/forkmesh-office-logo.png",
    animations: "disabled",
  });
  expect(logo.panels).toHaveLength(2);
  expect(logo.panels.every((panel) =>
    panel.throughCutouts === 7 &&
    panel.metalness >= 0.78 &&
    panel.roughness <= 0.08 &&
    panel.hasEnvironment
  )).toBe(true);
  expect(logo.letterPieces).toHaveLength(14);
  expect(logo.letterPieces.every((piece) =>
    piece.materialCount === 6 &&
    piece.mirroredOuterFace &&
    piece.matteSides
  )).toBe(true);
  expect(logo.oldOverlays).toEqual([]);
  expect(logo.outerTilt[0]).toBeCloseTo(0, 7);
  expect(logo.outerTilt[1]).toBeCloseTo(0, 7);
  expect(logo.fixedTilt.some((value) => Math.abs(value) > 0.1)).toBe(true);
  expect(logo.supportCount).toBe(1);
  expect(logo.contactSupportDistance).toBeLessThan(0.03);
  expect(logo.reflection).toEqual({
    captureCount: 1,
    capturePolicy: "dirty-idle-once",
    capturedPlayer: true,
  });
});

test("Office clear facade keeps stable glazing while floor displays stay culled", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-clear-facade");
  await waitForWorld(page);
  await page.locator("forkmesh-world").evaluate((shell, position) => {
    shell.world.setSpawn({
      space: "town-square",
      x: position[0],
      y: position[1],
      z: position[2],
      heading: 0,
    });
    shell.world.setPaused(false);
  }, OFFICE_ENTRANCE_POSITION);
  const glass = await page.locator("forkmesh-world").evaluate((shell) => {
    const scene = shell.world.scene;
    const transparentMeshes = (root) => {
      const meshes = [];
      root?.traverse((object) => {
        if (object.isMesh && object.material?.transparent) {
          meshes.push(object);
        }
      });
      return meshes;
    };
    const exterior = [];
    scene.traverse((object) => {
      if (
        object.isMesh &&
        object.userData?.landmark === "office" &&
        object.material?.name === "forkmesh-office-clear-glass"
      ) {
        exterior.push(object);
      }
    });
    const teamLayers = [
      "marketing",
      "engineering",
    ].flatMap((floorId) => transparentMeshes(
      scene.getObjectByName(`forkmesh-office-floor-${floorId}`)
    ));
    const teamFloorsVisible = [
      "marketing",
      "engineering",
    ].every((floorId) =>
      scene.getObjectByName(`forkmesh-office-floor-${floorId}`)?.visible !== false
    );
    const officeInterior = scene.getObjectByName("forkmesh-office-interior");
    const rooftop = transparentMeshes(
      scene.getObjectByName("forkmesh-office-floor-rooftop")
    );
    const shaft = transparentMeshes(
      scene.getObjectByName("forkmesh-office-glass-elevator-shaft")
    );
    const car = transparentMeshes(
      scene.getObjectByName("forkmesh-office-glass-elevator-car")
    );
    const doors = [
      "forkmesh-office-sliding-door-left",
      "forkmesh-office-sliding-door-right",
    ].map((name) => scene.getObjectByName(name));
    return {
      exterior: {
        count: exterior.length,
        stable: exterior.every((mesh) =>
          mesh.material.transparent === true &&
          mesh.material.opacity === 0.24 &&
          mesh.material.depthWrite === false &&
          mesh.castShadow === false &&
          mesh.receiveShadow === false
        ),
      },
      teamLayerCount: teamLayers.length,
      teamFloorsVisible,
      interiorVisible: officeInterior?.visible !== false,
      rooftop: {
        count: rooftop.length,
        stable: rooftop.every((mesh) => mesh.material.depthWrite === false),
      },
      shaftLayerCount: shaft.length,
      car: {
        count: car.length,
        stable: car.every((mesh) =>
          mesh.material.depthWrite === false && mesh.castShadow === false
        ),
      },
      doors: {
        count: doors.filter(Boolean).length,
        stable: doors.every((mesh) =>
          mesh?.material?.transparent === true &&
          mesh.material.opacity === 0.3 &&
          mesh.material.metalness === 0 &&
          mesh.material.depthWrite === false &&
          mesh.castShadow === false &&
          mesh.receiveShadow === false
        ),
      },
    };
  });
  expect(glass.exterior).toEqual({ count: 12, stable: true });
  expect(glass.teamLayerCount).toBeGreaterThan(0);
  expect(glass.teamFloorsVisible).toBe(false);
  expect(glass.interiorVisible).toBe(false);
  expect(glass.rooftop.count).toBeGreaterThanOrEqual(5);
  expect(glass.rooftop.stable).toBe(true);
  expect(glass.shaftLayerCount).toBe(0);
  expect(glass.car.count).toBeGreaterThanOrEqual(5);
  expect(glass.car.stable).toBe(true);
  expect(glass.doors).toEqual({ count: 2, stable: true });
});

test("Office elevator exposes five floors while enforcing team access", async ({
  page,
}) => {
  test.setTimeout(120_000);
  await prepareWorldPage(page, "office-elevator-access", {
    session: {
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-office-token",
    },
    officeFloorAccess: {
      allowedFloorIds: ["engineering"],
      teams: ["engineering"],
    },
  });
  await waitForWorld(page);
  await walkIntoOffice(page);
  await expect(page.locator("[data-world-office-lobby]")).toBeHidden();
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) => {
      let allowed = false;
      shell.world.scene.traverse((object) => {
        if (
          object.userData?.officeFloorId === "engineering" &&
          object.userData?.officeFloorAllowed === true
        ) {
          allowed = true;
        }
      });
      return allowed;
    })
  ).toBe(true);

  const access = await page.locator("forkmesh-world").evaluate((shell) => {
    const destinations = new Map();
    const scene = shell.world.scene;
    const car = scene.getObjectByName("forkmesh-office-glass-elevator-car");
    const shaft = scene.getObjectByName(
      "forkmesh-office-glass-elevator-shaft"
    );
    const panel = scene.getObjectByName(
      "forkmesh-office-elevator-cabin-panel"
    );
    const buttons = [];
    const isDescendantOf = (object, ancestor) => {
      for (let current = object?.parent; current; current = current.parent) {
        if (current === ancestor) return true;
      }
      return false;
    };
    shell.world.scene.traverse((object) => {
      if (object.userData?.interactive !== "office-elevator-floor") return;
      buttons.push(object);
      destinations.set(object.userData.officeFloorId, {
        id: object.userData.officeFloorId,
        level: object.userData.officeFloorLevel,
        number: object.userData.officeFloorNumber,
        teamLabel: object.userData.officeFloorTeamLabel,
        allowed: object.userData.officeFloorAllowed === true,
        y: Number(object.position.y.toFixed(2)),
      });
    });
    return {
      destinations: [...destinations.values()].sort((left, right) =>
        left.level - right.level
      ),
      buttonCount: buttons.length,
      buttonsMoveWithCar:
        Boolean(car) && buttons.every((button) => isDescendantOf(button, car)),
      panelParent: panel?.parent?.name || "",
      carPosition: car
        ? { x: car.position.x, y: car.position.y, z: car.position.z }
        : null,
      shaftPosition: shaft
        ? { x: shaft.position.x, y: shaft.position.y, z: shaft.position.z }
        : null,
    };
  });
  expect(access.destinations).toHaveLength(4);
  expect(access.destinations.map((floor) => floor.id)).toEqual([
    "lobby",
    "marketing",
    "engineering",
    "rooftop",
  ]);
  expect(access.buttonCount).toBe(4);
  expect(access.destinations.map((floor) => floor.number)).toEqual([
    1, 2, 3, 4,
  ]);
  expect(access.destinations[0].teamLabel).toBe("LOBBY");
  expect(access.destinations[2].teamLabel).toBe("ENGINEERING");
  expect(access.destinations[3].teamLabel).toBe("ROOF");
  expect(access.destinations[0].y).toBe(access.destinations[1].y);
  expect(access.destinations[1].y).toBeLessThan(access.destinations[2].y);
  expect(access.destinations[2].y).toBe(access.destinations[3].y);
  expect(access.buttonsMoveWithCar).toBe(true);
  expect(access.panelParent).toBe("forkmesh-office-glass-elevator-car");
  expect(access.carPosition).toMatchObject({ x: 18, y: 0 });
  expect(access.shaftPosition).toMatchObject({ x: 18, y: 0 });
  expect(access.carPosition.z).toBeGreaterThanOrEqual(44.5);
  expect(access.shaftPosition.z).toBeCloseTo(access.carPosition.z, 5);
  expect(
    Object.fromEntries(
      access.destinations.map((floor) => [floor.id, floor.allowed])
    ),
  ).toMatchObject({
    lobby: true,
    marketing: false,
    engineering: true,
    rooftop: true,
  });

  // Calling the controller from elsewhere on the floor must not teleport the
  // avatar into the elevator. Only a visitor physically inside the cabin may
  // start a ride.
  const outsideCabin = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.travelToOfficeFloor("engineering")
  );
  expect(outsideCabin).toBe(false);

  const cabinPosition = await page
    .locator("forkmesh-world")
    .evaluate((shell) => {
      const scene = shell.world.scene;
      const car = scene.getObjectByName("forkmesh-office-glass-elevator-car");
      const avatar = shell.world.player;
      car.updateWorldMatrix(true, false);
      avatar.position.x = car.matrixWorld.elements[12] - 1.25;
      avatar.position.z = car.matrixWorld.elements[14] - 1.25;
      return { x: avatar.position.x, z: avatar.position.z };
    });
  const locked = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.travelToOfficeFloor("marketing")
  );
  expect(locked).toBe(false);
  const travelled = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.travelToOfficeFloor("engineering")
  );
  expect(travelled).toBe(true);
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) => ({
      space: shell.world.getPosition().space,
      avatar: (() => {
        const object = shell.world.player;
        return object
          ? { x: object.position.x, y: object.position.y, z: object.position.z }
          : null;
      })(),
      carHeight: shell.world.scene.getObjectByName(
        "forkmesh-office-glass-elevator-car"
      )?.position.y,
    }))
  ).toMatchObject({
    space: "office-engineering",
    avatar: {
      x: cabinPosition.x,
      y: 32.38,
      z: cabinPosition.z,
    },
    carHeight: 32,
  });
  // The elevator changes the active floor while the camera can remain in the
  // same near-LOD class. The story itself must make the destination visible;
  // waiting for a later zoom update once caused a successful arrival to render
  // an empty Office level.
  const arrivedFloor = await page.locator("forkmesh-world").evaluate((shell) => {
    const deferredByFloor = {};
    shell.world.scene.traverse((object) => {
      if (object.userData?.officeFloorDeferredDisplay !== true) return;
      const floorId = object.userData.officeFloorId;
      deferredByFloor[floorId] ||= [];
      deferredByFloor[floorId].push(object.visible === true);
    });
    const engineering = shell.world.scene.getObjectByName(
      "forkmesh-office-floor-engineering",
    );
    const marketing = shell.world.scene.getObjectByName(
      "forkmesh-office-floor-marketing",
    );
    const slab = engineering?.getObjectByName(
      "forkmesh-office-floor-slab-engineering",
    );
    return {
      engineeringVisible: engineering?.visible === true,
      engineeringSlabVisible: slab?.visible === true,
      priorFloorHidden: marketing?.visible === false,
      deferredByFloor,
      deferredCount:
        shell.world.player.parent.userData.officeDeferredFloorDisplayCount,
      deferredVisibleCount:
        shell.world.player.parent.userData.officeDeferredFloorVisibleCount,
    };
  });
  expect(arrivedFloor).toMatchObject({
    engineeringVisible: true,
    engineeringSlabVisible: true,
    priorFloorHidden: true,
  });
  expect(arrivedFloor.deferredByFloor.engineering.length).toBeGreaterThan(0);
  expect(arrivedFloor.deferredByFloor.engineering.every(Boolean)).toBe(true);
  expect(arrivedFloor.deferredByFloor.marketing.some(Boolean)).toBe(false);
  expect(arrivedFloor.deferredByFloor.rooftop.some(Boolean)).toBe(false);
  expect(arrivedFloor.deferredVisibleCount).toBe(
    arrivedFloor.deferredByFloor.engineering.length,
  );
  expect(arrivedFloor.deferredCount).toBe(
    Object.values(arrivedFloor.deferredByFloor).flat().length,
  );
});

test("Local controls carry Work and Security tabs instead of a session card", async ({
  page,
}) => {
  const taskId = "b".repeat(32);
  const officeTaskFixture = {
    canManage: false,
    requests: [],
    tasks: [
      {
        id: taskId,
        title: "Write the launch digest",
        assignee: "alice",
        status: "idle",
        elapsedMs: 0,
        startedAt: 0,
        nextCheckinAt: 0,
        createdAt: FIXED_NOW,
        updatedAt: FIXED_NOW,
        lastCheckin: null,
      },
    ],
  };
  const accountSessionFixture = {
    requests: [],
    sessions: [
      {
        id: "c".repeat(32),
        deviceLabel: "Web browser",
        ipAddress: "203.0.113.9",
        createdAt: FIXED_NOW - 60_000,
        lastSeenAt: FIXED_NOW - 60_000,
        expiresAt: FIXED_NOW + 60_000,
        current: true,
      },
      {
        id: "d".repeat(32),
        deviceLabel: "Desktop node",
        ipAddress: "198.51.100.7",
        createdAt: FIXED_NOW - 7_200_000,
        lastSeenAt: FIXED_NOW - 3_600_000,
        expiresAt: FIXED_NOW + 60_000,
        current: false,
      },
    ],
  };
  await prepareWorldPage(page, "local-controls-tabs", {
    session: {
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-token",
    },
    officeTaskFixture,
    accountSessionFixture,
  });
  await waitForWorld(page);
  // The owner-only avatar plate carries assigned work, never a session card.
  const board = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.setSelfWorkBoard({}),
  );
  expect(board.state).toBe("locked");

  await page.locator("[data-world-settings-open]").first().click();
  await expect(page.locator("[data-world-settings]")).toHaveAttribute(
    "data-open",
    "true",
  );

  // The View tab is the default, so the environment controls stay reachable.
  await expect(page.locator("[data-world-light-level]")).toBeVisible();
  await expect(page.locator("[data-world-session-list]")).toBeHidden();

  await page.locator('[data-world-settings-tab="security"]').click();
  const sessions = page.locator("[data-world-session-list] > li");
  await expect(sessions).toHaveCount(2);
  await expect(sessions.first()).toContainText("Web browser · this device");
  await expect(sessions.first()).toContainText("203.0.113.9");
  await expect(sessions.nth(1)).toContainText("198.51.100.7");
  await expect(page.locator("[data-world-light-level]")).toBeHidden();

  // Revoking another device leaves this one signed in and does not reload.
  await sessions
    .nth(1)
    .locator("[data-world-session-revoke]")
    .click();
  await expect(sessions).toHaveCount(1);
  expect(
    accountSessionFixture.requests.some(
      (request) =>
        request.method === "DELETE" && request.target === "d".repeat(32),
    ),
  ).toBe(true);

  await page.locator('[data-world-settings-tab="work"]').click();
  await expect(page.locator("[data-world-work-total]")).toHaveText("1");
  await expect(page.locator("[data-world-work-active]")).toHaveText("0");
  const workTask = page
    .locator("[data-world-organization-task-list] .world-task-row")
    .filter({ hasText: "Write the launch digest" })
    .first();
  await expect(workTask).toContainText("Write the launch digest");
  await workTask.locator('[data-world-office-task-action="start"]').click();
  await expect
    .poll(() =>
      officeTaskFixture.requests.filter(
        (request) =>
          request.path ===
          `/api/tasks/${taskId}/start`,
      ).length,
    )
    .toBeGreaterThan(0);
  // The row flips to Stop once the server-timed start lands.
  await expect(
    page.locator(
      '[data-world-organization-task-list] [data-world-office-task-action="stop"]',
    ),
  ).toBeVisible();
  await expect(page.locator("[data-world-work-active]")).toHaveText("1");

});


test("Office marketing tasks can be assigned, timed, and checked in privately", async ({
  page,
}) => {
  const taskId = "a".repeat(32);
  const officeTaskFixture = {
    canManage: true,
    members: ["alice", "bob"],
    requests: [],
    tasks: [
      {
        id: taskId,
        title: "Prepare launch digest",
        department: "marketing",
        assignee: "alice",
        status: "idle",
        elapsedMs: 0,
        startedAt: 0,
        nextCheckinAt: 0,
        createdAt: FIXED_NOW,
        updatedAt: FIXED_NOW,
        lastCheckin: null,
      },
    ],
  };
  const worldFrames = [];
  await prepareWorldPage(page, "office-marketing-tasks", {
    session: {
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-token",
    },
    officeTaskFixture,
    worldSocketHandler: (socket, id) => {
      socket.onMessage((raw) => worldFrames.push(String(raw)));
      socket.send(JSON.stringify({ type: "welcome", id, peers: [] }));
    },
  });
  await waitForWorld(page);
  await page.evaluate(async () => {
    const shell = document.querySelector("forkmesh-world");
    await shell.ensureOfficeRuntime({ userInitiated: true });
    shell.officeTasks.setActive(true);
    shell.officeTasks.open();
  });

  const panel = page.locator("[data-world-office-task-panel]");
  await expect(panel).toBeVisible();
  const originalTask = panel
    .locator("[data-world-office-task-list] > li")
    .filter({ hasText: "Prepare launch digest" });
  await expect(originalTask).toContainText("@alice");
  await originalTask.getByRole("button", { name: "Start" }).click();
  await expect(originalTask.getByRole("button", { name: "Stop" })).toBeVisible();
  await expect
    .poll(async () =>
      originalTask.locator("[data-world-office-task-elapsed]").textContent(),
    )
    .not.toBe("0:00");

  await page.evaluate((id) => {
    document.querySelector("forkmesh-world").officeTasks.showCheckin(id);
  }, taskId);
  const checkin = page.locator("[data-world-office-task-checkin]");
  await expect(checkin).toBeVisible();
  await checkin.getByRole("button", { name: "Blocked" }).click();
  await expect(originalTask).toContainText("last check-in: Blocked");

  await page.evaluate(async () => {
    const tasks = document.querySelector("forkmesh-world").officeTasks;
    tasks.stopActiveForDeparture();
    await new Promise((resolve) => setTimeout(resolve, 50));
    await tasks.refresh({ quiet: true });
  });
  await expect(originalTask.getByRole("button", { name: "Start" })).toBeVisible();
  expect(
    officeTaskFixture.requests.some(
      (request) =>
        request.method === "POST" &&
        request.path.endsWith("/stop-active"),
    ),
  ).toBe(true);

  await panel.locator("[data-world-office-task-name]").fill(
    "Schedule community demo",
  );
  await panel.locator("[data-world-office-task-assignee]").selectOption("bob");
  await panel.getByRole("button", { name: "Add task" }).click();
  await expect(panel).toContainText("Schedule community demo");
  await expect(panel).toContainText("@bob");

  expect(
    officeTaskFixture.requests.some(
      (request) =>
        request.method === "POST" &&
        request.path.endsWith(`/${taskId}/checkin`) &&
        request.body.state === "blocked",
    ),
  ).toBe(true);
  expect(JSON.stringify(worldFrames)).not.toContain("Prepare launch digest");
  expect(JSON.stringify(worldFrames)).not.toContain("Schedule community demo");
});

test("ForkMesh Office defers the channel directory until fallback is opened", async ({
  page,
}) => {
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    () => {},
  );
  await prepareWorldPage(page, "office-admin", {
    session: {
      kind: "user",
      nodeName: "admin",
      email: "admin@example.test",
      sessionToken: "admin-token",
      isAdmin: true,
    },
    chatPassphrase: "playwright-office-admin-passphrase",
    chatChannels: [
      {
        id: "a".repeat(32),
        name: "announcements",
        visibility: "public",
        canManage: true,
        keyVersion: 1,
        updatedAt: FIXED_NOW,
      },
      {
        id: "b".repeat(32),
        name: "leadership",
        visibility: "private",
        canManage: true,
        keyVersion: 1,
        updatedAt: FIXED_NOW,
      },
    ],
  });
  await waitForWorld(page);
  await walkIntoOffice(page);
  const nativeRooms = page.locator("[data-world-office-room-board]");
  await expect(nativeRooms).toContainText("#general");
  await expect(nativeRooms).not.toContainText("#announcements");
  await expect(nativeRooms).not.toContainText("#leadership");
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.officeController.openFallback();
  });

  const office = page.frameLocator("[data-world-office-frame]");
  await expect(office.locator("#chat-rooms")).toContainText("#general");
  await expect(office.locator("#chat-rooms")).toContainText("#announcements");
  await expect(office.locator("#chat-rooms")).toContainText("#leadership");
  await expect(office.locator("#chat-office-manage")).toBeVisible();
  await expect(office.locator("#chat-channel-create")).toBeHidden();
  await expect(office.locator("#chat-channel-manage")).toBeHidden();
});

test("ForkMesh Office explains an expired authorized session", async ({
  page,
}) => {
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    () => {},
  );
  await prepareWorldPage(page, "office-expired", {
    session: {
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "expired-token",
    },
    chatPassphrase: "playwright-office-expired-passphrase",
    chatChannelStatus: 401,
  });
  await waitForWorld(page);
  await walkIntoOffice(page);
  await expect(page.locator("[data-world-office-room-board]")).toContainText(
    "#general",
  );
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.officeController.openFallback();
  });

  const office = page.frameLocator("[data-world-office-frame]");
  await expect(office.locator("#chat-office-alert")).toContainText(
    "Your session expired. Log in again to use authorized channels.",
  );
  await expect(office.locator("#chat-office-login")).toBeVisible();
  await expect(office.locator("#chat-rooms")).toContainText("#general");
});

test("reward-program links deep-link to the self-custodial fountain controls", async ({
  page,
}) => {
  await prepareWorldPage(page, "reward-program-deep-link");
  await waitForWorld(page, "/world/?landmark=fountain");

  const panel = page.locator("[data-world-detail]");
  await expect(panel).toBeVisible();
  await expect(
    panel.getByRole("heading", {
      name: "Join or fund the community reward program",
    }),
  ).toBeVisible();
  await expect(panel).toContainText(
    "does not purchase ownership, guaranteed rewards, investment returns, or governance control",
  );
  await expect(
    panel.getByRole("button", { name: "Prepare direct wallet transfer" }),
  ).toBeVisible();
});

test("detail panels overlay the desktop without dimming or reframing the World", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await prepareWorldPage(page, "non-modal-world-detail");
  await waitForWorld(page);
  await expect
    .poll(() => page.evaluate(() => window.innerWidth))
    .toBe(1280);
  await expect
    .poll(() =>
      page.evaluate(() => matchMedia("(max-width: 720px)").matches),
    )
    .toBe(false);
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setPaused(true);
    shell.detailTestFocusCalls = 0;
    const focusLandmark = shell.world.focusLandmark;
    shell.world.focusLandmark = (...args) => {
      shell.detailTestFocusCalls += 1;
      return focusLandmark(...args);
    };
  });

  const before = await page.locator("forkmesh-world").evaluate((shell) => {
    const canvas = shell.world.renderer.domElement;
    const rect = canvas.getBoundingClientRect();
    return {
      canvas: {
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height,
      },
      camera: shell.world.camera.position.toArray(),
    };
  });
  const trigger = page.getByRole("button", {
    name: /open World and privacy settings/i,
  });
  await trigger.focus();
  await trigger.evaluate((button) => {
    button.closest("forkmesh-world").openLandmark("fountain", {
      returnFocus: button,
    });
  });
  const detail = page.locator("[data-world-detail]");
  await expect(detail).toBeVisible();
  await expect(detail).toHaveAttribute("role", "dialog");
  await expect(detail).toHaveAttribute("aria-modal", "false");
  await expect(
    detail.getByRole("button", { name: "Close SOL reward fountain" }),
  ).toBeFocused();

  const openState = await page.locator("forkmesh-world").evaluate((shell) => {
    const canvas = shell.world.renderer.domElement;
    const rect = canvas.getBoundingClientRect();
    const backdrop = shell.querySelector("[data-world-detail-backdrop]");
    const backdropStyle = getComputedStyle(backdrop);
    return {
      focusCalls: shell.detailTestFocusCalls,
      canvas: {
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height,
      },
      camera: shell.world.camera.position.toArray(),
      backdrop: {
        opacity: backdropStyle.opacity,
        visibility: backdropStyle.visibility,
        pointerEvents: backdropStyle.pointerEvents,
      },
    };
  });
  expect(openState.focusCalls).toBe(0);
  expect(openState.canvas).toEqual(before.canvas);
  expect(openState.camera).toEqual(before.camera);
  expect(openState.backdrop).toEqual({
    opacity: "0",
    visibility: "hidden",
    pointerEvents: "none",
  });
  expect(await page.locator("forkmesh-world").evaluate((shell) =>
    shell.detailReturnFocus === shell.querySelector("[data-world-settings-open]")
  )).toBe(true);

  await page.locator("[data-world-detail-close]").click();
  await expect(trigger).toBeFocused();

  await page.setViewportSize({ width: 600, height: 800 });
  await page.locator("forkmesh-world").evaluate((shell) =>
    shell.openLandmark("fountain"),
  );
  await expect(detail).toBeVisible();
  const mobileBackdrop = page.locator("[data-world-detail-backdrop]");
  await expect(mobileBackdrop).toHaveCSS("visibility", "visible");
  await expect(mobileBackdrop).toHaveCSS("pointer-events", "auto");
  await mobileBackdrop.click({ position: { x: 8, y: 8 } });
  await expect(detail).toHaveAttribute("data-open", "false");
});

async function freezeWorld(page) {
  await page.locator("forkmesh-world").evaluate((shell) => {
    // The default theme is fixed full daylight. Wall-clock time never
    // participates in scene lighting.
    shell.world.setTheme("world");
    shell.world.setLightLevel(100);
    // Frame the Office for the baseline so the meeting-room work is visible in
    // every viewport's shot (adhoc: PR #47). Retakes world-*.png baselines.
    shell.world.focusLandmark("office", { move: false });
  });
  // The camera intentionally eases from its spawn position. Let that bounded
  // interpolation converge before pausing so the WebGL baseline does not
  // depend on how many startup frames a busy CI host happened to paint.
  await page.waitForTimeout(1600);
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.closeLandmark();
    shell.world.setPaused(true);
    shell.dismissHudNotice?.();
  });
}

async function openOfficeForVisual(page) {
  await walkIntoOffice(page);
  await expect(page.locator("[data-world-office-lobby]")).toBeHidden();
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setPaused(true);
  });
}

async function renderedWorldState(page) {
  return page.locator("forkmesh-world").evaluate((shell) => {
    const { camera, renderer, scene } = shell.world;
    const canvas = renderer.domElement;
    const gl = renderer.getContext();
    renderer.render(scene, camera);
    const pixel = new Uint8Array(4);
    gl.readPixels(
      Math.floor(gl.drawingBufferWidth / 2),
      Math.floor(gl.drawingBufferHeight / 2),
      1,
      1,
      gl.RGBA,
      gl.UNSIGNED_BYTE,
      pixel,
    );
    const rect = canvas.getBoundingClientRect();
    return {
      cssWidth: Math.round(rect.width),
      cssHeight: Math.round(rect.height),
      bufferWidth: gl.drawingBufferWidth,
      bufferHeight: gl.drawingBufferHeight,
      contextLost: gl.isContextLost(),
      frame: renderer.info.render.frame,
      pixel: [...pixel],
      cameraFinite: camera.position.toArray().every(Number.isFinite),
      space: shell.world.getPosition().space,
    };
  });
}

async function dragThumbstick(
  page,
  locator,
  { x = 0, y = -1, durationMs = 300 } = {},
) {
  const box = await locator.boundingBox();
  expect(box).not.toBeNull();
  const client = await page.context().newCDPSession(page);
  const start = {
    x: Math.round(box.x + box.width / 2),
    y: Math.round(box.y + box.height / 2),
    id: 1,
  };
  const travel = Math.max(1, Math.min(box.width, box.height) * 0.3);
  const point = {
    x: Math.round(start.x + x * travel),
    y: Math.round(start.y + y * travel),
    id: 1,
  };
  await client.send("Input.dispatchTouchEvent", {
    type: "touchStart",
    touchPoints: [start],
  });
  await client.send("Input.dispatchTouchEvent", {
    type: "touchMove",
    touchPoints: [point],
  });
  await page.waitForTimeout(durationMs);
  await client.send("Input.dispatchTouchEvent", {
    type: "touchEnd",
    touchPoints: [],
  });
  await client.detach();
}


test("@critical enhanced Town Square starts in WebGL and keeps keyboard navigation", async ({
  page,
}) => {
  await prepareWorldPage(page, "desktop-a");
  await waitForWorld(page);

  await expect(page.locator("[data-world-root]")).toBeVisible();
  await expect(page.locator("[data-world-canvas-wrap] canvas")).toHaveCount(1);
  await expect(page.locator(".world-webgl-fallback")).toHaveCount(0);
  await expect(page.locator("#world-information")).toHaveCount(1);
  await expect(page.locator("[data-world-loading]")).toHaveCount(0);
  await expect(page.locator(".world-arrival-card")).toHaveCount(0);

  await page.locator(".world-skip-link").focus();
  await page.keyboard.press("Enter");
  await expect(page).toHaveURL(/#world-information$/);
  await expect(page.locator("#world-information")).toBeFocused();

  await page.keyboard.press("Escape");
  const before = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getPosition(),
  );
  await page.keyboard.down("w");
  await page.waitForTimeout(220);
  await page.keyboard.up("w");
  const after = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getPosition(),
  );
  expect(Math.hypot(after.x - before.x, after.z - before.z)).toBeGreaterThan(
    0.05,
  );
});

test("the side portal isolates the beach scene and provides a return portal", async ({
  page,
}) => {
  await prepareWorldPage(page, "beach-scene-portal");
  await waitForWorld(page);

  const state = await page.locator("forkmesh-world").evaluate((shell) => {
    const scene = shell.world.scene;
    const beach = scene.getObjectByName("forkmesh-beach-scene");
    const entry = scene.getObjectByName("forkmesh-beach-entry-portal");
    const exit = scene.getObjectByName("forkmesh-beach-return-portal");
    const worldRoot = scene.children.find((child) =>
      Object.hasOwn(child.userData || {}, "activeEnclosureScene")
    );
    const initial = {
      beachVisible: beach?.visible,
      entryVisible: entry?.visible,
      exitVisible: exit?.visible,
      active: worldRoot?.userData?.activeEnclosureScene,
    };
    const entered = shell.world.enterBeachScene();
    const atBeach = {
      entered,
      isBeach: shell.world.isBeachScene(),
      beachVisible: beach?.visible,
      entryVisible: entry?.visible,
      exitVisible: exit?.visible,
      active: worldRoot?.userData?.activeEnclosureScene,
      position: shell.world.getPosition(),
    };
    const left = shell.world.leaveBeachScene();
    return {
      initial,
      atBeach,
      returned: {
        left,
        isBeach: shell.world.isBeachScene(),
        beachVisible: beach?.visible,
        entryVisible: entry?.visible,
        active: worldRoot?.userData?.activeEnclosureScene,
        position: shell.world.getPosition(),
      },
    };
  });

  expect(state.initial).toEqual({
    beachVisible: false,
    entryVisible: true,
    exitVisible: true,
    active: "",
  });
  expect(state.atBeach.entered).toBe(true);
  expect(state.atBeach.isBeach).toBe(true);
  expect(state.atBeach.beachVisible).toBe(true);
  expect(state.atBeach.entryVisible).toBe(false);
  expect(state.atBeach.exitVisible).toBe(true);
  expect(state.atBeach.active).toBe("beach");
  expect(state.atBeach.position.space).toBe("beach");
  expect(state.returned.left).toBe(true);
  expect(state.returned.isBeach).toBe(false);
  expect(state.returned.beachVisible).toBe(false);
  expect(state.returned.entryVisible).toBe(true);
  expect(state.returned.active).toBe("");
  expect(state.returned.position.space).toBe("town-square");
});

test("landmark tree clusters stay local while arrival faces inward", async ({
  page,
}) => {
  await prepareWorldPage(page, "world-tree-field");
  await waitForWorld(page);

  const state = await page.locator("forkmesh-world").evaluate((shell) => {
    const scene = shell.world.scene;
    const treeField = scene.getObjectByName("forkmesh-town-mixed-landscape");
    const trees = (treeField?.userData?.treeLayout || [])
      .map((tree) => ({
        x: tree.x,
        z: tree.z,
        radius: Math.hypot(tree.x, tree.z),
        clusterRadius: Math.min(
          Math.hypot(tree.x, tree.z),
          Math.hypot(tree.x - 130, tree.z),
        ),
      }));
    const treeBatches = [
      scene.getObjectByName("forkmesh-town-tree-trunks"),
      scene.getObjectByName("forkmesh-town-tree-crowns"),
    ];
    let palePlaza = false;
    let fountainPool = false;
    scene.traverse((object) => {
      const geometry = object.geometry;
      if (geometry?.type !== "CylinderGeometry") return;
      const { radiusTop, radiusBottom, height } = geometry.parameters || {};
      if (
        Math.abs(Number(radiusTop) - 16.5) < 0.001 &&
        Math.abs(Number(radiusBottom) - 17.4) < 0.001
      ) {
        palePlaza = true;
      }
      if (
        Math.abs(Number(radiusTop) - 4.4) < 0.001 &&
        Math.abs(Number(radiusBottom) - 4.4) < 0.001 &&
        Math.abs(Number(height) - 0.16) < 0.001
      ) {
        fountainPool = true;
      }
    });

    shell.world.setSpawn({
      x: 0,
      y: 0.38,
      z: 30,
      heading: 0,
      space: "town-square",
    });
    shell.world.setRemotePlayers([
      {
        id: "arrival-facing-peer",
        name: "Arrival peer",
        x: 2,
        y: 0.38,
        z: 30,
        heading: 0,
        accountStatus: "Guest",
      },
    ]);
    const remote = scene.getObjectByName("avatar:arrival-facing-peer");
    return {
      palePlaza,
      fountainPool,
      signHeading: scene.getObjectByName("world-arrival-plaque")?.rotation.y,
      playerHeading: shell.world.player.rotation.y,
      remoteHeading: remote?.userData?.targetHeading,
      treeCount: trees.length,
      treeBatchCount: treeBatches.filter((batch) => batch?.isInstancedMesh)
        .length,
      treeInstances: treeBatches.map((batch) => batch?.count || 0),
      radii: trees.map((tree) => tree.radius),
      clusterRadii: trees.map((tree) => tree.clusterRadius),
      quadrants: [0, 1, 2, 3].map(
        (quadrant) =>
          trees.filter((tree) => {
            const actual =
              tree.x >= 0
                ? tree.z >= 0 ? 0 : 3
                : tree.z >= 0 ? 1 : 2;
            return actual === quadrant;
          }).length,
      ),
      arrivalTrees: trees.filter(
        (tree) =>
          tree.x >= -10.8 &&
          tree.x <= 10.8 &&
          tree.z >= 14 &&
          tree.z <= 32.4,
      ).length,
      cabinetTrees: trees.filter(
        (tree) =>
          tree.x >= 24 &&
          tree.x <= 52 &&
          tree.z >= -14 &&
          tree.z <= 14,
      ).length,
    };
  });

  expect(state.palePlaza).toBe(false);
  expect(state.fountainPool).toBe(true);
  expect(state.signHeading).toBeCloseTo(Math.PI, 5);
  expect(state.playerHeading).toBeCloseTo(Math.PI, 5);
  expect(state.remoteHeading).toBeCloseTo(Math.PI, 5);
  expect(state.treeCount).toBeGreaterThan(0);
  expect(state.treeCount).toBeLessThanOrEqual(33);
  expect(state.treeBatchCount).toBe(2);
  expect(state.treeInstances).toEqual([state.treeCount, state.treeCount]);
  expect(Math.min(...state.clusterRadii)).toBeGreaterThanOrEqual(6.4);
  expect(Math.max(...state.clusterRadii)).toBeLessThanOrEqual(8.5);
  expect(state.arrivalTrees).toBe(0);
  expect(state.cabinetTrees).toBe(0);
});

test("an expired persisted session stays guest-only without private request fanout or Three.js material warnings", async ({
  page,
}) => {
  const privateRequests = [];
  const materialWarnings = [];
  page.on("request", (request) => {
    const path = new URL(request.url()).pathname;
    if (
      path === "/api/world/media/spaces" ||
      path === "/api/rewards/pending" ||
      path === "/api/notifications" ||
      path.startsWith("/api/orgs/")
    ) {
      privateRequests.push(path);
    }
  });
  page.on("console", (message) => {
    if (
      message.type() === "warning" &&
      message.text().includes("THREE.Material")
    ) {
      materialWarnings.push(message.text());
    }
  });
  await prepareWorldPage(page, "expired-session", {
    session: {
      nodeName: "expired-user",
      sessionToken: "expired-session-token",
    },
    ticketAuthenticated: false,
  });
  await waitForWorld(page);

  await expect(page.locator("[data-world-canvas-wrap] canvas")).toHaveCount(1);
  await expect(page.locator(".world-webgl-fallback")).toHaveCount(0);
  expect(privateRequests).toEqual([]);
  expect(materialWarnings).toEqual([]);
});

test("double-clicking the ground dashes the avatar to that spot", async ({
  page,
}) => {
  await prepareWorldPage(page, "desktop-dblclick-dash");
  await waitForWorld(page);

  const canvas = page.locator("[data-world-canvas-wrap] canvas");
  const box = await canvas.boundingBox();
  expect(box).not.toBeNull();

  const before = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getPosition(),
  );
  // Well away from the avatar, still on the plaza floor.
  await page.mouse.dblclick(
    Math.round(box.x + box.width * 0.3),
    Math.round(box.y + box.height * 0.4),
  );
  await page.waitForTimeout(400);
  const after = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getPosition(),
  );
  // A dash covers far more ground in 400ms than the walking cap would.
  expect(Math.hypot(after.x - before.x, after.z - before.z)).toBeGreaterThan(3);
});

test("desktop camera uses visible-cursor drag look, capped movement acceleration, and wheel zoom", async ({
  page,
}) => {
  test.setTimeout(60_000);
  await prepareWorldPage(page, "desktop-drag-controls");
  await waitForWorld(page);

  const canvas = page.locator("[data-world-canvas-wrap] canvas");
  const box = await canvas.boundingBox();
  expect(box).not.toBeNull();
  const centre = {
    x: Math.round(box.x + box.width / 2),
    y: Math.round(box.y + box.height / 2),
  };

  const beforeGroundClick = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getPosition(),
  );
  await page.mouse.click(centre.x, centre.y);
  await page.waitForTimeout(160);
  const afterGroundClick = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getPosition(),
  );
  expect(
    Math.hypot(
      afterGroundClick.x - beforeGroundClick.x,
      afterGroundClick.z - beforeGroundClick.z,
    ),
  ).toBeLessThan(0.01);
  await expect(canvas).toHaveCSS("cursor", "grab");
  expect(await page.evaluate(() => document.pointerLockElement)).toBeNull();

  const beforeLook = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getCameraState(),
  );
  await page.mouse.move(centre.x, centre.y);
  await page.mouse.down({ button: "left" });
  await expect(canvas).toHaveCSS("cursor", "grabbing");
  await page.mouse.move(centre.x + 120, centre.y + 35, { steps: 4 });
  await page.mouse.up({ button: "left" });
  const afterLook = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getCameraState(),
  );
  expect(afterLook.yaw).not.toBeCloseTo(beforeLook.yaw, 4);
  expect(afterLook.pitch).not.toBeCloseTo(beforeLook.pitch, 4);
  expect(afterLook.yaw).toBeLessThan(beforeLook.yaw);
  expect(afterLook.pitch).toBeGreaterThan(beforeLook.pitch);
  expect(afterLook.dragging).toBe(false);
  expect(afterLook.pointerLocked).toBe(false);
  expect(await page.evaluate(() => document.pointerLockElement)).toBeNull();
  await expect(canvas).toHaveCSS("cursor", "grab");

  const beforeMove = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getPosition(),
  );
  await page.keyboard.down("ArrowUp");
  await page.waitForTimeout(120);
  const earlyMovement = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getMovementState(),
  );
  await page.waitForTimeout(760);
  const acceleratedMovement = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getMovementState(),
  );
  await page.keyboard.up("ArrowUp");
  const afterMove = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getPosition(),
  );
  expect(acceleratedMovement.speed).toBeGreaterThan(earlyMovement.speed);
  expect(acceleratedMovement.speed).toBeLessThanOrEqual(
    acceleratedMovement.maxSpeed,
  );
  const releasedMovement = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getMovementState(),
  );
  expect(releasedMovement.speed).toBe(releasedMovement.baseSpeed);
  expect(releasedMovement.keyboardActive).toBe(false);
  const displacement = {
    x: afterMove.x - beforeMove.x,
    z: afterMove.z - beforeMove.z,
  };
  const cameraForward = {
    x: -Math.sin(afterLook.yaw),
    z: -Math.cos(afterLook.yaw),
  };
  expect(
    displacement.x * cameraForward.x + displacement.z * cameraForward.z,
  ).toBeGreaterThan(0.05);

  await page.keyboard.down("w");
  await page.waitForTimeout(180);
  await page.evaluate(() => window.dispatchEvent(new Event("blur")));
  const blurredMovement = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getMovementState(),
  );
  expect(blurredMovement.speed).toBe(blurredMovement.baseSpeed);
  expect(blurredMovement.keyboardActive).toBe(false);
  await page.keyboard.up("w");

  await page.mouse.move(centre.x, centre.y);
  const initialZoom = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getCameraState().zoom,
  );
  await page.mouse.wheel(0, 480);
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.getCameraState().zoom,
    ),
  ).toBeGreaterThan(initialZoom);
  const zoomedOut = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getCameraState().zoom,
  );
  await page.mouse.wheel(0, -960);
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.getCameraState().zoom,
    ),
  ).toBeLessThan(zoomedOut);
});

test("refresh restores one bounded identity-local position without private history", async ({
  page,
}) => {
  const serverArrival = {
    id: "position-restore",
    name: "visitor",
    status: "exploring",
    x: -8.1,
    y: 0.38,
    z: 30,
    yaw: 0,
    space: "town-square",
  };
  await prepareWorldPage(page, "position-restore", {
    worldSocketHandler(socket, socketId) {
      socket.send(JSON.stringify({
        type: "welcome",
        id: socketId,
        self: serverArrival,
        peers: [],
      }));
    },
  });
  await waitForWorld(page);

  await page.locator("forkmesh-world").evaluate((shell) => {
    // A normal Town Square position survives while private activity copy does
    // not become part of the local position record.
    const position = {
      x: 16.3,
      y: 0.38,
      z: 26.5,
      heading: 1.2,
      space: "town-square",
      moving: false,
      activity: "private/repository?token=must-not-persist",
    };
    shell.currentSpace = position.space;
    shell.world.setSpawn(position);
    shell.handleMovement(position);
  });
  const storedBeforeRefresh = await page.evaluate(() => {
    const keys = Object.keys(localStorage).filter((key) =>
      key.startsWith("forkmesh.world.position.v1."),
    );
    return {
      keys,
      record: JSON.parse(localStorage.getItem(keys[0]) || "{}"),
    };
  });
  expect(storedBeforeRefresh.keys).toHaveLength(1);
  expect(Object.keys(storedBeforeRefresh.record).sort()).toEqual(
    ["heading", "space", "updatedAt", "x", "y", "z"].sort(),
  );
  expect(storedBeforeRefresh.record.space).toBe("town-square");
  const positionBeforeRefresh = await page
    .locator("forkmesh-world")
    .evaluate((shell) => shell.world.getPosition());
  expect(positionBeforeRefresh.space).toBe("town-square");
  expect(JSON.stringify(storedBeforeRefresh.record)).not.toContain("private");
  expect(JSON.stringify(storedBeforeRefresh.record)).not.toContain("token");

  await page.reload();
  await waitForWorldReady(page);
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) => {
      const position = shell.world.getPosition();
      return (
        shell.spawnSelected === true &&
        position.space === "town-square" &&
        Math.abs(position.x - 16.3) < 0.001 &&
        Math.abs(position.z - 26.5) < 0.001
      );
    }),
  ).toBe(true);
  const restored = await page.locator("forkmesh-world").evaluate((shell) => ({
    position: shell.world.getPosition(),
    currentSpace: shell.currentSpace,
    restoredPosition: shell.restoredPosition,
    storedRecord: JSON.parse(
      localStorage.getItem(
        Object.keys(localStorage).find((key) =>
          key.startsWith("forkmesh.world.position.v1."),
        ) || "",
      ) || "null",
    ),
    storedRecords: Object.keys(localStorage).filter((key) =>
      key.startsWith("forkmesh.world.position.v1."),
    ).length,
  }));
  expect(restored).toMatchObject({
    currentSpace: "town-square",
    position: { space: "town-square" },
    restoredPosition: { space: "town-square" },
    storedRecord: { space: "town-square" },
  });
  expect(restored.position.x).toBeCloseTo(16.3, 3);
  expect(restored.position.y).toBeCloseTo(0.38, 3);
  expect(restored.position.z).toBeCloseTo(26.5, 3);
  expect(restored.position.heading).toBeCloseTo(1.2, 3);
  expect(restored.storedRecords).toBe(1);
});

test("refresh from the Office restores the last Town Square location", async ({
  page,
}) => {
  await prepareWorldPage(page, "office-refresh-position");
  await waitForWorld(page);

  await page.locator("forkmesh-world").evaluate((shell) => {
    const position = {
      x: -18.2,
      y: 0.38,
      z: 14.6,
      heading: -0.7,
      space: "town-square",
      moving: false,
      activity: "exploring the Town Square",
    };
    shell.currentSpace = position.space;
    shell.world.setSpawn(position);
    shell.handleMovement(position);
    shell.world.enterOfficeLobby();
  });
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.getPosition().space,
    ),
  ).toBe("office-lobby");

  await page.reload();
  await waitForWorldReady(page);
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) => {
      const position = shell.world.getPosition();
      return (
        shell.spawnSelected === true &&
        position.space === "town-square" &&
        Math.abs(position.x + 18.2) < 0.001 &&
        Math.abs(position.z - 14.6) < 0.001
      );
    }),
  ).toBe(true);
  await expect(page.locator("[data-world-office-lobby]")).toBeHidden();
});

test("mobile refresh keeps a live low-memory renderer and recovers cached pages and WebGL", async ({
  browser,
}) => {
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
    deviceScaleFactor: 3,
  });
  const page = await context.newPage();
  const pageErrors = [];
  page.on("pageerror", (error) => pageErrors.push(error.message));
  await prepareWorldPage(page, "mobile-refresh");
  await waitForWorld(page);

  const readRenderer = () =>
    page.locator("forkmesh-world").evaluate((shell) => {
      const { renderer, scene, camera } = shell.world;
      const canvas = renderer.domElement;
      const gl = renderer.getContext();
      renderer.render(scene, camera);
      const pixel = new Uint8Array(4);
      gl.readPixels(
        Math.max(0, Math.floor(gl.drawingBufferWidth / 2)),
        Math.max(0, Math.floor(gl.drawingBufferHeight / 2)),
        1,
        1,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        pixel,
      );
      const rect = canvas.getBoundingClientRect();
      const diagnostics = shell.world.getDiagnostics();
      return {
        canvases: shell.querySelectorAll("[data-world-canvas-wrap] canvas")
          .length,
        cssWidth: Math.round(rect.width),
        cssHeight: Math.round(rect.height),
        bufferWidth: gl.drawingBufferWidth,
        bufferHeight: gl.drawingBufferHeight,
        pixelRatio: renderer.getPixelRatio(),
        antialias: gl.getContextAttributes()?.antialias,
        stencil: gl.getContextAttributes()?.stencil,
        shadows: renderer.shadowMap.enabled,
        contextLost: gl.isContextLost(),
        frame: renderer.info.render.frame,
        pixel: [...pixel],
        coarse: matchMedia("(pointer: coarse)").matches,
        residency: diagnostics.residency,
        debugHidden: shell.querySelector("[data-world-diagnostics]")?.hidden === true,
      };
    });

  for (let cycle = 0; cycle < 3; cycle += 1) {
    if (cycle) {
      await page.reload({ waitUntil: "domcontentloaded" });
      await waitForWorldReady(page);
    }
    const renderer = await readRenderer();
    expect(renderer).toMatchObject({
      canvases: 1,
      cssWidth: 390,
      cssHeight: 844,
      bufferWidth: 390,
      bufferHeight: 844,
      pixelRatio: 1,
      antialias: false,
      stencil: false,
      shadows: false,
      contextLost: false,
      coarse: true,
      residency: {
        enabled: true,
        resident: 1,
        total: 5,
        active: ["members"],
      },
      debugHidden: true,
    });
    expect(renderer.frame).toBeGreaterThan(0);
    expect(renderer.pixel[0] + renderer.pixel[1] + renderer.pixel[2])
      .toBeGreaterThan(0);
  }

  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setSpawn({ x: 130, y: 0.38, z: 0, heading: 0 });
  });
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.getDiagnostics().residency.active,
    ),
  ).toContain("repositories");
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setSpawn({ x: 0, y: 0.38, z: 0, heading: 0 });
  });
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) => {
      const residency = shell.world.getDiagnostics().residency;
      return {
        active: residency.active,
        evictions: residency.evictions,
      };
    }),
  ).toEqual({
    active: [],
    evictions: 2,
  });
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) => {
      const residency = shell.world.getDiagnostics().residency;
      return residency.releasedGeometries + residency.releasedTextures;
    }),
  ).toBeGreaterThan(0);

  const frameBeforeCache = await page.locator("forkmesh-world").evaluate(
    (shell) => {
      window.__forkmeshMobileRenderer = shell.world.renderer;
      const frame = shell.world.renderer.info.render.frame;
      window.dispatchEvent(
        new PageTransitionEvent("pagehide", { persisted: true }),
      );
      return frame;
    },
  );
  const cachedState = await page.locator("forkmesh-world").evaluate(
    (shell) => ({
      destroyed: shell.destroyed,
      sameRenderer: shell.world?.renderer === window.__forkmeshMobileRenderer,
      canvases: shell.querySelectorAll("[data-world-canvas-wrap] canvas")
        .length,
    }),
  );
  expect(cachedState).toEqual({
    destroyed: false,
    sameRenderer: true,
    canvases: 1,
  });
  await page.evaluate(() => {
    window.dispatchEvent(
      new PageTransitionEvent("pageshow", { persisted: true }),
    );
  });
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.renderer.info.render.frame,
    ),
  ).toBeGreaterThan(frameBeforeCache);

  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.renderer.forceContextLoss();
  });
  await expect(page.locator("[data-world-renderer-recovery]")).toBeVisible();
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.renderer.getContext().isContextLost(),
    ),
  ).toBe(true);
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.renderer.forceContextRestore();
  });
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.renderer.getContext().isContextLost(),
    ),
  ).toBe(false);
  await expect(page.locator("[data-world-renderer-recovery]")).toBeHidden();
  expect(pageErrors).toEqual([]);
  await context.close();
});

test("a phone never draws a frame with a paged-out district on screen", async ({
  browser,
}) => {
  test.setTimeout(120_000);
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
    deviceScaleFactor: 3,
  });
  const page = await context.newPage();
  const pageErrors = [];
  page.on("pageerror", (error) => pageErrors.push(error.message));
  await prepareWorldPage(page, "phone-district-pop-in");
  await waitForWorld(page);

  const sweep = (view) =>
    page.locator("forkmesh-world").evaluate(
      (shell, { spot, camera: cameraView }) =>
        new Promise((resolve) => {
          const { scene, camera } = shell.world;
          shell.world.setSpawn({ ...spot, space: "town-square" });
          shell.world.setCameraView(cameraView);
          const point = camera.position.clone();
          const offenders = new Set();
          let frames = 0;
          const onScreen = (root) => {
            let seen = false;
            root.traverse((child) => {
              if (seen || !child.isMesh || !child.geometry) return;
              let box = child.geometry.boundingBox;
              if (child.isInstancedMesh) {
                if (!child.boundingBox) child.computeBoundingBox();
                box = child.boundingBox;
              } else if (!box) {
                child.geometry.computeBoundingBox();
                box = child.geometry.boundingBox;
              }
              if (!box || box.isEmpty()) return;
              child.updateWorldMatrix(true, false);
              for (let i = 0; i <= 2 && !seen; i += 1) {
                for (let j = 0; j <= 2 && !seen; j += 1) {
                  for (let k = 0; k <= 2 && !seen; k += 1) {
                    point.set(
                      box.min.x + ((box.max.x - box.min.x) * i) / 2,
                      box.min.y + ((box.max.y - box.min.y) * j) / 2,
                      box.min.z + ((box.max.z - box.min.z) * k) / 2,
                    );
                    point.applyMatrix4(child.matrixWorld).project(camera);
                    seen =
                      Math.abs(point.x) <= 1 &&
                      Math.abs(point.y) <= 1 &&
                      point.z >= -1 &&
                      point.z <= 1;
                  }
                }
              }
            });
            return seen;
          };
          const sample = () => {
            scene.traverse((root) => {
              if (root.userData?.compactDistrictResident !== false) return;
              if (
                root.userData.compactDistantProxy === true &&
                root.parent?.visible === false
              ) {
                return;
              }
              if (onScreen(root)) offenders.add(root.name || root.type);
            });
            frames += 1;
            if (frames < 30) requestAnimationFrame(sample);
            else {
              resolve({
                offenders: [...offenders],
                inView: shell.world.getDiagnostics().residency.inView,
              });
            }
          };
          requestAnimationFrame(sample);
        }),
      view,
    );

  const spots = [
    { x: 0, y: 0.38, z: 0, heading: 0 },
    { x: 0, y: 0.38, z: -60, heading: 0 },
  ];
  const cameras = [
    { yaw: 0, zoom: 1 },
    { yaw: Math.PI / 2, zoom: 1 },
    { yaw: Math.PI, zoom: 1 },
    { yaw: -Math.PI / 2, zoom: 1 },
    { yaw: Math.PI / 4, zoom: 1.4 },
    { mode: "first-person", yaw: Math.PI / 2, pitch: 0 },
    { mode: "first-person", yaw: -Math.PI / 2, pitch: 0 },
  ];
  const seenInView = new Set();
  for (const spot of spots) {
    for (const camera of cameras) {
      const result = await sweep({ spot, camera });
      expect(result.offenders, JSON.stringify({ spot, camera })).toEqual([]);
      result.inView.forEach((id) => seenInView.add(id));
    }
  }
  expect(seenInView.size).toBeGreaterThan(0);
  const residency = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getDiagnostics().residency,
  );
  expect(residency.enabled).toBe(true);
  expect(pageErrors).toEqual([]);
  await context.close();
});

test("a 4K display keeps the full World inside a 1080p raster budget", async ({
  browser,
}) => {
  const context = await browser.newContext({
    viewport: { width: 3840, height: 2160 },
    screen: { width: 3840, height: 2160 },
    deviceScaleFactor: 1,
  });
  const page = await context.newPage();
  await prepareWorldPage(page, "desktop-4k-memory");
  await waitForWorld(page);

  const state = await page.locator("forkmesh-world").evaluate((shell) => {
    const diagnostics = shell.world.getDiagnostics();
    let scaledCanvasTextures = 0;
    shell.world.scene.traverse((object) => {
      const materials = Array.isArray(object.material)
        ? object.material
        : [object.material];
      materials.filter(Boolean).forEach((material) => {
        if (material.map?.image?.dataset?.textureScale === "0.5") {
          scaledCanvasTextures += 1;
        }
      });
    });
    return {
      compact: diagnostics.output.compactRenderer,
      lowMemory: diagnostics.output.memoryConstrainedRenderer,
      antialias: diagnostics.output.antialias,
      shadows: diagnostics.shadowsEnabled,
      drawingPixels:
        diagnostics.output.drawingBufferWidth *
        diagnostics.output.drawingBufferHeight,
      pixelBudget: diagnostics.output.pixelBudget,
      pixelRatio: diagnostics.pixelRatio,
      triangles: diagnostics.rendererTriangles,
      stars: diagnostics.sky.stars,
      satelliteLimit: diagnostics.sky.satelliteLimit,
      scaledCanvasTextures,
    };
  });

  expect(state).toMatchObject({
    compact: false,
    lowMemory: true,
    antialias: false,
    shadows: true,
    pixelBudget: 1920 * 1080,
    stars: 1100,
    satelliteLimit: 160,
  });
  expect(state.drawingPixels).toBeLessThanOrEqual(state.pixelBudget);
  expect(state.pixelRatio).toBeLessThan(1);
  expect(state.triangles).toBeGreaterThan(0);
  expect(state.scaledCanvasTextures).toBeGreaterThan(0);
  await context.close();
});

test("stars, planets, and satellites are drawn only while looking up at night", async ({
  page,
}) => {
  await prepareWorldPage(page, "world-deep-sky-gate");
  await waitForWorld(page);

  const shell = page.locator("forkmesh-world");
  const sample = async (mode, pitch) => {
    const startedAtFrame = await shell.evaluate(
      (element, view) => {
        element.world.setDaylightMode(view.mode);
        // A negative orbit pitch drops the eye below its target, which is how
        // this camera cranes upward.
        element.world.setCameraView({ pitch: view.pitch });
        return element.world.renderer.info.render.frame;
      },
      { mode, pitch },
    );
    // The gate is sampled in the animation loop, not by the setters.
    await page.waitForFunction(
      (frame) =>
        document.querySelector("forkmesh-world").world.renderer.info.render
          .frame > frame + 2,
      startedAtFrame,
    );
    return shell.evaluate((element) => {
      const sky = element.world.getDiagnostics().sky;
      const group = element.world.scene.getObjectByName("forkmesh-world-sky");
      return {
        lookingUp: sky.lookingUp,
        night: sky.night,
        deepSkyVisible: sky.deepSkyVisible,
        drawCalls: sky.drawCalls,
        starsVisible: Boolean(
          group.getObjectByName("forkmesh-world-stars")?.visible,
        ),
        satellitesVisible: Boolean(
          group.getObjectByName("forkmesh-world-satellites")?.visible,
        ),
        planetInstances: group.getObjectByName("forkmesh-world-planets").count,
      };
    });
  };

  // Daylight keeps the sun and moon instances and nothing else, even with the
  // camera craned all the way back.
  expect(await sample("day", -1.2)).toMatchObject({
    night: false,
    lookingUp: true,
    deepSkyVisible: false,
    drawCalls: 2,
    starsVisible: false,
    satellitesVisible: false,
    planetInstances: 2,
  });
  // Night at eye level is just as cheap.
  expect(await sample("night", 0.6)).toMatchObject({
    night: true,
    lookingUp: false,
    deepSkyVisible: false,
    drawCalls: 2,
    starsVisible: false,
    satellitesVisible: false,
    planetInstances: 2,
  });
  // Only looking up at a night sky pays for the six planets and the stars.
  const nightLookingUp = await sample("night", -1.2);
  expect(nightLookingUp).toMatchObject({
    night: true,
    lookingUp: true,
    deepSkyVisible: true,
    starsVisible: true,
    satellitesVisible: true,
    planetInstances: 8,
  });
  expect(nightLookingUp.drawCalls).toBeGreaterThan(2);
});

test("@critical default town stays inside the renderer workload budget", async ({
  page,
}, testInfo) => {
  await prepareWorldPage(page, "world-render-workload");
  await waitForWorld(page);

  // Exercise the normal chase-camera path before sampling. The gate is based
  // on renderer.info, so it measures submitted work consistently on hardware
  // WebGL and CI's software renderer without pretending they share an FPS.
  await page.keyboard.down("KeyW");
  await page.waitForTimeout(900);
  await page.keyboard.up("KeyW");
  await page.waitForTimeout(100);
  const measured = await page.locator("forkmesh-world").evaluate((shell) => {
    const diagnostics = shell.world.getDiagnostics();
    const trunks = shell.world.scene.getObjectByName(
      "forkmesh-town-tree-trunks",
    );
    const crowns = shell.world.scene.getObjectByName(
      "forkmesh-town-tree-crowns",
    );
    return {
      ...diagnostics.renderWorkload,
      treeBatches: [trunks, crowns].filter((mesh) => mesh?.isInstancedMesh)
        .length,
      treeInstances: [trunks?.count || 0, crowns?.count || 0],
    };
  });
  await testInfo.attach("world-render-workload.json", {
    body: JSON.stringify(measured, null, 2),
    contentType: "application/json",
  });

  expect(measured.drawCallBudget).toBe(WORLD_RENDER_DRAW_CALL_BUDGET);
  expect(measured.triangleBudget).toBe(WORLD_RENDER_TRIANGLE_BUDGET);
  expect(measured.drawCalls).toBeGreaterThan(20);
  expect(measured.triangles).toBeGreaterThan(5_000);
  expect(measured.drawCalls).toBeLessThanOrEqual(
    WORLD_RENDER_DRAW_CALL_BUDGET,
  );
  expect(measured.triangles).toBeLessThanOrEqual(
    WORLD_RENDER_TRIANGLE_BUDGET,
  );
  expect(measured.withinBudget).toBe(true);
  expect(measured.treeBatches).toBe(2);
  expect(measured.treeInstances[0]).toBeGreaterThan(0);
  expect(measured.treeInstances[1]).toBe(measured.treeInstances[0]);
});

test("@critical camera, avatar, and chat present within the 100ms input budget", async ({
  browser,
}) => {
  test.slow();
  const devices = [
    {
      name: "desktop",
      options: { viewport: { width: 1280, height: 720 } },
    },
    {
      name: "mobile",
      options: {
        viewport: { width: 390, height: 844 },
        hasTouch: true,
        isMobile: true,
        deviceScaleFactor: 3,
      },
    },
  ];
  const measurements = {};

  for (const device of devices) {
    const context = await browser.newContext(device.options);
    const devicePage = await context.newPage();
    await prepareWorldPage(devicePage, `input-latency-${device.name}`, {
      chatPassphrase: "playwright-public-world-general-passphrase",
    });
    await devicePage.emulateMedia({ reducedMotion: "no-preference" });
    await waitForWorld(devicePage);
    measurements[device.name] = await measureWorldInputResponses(devicePage);
    await context.close();
  }

  await test.info().attach("world-input-response.json", {
    body: Buffer.from(
      JSON.stringify(
        {
          budgetMs: WORLD_INPUT_RESPONSE_BUDGET_MS,
          sampleCount: WORLD_INPUT_RESPONSE_SAMPLES,
          measurements,
        },
        null,
        2,
      ),
    ),
    contentType: "application/json",
  });

  for (const [device, measurement] of Object.entries(measurements)) {
    for (const interaction of ["camera", "avatar", "chat"]) {
      const observed = measurement[`${interaction}Ms`];
      expect(
        observed,
        `${device} ${interaction} p95 was ${observed.toFixed(1)}ms; ` +
          `samples=${measurement.samples[interaction]
            .map((sample) => sample.toFixed(1))
            .join(",")}`,
      ).toBeLessThan(WORLD_INPUT_RESPONSE_BUDGET_MS);
    }
  }
});

test("busy walking stays connected while movement frames remain within the soft budget", async ({
  page,
}) => {
  const frames = [];
  let socketCount = 0;
  let rateDisconnects = 0;
  let rateWindowStartedAt = 0;
  let rateWindowCount = 0;
  await prepareWorldPage(page, "busy-movement", {
    worldSocketHandler(socket, socketId) {
      socketCount += 1;
      socket.onMessage((raw) => {
        const now = performance.now();
        if (!rateWindowStartedAt || now - rateWindowStartedAt >= 1000) {
          rateWindowStartedAt = now;
          rateWindowCount = 0;
        }
        rateWindowCount += 1;
        const frame = JSON.parse(String(raw));
        frames.push({ frame, at: now });
        if (rateWindowCount > 4) {
          rateDisconnects += 1;
          void socket.close({ code: 1008, reason: "soft rate budget" });
        }
      });
      socket.send(JSON.stringify({
        type: "welcome",
        id: socketId,
        self: {
          id: socketId,
          name: "visitor",
          status: "exploring",
          x: -8.1,
          y: 0.38,
          z: 30,
          yaw: 0,
          space: "town-square",
        },
        peers: [],
      }));
    },
  });
  await waitForWorld(page);

  await page.keyboard.down("ArrowUp");
  await page.waitForTimeout(3300);
  await page.keyboard.up("ArrowUp");
  await page.waitForTimeout(350);

  const connection = await page.locator("forkmesh-world").evaluate((shell) => ({
    readyState: shell.socket?.readyState,
    openState: WebSocket.OPEN,
    peerId: shell.serverPeerId,
  }));
  const movementFrames = frames.filter(({ frame }) => frame.type === "move");
  const activeFrames = movementFrames.filter(({ frame }) => frame.moving);
  expect(rateDisconnects).toBe(0);
  expect(socketCount).toBe(1);
  expect(connection.readyState).toBe(connection.openState);
  expect(connection.peerId).toBe("busy-movement");
  expect(activeFrames.length).toBeGreaterThanOrEqual(2);
  expect(activeFrames.length).toBeLessThanOrEqual(4);
  expect(movementFrames.at(-1).frame.moving).toBe(false);
  expect(movementFrames.length).toBeLessThanOrEqual(7);
});

test("local diagnostics report renderer and existing socket state without new telemetry", async ({
  page,
}) => {
  await page.addInitScript(() => {
    localStorage.setItem(
      "forkmesh.world.settings.v1",
      JSON.stringify({ debugPanel: true }),
    );
  });
  let socketCount = 0;
  await prepareWorldPage(page, "world-diagnostics", {
    worldSocketHandler(socket, socketId) {
      socketCount += 1;
      socket.send(JSON.stringify({
        type: "welcome",
        id: socketId,
        peers: [],
      }));
    },
  });
  await waitForWorld(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return Boolean(shell?.distanceTimer);
  });
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.lastMovementSentAt = performance.now();
    shell.queueMovementPresence({
      x: 1,
      y: 0.38,
      z: 1,
      heading: 0,
      moving: true,
    });
    shell.queueMovementPresence({
      x: 2,
      y: 0.38,
      z: 2,
      heading: 0,
      moving: true,
    });
    shell.sendPresence({ type: "presence" });
    shell.sendPresence({ type: "presence" });
    shell.renderDiagnostics();
  });
  await page.waitForTimeout(1150);

  const diagnostics = page.locator("[data-world-diagnostics]");
  const perfBar = page.locator("[data-world-perf-bar]");
  const liveChart = perfBar.locator("[data-world-diagnostics-chart]");
  await expect(diagnostics).not.toHaveAttribute("open", "");
  await expect(diagnostics.locator("[data-world-diagnostics-chart]")).toHaveCount(0);
  await expect(liveChart).toBeVisible();
  const barBox = await perfBar.boundingBox();
  const worldBox = await page.locator(".fm-world").boundingBox();
  expect(barBox.y).toBeGreaterThanOrEqual(worldBox.y + worldBox.height - 1);
  expect(barBox.width).toBeGreaterThanOrEqual(worldBox.width - 1);
  const tileBox = await liveChart
    .locator('[data-world-diagnostics-chart-metric="fps"]')
    .boundingBox();
  expect(Math.abs(tileBox.width - tileBox.height)).toBeLessThan(1);
  await expect(
    liveChart.locator('[data-world-diagnostics-chart-value="fps"]'),
  ).not.toHaveText("—");
  await expect(
    liveChart.locator('[data-world-diagnostics-chart-value="triangles"]'),
  ).not.toHaveText("—");
  await expect(
    liveChart.locator('[data-world-diagnostics-chart-value="memory"]'),
  ).toContainText("MB");
  for (const metric of ["fps", "triangles", "memory"]) {
    const line = liveChart.locator(
      `[data-world-diagnostics-chart-line="${metric}"]`,
    );
    await expect(line).toHaveAttribute(
      "points",
      /\d+\.\d,\d+\.\d \d+\.\d,\d+\.\d/,
    );
    const chartPoints = (await line.getAttribute("points"))
      .trim()
      .split(/\s+/)
      .map((point) => point.split(",").map(Number));
    expect(chartPoints.length).toBeGreaterThanOrEqual(2);
    for (const [, y] of chartPoints) {
      expect(y).toBeGreaterThan(0);
      expect(y).toBeLessThan(40);
    }
  }
  const collapsedBox = await diagnostics.boundingBox();
  expect(collapsedBox?.width).toBeLessThan(80);
  expect(collapsedBox?.height).toBeGreaterThanOrEqual(54);
  await expect(diagnostics.locator("summary")).toContainText("FPS");
  await expect(diagnostics.locator("summary")).toContainText("N online");
  await expect(diagnostics.locator("summary")).toContainText("1p");
  await expect(diagnostics.locator("summary")).toContainText("v0.7.0");
  await expect(
    diagnostics.locator("[data-world-diagnostics-music-progress]"),
  ).toHaveCount(0);
  for (const compactMetric of [
    "renderer",
    "frame",
    "input",
    "world",
    "connection",
    "traffic",
    "queues",
    "build",
  ]) {
    await expect(
      diagnostics.locator(
        `[data-world-diagnostics-${compactMetric}-compact]`,
      ),
    ).not.toBeEmpty();
  }
  await diagnostics.hover();
  await expect(diagnostics).toHaveAttribute("open", "");
  await expect(diagnostics).toContainText("ms/frame");
  await expect(diagnostics).toContainText("triangles");
  await expect(diagnostics).toContainText("Socket frames");
  await expect(diagnostics).toContainText("coalesced");
  await expect(diagnostics).toContainText("dddddddddddd");
  await expect(diagnostics).toContainText(
    "Nothing here is transmitted while you are in the World",
  );
  const triangleView = diagnostics.locator("[data-world-triangle-view]");
  await expect(triangleView).toHaveText("View triangles");
  await expect(triangleView).toHaveAttribute("aria-pressed", "false");
  await triangleView.click();
  await expect(triangleView).toHaveText("Show normal world");
  await expect(triangleView).toHaveAttribute("aria-pressed", "true");
  const triangleState = await page
    .locator("forkmesh-world")
    .evaluate((shell) => {
      const { camera, scene } = shell.world;
      let visibleMeshes = 0;
      let visibleNonMeshes = 0;
      scene.traverse((object) => {
        if (!object.layers?.test(camera.layers)) return;
        if (object.isMesh) visibleMeshes += 1;
        else if (object.isLine || object.isPoints || object.isSprite) {
          visibleNonMeshes += 1;
        }
      });
      return {
        active: shell.world.isTriangleView(),
        wireframe: scene.overrideMaterial?.wireframe,
        labelsHidden: shell.querySelector("[data-world-label-layer]")?.hidden,
        visibleMeshes,
        visibleNonMeshes,
      };
    });
  expect(triangleState).toMatchObject({
    active: true,
    wireframe: true,
    labelsHidden: true,
    visibleNonMeshes: 0,
  });
  expect(triangleState.visibleMeshes).toBeGreaterThan(0);
  await triangleView.click();
  await expect(triangleView).toHaveText("View triangles");
  await expect(triangleView).toHaveAttribute("aria-pressed", "false");
  expect(
    await page.locator("forkmesh-world").evaluate((shell) => ({
      active: shell.world.isTriangleView(),
      overrideMaterial: shell.world.scene.overrideMaterial,
      labelsHidden: shell.querySelector("[data-world-label-layer]")?.hidden,
    })),
  ).toEqual({
    active: false,
    overrideMaterial: null,
    labelsHidden: false,
  });

  const snapshot = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.lastDiagnosticsSnapshot,
  );
  expect(snapshot.renderer.fps).toBeGreaterThan(0);
  expect(snapshot.renderer.frameTimeMs).toBeGreaterThan(0);
  expect(snapshot.renderer.calls).toBeGreaterThan(0);
  expect(snapshot.renderer.triangles).toBeGreaterThan(0);
  expect(snapshot.history.length).toBeGreaterThanOrEqual(2);
  expect(snapshot.history.at(-1)).toMatchObject({
    fps: expect.any(Number),
    triangles: expect.any(Number),
    memoryMB: expect.any(Number),
  });
  expect(snapshot.connection).toMatchObject({
    state: "online",
    peers: 1,
    reconnects: 0,
  });
  expect(snapshot.traffic.inboundFrames).toBeGreaterThanOrEqual(1);
  expect(snapshot.traffic.outboundFrames).toBeGreaterThanOrEqual(1);
  expect(snapshot.queues.movementCoalesced).toBeGreaterThanOrEqual(1);
  expect(snapshot.queues.profileCoalesced).toBeGreaterThanOrEqual(1);
  expect(snapshot.build).toEqual({
    version: "0.7.0",
    revision: "d".repeat(40),
  });
  expect(Object.keys(snapshot).sort()).toEqual(
    [
      "build",
      "complexity",
      "connection",
      "history",
      "memory",
      "output",
      "position",
      "queues",
      "renderer",
      "residency",
      "traffic",
    ].sort(),
  );
  expect(JSON.stringify(snapshot)).not.toContain("127.0.0.1");
  expect(JSON.stringify(snapshot)).not.toContain("/world/");
  expect(socketCount).toBe(1);
});

test("the Debug tab lists every individual object drawing triangles and sorts by any column", async ({
  page,
}) => {
  await prepareWorldPage(page, "world-debug-objects");
  await waitForWorld(page);

  await page.locator("[data-world-settings-open]").first().click();
  await page.locator('[data-world-settings-tab="debug"]').click();

  const rows = page.locator("[data-world-object-list] .world-object-row");
  await expect(rows.first()).toBeVisible();
  const status = page.locator("[data-world-object-status]");
  await expect(status).toContainText("objects");
  await expect(status).toContainText("triangles");

  // Every row is one real mesh, and the scene walk agrees with the table.
  const objects = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.listSceneObjects(),
  );
  expect(objects.length).toBeGreaterThan(10);
  expect(objects.every((object) => object.triangles > 0)).toBe(true);
  expect(objects.every((object) => typeof object.label === "string")).toBe(true);
  expect(new Set(objects.map((object) => object.element)).size).toBeGreaterThan(
    1,
  );

  const readLabels = async () =>
    page
      .locator("[data-world-object-list] .world-object-row strong")
      .allInnerTexts();

  // Default order is heaviest triangle count first.
  const labelsByTriangles = await readLabels();
  expect(labelsByTriangles.length).toBeGreaterThan(1);

  // Clicking the active column reverses it; clicking another sorts by it.
  await page.locator('[data-world-object-sort="triangles"]').click();
  await expect(
    page.locator('[data-world-object-sort="triangles"]'),
  ).toHaveAttribute("aria-sort", "ascending");
  expect(await readLabels()).not.toEqual(labelsByTriangles);

  await page.locator('[data-world-object-sort="label"]').click();
  await expect(page.locator('[data-world-object-sort="label"]')).toHaveAttribute(
    "aria-sort",
    "ascending",
  );
  const byLabel = await readLabels();
  expect(byLabel).toEqual([...byLabel].sort((a, b) => a.localeCompare(b)));

  // Re-walking keeps the panel populated.
  await page.locator("[data-world-object-refresh]").click();
  await expect(rows.first()).toBeVisible();
});

test("desert plots ring the hexagonal city and their signs open Start your own instance", async ({
  page,
}) => {
  await prepareWorldPage(page, "world-desert-plots");
  await waitForWorld(page);

  const desert = await page.locator("forkmesh-world").evaluate((shell) => {
    const scene = shell.world.scene;
    const plots = scene.getObjectByName("forkmesh-desert-plots");
    const signs = plots?.children.filter((child) =>
      child.name.startsWith("forkmesh-start-world-sign-"),
    );
    return {
      element: shell.world
        .listWorldElements()
        .some((entry) => entry.id === "desert-plots"),
      edgeWalk: Boolean(scene.getObjectByName("forkmesh-void-walk-rails")),
      sand: plots?.getObjectByName("forkmesh-desert-plot-sand")?.userData
        ?.ground,
      signs: signs?.length,
      clickable: signs?.every((sign) =>
        sign.children.some(
          (child) => child.userData.interactive === "start-world-sign",
        ),
      ),
      plots: signs?.map(
        (sign) =>
          sign.children.find(
            (child) => child.userData.interactive === "start-world-sign",
          )?.userData.plot,
      ),
    };
  });
  expect(desert).toEqual({
    element: true,
    edgeWalk: false,
    sand: true,
    signs: 12,
    clickable: true,
    plots: Array.from({ length: 12 }, (_, index) => index + 1),
  });

  await expect(page.locator("[data-world-start-world]")).toHaveCount(0);
  await expect(page.locator("[data-world-instance-launcher]")).toHaveCount(0);
  await page
    .context()
    .route(/\/start\?plot=/, (route) =>
      route.fulfill({ status: 200, contentType: "text/html", body: "start" }),
    );
  const [startPage] = await Promise.all([
    page.context().waitForEvent("page"),
    page
      .locator("forkmesh-world")
      .evaluate((shell) => shell.openStartInstance(3)),
  ]);
  await startPage.waitForLoadState();
  const opened = new URL(startPage.url());
  expect(opened.pathname).toBe("/start");
  expect(opened.searchParams.get("plot")).toBe("3");
  await startPage.close();
});

test("every element in the Elements tab opens into its own pieces, each list sorted on its own", async ({
  page,
}) => {
  await prepareWorldPage(page, "world-element-parts");
  await waitForWorld(page);

  await page.locator("[data-world-settings-open]").first().click();
  await page.locator('[data-world-settings-tab="elements"]').click();

  const officeRow = page.locator(
    '[data-world-element-expand="office-interior"]',
  );
  await expect(officeRow).toBeVisible();
  await expect(officeRow).toHaveAttribute("aria-expanded", "false");

  // Opening an element lists the pieces inside it, not the element again.
  await officeRow.click();
  await expect(officeRow).toHaveAttribute("aria-expanded", "true");
  const parts = page.locator("[data-world-element-list] .world-element-part");
  await expect(parts.first()).toBeVisible();
  const partCount = await parts.count();
  expect(partCount).toBeGreaterThan(1);

  // The pieces of one element never outweigh the element itself.
  const totals = await page.locator("forkmesh-world").evaluate((shell) => {
    const element = shell.world
      .listWorldElements()
      .find((entry) => entry.id === "office-interior");
    const walk = shell.world.listWorldElementParts("office-interior", []);
    return {
      elementTriangles: element.triangles,
      partTriangles: walk.parts.reduce((sum, part) => sum + part.triangles, 0),
      parts: walk.parts.length,
      total: walk.total,
    };
  });
  expect(totals.parts).toBeGreaterThan(1);
  expect(totals.partTriangles).toBe(totals.elementTriangles);

  const scope = "office-interior";
  const readParts = async () =>
    page
      .locator(
        `[data-world-element-list] .world-element-part:not([style*="depth:2"]) strong`,
      )
      .allInnerTexts();
  const byDrawn = await readParts();

  // Each opened list sorts on its own header.
  await page
    .locator(
      `[data-world-element-part-scope="${scope}"][data-world-element-part-sort="label"]`,
    )
    .click();
  await expect(
    page.locator(
      `[data-world-element-part-scope="${scope}"][data-world-element-part-sort="label"]`,
    ),
  ).toHaveAttribute("aria-sort", "ascending");
  const byLabelParts = await readParts();
  expect(byLabelParts).toEqual(
    [...byLabelParts].sort((a, b) => a.localeCompare(b)),
  );
  expect(byLabelParts).not.toEqual(byDrawn);

  // A piece with pieces of its own opens again, one level deeper.
  const nested = page
    .locator('.world-element-part [data-world-element-expand^="office-interior/"]')
    .first();
  await expect(nested).toBeVisible();
  await nested.click();
  await expect(nested).toHaveAttribute("aria-expanded", "true");
  await expect(
    page.locator("[data-world-element-list] .world-element-subhead"),
  ).toHaveCount(2);

  // The checkbox still switches the whole element off, and the tree survives.
  await page
    .locator('[data-world-element-toggle="office-interior"]')
    .setChecked(false);
  await expect(
    page.locator('[data-world-element-toggle="office-interior"]'),
  ).not.toBeChecked();
  await expect(officeRow).toHaveAttribute("aria-expanded", "true");
  await expect(parts.first()).toBeVisible();

  // Collapsing hides every row underneath it.
  await page
    .locator('[data-world-element-toggle="office-interior"]')
    .setChecked(true);
  await officeRow.click();
  await expect(officeRow).toHaveAttribute("aria-expanded", "false");
  await expect(parts).toHaveCount(0);
});

test("right-clicking a piece of the world deletes it, and the Elements tab puts it back", async ({
  page,
}) => {
  // Boot, two settings round trips, a delete and a restore do not fit in the
  // default per-test budget.
  test.slow();
  await prepareWorldPage(page, "world-object-delete");
  await waitForWorld(page);

  const countSceneObjects = () =>
    page.locator("forkmesh-world").evaluate((shell) => {
      let objects = 0;
      shell.world.scene.traverse(() => {
        objects += 1;
      });
      return objects;
    });

  const canvas = page.locator("[data-world-canvas-wrap]");
  const box = await canvas.boundingBox();
  const aim = { x: box.x + box.width / 2, y: box.y + box.height * 0.66 };
  const menu = page.locator("[data-world-object-menu]");

  // Without the tool switched on, the browser keeps its own context menu.
  // Dispatched rather than clicked, so no native menu is left open over the
  // canvas for the rest of this test.
  await canvas.dispatchEvent("contextmenu");
  await expect(menu).toBeHidden();

  // Turning the debug panel on is the visitor-facing way in; an administrator
  // has it without asking.
  await page.locator("[data-world-settings-open]").first().click();
  await page.locator('[data-world-settings-tab="debug"]').click();
  await page.locator("[data-world-debug-panel]").check();
  await page.keyboard.press("Escape");
  await expect(page.locator("[data-world-settings]")).toHaveAttribute(
    "data-open",
    "false",
  );

  const before = await countSceneObjects();
  await page.mouse.click(aim.x, aim.y, { button: "right" });
  await expect(menu).toBeVisible();
  const deleteAction = menu.locator("[data-world-object-delete]").first();
  const key = await deleteAction.getAttribute("data-world-object-delete");
  expect(key).toMatch(/^[a-z0-9-]+:\d+(\.\d+)*$/);

  // Deleting takes it out of the scene graph and off the device's clickable
  // list, and remembers the choice on this browser.
  await deleteAction.click();
  await expect(menu).toBeHidden();
  expect(await countSceneObjects()).toBeLessThan(before);
  expect(
    await page
      .locator("forkmesh-world")
      .evaluate((shell) =>
        shell.world.listDeletedWorldObjects().map((entry) => entry.key),
      ),
  ).toEqual([key]);
  expect(
    await page.evaluate(() =>
      JSON.parse(
        localStorage.getItem("forkmesh.world.deletedObjects.v1") || "[]",
      ).map((entry) => entry.key),
    ),
  ).toEqual([key]);

  // Everything deleted is listed in the Elements tab, and one click puts it
  // back exactly where it was.
  await page.locator("[data-world-settings-open]").first().click();
  await page.locator('[data-world-settings-tab="elements"]').click();
  const deletedRows = page.locator(
    "[data-world-deleted-list] .world-deleted-row",
  );
  await expect(deletedRows).toHaveCount(1);
  await deletedRows.locator("[data-world-object-restore]").click();
  await expect(deletedRows).toHaveCount(0);
  expect(await countSceneObjects()).toBe(before);
  expect(
    await page.evaluate(
      () => localStorage.getItem("forkmesh.world.deletedObjects.v1") || "[]",
    ),
  ).toBe("[]");
});

test("a deleted piece is still gone after a reload, and still comes back", async ({
  page,
}) => {
  test.slow();
  await prepareWorldPage(page, "world-object-delete-reload");
  await waitForWorld(page);

  const countSceneObjects = () =>
    page.locator("forkmesh-world").evaluate((shell) => {
      let objects = 0;
      shell.world.scene.traverse(() => {
        objects += 1;
      });
      return objects;
    });

  await page.locator("[data-world-settings-open]").first().click();
  await page.locator('[data-world-settings-tab="debug"]').click();
  await page.locator("[data-world-debug-panel]").check();
  await page.keyboard.press("Escape");

  const box = await page.locator("[data-world-canvas-wrap]").boundingBox();
  const before = await countSceneObjects();
  await page.mouse.click(box.x + box.width / 2, box.y + box.height * 0.66, {
    button: "right",
  });
  const menu = page.locator("[data-world-object-menu]");
  await expect(menu).toBeVisible();
  const deleteAction = menu.locator("[data-world-object-delete]").first();
  const key = await deleteAction.getAttribute("data-world-object-delete");
  await deleteAction.click();
  const afterDelete = await countSceneObjects();
  expect(afterDelete).toBeLessThan(before);

  // The world opens the way it was left: the piece comes out again as its
  // element is rebuilt, not as a leftover entry waiting for one.
  await page.reload();
  await waitForWorldReady(page);
  expect(await countSceneObjects()).toBe(afterDelete);
  expect(
    await page
      .locator("forkmesh-world")
      .evaluate((shell) => shell.world.listDeletedWorldObjects()),
  ).toEqual([
    {
      key,
      label: "campfire-member-circle-dirt",
      elementId: "campfire",
      elementLabel: "Members Circle campfire",
      pending: false,
    },
  ]);

  await page.locator("[data-world-settings-open]").first().click();
  await page.locator('[data-world-settings-tab="elements"]').click();
  await page.locator("[data-world-object-restore]").first().click();
  expect(await countSceneObjects()).toBe(before);
});

test("the topbar has no clock or emote actions and local light level survives movement without becoming presence data", async ({
  page,
}) => {
  await prepareWorldPage(page, "local-light-level");
  await waitForWorld(page);

  await expect(page.locator("[data-world-clock]")).toHaveCount(0);
  await expect(page.locator("[data-world-emote]")).toHaveCount(0);
  const initial = await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setDaylightMode("day");
    return shell.world.getEnvironmentState();
  });
  expect(initial.theme).toBe("world");
  expect(initial.lightLevel).toBe(100);

  await page.keyboard.down("ArrowRight");
  await page.waitForTimeout(320);
  await page.keyboard.up("ArrowRight");
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.travelToRegion("east");
  });
  const afterMovement = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getEnvironmentState(),
  );
  expect(afterMovement).toEqual(initial);

  await page.locator("[data-world-settings-open]").first().click();
  const light = page.locator("[data-world-light-level]");
  await light.fill("65");
  await expect(page.locator("[data-world-light-level-output]")).toHaveText(
    "65%",
  );
  const adjusted = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getEnvironmentState(),
  );
  expect(adjusted.lightLevel).toBe(65);
  expect(adjusted.sunIntensity).toBeLessThan(initial.sunIntensity);
  expect(adjusted.sunPosition).toEqual(initial.sunPosition);
  const stored = await page.evaluate(() =>
    JSON.parse(localStorage.getItem("forkmesh.world.settings.v1") || "{}"),
  );
  expect(stored.lightLevel).toBe(65);
  const publicIdentityState = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.identity,
  );
  expect(publicIdentityState.lightLevel).toBeUndefined();
});

// The four presets the Preferences panel offers under "Personal environment",
// with the layer each one is expected to switch on. Reported as issue #633:
// picking one only showed a notice, and whatever did change was drawn at the middle of
// the map, so a visitor standing anywhere else saw the same World as before.
const ENVIRONMENT_PRESETS = [
  { id: "world", label: "Full daylight", field: "" },
  { id: "rain", label: "Daylight + rain", field: "rain" },
  { id: "snow", label: "Daylight + snow", field: "snow" },
  { id: "winter", label: "Daylight + winter", field: "snow" },
];
// Far enough from the origin that the old map-centred particle field would be
// nowhere near the avatar, and inside WORLD_RADIUS so it is somewhere a
// visitor can actually walk to.
const OFF_CENTRE_SPOT = { x: 286, z: -204 };

test("every personal environment preset repaints the sky, the light, and the weather around the visitor", async ({
  page,
}) => {
  await prepareWorldPage(page, "environment-presets");
  await waitForWorld(page);

  await page.locator("forkmesh-world").evaluate((shell, spot) => {
    // Hold the sky at midday so the presets are compared against each other
    // and not against the clock drifting underneath them.
    shell.world.setDaylightMode("day");
    shell.world.setSpawn({ ...spot, space: "town-square" });
  }, OFF_CENTRE_SPOT);

  const readEnvironment = () =>
    page.locator("forkmesh-world").evaluate((shell) => ({
      environment: shell.world.getEnvironmentState(),
      position: shell.world.getPosition(),
    }));

  const daylight = await readEnvironment();
  expect(daylight.position.x).toBeCloseTo(OFF_CENTRE_SPOT.x, 3);
  expect(daylight.position.z).toBeCloseTo(OFF_CENTRE_SPOT.z, 3);
  expect(daylight.environment.theme).toBe("world");

  await page.locator("[data-world-settings-open]").first().click();
  await page.locator('[data-world-settings-tab="view"]').click();

  const seen = [];
  for (const preset of ENVIRONMENT_PRESETS) {
    const option = page.locator(`[data-world-theme="${preset.id}"]`);
    await expect(option).toHaveText(preset.label);
    await option.click();
    await expect(option).toHaveAttribute("aria-pressed", "true");
    await expect(
      page.locator("[data-world-settings] [data-world-inline-status]"),
    ).toContainText(`${preset.label} is saved to your account`);

    const { environment } = await readEnvironment();
    expect(environment.theme).toBe(preset.id);

    // The particle system the preset asks for, and only that one.
    expect(environment.weather.kind).toBe(preset.field);
    expect(environment.weather.rain).toBe(preset.field === "rain");
    expect(environment.weather.snow).toBe(preset.field === "snow");

    if (preset.field) {
      // Drawn around the avatar, not around the origin. The whole field is
      // within one field-radius of where the visitor is standing.
      expect(environment.weather.center[0]).toBeCloseTo(OFF_CENTRE_SPOT.x, 0);
      expect(environment.weather.center[2]).toBeCloseTo(OFF_CENTRE_SPOT.z, 0);
      expect(environment.weather.farthest).toBeLessThan(58);
      expect(environment.weather.span[0]).toBeGreaterThan(70);
      expect(environment.weather.span[2]).toBeGreaterThan(70);
    }

    seen.push({
      id: preset.id,
      background: environment.background,
      skyTint: environment.sky.tint,
      sun: Number(environment.sunIntensity.toFixed(4)),
      hemisphere: Number(environment.hemisphereIntensity.toFixed(4)),
      exposure: Number(environment.exposure.toFixed(4)),
    });
  }

  // Sky colour, sky-body tint, and all three global light terms are distinct
  // per preset: no two of the four render the same environment.
  for (const key of ["background", "skyTint", "sun", "hemisphere", "exposure"]) {
    expect(new Set(seen.map((entry) => entry[key])).size).toBe(seen.length);
  }
  // Full daylight is the un-tinted baseline; the three weather presets each
  // pull the sky bodies toward their own overcast colour.
  expect(seen[0].skyTint).toBe("");
  seen.slice(1).forEach((entry) => expect(entry.skyTint).toMatch(/^#[0-9a-f]{6}$/));

  // Walking keeps the preset with you: the field re-centres on the avatar
  // instead of being left behind at the spot it was switched on.
  const walked = await page.locator("forkmesh-world").evaluate(async (shell) => {
    shell.world.setSpawn({ x: -338, z: 402, space: "town-square" });
    await new Promise((resolve) => requestAnimationFrame(() => resolve()));
    await new Promise((resolve) => requestAnimationFrame(() => resolve()));
    return shell.world.getEnvironmentState().weather;
  });
  expect(walked.kind).toBe("snow");
  expect(walked.center[0]).toBeCloseTo(-338, 0);
  expect(walked.center[2]).toBeCloseTo(402, 0);
  expect(walked.farthest).toBeLessThan(58);
});

test("a handshake is offered to one visitor and poses both avatars once accepted", async ({
  page,
}) => {
  test.slow();
  const frames = [];
  let relay = null;
  await prepareWorldPage(page, "handshake-host", {
    worldSocketHandler(socket, socketId) {
      relay = socket;
      socket.onMessage((raw) => frames.push(JSON.parse(String(raw))));
      socket.send(JSON.stringify({
        type: "welcome",
        id: socketId,
        peers: [
          {
            id: "peer-neighbor",
            name: "Neighbor",
            status: "available",
            x: 3,
            y: 0.38,
            z: 4,
            yaw: 0,
            space: "town-square",
          },
        ],
      }));
    },
  });
  await waitForWorld(page);

  // A handshake is offered from the selected visitor's own profile, so it is
  // always addressed at exactly one live peer.
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.openWorldMemberDetail({
      peerId: "peer-neighbor",
      name: "Neighbor",
      accountStatus: "Guest",
    });
  });
  await page.getByRole("button", { name: "Offer handshake" }).click();
  await expect
    .poll(() => frames.filter((frame) => frame.kind === "handshake-offer"))
    .toEqual([
      { type: "interaction", kind: "handshake-offer", target: "peer-neighbor" },
    ]);
  await expect(
    page.getByRole("button", { name: "Handshake offered" }),
  ).toBeDisabled();

  // The relay publishes the accepted pair to the room; both halves pose.
  relay.send(JSON.stringify({
    type: "interaction",
    kind: "handshake",
    from: "peer-neighbor",
    with: "handshake-host",
  }));
  await expect
    .poll(() =>
      page.locator("forkmesh-world").evaluate((shell) => ({
        self:
          Number(shell.world.player.userData.handshakeStartedAt || 0) > 0,
        peer:
          Number(
            shell.world.scene.getObjectByName("avatar:peer-neighbor")
              ?.userData?.handshakeStartedAt || 0,
          ) > 0,
      })),
    )
    .toEqual({ self: true, peer: true });
  await expect(
    page.getByRole("button", { name: "Offer handshake" }),
  ).toBeEnabled();

  // An offer arriving from that peer turns the same panel into the answer.
  relay.send(JSON.stringify({
    type: "interaction",
    kind: "handshake-offer",
    from: "peer-neighbor",
  }));
  await page.getByRole("button", { name: "Shake hands back" }).click();
  await expect
    .poll(() => frames.filter((frame) => frame.kind === "handshake-accept"))
    .toEqual([
      {
        type: "interaction",
        kind: "handshake-accept",
        target: "peer-neighbor",
      },
    ]);
  // Answering consumes the offer: the panel goes back to offering one.
  await expect(
    page.getByRole("button", { name: "Offer handshake" }),
  ).toBeVisible();
});

test("Unicode emoji status is local-persisted, coalesced, and available to every avatar label", async ({
  page,
  context,
}) => {
  const frames = [];
  await prepareWorldPage(page, "emoji-status-owner", {
    worldSocketHandler(socket, socketId) {
      socket.onMessage((raw) => frames.push(JSON.parse(String(raw))));
      socket.send(JSON.stringify({
        type: "welcome",
        id: socketId,
        self: {
          id: socketId,
          name: "owner",
          status: "available",
          x: -8.1,
          y: 0.38,
          z: 30,
          yaw: 0,
          space: "town-square",
        },
        peers: [
          {
            id: "peer-status",
            name: "Peer",
            status: "available",
            statusEmoji: "🚀",
            statusNote: "shipping",
            x: 3,
            y: 0.38,
            z: 4,
            yaw: 0,
            space: "town-square",
          },
        ],
      }));
    },
  });
  await waitForWorld(page);
  await expect(
    page.locator('[data-player-label="peer-status"]'),
  ).toHaveAttribute("aria-label", "Peer, public status 🚀 shipping");
  const peerStatus = await page.locator("forkmesh-world").evaluate((shell) => {
    const avatar = shell.world.scene.getObjectByName("avatar:peer-status");
    const sprite = avatar?.getObjectByName("forkmesh-avatar-emoji-status");
    return {
      emoji: avatar?.userData?.statusEmoji,
      note: avatar?.userData?.statusNote,
      spriteVisible: Boolean(sprite?.visible),
    };
  });
  expect(peerStatus).toEqual({
    emoji: "🚀",
    note: "shipping",
    spriteVisible: false,
  });

  const firstChangeFrameIndex = frames.length;
  const pageCount = context.pages().length;
  await page.locator("[data-world-settings-open]").first().click();
  await page.locator(".world-emoji-picker").evaluate((picker) => {
    picker.open = true;
  });
  await page.locator("[data-world-emoji-category]").selectOption("gestures");
  await page.getByRole("button", {
    name: "Use 🧑‍💻 as public status",
  }).click();
  const note = page.locator("[data-world-status-note]");
  await note.fill("coding");
  await note.dispatchEvent("change");
  await expect(page.locator("[data-world-status-preview]")).toHaveText(
    "🧑‍💻 coding",
  );
  await expect.poll(() =>
    frames.filter(
      (frame) =>
        frame.type === "presence" &&
        frame.statusEmoji === "🧑‍💻" &&
        frame.statusNote === "coding",
    ).length,
  ).toBe(1);
  const statusFrames = frames
    .slice(firstChangeFrameIndex)
    .filter(
      (frame) => frame.type === "presence" && Boolean(frame.statusEmoji),
    );
  expect(statusFrames.length).toBeLessThanOrEqual(2);
  expect(
    frames
      .filter((frame) => frame.type === "move")
      .some((frame) => "statusEmoji" in frame || "statusNote" in frame),
  ).toBe(false);

  const saved = await page.evaluate(() =>
    JSON.parse(localStorage.getItem("forkmesh.world.settings.v1") || "{}"),
  );
  expect(saved.statusEmoji).toBe("🧑‍💻");
  expect(saved.statusNote).toBe("coding");
  const ownStatus = await page.locator("forkmesh-world").evaluate((shell) => ({
    emoji: shell.world.player.userData.statusEmoji,
    note: shell.world.player.userData.statusNote,
    spriteVisible: Boolean(
      shell.world.player.getObjectByName("forkmesh-avatar-emoji-status")
        ?.visible,
    ),
  }));
  expect(ownStatus).toEqual({
    emoji: "🧑‍💻",
    note: "coding",
    spriteVisible: false,
  });

  const codingFramesBeforeDuplicate = frames.filter(
    (frame) =>
      frame.type === "presence" &&
      frame.statusEmoji === "🧑‍💻" &&
      frame.statusNote === "coding",
  ).length;
  await note.dispatchEvent("change");
  await page.waitForTimeout(450);
  expect(frames.filter(
    (frame) =>
      frame.type === "presence" &&
      frame.statusEmoji === "🧑‍💻" &&
      frame.statusNote === "coding",
  )).toHaveLength(codingFramesBeforeDuplicate);

  await note.fill("two words");
  await note.dispatchEvent("change");
  await expect(note).toHaveValue("coding");
  expect(context.pages()).toHaveLength(pageCount);
  expect(new URL(page.url()).pathname).toBe("/world/");
});

test("@critical two live clients synchronize movement and camera look without leaking disabled badge fields", async ({
  context,
}) => {
  const observer = await context.newPage();
  await prepareWorldPage(observer, "observer", {
    worldSocketHandler: () => {},
  });
  await waitForWorld(observer);

  await observer.evaluate((settings) => {
    localStorage.setItem(
      "forkmesh.world.settings.v1",
      JSON.stringify(settings),
    );
  }, PRIVATE_SETTINGS);

  const privateClient = await context.newPage();
  await prepareWorldPage(privateClient, "private-client", {
    worldSocketHandler: () => {},
  });
  await waitForWorld(privateClient);

  await observer.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return shell?.localPeers?.size > 0;
  });
  let peer = await observer.locator("forkmesh-world").evaluate((shell) =>
    [...shell.localPeers.values()][0],
  );
  expect(peer.name).toBe("Private visitor");
  expect(peer.flag).toBe("◌");
  expect(peer.browser).toBe("Hidden");
  expect(peer.os).toBe("Hidden");
  expect(peer.localTime).toBe("");
  expect(peer.nodes).toEqual([]);
  expect(peer.status).toBe("hidden");

  await privateClient.locator("forkmesh-world").evaluate((shell) => {
    shell.handleMovement({
      x: 11.75,
      y: 0.38,
      z: -4.25,
      heading: 0.6,
      activity: "private/repository?token=must-not-cross",
    });
  });
  await observer.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    const peer = [...(shell?.localPeers?.values?.() || [])][0];
    return peer?.x === 11.75 && peer?.z === -4.25;
  });
  peer = await observer.locator("forkmesh-world").evaluate((shell) =>
    [...shell.localPeers.values()][0],
  );
  expect(peer.activity).toBe("online");
  expect(JSON.stringify(peer)).not.toContain("private/repository");
  expect(JSON.stringify(peer)).not.toContain("must-not-cross");

  // Camera orbit rides the same bounded public-presence shape as movement.
  // The observing client sees the head turn and tilt while the mover's body
  // keeps the heading assigned by its actual walk.
  await privateClient.locator("forkmesh-world").evaluate((shell) => {
    shell.handleCameraLook({ yaw: -1.1, pitch: 0.5 });
    shell.broadcastLocalPresence();
  });
  await observer.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    const peer = [...(shell?.localPeers?.values?.() || [])][0];
    return peer?.lookYaw === -1.1 && peer?.lookPitch === 0.5;
  });
  const remoteLook = await observer.locator("forkmesh-world").evaluate(
    (shell) => {
      const [peer] = [...shell.localPeers.values()];
      const avatar = shell.world.scene.getObjectByName(`avatar:local:${peer.id}`);
      return {
        body: avatar?.userData?.targetHeading,
        yaw: avatar?.userData?.targetLookYaw,
        pitch: avatar?.userData?.targetLookPitch,
      };
    },
  );
  expect(remoteLook).toEqual({ body: 0.6, yaw: -1.1, pitch: 0.5 });
  await observer.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    const [peer] = [...(shell?.localPeers?.values?.() || [])];
    const avatar = shell?.world?.scene?.getObjectByName(`avatar:local:${peer?.id}`);
    const head = avatar?.userData?.headRig?.rotation;
    return head?.y < -0.2 && head?.x < -0.1;
  });
});

test("landscape thumbstick remains visible and moves the avatar", async ({
  browser,
}) => {
  const context = await browser.newContext({
    viewport: { width: 844, height: 390 },
    hasTouch: true,
    isMobile: true,
  });
  const page = await context.newPage();
  await prepareWorldPage(page, "landscape-touch");
  await waitForWorld(page);

  const control = page.locator("[data-world-thumbstick]");
  await expect(control).toBeVisible();
  const canvasBox = await page.locator("[data-world-canvas-wrap]").boundingBox();
  expect(canvasBox).not.toBeNull();
  expect(canvasBox.height).toBeLessThanOrEqual(390);

  const before = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getPosition(),
  );
  await dragThumbstick(page, control);
  const after = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getPosition(),
  );
  expect(Math.hypot(after.x - before.x, after.z - before.z)).toBeGreaterThan(
    0.05,
  );
  await context.close();
});

test("thumbstick motion is continuous, proportional, and recenters on release", async ({
  browser,
}) => {
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
  });
  const page = await context.newPage();
  await prepareWorldPage(page, "analog-thumbstick");
  await waitForWorld(page);

  await expect(page.locator(".world-controls")).toHaveCount(0);
  await expect(page.locator("[data-move]")).toHaveCount(0);
  const thumbstick = page.locator("[data-world-thumbstick]");
  const handle = page.locator("[data-world-thumbstick-handle]");
  await expect(thumbstick).toBeVisible();
  const box = await thumbstick.boundingBox();
  const handleBox = await handle.boundingBox();
  expect(box).not.toBeNull();
  expect(handleBox).not.toBeNull();

  const centre = {
    x: Math.round(box.x + box.width / 2),
    y: Math.round(box.y + box.height / 2),
  };
  const travel =
    Math.min(box.width, box.height) / 2 -
    Math.max(handleBox.width, handleBox.height) / 2 -
    5;
  const point = (strength) => ({
    x: centre.x,
    y: Math.round(centre.y - travel * strength),
    id: 1,
  });
  const client = await page.context().newCDPSession(page);
  const before = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getPosition(),
  );

  await client.send("Input.dispatchTouchEvent", {
    type: "touchStart",
    touchPoints: [{ ...centre, id: 1 }],
  });
  await client.send("Input.dispatchTouchEvent", {
    type: "touchMove",
    touchPoints: [point(0.5)],
  });
  await page.waitForTimeout(300);
  const partialState = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getMovementState(),
  );
  const partialPosition = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.world.getPosition(),
  );

  await client.send("Input.dispatchTouchEvent", {
    type: "touchMove",
    touchPoints: [point(1)],
  });
  await page.waitForTimeout(300);
  const fullState = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getMovementState(),
  );
  const fullPosition = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getPosition(),
  );
  await client.send("Input.dispatchTouchEvent", {
    type: "touchEnd",
    touchPoints: [],
  });

  expect(partialState.touchStrength).toBeGreaterThan(0.3);
  expect(partialState.touchStrength).toBeLessThan(0.7);
  expect(fullState.touchStrength).toBeGreaterThan(0.9);
  const partialDistance = Math.hypot(
    partialPosition.x - before.x,
    partialPosition.z - before.z,
  );
  const fullDistance = Math.hypot(
    fullPosition.x - partialPosition.x,
    fullPosition.z - partialPosition.z,
  );
  expect(fullDistance).toBeGreaterThan(partialDistance * 1.35);
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.getMovementState().touchActive,
    ),
  ).toBe(false);
  await expect(thumbstick).toHaveAttribute("data-active", "false");
  expect(
    await handle.evaluate((element) =>
      getComputedStyle(element).getPropertyValue("--thumb-y").trim(),
    ),
  ).toBe("0px");
  await client.detach();
  await context.close();
});

test("thumbstick recenters when its terminal event arrives outside the control", async ({
  browser,
}) => {
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
  });
  const page = await context.newPage();
  await prepareWorldPage(page, "thumbstick-interrupted-release");
  await waitForWorld(page);

  const thumbstick = page.locator("[data-world-thumbstick]");
  const handle = page.locator("[data-world-thumbstick-handle]");
  const box = await thumbstick.boundingBox();
  expect(box).not.toBeNull();
  const centre = {
    x: Math.round(box.x + box.width / 2),
    y: Math.round(box.y + box.height / 2),
  };
  const dispatchThumbstickPointer = (type, pointerId) =>
    thumbstick.evaluate(
      (element, { type, pointerId, x, y }) => {
        element.dispatchEvent(
          new PointerEvent(type, {
            pointerId,
            pointerType: "touch",
            clientX: x,
            clientY: y,
            bubbles: true,
            cancelable: true,
          }),
        );
      },
      {
        type,
        pointerId,
        x: centre.x,
        y: centre.y - (type === "pointermove" ? box.height * 0.3 : 0),
      },
    );
  await dispatchThumbstickPointer("pointerdown", 1);
  await dispatchThumbstickPointer("pointermove", 1);
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.getMovementState().touchActive,
    ),
  ).toBe(true);

  // Simulate a mobile browser delivering the terminal event at window rather
  // than at the captured control. The element listener alone cannot see this.
  await page.evaluate(() => {
    window.dispatchEvent(
      new PointerEvent("pointercancel", {
        pointerId: 1,
        bubbles: true,
        cancelable: true,
      }),
    );
  });

  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.getMovementState().touchActive,
    ),
  ).toBe(false);
  await expect(thumbstick).toHaveAttribute("data-active", "false");
  for (const property of ["--thumb-x", "--thumb-y"]) {
    expect(
      await handle.evaluate((element, name) =>
        getComputedStyle(element).getPropertyValue(name).trim(),
        property,
      ),
    ).toBe("0px");
  }

  // A fresh touch must be accepted instead of being blocked by stale state.
  await dispatchThumbstickPointer("pointerdown", 2);
  await dispatchThumbstickPointer("pointermove", 2);
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.getMovementState().touchActive,
    ),
  ).toBe(true);
  await dispatchThumbstickPointer("pointerup", 2);
  await context.close();
});

test("thumbstick walking defers renderer resize until release", async ({
  browser,
}) => {
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
  });
  const page = await context.newPage();
  await prepareWorldPage(page, "thumbstick-resize");
  await waitForWorld(page);

  const thumbstick = page.locator("[data-world-thumbstick]");
  const box = await thumbstick.boundingBox();
  expect(box).not.toBeNull();
  const centre = {
    x: Math.round(box.x + box.width / 2),
    y: Math.round(box.y + box.height / 2),
    id: 1,
  };
  const client = await page.context().newCDPSession(page);
  await client.send("Input.dispatchTouchEvent", {
    type: "touchStart",
    touchPoints: [centre],
  });
  await client.send("Input.dispatchTouchEvent", {
    type: "touchMove",
    touchPoints: [{ ...centre, y: centre.y - box.height * 0.3 }],
  });

  const bufferBeforeResize = await page
    .locator(".world-canvas")
    .evaluate((canvas) => ({ width: canvas.width, height: canvas.height }));
  await page.locator("[data-world-canvas-wrap]").evaluate((wrap) => {
    wrap.style.height = `${Math.max(
      240,
      Math.round(wrap.getBoundingClientRect().height - 120),
    )}px`;
  });
  await page.waitForTimeout(150);
  expect(
    await page
      .locator(".world-canvas")
      .evaluate((canvas) => ({ width: canvas.width, height: canvas.height })),
  ).toEqual(bufferBeforeResize);

  await client.send("Input.dispatchTouchEvent", {
    type: "touchEnd",
    touchPoints: [],
  });
  await expect.poll(() =>
    page.locator(".world-canvas").evaluate((canvas) => canvas.height),
  ).not.toBe(bufferBeforeResize.height);
  await client.detach();
  await context.close();
});

test("one-finger look and two-finger pinch use distinct bounded gestures", async ({
  browser,
}) => {
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
  });
  const page = await context.newPage();
  await prepareWorldPage(page, "mobile-full-range-pinch");
  await waitForWorld(page);

  const canvas = page.locator("[data-world-canvas-wrap] canvas");
  const box = await canvas.boundingBox();
  expect(box).not.toBeNull();
  const centreX = Math.round(box.x + box.width / 2);
  const centreY = Math.round(box.y + box.height * 0.62);
  const client = await page.context().newCDPSession(page);
  const beforeLook = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getCameraState(),
  );
  await client.send("Input.dispatchTouchEvent", {
    type: "touchStart",
    touchPoints: [{ x: centreX, y: centreY, id: 7 }],
  });
  await client.send("Input.dispatchTouchEvent", {
    type: "touchMove",
    touchPoints: [{ x: centreX + 80, y: centreY + 30, id: 7 }],
  });
  await client.send("Input.dispatchTouchEvent", {
    type: "touchEnd",
    touchPoints: [],
  });
  const afterLook = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getCameraState(),
  );
  expect(afterLook.yaw).toBeLessThan(beforeLook.yaw);
  expect(afterLook.pitch).toBeGreaterThan(beforeLook.pitch);
  expect(afterLook.zoom).toBeCloseTo(beforeLook.zoom, 6);

  const points = (spread) => [
    { x: centreX - spread, y: centreY, id: 1 },
    { x: centreX + spread, y: centreY, id: 2 },
  ];

  await client.send("Input.dispatchTouchEvent", {
    type: "touchStart",
    touchPoints: points(10),
  });
  await client.send("Input.dispatchTouchEvent", {
    type: "touchMove",
    touchPoints: points(185),
  });
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.getCameraState(),
    ),
  ).toMatchObject({ zoom: 0.06, minZoom: 0.06 });

  await client.send("Input.dispatchTouchEvent", {
    type: "touchMove",
    touchPoints: points(0.2),
  });
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate(
      (shell) => shell.world.getCameraState(),
    ),
  ).toMatchObject({ zoom: 28, maxZoom: 28 });

  await client.send("Input.dispatchTouchEvent", {
    type: "touchEnd",
    touchPoints: [],
  });
  await client.detach();
  await context.close();
});

test("construction markers distinguish verified live landmarks from unavailable ones", async ({
  page,
}) => {
  await prepareWorldPage(page, "construction-truth", {
    repositoryFixture: {},
  });
  await waitForWorld(page);
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.openLandmark("repositories");
  });
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return (
      shell?.repositoryCatalogState === "ready" &&
      shell?.landmarkCapabilities?.repositories?.live === true
    );
  });
  const detail = page.locator("[data-world-detail]");
  await expect(detail).toBeVisible();
  await expect(detail.getByRole("heading", { name: "Repository portals" }))
    .toBeVisible();
  await expect(
    detail.locator(
      '[data-world-construction-marker="repositories"]',
    ),
  ).toBeHidden();
});

test("failed live checks fail closed to construction without blocking navigation", async ({
  page,
}) => {
  await prepareWorldPage(page, "construction-unavailable", {
    unavailablePaths: [
      "/api/repo/forkmesh/forkmesh/mirrors",
      "/api/repositories",
    ],
  });
  await waitForWorld(page);
  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.openLandmark("repositories");
  });
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return (
      shell?.repositoryCatalogState === "unavailable" &&
      shell?.landmarkCapabilities?.repositories?.live === false
    );
  });
  await expect(
    page.getByRole("heading", { name: "Repository portals" }),
  ).toBeVisible();
  await expect(
    page.locator(
      '[data-world-detail] [data-world-construction-marker="repositories"]',
    ),
  ).toBeVisible();
});

test("approved instances stay truthful", async ({
  page,
}) => {
  await prepareWorldPage(page, "truthful-panels");
  await waitForWorld(page);

  const instances = await page.locator("forkmesh-world").evaluate((shell) => {
    return shell.federatedInstances.map((instance) => ({
      ...instance,
      hasToken: Object.hasOwn(instance, "token"),
      hasFederationPublicKey: Object.hasOwn(
        instance,
        "federationPublicKey",
      ),
    }));
  });
  expect(instances).toEqual([
    {
      id: "0123456789abcdef01234567",
      label: "North relay",
      origin: "https://north.example/",
      approved: true,
      joinedAt: 0,
      health: "online",
      online: true,
      healthEvidence: "fresh-verified-node-health",
      hasToken: false,
      hasFederationPublicKey: false,
    },
    {
      id: "89abcdef0123456789abcdef",
      label: "Garden relay",
      origin: "https://garden.example/",
      approved: true,
      joinedAt: 0,
      health: "awaiting_verified_health",
      online: false,
      healthEvidence: "no-fresh-verified-node-health",
      hasToken: false,
      hasFederationPublicKey: false,
    },
  ]);
});

test("System Capacity fits one height-scaled bar per populated D1 table", async ({
  page,
}) => {
  test.setTimeout(60_000);
  await prepareWorldPage(page, "system-capacity-bars", {
    session: {
      sessionToken: "playwright-admin-session",
      nodeName: "root",
    },
    systemCapacityTables: [
      { name: "users", rowCount: 2 },
      { name: "repositories", rowCount: 27 },
      { name: "world_events", rowCount: 4096 },
    ],
    systemCapacityD1Storage: {
      bytes: 14_811_136,
      freeDatabaseLimitBytes: 500_000_000,
      paidDatabaseLimitBytes: 10_000_000_000,
      includedAccountStorageBytes: 5_000_000_000,
    },
  });
  await waitForWorld(page);

  const capacity = await page.locator("forkmesh-world").evaluate((shell) => {
    const platform = shell.world.scene.getObjectByName(
      "system-capacity-infrastructure",
    );
    const tableLayer = shell.world.scene.getObjectByName(
      "system-capacity-database-tables",
    );
    const d1Storage = shell.world.scene.getObjectByName(
      "system-capacity-d1-storage",
    );
    const bars = [];
    tableLayer?.traverse((object) => {
      if (String(object.name || "").startsWith("system-capacity-table:")) {
        bars.push({
          name: object.userData.tableName,
          rowCount: object.userData.rowCount,
          height: object.scale.y,
          x: object.position.x,
          z: object.position.z,
        });
      }
    });
    return {
      platformName: platform?.name || "",
      visibleTableCount: platform?.userData.visibleTableCount,
      floor: platform?.userData.officeFloorId || "",
      d1StorageVisible: Boolean(d1Storage),
      bars,
    };
  });

  expect(capacity.platformName).toBe("system-capacity-infrastructure");
  expect(capacity.visibleTableCount).toBe(3);
  expect(capacity.floor).toBe("engineering");
  expect(capacity.d1StorageVisible).toBe(true);
  expect(capacity.bars.map((bar) => bar.name).sort()).toEqual([
    "repositories",
    "users",
    "world_events",
  ]);
  const byRows = [...capacity.bars].sort(
    (left, right) => left.rowCount - right.rowCount,
  );
  expect(byRows[0].height).toBeLessThan(byRows[1].height);
  expect(byRows[1].height).toBeLessThan(byRows[2].height);
  for (const bar of capacity.bars) {
    expect(Math.abs(bar.x)).toBeLessThanOrEqual(5.75);
    expect(bar.z).toBeGreaterThanOrEqual(-3.45);
    expect(bar.z).toBeLessThanOrEqual(3.35);
  }
});

test("a stale offline alias cannot erase the live mirrors' flagship pin", async ({
  page,
}) => {
  test.setTimeout(60_000);
  await prepareWorldPage(page, "world-stale-offline-alias", {
    repositoryFixture: { staleOfflineAlias: true },
  });
  await waitForWorld(page);
  await enterRepositoryDistrict(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return shell?.repositoryMapState === "ready";
  });

  const snapshot = await page.locator("forkmesh-world").evaluate((shell) => {
    const alias = shell.repositories.find(
      (record) =>
        record.owner === "forkmesh" &&
        record.name === "forkmesh" &&
        record.source === "organization-alias",
    );
    return {
      alias: alias
        ? {
            commit: alias.commit,
            stateHash: alias.stateHash,
            servingOwner: alias.servingOwner,
            mirrorAliases: alias.mirrorAliases,
          }
        : null,
      mapState: shell.repositoryMapState,
      activeCommit: shell.activeRepository?.commit || "",
    };
  });
  expect(snapshot).toEqual({
    alias: {
      commit: "a".repeat(40),
      stateHash: "c".repeat(64),
      servingOwner: "mirror2",
      mirrorAliases: [
        { owner: "jett", name: "forkmesh" },
        { owner: "mirror2", name: "forkmesh" },
        { owner: "mirror3", name: "forkmesh" },
      ],
    },
    mapState: "ready",
    activeCommit: "a".repeat(40),
  });
});

test("the flagship pin keeps user ownership separate from source node identity", async ({
  page,
}) => {
  test.setTimeout(60_000);
  await prepareWorldPage(page, "world-source-user-node-identity", {
    repositoryFixture: { sourceUserOwner: true },
  });
  await waitForWorld(page);
  await enterRepositoryDistrict(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return shell?.repositoryMapState === "ready";
  });

  const snapshot = await page.locator("forkmesh-world").evaluate((shell) => {
    const alias = shell.repositories.find(
      (record) =>
        record.owner === "forkmesh" &&
        record.name === "forkmesh" &&
        record.source === "organization-alias",
    );
    return {
      aliasCommit: alias?.commit || "",
      servingOwner: alias?.servingOwner || "",
      activeCommit: shell.activeRepository?.commit || "",
    };
  });
  expect(snapshot).toEqual({
    aliasCommit: "a".repeat(40),
    servingOwner: "jett",
    activeCommit: "a".repeat(40),
  });
});

test("repository portals and the 3D size sunburst use the verified catalog tree", async ({
  page,
}) => {
  test.setTimeout(60_000);
  const sourceReads = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    const paths = url.searchParams.getAll("path");
    if (
      url.pathname === "/api/repo/forkmesh/forkmesh/blobs" &&
      url.searchParams.get("ref") === "a".repeat(40) &&
      paths.length === 1 &&
      paths[0] === "src/[id]+C++.tsx"
    ) {
      sourceReads.push({
        pathname: url.pathname,
        ref: url.searchParams.get("ref"),
        paths,
      });
    }
  });
  await prepareWorldPage(page, "world-repository-size-rings", {
    repositoryFixture: { staleOfflineAlias: true },
  });
  await waitForWorld(page);
  await enterRepositoryDistrict(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return shell?.repositoryMapState === "ready";
  });

  const initial = await page.locator("forkmesh-world").evaluate((shell) => {
    const scene = shell.world.scene;
    const segments = [];
    const portals = [];
    const sizeLayer = scene.getObjectByName("repository-3d-size-map");
    scene.traverse((object) => {
      if (String(object.name || "").startsWith("repository-portal:")) {
        portals.push(object.name);
      }
      if (String(object.name || "").startsWith("repository-size-segment-")) {
        segments.push(object.userData.repositorySizeNode);
      }
    });
    return {
      repositories: shell.repositories.map(
        (record) => `${record.owner}/${record.name}`,
      ),
      portals,
      segments,
      portalUuid: sizeLayer?.parent?.uuid,
      sizeMount: sizeLayer?.parent?.name,
      sizeMountRadius: sizeLayer?.parent
        ? Math.hypot(
            sizeLayer.parent.position.x - 130,
            sizeLayer.parent.position.z,
          )
        : 0,
      legacyVisible:
        scene.getObjectByName("repository-legacy-file-graph")?.visible,
      relationshipsVisible:
        scene.getObjectByName("repository-entity-layer")?.visible,
      hasPerimeter:
        Boolean(scene.getObjectByName("repository-size-map-perimeter")),
    };
  });
  expect(initial.portals).toEqual(["repository-portal:forkmesh/forkmesh"]);
  expect(initial.portals).toHaveLength(initial.repositories.length);
  expect(initial.sizeMount).toBe("repository-portal:forkmesh/forkmesh");
  expect(initial.sizeMountRadius).toBeCloseTo(31, 5);
  expect(initial.legacyVisible).toBe(false);
  expect(initial.relationshipsVisible).toBe(false);
  expect(initial.hasPerimeter).toBe(false);
  expect(
    initial.segments.map(({ path, type, size }) => ({ path, type, size })),
  ).toEqual([
    { path: "src", type: "directory", size: 4096 },
    { path: "src/[id]+C++.tsx", type: "file", size: 4096 },
    { path: "README.md", type: "file", size: 1200 },
  ]);
  expect(initial.segments[0].percent).toBeCloseTo((4096 / 5296) * 100, 4);

  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.selectRepositorySizeNode({
      owner: "forkmesh",
      name: "forkmesh",
      type: "directory",
      path: "src",
    });
  });
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return shell?.activeRepository?.path === "src";
  });
  const focused = await page.locator("forkmesh-world").evaluate((shell) => {
    const layer = shell.world.scene.getObjectByName("repository-3d-size-map");
    const hub = layer?.getObjectByName("repository-size-map-hub");
    const segment = layer?.getObjectByName("repository-size-segment-0");
    return {
      focus: layer?.userData?.repositorySizeFocus,
      portalUuid: layer?.parent?.uuid,
      hubTarget: hub?.userData?.repositorySizeNode?.targetPath,
      segment: segment?.userData?.repositorySizeNode,
      camera: shell.world.getCameraState(),
    };
  });
  expect(focused.focus).toBe("src");
  expect(focused.portalUuid).toBe(initial.portalUuid);
  expect(focused.hubTarget).toBe("");
  expect(focused.segment).toMatchObject({
    path: "src/[id]+C++.tsx",
    type: "file",
    size: 4096,
    percent: 100,
  });
  expect(focused.camera.yaw).toBeCloseTo(-Math.PI, 5);
  expect(focused.camera.pitch).toBeCloseTo(0.16, 5);
  expect(focused.camera.zoom).toBeLessThanOrEqual(0.55);

  const injectedPortals = await page
    .locator("forkmesh-world")
    .evaluate((shell) => {
      shell.world.updateRepositoryCatalog(
        Array.from({ length: 200 }, (_, index) => ({
          owner: "orbit",
          name: `repo-${index + 1}`,
          liveHost: index % 2 === 0,
          sizeBytes: (index + 1) * 1024,
        })),
        {},
      );
      const portals = [];
      shell.world.scene.traverse((object) => {
        if (String(object.name || "").startsWith("repository-portal:")) {
          portals.push({
            name: object.name,
            radius: Math.hypot(object.position.x - 130, object.position.z),
            faceScale: object.userData.repositoryFace?.scale.x,
            labelVisible: object.userData.repositoryLabel?.visible,
          });
        }
      });
      return portals;
    });
  expect(injectedPortals).toHaveLength(200);
  expect(injectedPortals.map(({ name }) => name)).toContain(
    "repository-portal:orbit/repo-1",
  );
  expect(injectedPortals.map(({ name }) => name)).toContain(
    "repository-portal:orbit/repo-200",
  );
  injectedPortals.forEach(({ radius }) => expect(radius).toBeCloseTo(31, 5));
  injectedPortals.forEach(({ faceScale }) =>
    expect(faceScale).toBeLessThan(0.7),
  );
  expect(injectedPortals.filter(({ labelVisible }) => labelVisible)).toHaveLength(
    200,
  );

  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.selectRepositorySizeNode({
      owner: "forkmesh",
      name: "forkmesh",
      type: "file",
      path: "src/[id]+C++.tsx",
    });
  });
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return (
      Boolean(shell?.repositoryFile?.state) &&
      shell.repositoryFile.state !== "loading"
    );
  });
  expect(
    await page.locator("forkmesh-world").evaluate((shell) => ({
      activeCommit: shell.activeRepository?.commit || "",
      view: shell.repositoryView,
      file: {
        path: shell.repositoryFile?.path || "",
        state: shell.repositoryFile?.state || "",
        kind: shell.repositoryFile?.kind || "",
        text: shell.repositoryFile?.text || "",
      },
    })),
  ).toEqual({
    activeCommit: "a".repeat(40),
    view: "file",
    file: {
      path: "src/[id]+C++.tsx",
      state: "ready",
      kind: "text",
      text: 'export const routeName = "[id]+C++";\n',
    },
  });
  expect(sourceReads).toEqual([
    {
      pathname: "/api/repo/forkmesh/forkmesh/blobs",
      ref: "a".repeat(40),
      paths: ["src/[id]+C++.tsx"],
    },
  ]);
});

test("disagreeing eligible mirrors still show an authoritative tree-first flagship map", async ({
  page,
}) => {
  const repositoryReads = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (url.pathname.startsWith("/api/repo/forkmesh/forkmesh/")) {
      repositoryReads.push(url.pathname);
    }
  });
  await prepareWorldPage(page, "world-conflicting-live-alias", {
    repositoryFixture: { conflictingHealthyAlias: true },
  });
  await waitForWorld(page);
  await enterRepositoryDistrict(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return shell?.repositoryMapState === "ready";
  });

  const snapshot = await page.locator("forkmesh-world").evaluate((shell) => {
    const alias = shell.repositories.find(
      (record) =>
        record.owner === "forkmesh" &&
        record.name === "forkmesh" &&
        record.source === "organization-alias",
    );
    return {
      commit: alias?.commit || "",
      stateHash: alias?.stateHash || "",
      activeCommit: shell.activeRepository?.commit || "",
    };
  });
  expect(snapshot).toEqual({
    commit: "",
    stateHash: "",
    activeCommit: "a".repeat(40),
  });
  expect(
    repositoryReads.filter((path) => path.endsWith("/tree")),
  ).not.toHaveLength(0);
});

test("the flagship portal opens from the authoritative tree while mirrors converge", async ({
  page,
}) => {
  const repositoryFixture = { conflictingHealthyAlias: true };
  await prepareWorldPage(page, "world-late-flagship-pin", {
    repositoryFixture,
  });
  await waitForWorld(page);
  await enterRepositoryDistrict(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return shell?.repositoryMapState === "ready";
  });

  const opened = await page.locator("forkmesh-world").evaluate((shell) => {
    const sizeLayer = shell.world.scene.getObjectByName(
      "repository-3d-size-map",
    );
    return {
      active: `${shell.activeRepository?.owner}/${shell.activeRepository?.repo}`,
      manualSelection: shell.repositoryManualSelection,
      mount: sizeLayer?.parent?.name || "",
      faceHidden:
        sizeLayer?.parent?.userData?.repositoryFace?.visible === false,
    };
  });
  expect(opened).toEqual({
    active: "forkmesh/forkmesh",
    manualSelection: "",
    mount: "repository-portal:forkmesh/forkmesh",
    faceHidden: true,
  });
});

test("one complete healthy-mirror attestation can pin the flagship map", async ({
  page,
}) => {
  const repositoryReads = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (url.pathname.startsWith("/api/repo/forkmesh/forkmesh/")) {
      repositoryReads.push(url.pathname);
    }
  });
  await prepareWorldPage(page, "world-incomplete-live-alias", {
    repositoryFixture: { missingHealthyStateHash: true },
  });
  await waitForWorld(page);
  await enterRepositoryDistrict(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return shell?.repositoryMapState === "ready";
  });

  const snapshot = await page.locator("forkmesh-world").evaluate((shell) => {
    const alias = shell.repositories.find(
      (record) =>
        record.owner === "forkmesh" &&
        record.name === "forkmesh" &&
        record.source === "organization-alias",
    );
    return {
      commit: alias?.commit || "",
      stateHash: alias?.stateHash || "",
      activeCommit: shell.activeRepository?.commit || "",
    };
  });
  expect(snapshot).toEqual({
    commit: "a".repeat(40),
    stateHash: "",
    activeCommit: "a".repeat(40),
  });
  expect(
    repositoryReads.filter((path) => path.endsWith("/tree")),
  ).not.toHaveLength(0);
});

test("repository stars toggle in-place through the exact organization alias", async ({
  page,
  context,
}) => {
  const starFixture = { count: 7, starred: false, requests: [] };
  await prepareWorldPage(page, "world-repository-star", {
    session: {
      nodeName: "star-tester",
      sessionToken: "star-session-token",
    },
    repositoryFixture: {},
    repositoryStarFixture: starFixture,
  });
  await openWorldRepositoryExplorer(page);

  const originalURL = page.url();
  const originalPages = context.pages().length;
  const star = page.locator(
    '[data-world-repo-star][data-world-repo-key="forkmesh/forkmesh"]',
  );
  await expect(star).toBeVisible();
  await expect(star).toHaveAttribute("aria-pressed", "false");
  await expect(star.locator("[data-world-repo-star-count]")).toHaveText("7");

  await star.click();
  await expect(star).toHaveAttribute("aria-pressed", "true");
  await expect(star.locator("[data-world-repo-star-count]")).toHaveText("8");

  await star.click();
  await expect(star).toHaveAttribute("aria-pressed", "false");
  await expect(star.locator("[data-world-repo-star-count]")).toHaveText("7");

  expect(page.url()).toBe(originalURL);
  expect(context.pages()).toHaveLength(originalPages);
  expect(
    starFixture.requests.filter((request) => request.method === "GET").length,
  ).toBeGreaterThanOrEqual(1);
  expect(
    starFixture.requests
      .filter((request) => request.method !== "GET")
      .map((request) => ({
        path: request.path,
        method: request.method,
        authorization: request.authorization,
        body: request.body,
      })),
  ).toEqual([
    {
      path: "/api/repo/forkmesh/forkmesh/star",
      method: "POST",
      authorization: "Bearer star-session-token",
      body: {},
    },
    {
      path: "/api/repo/forkmesh/forkmesh/star",
      method: "DELETE",
      authorization: "Bearer star-session-token",
      body: {},
    },
  ]);
  expect(
    starFixture.requests.every(
      (request) =>
        request.path === "/api/repo/forkmesh/forkmesh/star",
    ),
  ).toBe(true);
});

test("signed-out repository star opens the in-world account panel without mutation", async ({
  page,
  context,
}) => {
  const starFixture = { count: 11, starred: false, requests: [] };
  await prepareWorldPage(page, "world-repository-star-guest", {
    repositoryFixture: {},
    repositoryStarFixture: starFixture,
  });
  await openWorldRepositoryExplorer(page);

  const originalURL = page.url();
  const originalPages = context.pages().length;
  const star = page.locator(
    '[data-world-repo-star][data-world-repo-key="forkmesh/forkmesh"]',
  );
  await expect(star).toHaveAttribute("aria-pressed", "false");
  await expect(star.locator("[data-world-repo-star-count]")).toHaveText("11");
  await star.click();

  const account = page.locator("[data-world-account-panel]");
  await expect(account).toHaveAttribute("data-open", "true");
  await expect(account).toHaveAttribute("aria-hidden", "false");
  expect(
    starFixture.requests.filter((request) =>
      ["POST", "DELETE"].includes(request.method),
    ),
  ).toHaveLength(0);
  expect(
    starFixture.requests.filter((request) => request.method === "GET").length,
  ).toBeGreaterThanOrEqual(1);
  expect(page.url()).toBe(originalURL);
  expect(context.pages()).toHaveLength(originalPages);
});

test("the repository circle carries the stored star total and its fediverse followers", async ({
  page,
}) => {
  const starFixture = { count: 1284, starred: false, requests: [] };
  const followerFixture = {
    followers: 9,
    requests: [],
    followersList: [
      {
        handle: "@kate@mastodon.social",
        url: "https://mastodon.social/users/kate",
        name: "Kate Mirrors",
        avatarUrl: "https://files.mastodon.social/kate.png",
        about: "Rust, embedded, and self-hosted git.",
        instance: "mastodon.social",
        profileUrl: "https://mastodon.social/@kate",
        followedAt: 1_767_225_600_000,
      },
      {
        handle: "@sam@fosstodon.org",
        url: "https://fosstodon.org/users/sam",
        // No cached actor document yet: name/avatar/bio are still empty.
        instance: "fosstodon.org",
        profileUrl: "https://fosstodon.org/users/sam",
        followedAt: 1_767_139_200_000,
      },
      // Hostile rows: a non-https avatar and an internal host are dropped
      // before anything reaches a texture loader.
      {
        handle: "@mallory@evil.test",
        url: "https://evil.test/users/mallory",
        avatarUrl: "javascript:alert(1)",
        profileUrl: "https://evil.test/users/mallory",
      },
    ],
  };
  await prepareWorldPage(page, "world-repository-followers", {
    repositoryFixture: {},
    repositoryStarFixture: starFixture,
    repositoryFollowerFixture: followerFixture,
  });
  await waitForWorld(page);
  await enterRepositoryDistrict(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return (
      shell?.repositoryFollowerStates?.get("forkmesh/forkmesh")?.status ===
      "ready"
    );
  });
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return Boolean(
      shell?.world?.scene?.getObjectByName(
        "repository-star-button:forkmesh/forkmesh",
      )?.userData?.repositoryStar?.starCount,
    );
  });

  const portal = await page.locator("forkmesh-world").evaluate((shell) => {
    const scene = shell.world.scene;
    const starButton = scene.getObjectByName(
      "repository-star-button:forkmesh/forkmesh",
    );
    const gallery = scene.getObjectByName(
      "repository-fediverse-follower-icons:forkmesh/forkmesh",
    );
    const icons = [];
    gallery?.children.forEach((child) => {
      if (String(child.name || "").startsWith("repository-follower-icon:")) {
        icons.push({
          name: child.name,
          follower: child.userData.repositoryFollower,
        });
      }
    });
    return {
      starCountFromDatabase: starButton?.userData?.repositoryStar?.starCount,
      starCaptionPresent: Boolean(
        scene.getObjectByName("repository-star-caption:forkmesh/forkmesh"),
      ),
      followerCount: shell.repositoryFollowerStates.get("forkmesh/forkmesh")
        ?.count,
      icons,
      captionPresent: Boolean(
        gallery?.getObjectByName?.(
          "repository-fediverse-follower-caption",
        ),
      ),
    };
  });

  // The star on top of the circle reports the relay's stored repo_stars total.
  expect(portal.starCountFromDatabase).toBe(1284);
  expect(portal.starCaptionPresent).toBe(true);
  // The caption uses the authoritative total, not the capped list length.
  expect(portal.followerCount).toBe(9);
  expect(portal.captionPresent).toBe(true);
  expect(portal.icons.map(({ name }) => name)).toEqual([
    "repository-follower-icon:@kate@mastodon.social",
    "repository-follower-icon:@sam@fosstodon.org",
    "repository-follower-icon:@mallory@evil.test",
  ]);
  expect(portal.icons[0].follower).toMatchObject({
    handle: "@kate@mastodon.social",
    name: "Kate Mirrors",
    about: "Rust, embedded, and self-hosted git.",
    instance: "mastodon.social",
    avatar: "https://files.mastodon.social/kate.png",
    profileUrl: "https://mastodon.social/@kate",
  });
  expect(portal.icons[1].follower).toMatchObject({
    handle: "@sam@fosstodon.org",
    name: "",
    about: "",
    avatar: "",
  });
  expect(portal.icons[2].follower.avatar).toBe("");
  expect(
    followerFixture.requests.filter((request) => request.method === "GET")
      .length,
  ).toBeGreaterThanOrEqual(1);
});

test("camera toggle enters first-person and restores the local player", async ({
  page,
}) => {
  await prepareWorldPage(page, "world-first-person-toggle");
  await waitForWorld(page);
  await pauseWorld(page);
  await expandWorldRightRail(page);

  const toggle = page.locator("[data-world-camera-toggle]");
  await expect(toggle).toBeVisible();
  await expect(toggle).toHaveAttribute("aria-pressed", "false");
  await expect(toggle).toHaveAttribute(
    "aria-label",
    "Enter first-person view",
  );
  await expect(toggle.locator("[data-world-camera-label]")).toHaveText(
    "First person",
  );

  await toggle.click();
  await expect(toggle).toHaveAttribute("aria-pressed", "true");
  await expect(toggle).toHaveAttribute(
    "aria-label",
    "Exit first-person view",
  );
  await expect(toggle.locator("[data-world-camera-label]")).toHaveText(
    "Third person",
  );
  expect(
    await page.locator("forkmesh-world").evaluate((shell) => ({
      camera: shell.world.getCameraState(),
      playerVisible: shell.world.player.visible,
      canvasMode: shell.world.renderer.domElement.dataset.cameraMode,
    })),
  ).toMatchObject({
    camera: { mode: "first-person", firstPerson: true },
    playerVisible: false,
    canvasMode: "first-person",
  });

  await toggle.click();
  await expect(toggle).toHaveAttribute("aria-pressed", "false");
  await expect(toggle).toHaveAttribute(
    "aria-label",
    "Enter first-person view",
  );
  await expect(toggle.locator("[data-world-camera-label]")).toHaveText(
    "First person",
  );
  expect(
    await page.locator("forkmesh-world").evaluate((shell) => ({
      camera: shell.world.getCameraState(),
      playerVisible: shell.world.player.visible,
      canvasMode: shell.world.renderer.domElement.dataset.cameraMode,
    })),
  ).toMatchObject({
    camera: { mode: "third-person", firstPerson: false },
    playerVisible: true,
    canvasMode: "third-person",
  });
});

test("visiting the repository sunburst frames it in third-person", async ({
  page,
}) => {
  await prepareWorldPage(page, "world-repository-third-person", {
    repositoryFixture: {},
  });
  await openWorldRepositoryExplorer(page);

  await page.locator("[data-world-repo-scene]").click();
  await expandWorldRightRail(page);
  const toggle = page.locator("[data-world-camera-toggle]");
  await expect(toggle).toHaveAttribute("aria-pressed", "false");
  await expect(toggle).toHaveAttribute(
    "aria-label",
    "Enter first-person view",
  );
  const framed = await page.locator("forkmesh-world").evaluate((shell) => ({
    camera: shell.world.getCameraState(),
    playerVisible: shell.world.player.visible,
    canvasMode: shell.world.renderer.domElement.dataset.cameraMode,
  }));
  expect(framed).toMatchObject({
    camera: { mode: "third-person", firstPerson: false },
    playerVisible: true,
    canvasMode: "third-person",
  });
  expect(framed.camera.zoom).toBeLessThanOrEqual(0.55);

  // First person stays available, just never automatic.
  await toggle.click();
  await expect(toggle).toHaveAttribute("aria-pressed", "true");
  expect(
    await page.locator("forkmesh-world").evaluate((shell) => ({
      mode: shell.world.getCameraState().mode,
      playerVisible: shell.world.player.visible,
    })),
  ).toEqual({ mode: "first-person", playerVisible: false });
});

test("zooming first-person all the way back restores third-person", async ({
  page,
}) => {
  await prepareWorldPage(page, "world-first-person-zoom-out");
  await waitForWorld(page);
  await pauseWorld(page);
  await expandWorldRightRail(page);

  const toggle = page.locator("[data-world-camera-toggle]");
  await toggle.click();
  await expect(toggle).toHaveAttribute("aria-pressed", "true");

  const canvas = page.locator("[data-world-canvas-wrap] canvas");
  const box = await canvas.boundingBox();
  expect(box).not.toBeNull();
  const centre = {
    x: Math.round(box.x + box.width / 2),
    y: Math.round(box.y + box.height / 2),
  };
  await page.mouse.move(centre.x, centre.y);

  // Widening the eyes stays in first person right up to the zoom-out floor.
  await page.mouse.wheel(0, 480);
  await expect
    .poll(() =>
      page
        .locator("forkmesh-world")
        .evaluate((shell) => shell.world.getCameraState().mode),
    )
    .toBe("first-person");
  await page.mouse.wheel(0, 480);
  const floored = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getCameraState(),
  );
  expect(floored.mode).toBe("first-person");
  expect(floored.zoom).toBeCloseTo(floored.minZoom, 5);

  // One more notch back steps out of the avatar's head entirely.
  await page.mouse.wheel(0, 480);
  await expect(toggle).toHaveAttribute("aria-pressed", "false");
  await expect(toggle).toHaveAttribute(
    "aria-label",
    "Enter first-person view",
  );
  expect(
    await page.locator("forkmesh-world").evaluate((shell) => ({
      mode: shell.world.getCameraState().mode,
      playerVisible: shell.world.player.visible,
      canvasMode: shell.world.renderer.domElement.dataset.cameraMode,
      fov: shell.world.camera.fov,
    })),
  ).toEqual({
    mode: "third-person",
    playerVisible: true,
    canvasMode: "third-person",
    fov: 44,
  });
});

test("pull requests open and become viewed entirely inside the repository World", async ({
  page,
  context,
}) => {
  test.slow();
  const repositoryRequests = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (url.pathname.startsWith("/api/repo/forkmesh/forkmesh/")) {
      repositoryRequests.push(url);
    }
  });
  await prepareWorldPage(page, "world-pull-review", {
    repositoryFixture: {},
  });
  const originalPages = context.pages().length;
  const review = await openWorldPullReview(page);
  const originalURL = page.url();
  await expect(
    page.getByRole("heading", { name: "forkmesh/forkmesh #44" }),
  ).toBeVisible();
  await expect(
    review.locator("[data-repo-record-detail='pulls']"),
  ).toBeVisible();
  await expect(review.locator("[data-repo-record-hero]")).toContainText(
    "Review inside the World",
  );
  await expect(
    page.locator(
      "[data-world-repository-web-workbench='pull'] iframe",
    ),
  ).toHaveAttribute("src", "/forkmesh/forkmesh/pull/44");
  await review.getByRole("button", { name: /Files changed/ }).click();
  await expect(review.locator("[data-repo-pull-file]")).toHaveCount(2);
  await expect(review.locator("[data-repo-pull-diff-file]")).toHaveCount(2);
  await expect(review.locator("[data-repo-pull-diff-list]")).toContainText(
    "world-pr-diff-visible",
  );
  await expect(review.locator("[data-repo-pull-viewed-summary]")).toHaveText(
    "0 of 2 viewed",
  );
  expect(page.url()).toBe(originalURL);
  expect(context.pages()).toHaveLength(originalPages);
  const viewedButtons = review.locator("[data-repo-pull-viewed]");
  await viewedButtons.nth(0).click();
  await viewedButtons.nth(1).click();
  await expect(review.locator("[data-repo-pull-viewed-summary]")).toHaveText(
    "2 of 2 viewed",
  );
  await expect(
    review.locator("[data-repo-pull-diff-file][data-viewed='true']"),
  ).toHaveCount(2);
  const pullReads = repositoryRequests.filter(
    (url) =>
      (url.pathname.endsWith("/tree") &&
        url.searchParams.get("path") === ".forkmesh/pulls") ||
      (url.pathname.endsWith("/blobs") &&
        url.searchParams
          .getAll("path")
          .some((item) => item.startsWith(".forkmesh/pulls/"))),
  );
  expect(pullReads.length).toBeGreaterThanOrEqual(3);
  for (const request of pullReads) {
    expect(request.searchParams.get("ref")).toBe("b".repeat(40));
    expect(request.searchParams.get("ref")).not.toBe("main");
    expect(request.searchParams.get("ref")).not.toBe("a".repeat(40));
  }
  expect(
    repositoryRequests.filter((url) => url.pathname.endsWith("/branches")),
  ).toHaveLength(2);
});

test("a fresh map resolves exact pull metadata before slow issue scans", async ({
  page,
}) => {
  test.slow();
  const repositoryRequests = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (url.pathname.startsWith("/api/repo/forkmesh/forkmesh/")) {
      repositoryRequests.push(url);
    }
  });
  await prepareWorldPage(page, "world-pull-priority", {
    repositoryFixture: {
      issueTreeDelayMs: 900,
      // Model a small mirror whose pull-metadata read would be rejected once
      // lower-priority issue fanout has occupied its request capacity.
      rejectPullMetadataAfterIssueFanout: true,
    },
  });
  await waitForWorld(page);
  await enterRepositoryDistrict(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return shell?.repositoryMapState === "ready" &&
      shell?.activeRepository?.pullCountSource === "metadata-tree";
  });

  const snapshot = await page.locator("forkmesh-world").evaluate((shell) => ({
    pullCount: shell.activeRepository?.pullCount,
    pullCountSource: shell.activeRepository?.pullCountSource,
    pullMetadataCommit:
      shell.activeRepository?.entityRecords?.pullMetadataCommit,
    pullCountExact: shell.activeRepository?.entityRecords?.pullCountExact,
    pullsAvailable: shell.activeRepository?.entityRecords?.pullsAvailable,
  }));
  expect(snapshot).toEqual({
    pullCount: 2,
    pullCountSource: "metadata-tree",
    pullMetadataCommit: "b".repeat(40),
    pullCountExact: true,
    pullsAvailable: true,
  });

  const branchesIndex = repositoryRequests.findIndex((url) =>
    url.pathname.endsWith("/branches"),
  );
  const pullTreeIndex = repositoryRequests.findIndex(
    (url) =>
      url.pathname.endsWith("/tree") &&
      url.searchParams.get("path") === ".forkmesh/pulls" &&
      url.searchParams.get("ref") === "b".repeat(40),
  );
  const pullBlobsIndex = repositoryRequests.findIndex(
    (url) =>
      url.pathname.endsWith("/blobs") &&
      url.searchParams.get("ref") === "b".repeat(40) &&
      url.searchParams
        .getAll("path")
        .some((path) => path.startsWith(".forkmesh/pulls/")),
  );
  const firstIssueIndex = repositoryRequests.findIndex(
    (url) =>
      url.pathname.endsWith("/tree") &&
      String(url.searchParams.get("path") || "").startsWith(
        ".forkmesh/issues",
      ),
  );
  expect(branchesIndex).toBeGreaterThanOrEqual(0);
  expect(pullTreeIndex).toBeGreaterThan(branchesIndex);
  expect(pullBlobsIndex).toBeGreaterThan(pullTreeIndex);
  expect(firstIssueIndex).toBeGreaterThan(branchesIndex);
  expect(firstIssueIndex).toBeGreaterThan(pullTreeIndex);
  expect(firstIssueIndex).toBeGreaterThan(pullBlobsIndex);
});

test("four fresh single-core contexts keep exact pull metadata ahead of issue scans", async ({
  browser,
}) => {
  test.setTimeout(180_000);
  for (let iteration = 0; iteration < 4; iteration += 1) {
    const context = await browser.newContext();
    const page = await context.newPage();
    const repositoryFixture = { singleCoreDelayMs: 60 };
    const repositoryRequests = [];
    page.on("request", (request) => {
      const url = new URL(request.url());
      if (url.pathname.startsWith("/api/repo/forkmesh/forkmesh/")) {
        repositoryRequests.push(url);
      }
    });
    await prepareWorldPage(page, `single-core-${iteration}`, {
      repositoryFixture,
    });
    await waitForWorld(page);
    await enterRepositoryDistrict(page);
    await page.waitForFunction(() => {
      const shell = document.querySelector("forkmesh-world");
      return shell?.repositoryMapState === "ready" &&
        shell?.activeRepository?.pullCountSource === "metadata-tree";
    });

    const snapshot = await page.locator("forkmesh-world").evaluate((shell) => ({
      commit: shell.activeRepository?.commit,
      pullCount: shell.activeRepository?.pullCount,
      pullCountSource: shell.activeRepository?.pullCountSource,
      pullMetadataCommit:
        shell.activeRepository?.entityRecords?.pullMetadataCommit,
      pullCountExact: shell.activeRepository?.entityRecords?.pullCountExact,
    }));
    expect(snapshot).toEqual({
      commit: "a".repeat(40),
      pullCount: 2,
      pullCountSource: "metadata-tree",
      pullMetadataCommit: "b".repeat(40),
      pullCountExact: true,
    });
    expect(repositoryFixture.maxMetadataActive).toBe(1);

    const branchesIndex = repositoryRequests.findIndex((url) =>
      url.pathname.endsWith("/branches"),
    );
    const pullTreeIndex = repositoryRequests.findIndex(
      (url) =>
        url.pathname.endsWith("/tree") &&
        url.searchParams.get("path") === ".forkmesh/pulls" &&
        url.searchParams.get("ref") === "b".repeat(40),
    );
    const pullBlobsIndex = repositoryRequests.findIndex(
      (url) =>
        url.pathname.endsWith("/blobs") &&
        url.searchParams.get("ref") === "b".repeat(40) &&
        url.searchParams
          .getAll("path")
          .every((path) =>
            /^\.forkmesh\/pulls\/(?:43|44)\/pull\.md$/.test(path),
          ),
    );
    const firstIssueIndex = repositoryRequests.findIndex(
      (url) =>
        url.pathname.endsWith("/tree") &&
        String(url.searchParams.get("path") || "").startsWith(
          ".forkmesh/issues",
        ),
    );
    expect(branchesIndex).toBeGreaterThanOrEqual(0);
    expect(pullTreeIndex).toBeGreaterThan(branchesIndex);
    expect(pullBlobsIndex).toBeGreaterThan(pullTreeIndex);
    expect(firstIssueIndex).toBeGreaterThan(pullBlobsIndex);
    expect(
      repositoryRequests.filter((url) => url.pathname.endsWith("/branches")),
    ).toHaveLength(1);
    expect(
      repositoryRequests.filter(
        (url) =>
          url.pathname.endsWith("/tree") &&
          url.searchParams.get("path") === ".forkmesh/pulls",
      ),
    ).toHaveLength(1);

    await context.close();
  }
});

test("an authenticated organization writer merges exact reviewed OIDs in-World", async ({
  page,
  context,
}) => {
  test.slow();
  const fixture = { mergeOutcome: "success" };
  const mergeRequests = [];
  const rootTreeRequests = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (
      url.pathname === "/api/repo/forkmesh/forkmesh/tree" &&
      (url.searchParams.get("path") || "") === ""
    ) {
      rootTreeRequests.push(url);
    }
    if (url.pathname.endsWith("/pulls/44/merge")) {
      mergeRequests.push({
        method: request.method(),
        authorization: request.headers().authorization || "",
        body: request.postDataJSON(),
      });
    }
  });
  await prepareWorldPage(page, "world-org-writer-merge", {
    session: {
      nodeName: "release-writer",
      sessionToken: "org-writer-session-token",
    },
    repositoryFixture: fixture,
    organizationViewerRole: "owner",
  });
  const review = await openWorldPullReview(page);
  const originalPages = context.pages().length;
  const originalRootReads = rootTreeRequests.length;
  const mergeButton = review.getByRole("button", {
    name: "Merge into mirror",
  });
  await expect(mergeButton).toBeVisible();
  await mergeButton.click();
  const mergePanel = review.locator("[data-repo-pull-merge-panel]");
  await expect(
    mergePanel,
  ).toContainText(
    "The selected mirror published the merge",
  );
  await expect(
    review.getByRole("button", { name: "Merged into mirror" }),
  ).toBeDisabled();

  expect(mergeRequests).toHaveLength(2);
  expect(mergeRequests.every((request) => request.method === "POST")).toBe(true);
  expect(
    mergeRequests.every(
      (request) =>
        request.authorization === "Bearer org-writer-session-token",
    ),
  ).toBe(true);
  expect(mergeRequests[0].body).toEqual(mergeRequests[1].body);
  expect(mergeRequests[0].body).toMatchObject({
    schemaVersion: 1,
    type: "forkmesh.pull-merge-v1",
    pullNumber: 44,
    expectedBaseOid: "a".repeat(40),
    expectedHeadOid: "e".repeat(40),
    expectedPullsOid: "b".repeat(40),
  });
  expect(mergeRequests[0].body.requestId).toMatch(
    /^[A-Za-z0-9_-]{12,80}$/,
  );
  expect(rootTreeRequests).toHaveLength(originalRootReads);
  expect(context.pages()).toHaveLength(originalPages);
  expect(new URL(page.url()).pathname).toMatch(/^\/world\/?$/);
});

test("an unauthenticated reviewer never receives a merge control", async ({
  page,
}) => {
  const mergeRequests = [];
  page.on("request", (request) => {
    if (new URL(request.url()).pathname.endsWith("/pulls/44/merge")) {
      mergeRequests.push(request);
    }
  });
  await prepareWorldPage(page, "world-unauthenticated-review", {
    repositoryFixture: {},
  });
  const review = await openWorldPullReview(page);
  await expect(review.locator("[data-repo-pull-merge]")).toHaveCount(0);
  await expect(
    review.locator("[data-repo-record-detail='pulls']"),
  ).toBeVisible();
  expect(mergeRequests).toHaveLength(0);
});

test("a forbidden merge is rendered safely and never reloads repository data", async ({
  page,
}) => {
  test.slow();
  const fixture = { mergeOutcome: "forbidden" };
  const mergeRequests = [];
  const rootTreeRequests = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (
      url.pathname === "/api/repo/forkmesh/forkmesh/tree" &&
      (url.searchParams.get("path") || "") === ""
    ) {
      rootTreeRequests.push(url);
    }
    if (url.pathname.endsWith("/pulls/44/merge")) mergeRequests.push(request);
  });
  await prepareWorldPage(page, "world-forbidden-merge", {
    session: {
      nodeName: "registered-reader",
      sessionToken: "reader-session-token",
    },
    repositoryFixture: fixture,
    organizationViewerRole: "owner",
  });
  const review = await openWorldPullReview(page);
  const rootReadsBeforeMerge = rootTreeRequests.length;
  await review
    .getByRole("button", { name: "Merge into mirror" })
    .click();
  const denied = review.locator("[data-repo-pull-merge-panel]");
  await expect(denied).toHaveAttribute("role", "status");
  await expect(denied).toContainText(
    "Only an organization owner can merge through a mirror",
  );
  await expect(
    review.getByRole("button", { name: "Retry mirror merge" }),
  ).toBeVisible();
  expect(mergeRequests).toHaveLength(1);
  expect(rootTreeRequests).toHaveLength(rootReadsBeforeMerge);
});

for (const scenario of [
  {
    outcome: "conflict",
  },
  {
    outcome: "stale",
  },
]) {
  test(`${scenario.outcome} merge result is explicit and does not reload`, async ({
    page,
  }) => {
    test.slow();
    const fixture = { mergeOutcome: scenario.outcome };
    const rootTreeRequests = [];
    page.on("request", (request) => {
      const url = new URL(request.url());
      if (
        url.pathname === "/api/repo/forkmesh/forkmesh/tree" &&
        (url.searchParams.get("path") || "") === ""
      ) {
        rootTreeRequests.push(url);
      }
    });
    await prepareWorldPage(page, `world-${scenario.outcome}-merge`, {
      session: {
        nodeName: "organization-writer",
        sessionToken: `${scenario.outcome}-session-token`,
      },
      repositoryFixture: fixture,
      organizationViewerRole: "owner",
    });
    const review = await openWorldPullReview(page);
    const rootReadsBeforeMerge = rootTreeRequests.length;
    await review
      .getByRole("button", { name: "Merge into mirror" })
      .click();
    const result = review.locator("[data-repo-pull-merge-panel]");
    await expect(result).toHaveAttribute("role", "status");
    await expect(result).toContainText(
      scenario.outcome === "conflict"
        ? "exact reviewed commits have a merge conflict"
        : "base branch changed",
    );
    expect(rootTreeRequests).toHaveLength(rootReadsBeforeMerge);
  });
}

test("bounded polling and a manual retry reuse one idempotency request", async ({
  page,
}) => {
  test.slow();
  const fixture = { mergeOutcome: "retry" };
  const mergeBodies = [];
  page.on("request", (request) => {
    if (new URL(request.url()).pathname.endsWith("/pulls/44/merge")) {
      mergeBodies.push(request.postDataJSON());
    }
  });
  await prepareWorldPage(page, "world-bounded-merge-poll", {
    session: {
      nodeName: "organization-writer",
      sessionToken: "bounded-poll-session-token",
    },
    repositoryFixture: fixture,
    organizationViewerRole: "owner",
  });
  const review = await openWorldPullReview(page);
  await review
    .getByRole("button", { name: "Merge into mirror" })
    .click();
  const mergePanel = review.locator("[data-repo-pull-merge-panel]");
  await expect(mergePanel).toContainText(
    "The mirror is still processing. Retry to check the same merge request.",
  );
  expect(mergeBodies).toHaveLength(8);
  await review
    .getByRole("button", { name: "Retry mirror merge" })
    .click();
  await expect(mergePanel).toContainText(
    "exact reviewed commits have a merge conflict",
  );
  expect(mergeBodies).toHaveLength(9);
  expect(new Set(mergeBodies.map((body) => body.requestId)).size).toBe(1);
  expect(mergeBodies.every((body) => body.type === "forkmesh.pull-merge-v1")).toBe(
    true,
  );
});

test("the protected merge control is touch-sized and announces state on mobile", async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await prepareWorldPage(page, "world-mobile-merge", {
    session: {
      nodeName: "mobile-writer",
      sessionToken: "mobile-writer-session-token",
    },
    repositoryFixture: {},
    organizationViewerRole: "owner",
  });
  const review = await openWorldPullReview(page);
  const panel = review.locator("[data-repo-pull-merge-panel]");
  const button = panel.getByRole("button", {
    name: "Merge into mirror",
  });
  await expect(panel).toHaveAttribute("role", "status");
  await expect(panel).toHaveAttribute("aria-live", "polite");
  await expect(panel).toHaveAttribute("aria-busy", "false");
  await expect(button).toBeVisible();
  const box = await button.boundingBox();
  expect(box).not.toBeNull();
  expect(box.height).toBeGreaterThanOrEqual(44);
  expect(box.width).toBeLessThanOrEqual(390);
  await button.focus();
  await expect(button).toBeFocused();
});

test("missing pull metadata branch fails closed without probing main", async ({
  page,
}) => {
  const repositoryRequests = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (url.pathname.startsWith("/api/repo/forkmesh/forkmesh/")) {
      repositoryRequests.push(url);
    }
  });
  await prepareWorldPage(page, "world-pull-unavailable", {
    repositoryFixture: { invalidPullBranch: true },
  });
  const workbench = await openWorldPullList(page);
  await expect(workbench.locator("[data-repo-pulls]")).toContainText(
    "Pull requests are unavailable until a reachable mirror host serves the .forkmesh/pulls/ folder.",
  );
  expect(
    repositoryRequests.filter(
      (url) =>
        url.pathname.endsWith("/tree") &&
        url.searchParams.get("path") === ".forkmesh/pulls",
    ),
  ).toHaveLength(0);
  expect(
    repositoryRequests.filter(
      (url) =>
        url.pathname.endsWith("/blobs") &&
        url.searchParams
          .getAll("path")
          .some((item) => item.startsWith(".forkmesh/pulls/")),
    ),
  ).toHaveLength(0);
});

test("a listed pull with unreadable metadata is unknown rather than open", async ({
  page,
}) => {
  await prepareWorldPage(page, "world-pull-record-unreadable", {
    repositoryFixture: { missingPullMetadata: true },
  });
  const workbench = await openWorldPullList(page);
  const unavailable = workbench.locator(
    "[data-repo-record-unavailable='pulls']",
  );
  await expect(unavailable).toHaveCount(1);
  await expect(unavailable).toContainText("Metadata unavailable");
  await expect(unavailable).toContainText("unknown");
  await expect(
    workbench.locator("[data-repo-record-kind='pulls']"),
  ).toHaveCount(1);
  await expect(workbench.locator("[data-repo-record-kind='pulls']")).toHaveAttribute(
    "data-repo-record-number",
    "44",
  );
  await expect(
    workbench.getByText(
      "1 pull-request record is listed by the tree but its metadata is unavailable.",
      { exact: false },
    ),
  ).toBeVisible();
});

test("live mirror cabinets expose a readable truthful technical panel", async ({
  page,
}) => {
  await prepareWorldPage(page, "mirror-cabinets");
  await waitForWorld(page);

  const cabinets = await page.locator("forkmesh-world").evaluate((shell) => {
    const items = [];
    shell.world.scene.traverse((object) => {
      if (String(object.name || "").startsWith("mirror-server-cabinet:")) {
        items.push({
          name: object.name,
          x: object.position.x,
          z: object.position.z,
        });
      }
    });
    return items;
  });
  expect(cabinets).toHaveLength(2);
  expect(cabinets[0].name).not.toBe(cabinets[1].name);
  expect(
    Math.hypot(
      cabinets[0].x - cabinets[1].x,
      cabinets[0].z - cabinets[1].z,
    ),
  ).toBeGreaterThan(2);

  const cabinetScreen = await page.locator("forkmesh-world").evaluate((shell) => {
    const scene = shell.world.scene;
    const technical = scene.getObjectByName("mirror-server-rear-panel");
    const screens = [
      "mirror-server-front-panel",
      "mirror-server-rear-panel",
      "mirror-server-agent-panel-left",
      "mirror-server-agent-panel-right",
    ].map((name) => {
      const screen = scene.getObjectByName(name);
      return {
        name,
        visible: screen?.visible === true,
        mipmaps: screen?.material?.map?.generateMipmaps === true,
        anisotropy: screen?.material?.map?.anisotropy,
      };
    });
    return {
      fields: technical?.userData?.cabinetFieldLabels || [],
      width: technical?.material?.map?.image?.width,
      height: technical?.material?.map?.image?.height,
      screens,
    };
  });
  expect(cabinetScreen.fields).toEqual(
    expect.arrayContaining([
      "NODE", "OWNER", "REPOS", "COMMIT", "MESSAGE", "AUTHOR",
      "SYNCED", "SIZE", "ISSUES", "COMMITS", "BRANCHES", "PULLS",
      "DISCUSSIONS", "HEALTH", "CPU", "RAM", "DISK", "PLATFORM",
      "VERSION", "NODE ID", "TUNNEL", "CLONES", "WEBSITE",
      "ARTIFACTS", "REACHABILITY", "PRIMARY REPO", "WORKTREES",
      "ACTIVITY", "LAST SEEN", "HOSTED SINCE", "REGION",
      "HTTPS ENDPOINT", "OPERATIONS",
    ]),
  );
  expect(cabinetScreen.width / cabinetScreen.height).toBeCloseTo(0.75, 3);
  expect(cabinetScreen.height).toBeGreaterThanOrEqual(512);
  expect(cabinetScreen.screens).toHaveLength(4);
  expect(
    cabinetScreen.screens.every(
      ({ visible, mipmaps, anisotropy }) =>
        visible && mipmaps && anisotropy === 8,
    ),
  ).toBe(true);

  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.focusNetworkNode("mirror2");
    shell.world.setCameraZoom(0.32);
  });
  await page.waitForTimeout(450);

  await page.locator("forkmesh-world").evaluate((shell) => {
    let mirror = null;
    shell.world.scene.traverse((object) => {
      if (object.userData?.nodeRecord?.name === "mirror2") {
        mirror = object.userData.nodeRecord;
      }
    });
    shell.openMirrorNodeDetail(mirror);
  });
  const detail = page.locator("[data-world-mirror-node-detail]");
  await expect(detail).toBeVisible();
  await expect(detail).toContainText("mirror2");
  await expect(detail).toContainText("25.0%");
  await expect(detail).toContainText("42");
  await expect(detail).toContainText("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  await expect(detail).toContainText("forkmesh/forkmesh");
  await expect(detail).toContainText(
    "operator-reported, bounded values signed into the public catalog",
  );
});

test("only nodes active within the past day stand in the cabinet yard", async ({
  page,
}) => {
  await prepareWorldPage(page, "mirror-cabinet-activity-window", {
    repositoryFixture: { dormantNodes: true },
  });
  await waitForWorld(page);

  const yard = await page.locator("forkmesh-world").evaluate((shell) => {
    const names = [];
    shell.world.scene.traverse((object) => {
      const name = String(object.name || "");
      if (name.startsWith("mirror-server-cabinet:")) {
        names.push(name.replace("mirror-server-cabinet:node:", ""));
      }
    });
    return names.sort();
  });
  // mirror2/mirror3 are online, mirror8 went quiet three hours ago, and
  // mirror9's last signed publication is thirty hours old.
  expect(yard).toEqual(["mirror2", "mirror3", "mirror8"]);
});

test("the authenticated member appears immediately and active time advances locally", async ({
  page,
}) => {
  const session = {
    nodeName: "jett",
    sessionToken: "world-activity-session",
  };
  await prepareWorldPage(page, "active-leaderboard", {
    session,
    ticketActivity: {
      totalActiveMs: 0,
      activityObservedAt: FIXED_NOW,
    },
    directoryUsers: [
      { name: "jett", nodes: [], totalActiveMs: 0 },
      { name: "alice", nodes: [], totalActiveMs: 120_000 },
    ],
  });
  await waitForWorld(page);

  const first = await page.locator("forkmesh-world").evaluate((shell) => {
    shell.syncMemberLounge();
    const members = shell.leaderboardMembers();
    const own = members.find((member) => member.name === "jett");
    const rankedNames = members
      .filter(
        (member) =>
          member.name &&
          (member.activeNow || Number(member.totalActiveMs) > 0),
      )
      .sort(
        (left, right) =>
          Number(right.totalActiveMs) - Number(left.totalActiveMs) ||
          left.name.localeCompare(right.name),
      )
      .slice(0, 6)
      .map((member) => member.name);
    return {
      activeNow: own.activeNow,
      totalActiveMs: own.totalActiveMs,
      rankedNames,
    };
  });
  expect(first.activeNow).toBe(true);
  expect(first.totalActiveMs).toBeGreaterThanOrEqual(0);
  expect(first.rankedNames).toEqual(["alice", "jett"]);

  await page.waitForTimeout(1_150);
  const advanced = await page
    .locator("forkmesh-world")
    .evaluate((shell) => shell.currentWorldActivityMs());
  expect(advanced).toBeGreaterThan(first.totalActiveMs + 900);

  const paused = await page.locator("forkmesh-world").evaluate((shell) => {
    shell.pauseWorldActivity();
    return {
      totalActiveMs: shell.currentWorldActivityMs(),
      running: Boolean(shell.worldActivityBaseAt),
      continuation: shell.worldActivityContinuation,
    };
  });
  await page.waitForTimeout(1_100);
  const stillPaused = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.currentWorldActivityMs(),
  );
  expect(paused.running).toBe(false);
  expect(paused.continuation).toBe("");
  expect(stillPaused).toBeCloseTo(paused.totalActiveMs, 3);
});

test("the open Members Circle shows its roster and places arrivals beside it", async ({
  page,
}) => {
  const session = {
    nodeName: "newcomer",
    sessionToken: "fresh-member-circle-session",
  };
  await prepareWorldPage(page, "fresh-member-circle", {
    session,
    directoryUsers: [
      {
        name: "newcomer",
        nodes: [],
        createdAt: FIXED_NOW - 1_000,
      },
      {
        name: "alice",
        nodes: [],
        createdAt: FIXED_NOW - 3_000,
      },
      {
        name: "bob",
        nodes: [],
        createdAt: FIXED_NOW - 2_000,
      },
    ],
  });
  await waitForWorld(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return Boolean(
      shell?.world?.scene?.getObjectByName("members-circle-member-info")
        ?.material?.map &&
        shell?.world?.scene?.getObjectByName("avatar:member:alice") &&
        shell?.world?.scene?.getObjectByName("avatar:member:bob"),
    );
  });

  const arrival = await page.locator("forkmesh-world").evaluate((shell) => {
    const player = shell.world.player;
    const circle = shell.world.scene.getObjectByName(
      "campfire-member-circle-dirt",
    );
    const sign = shell.world.scene.getObjectByName("members-circle-info-sign");
    const info = shell.world.scene.getObjectByName(
      "members-circle-member-info",
    );
    const center = circle.getWorldPosition(circle.position.clone());
    const removedYurt = shell.world.scene.getObjectByName(
      "members-center-yurt",
    );
    const startHere = shell.world.scene.getObjectByName(
      "forkmesh-start-here-map",
    );
    const memberPositions = ["alice", "bob"].map((name) => {
      const figure = shell.world.scene.getObjectByName(`avatar:member:${name}`);
      const position = figure.position;
      return {
        name,
        x: position.x,
        y: position.y,
        z: position.z,
        radius: Math.hypot(
          position.x - center.x,
          position.z - center.z,
        ),
      };
    });
    // The removed center pins and floating labels were created only by the
    // far-zoom LOD path, so cross that boundary before checking the scene.
    shell.world.setCameraZoom(2);
    return {
      placed: shell.freshArrivalCampfireSeated,
      activity: shell.lastMovement.activity,
      x: player.position.x,
      y: player.position.y,
      z: player.position.z,
      leftKnee: player.userData.leftKnee.rotation.x,
      circlePresent: Boolean(circle),
      circleVisible: circle.visible,
      yurtPresent: Boolean(removedYurt),
      signPosition: sign.position.toArray(),
      infoPosition: info.position.toArray(),
      infoRotation: info.rotation.y,
      infoHasTexture: Boolean(info.material.map),
      memberBenchesPresent: Boolean(
        shell.world.scene.getObjectByName("campfire-member-circle"),
      ),
      directoryFigures: circle.parent.userData.detailedMemberFigures || 0,
      memberPositions,
      dirtY: circle.position.y,
      oldPathSignPresent: Boolean(
        shell.world.scene.getObjectByName(
          "forkmesh-members-circle-path-sign",
        ),
      ),
      aerialMarkersPresent: Boolean(
        shell.world.scene.getObjectByName(
          "forkmesh-aerial-landmark-markers",
        ),
      ),
      startHere: {
        x: startHere.position.x,
        z: startHere.position.z,
        rotation: startHere.rotation.y,
      },
    };
  });

  expect(arrival.placed).toBe(true);
  expect(arrival.activity).toBe("visiting the Members Circle");
  expect(arrival.x).toBeCloseTo(0, 5);
  expect(arrival.z).toBeCloseTo(114.8, 5);
  expect(arrival.y).toBeCloseTo(0.38, 5);
  expect(arrival.leftKnee).toBeCloseTo(0, 5);
  expect(arrival.circlePresent).toBe(true);
  expect(arrival.circleVisible).toBe(true);
  expect(arrival.yurtPresent).toBe(false);
  expect(arrival.signPosition[0]).toBeCloseTo(10.2, 5);
  expect(arrival.signPosition[2]).toBeLessThan(-12);
  expect(arrival.infoPosition[0]).toBe(0);
  expect(arrival.infoPosition[1]).toBeGreaterThan(3);
  expect(arrival.infoRotation).toBeCloseTo(Math.PI, 5);
  expect(arrival.infoHasTexture).toBe(true);
  expect(arrival.memberBenchesPresent).toBe(false);
  expect(arrival.directoryFigures).toBe(2);
  expect(arrival.memberPositions).toHaveLength(2);
  for (const member of arrival.memberPositions) {
    expect(member.y).toBeCloseTo(0.4, 5);
    expect(member.radius).toBeGreaterThan(4);
    expect(member.radius).toBeLessThan(10);
  }
  expect(arrival.dirtY).toBeGreaterThan(0.105);
  expect(arrival.oldPathSignPresent).toBe(false);
  expect(arrival.aerialMarkersPresent).toBe(false);
  expect(arrival.startHere.x).toBe(0);
  expect(arrival.startHere.z).toBe(168);
  expect(arrival.startHere.rotation).toBeCloseTo(Math.PI, 5);

  // The entrance position persists as a normal standing location.
  await page.reload();
  await waitForWorld(page);
  const reloaded = await page.locator("forkmesh-world").evaluate((shell) => ({
    x: shell.world.player.position.x,
    y: shell.world.player.position.y,
    leftKnee: shell.world.player.userData.leftKnee.rotation.x,
    z: shell.world.player.position.z,
  }));
  expect(reloaded.x).toBeCloseTo(0, 5);
  expect(reloaded.y).toBeCloseTo(0.38, 5);
  expect(reloaded.z).toBeCloseTo(114.8, 5);
  expect(reloaded.leftKnee).toBeCloseTo(0, 5);
});

test("node cabinets stay fully detailed while open districts keep displays visible", async ({
  page,
}) => {
  await prepareWorldPage(page, "visible-enclosure-exits");
  await waitForWorld(page);

  const visit = async (objectName, mode) => {
    await page.locator("forkmesh-world").evaluate((shell, name) => {
      const destination = shell.world.scene.getObjectByName(name);
      const center = destination.getWorldPosition(
        shell.world.player.position.clone(),
      );
      shell.world.setSpawn({
        x: center.x,
        z: center.z,
        heading: 0,
        space: "town-square",
      });
    }, objectName);
    await page.waitForFunction(
      (expected) =>
        document.querySelector("forkmesh-world")?.world?.player?.parent?.userData
          ?.activeEnclosureScene === expected,
      mode,
    );
  };

  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setSpawn({
      x: 60,
      z: 0,
      heading: 0,
      space: "town-square",
    });
  });
  await page.waitForFunction(() =>
    Boolean(
      document
        .querySelector("forkmesh-world")
        ?.world?.scene?.getObjectByName("mirror-server-front-panel"),
    ),
  );
  // Standing well away from the yard: no box stand-ins exist at all, and the
  // cabinets and their displays keep drawing at full detail.
  const farNodes = await page.locator("forkmesh-world").evaluate((shell) => {
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-node-interior",
    );
    return {
      cover: shell.world.scene.getObjectByName("forkmesh-node-opaque-cover"),
      boxes: shell.world.scene.getObjectByName("forkmesh-node-box-lod"),
      detailedVisible: interior.visible,
      panelTargetVisible: shell.world.scene.getObjectByName(
        "mirror-server-front-panel",
      )?.visible,
      activeEnclosure: shell.world.player.parent.userData.activeEnclosureScene,
    };
  });
  expect(farNodes.cover).toBeUndefined();
  expect(farNodes.boxes).toBeUndefined();
  expect(farNodes.detailedVisible).toBe(true);
  expect(farNodes.panelTargetVisible).toBe(true);
  expect(farNodes.activeEnclosure).toBe("");

  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.setSpawn({
      x: 0,
      z: 0,
      heading: 0,
      space: "town-square",
    });
  });
  const nearNodes = await page.locator("forkmesh-world").evaluate((shell) => ({
    boxes: shell.world.scene.getObjectByName("forkmesh-node-box-lod"),
    detailedVisible: shell.world.scene.getObjectByName(
      "forkmesh-node-interior",
    ).visible,
    panelTargetVisible: shell.world.scene.getObjectByName(
      "mirror-server-front-panel",
    ).visible,
  }));
  expect(nearNodes).toEqual({
    boxes: undefined,
    detailedVisible: true,
    panelTargetVisible: true,
  });

  await visit("forkmesh-leaderboard-district", "");
  const leaderboards = await page.locator("forkmesh-world").evaluate((shell) => {
    const district = shell.world.scene.getObjectByName(
      "forkmesh-leaderboard-district",
    );
    const interior = shell.world.scene.getObjectByName(
      "forkmesh-leaderboard-interior",
    );
    const cover = shell.world.scene.getObjectByName(
      "forkmesh-leaderboard-opaque-cover",
    );
    return {
      districtVisible: district.visible,
      contentVisible: interior.visible,
      coverPresent: Boolean(cover),
      activeEnclosure: shell.world.player.parent.userData.activeEnclosureScene,
    };
  });
  expect(leaderboards).toEqual({
    districtVisible: true,
    contentVisible: true,
    coverPresent: false,
    activeEnclosure: "",
  });

  await visit("repository-district-content", "repositories");
  const repositories = await page.locator("forkmesh-world").evaluate((shell) => {
    const content = shell.world.scene.getObjectByName(
      "repository-district-content",
    );
    const dome = shell.world.scene.getObjectByName(
      "repository-geodesic-dome",
    );
    return {
      districtVisible: content.parent.visible,
      contentVisible: content.visible,
      importKioskVisible: shell.world.scene.getObjectByName(
        "repository-import-kiosk",
      ).visible,
      domePresent: Boolean(dome),
      activeEnclosure: shell.world.player.parent.userData.activeEnclosureScene,
    };
  });
  expect(repositories).toEqual({
    districtVisible: true,
    contentVisible: true,
    importKioskVisible: true,
    domePresent: false,
    activeEnclosure: "repositories",
  });

  await visit("members-circle-info-sign", "");
  const members = await page.locator("forkmesh-world").evaluate((shell) => {
    const circle = shell.world.scene.getObjectByName(
      "campfire-member-circle-dirt",
    );
    const sign = shell.world.scene.getObjectByName("members-circle-info-sign");
    const info = shell.world.scene.getObjectByName(
      "members-circle-member-info",
    );
    return {
      circleVisible: circle.visible,
      signVisible: sign.visible,
      infoHasTexture: Boolean(info.material.map),
      yurtPresent: Boolean(
        shell.world.scene.getObjectByName("members-center-yurt"),
      ),
      activeEnclosure: shell.world.player.parent.userData.activeEnclosureScene,
    };
  });
  expect(members).toEqual({
    circleVisible: true,
    signVisible: true,
    infoHasTexture: true,
    yurtPresent: false,
    activeEnclosure: "",
  });
});

// The aquarium Sky Office is an opt-in marketplace package: it ships switched
// off and joins the World only for a visitor who installed it. These specs
// seed that device choice and wait for the lazily loaded module.
async function installSkyOfficePackage(page) {
  await page.addInitScript(() => {
    localStorage.setItem(
      "forkmesh.world.installedPackages.v1",
      JSON.stringify(["forkmesh-sky-office"]),
    );
  });
}

async function waitForSkyOfficePackage(page) {
  await page.waitForFunction(() =>
    Boolean(
      document
        .querySelector("forkmesh-world")
        ?.world?.scene?.getObjectByName("forkmesh-sky-office-destination"),
    ),
  );
}

test("the sky office package is absent until a visitor installs it", async ({
  page,
}) => {
  await prepareWorldPage(page, "remote-sky-office-off");
  await waitForWorld(page);
  const before = await page.locator("forkmesh-world").evaluate((shell) => ({
    destination: Boolean(
      shell.world.scene.getObjectByName("forkmesh-sky-office-destination"),
    ),
    catalog: shell.world
      .worldPackageCatalog()
      .find((entry) => entry.id === "forkmesh-sky-office"),
    installed: shell.world.installedWorldPackageIds(),
  }));
  expect(before.destination).toBe(false);
  expect(before.catalog.installed).toBe(false);
  expect(before.catalog.installedByDefault).toBe(false);
  expect(before.installed).not.toContain("forkmesh-sky-office");

  const installed = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.installWorldPackage("forkmesh-sky-office"),
  );
  expect(installed).toBe(true);
  await waitForSkyOfficePackage(page);
  const after = await page.locator("forkmesh-world").evaluate((shell) => ({
    installed: shell.world.installedWorldPackageIds(),
    removed: shell.world.removeWorldPackage("forkmesh-sky-office"),
    destinationAfterRemove: Boolean(
      shell.world.scene.getObjectByName("forkmesh-sky-office-destination"),
    ),
  }));
  expect(after.installed).toContain("forkmesh-sky-office");
  expect(after.removed).toBe(true);
  expect(after.destinationAfterRemove).toBe(false);
});

test("remote aquarium office uses its 20x horizon LOD within budget", async ({
  page,
}) => {
  await installSkyOfficePackage(page);
  await prepareWorldPage(page, "remote-sky-office-lod");
  await waitForWorld(page);
  await waitForSkyOfficePackage(page);

  const contract = await page.locator("forkmesh-world").evaluate((shell) => {
    const destination = shell.world.scene.getObjectByName(
      "forkmesh-sky-office-destination",
    );
    const campus = destination?.getObjectByName("forkmesh-sky-office-campus");
    const lod = destination?.getObjectByName(
      "forkmesh-sky-office-distance-lod",
    );
    const high = lod?.getObjectByName("forkmesh-sky-office-high-fidelity");
    const low = lod?.getObjectByName("forkmesh-sky-office-low-poly");
    let meshes = 0;
    let transmissiveMaterials = 0;
    destination?.traverse?.((object) => {
      if (!object.isMesh) return;
      meshes += 1;
      const materials = Array.isArray(object.material)
        ? object.material
        : [object.material];
      transmissiveMaterials += materials.filter(
        (material) => Number(material?.transmission) > 0,
      ).length;
    });
    return {
      center: campus?.position?.toArray?.(),
      farPlane: shell.world.camera?.far,
      highVisible: high?.visible,
      lowVisible: low?.visible,
      loaded: destination?.userData?.forkmeshSkyOfficeLoaded,
      meshes,
      transmissiveMaterials,
      triangles:
        destination?.userData?.forkmeshSkyOfficeDistantTriangleCount,
      budget: destination?.userData?.forkmeshSkyOfficeDistantTriangleBudget,
    };
  });

  expect(contract.center).toEqual([-330, 0, -720 * 20]);
  expect(contract.farPlane).toBe(16_000);
  expect(contract.highVisible).toBe(false);
  expect(contract.lowVisible).toBe(true);
  expect(contract.loaded).toBe(false);
  expect(contract.meshes).toBeLessThanOrEqual(10);
  expect(contract.transmissiveMaterials).toBe(0);
  expect(contract.budget).toBe(900);
  expect(contract.triangles).toBeLessThanOrEqual(contract.budget);
});

test("restored panoramic Office and remote Sky campus use bounded LODs", async ({
  page,
}) => {
  await installSkyOfficePackage(page);
  await prepareWorldPage(page, "remote-main-office-lod");
  await waitForWorld(page);
  await waitForSkyOfficePackage(page);
  await moveToOfficeEntrance(page, { unpause: true });
  await expect.poll(() =>
    page.locator("forkmesh-world").evaluate((shell) =>
      shell.world.scene.getObjectByName("forkmesh-office-high-fidelity")?.visible,
    )
  ).toBe(true);

  const fromMainland = await page.locator("forkmesh-world").evaluate((shell) => {
    const office = shell.world.scene.getObjectByName(
      "forkmesh-office-destination",
    );
    const officeLod = office?.getObjectByName(
      "forkmesh-office-distance-lod",
    );
    const mainlandLod = shell.world.scene.getObjectByName(
      "forkmesh-mainland-distance-lod",
    );
    return {
      center: office?.position?.toArray?.(),
      rootVisible: office?.visible,
      highVisible: officeLod?.getObjectByName(
        "forkmesh-office-high-fidelity",
      )?.visible,
      lowVisible: officeLod?.getObjectByName(
        "forkmesh-office-low-poly",
      )?.visible,
      draws: office?.userData?.officeDistantDrawCount,
      budget: office?.userData?.officeDistantDrawBudget,
      mainlandHighVisible: mainlandLod?.getObjectByName(
        "forkmesh-mainland-high-fidelity",
      )?.visible,
      mainlandLowVisible: mainlandLod?.getObjectByName(
        "forkmesh-mainland-low-poly",
      )?.visible,
      mainlandVisible: shell.world.scene.getObjectByName(
        "forkmesh-continuous-city-land",
      )?.visible,
    };
  });
  expect(fromMainland.center).toEqual([0, 0, -207]);
  expect(fromMainland.rootVisible).toBe(true);
  expect(fromMainland.highVisible).toBe(true);
  expect(fromMainland.lowVisible).toBe(false);
  expect(fromMainland.draws).toBeGreaterThan(0);
  expect(fromMainland.draws).toBeLessThanOrEqual(fromMainland.budget);
  expect(fromMainland.mainlandHighVisible).toBe(true);
  expect(fromMainland.mainlandLowVisible).toBe(false);
  expect(fromMainland.mainlandVisible).toBe(true);

  await walkIntoOffice(page);
  await expect.poll(async () =>
    page.locator("forkmesh-world").evaluate((shell) =>
      shell.world.player.parent.userData.mainlandDetailLevel,
    )
  ).toBe("detailed");
  const fromOffice = await page.locator("forkmesh-world").evaluate((shell) => {
    const mainlandLod = shell.world.scene.getObjectByName(
      "forkmesh-mainland-distance-lod",
    );
    return {
      highVisible: mainlandLod?.getObjectByName(
        "forkmesh-mainland-high-fidelity",
      )?.visible,
      lowVisible: mainlandLod?.getObjectByName(
        "forkmesh-mainland-low-poly",
      )?.visible,
      mainlandVisible: shell.world.scene.getObjectByName(
        "forkmesh-continuous-city-land",
      )?.visible,
      triangles: mainlandLod?.userData?.mainlandDistantTriangleCount,
      budget: mainlandLod?.userData?.mainlandDistantTriangleBudget,
    };
  });
  expect(fromOffice.highVisible).toBe(true);
  expect(fromOffice.lowVisible).toBe(false);
  expect(fromOffice.mainlandVisible).toBe(true);
  expect(fromOffice.triangles).toBeLessThanOrEqual(fromOffice.budget);

  await page.locator("forkmesh-world").evaluate((shell) => {
    shell.world.leaveOfficeInterior();
    shell.world.setSpawn({
      space: "pkg-forkmesh-sky-office-campus",
      x: -330,
      y: 0.38,
      z: -720 * 20 + 100,
      heading: 0,
    });
  });
  await expect.poll(async () =>
    page.locator("forkmesh-world").evaluate((shell) =>
      shell.world.player.parent.userData.mainlandDetailLevel,
    )
  ).toBe("low");
  const fromSkyOffice = await page.locator("forkmesh-world").evaluate(
    (shell) => ({
      mainlandVisible: shell.world.scene.getObjectByName(
        "forkmesh-continuous-city-land",
      )?.visible,
      mainlandLowVisible: shell.world.scene.getObjectByName(
        "forkmesh-mainland-low-poly",
      )?.visible,
      mainOfficeVisible: shell.world.scene.getObjectByName(
        "forkmesh-office-destination",
      )?.visible,
    }),
  );
  expect(fromSkyOffice.mainlandVisible).toBe(false);
  expect(fromSkyOffice.mainlandLowVisible).toBe(true);
  expect(fromSkyOffice.mainOfficeVisible).toBe(false);
});

test("scene repeats share geometry while avatars keep full detail", async ({
  page,
}) => {
  await prepareWorldPage(page, "scene-camera-lod", {
    directoryUsers: [
      {
        name: "lod-member",
        nodes: [],
        createdAt: FIXED_NOW - 2_000,
      },
    ],
  });
  await waitForWorld(page);
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    const scene = shell?.world?.scene;
    const player = shell?.world?.player;
    return Boolean(
      scene?.getObjectByName("avatar:member:lod-member") &&
        scene?.getObjectByName("forkmesh-world-quadcopter-1") &&
        scene?.getObjectByName("forkmesh-world-quadcopter-2") &&
        scene?.getObjectByName("forkmesh-drone-race-course") &&
        player?.parent,
    );
  });

  const contract = await page.locator("forkmesh-world").evaluate((shell) => {
    const scene = shell.world.scene;
    const player = shell.world.player;
    const member = scene.getObjectByName("avatar:member:lod-member");
    const quadcopters = [1, 2].map((index) =>
      scene.getObjectByName(`forkmesh-world-quadcopter-${index}`),
    );
    const course = scene.getObjectByName("forkmesh-drone-race-course");
    const sharedGeometries = (root) => {
      const geometries = new Set();
      root.traverse((child) => {
        if (child.geometry?.userData?.forkmeshSharedResource === true) {
          geometries.add(child.geometry);
        }
      });
      return geometries;
    };
    const sharedMaterials = (root) => {
      const materials = new Set();
      root.traverse((child) => {
        const childMaterials = Array.isArray(child.material)
          ? child.material
          : child.material
            ? [child.material]
            : [];
        childMaterials.forEach((material) => materials.add(material));
      });
      return materials;
    };
    const intersects = (left, right) =>
      [...left].some((entry) => right.has(entry));
    const playerGeometry = sharedGeometries(player);
    const memberGeometry = sharedGeometries(member);
    const firstQuadGeometry = sharedGeometries(quadcopters[0]);
    const secondQuadGeometry = sharedGeometries(quadcopters[1]);
    const firstQuadMaterials = sharedMaterials(quadcopters[0]);
    const secondQuadMaterials = sharedMaterials(quadcopters[1]);
    const worldRoot = player.parent;

    return {
      avatars: {
        bothKeepFullDetail:
          player.userData.highDetail?.visible === true &&
          member.userData.highDetail?.visible === true &&
          player.userData.avatarLod === undefined &&
          member.userData.avatarLod === undefined,
        sharedGeometryMarked:
          playerGeometry.size > 0 && memberGeometry.size > 0,
        shareGeometry: intersects(playerGeometry, memberGeometry),
      },
      quadcopters: {
        rootCount: quadcopters.filter(Boolean).length,
        distinctRoots: quadcopters[0] !== quadcopters[1],
        bothAreWorldRoots: quadcopters.every(
          (quadcopter) => quadcopter.parent === worldRoot,
        ),
        bothUseLod: quadcopters.every(
          (quadcopter) => quadcopter.isLOD && quadcopter.levels.length === 2,
        ),
        sharedGeometryMarked:
          firstQuadGeometry.size > 0 && secondQuadGeometry.size > 0,
        shareGeometry: intersects(firstQuadGeometry, secondQuadGeometry),
        shareMaterials: intersects(firstQuadMaterials, secondQuadMaterials),
      },
      course: {
        isWorldRoot: course.parent === worldRoot,
        isLod: course.isLOD === true,
        levelCount: course.levels.length,
        ringCount: course.userData.ringCount,
        instancedCounts: course.levels.map((level) => ({
          instanced: level.object.isInstancedMesh === true,
          count: level.object.count,
        })),
      },
      nodes: {
        // Node cabinets are exempt from camera LOD entirely: the yard has no
        // box stand-ins and its contents are always mounted and visible.
        boxLodPresent: Boolean(scene.getObjectByName("forkmesh-node-box-lod")),
        interiorVisible:
          scene.getObjectByName("forkmesh-node-interior")?.visible === true,
      },
    };
  });

  expect(contract.avatars).toEqual({
    bothKeepFullDetail: true,
    sharedGeometryMarked: true,
    shareGeometry: true,
  });
  expect(contract.quadcopters).toEqual({
    rootCount: 2,
    distinctRoots: true,
    bothAreWorldRoots: true,
    bothUseLod: true,
    sharedGeometryMarked: true,
    shareGeometry: true,
    shareMaterials: true,
  });
  expect(contract.course).toEqual({
    isWorldRoot: true,
    isLod: true,
    levelCount: 2,
    ringCount: 10,
    instancedCounts: [
      { instanced: true, count: 10 },
      { instanced: true, count: 10 },
    ],
  });
  expect(contract.nodes).toEqual({
    boxLodPresent: false,
    interiorVisible: true,
  });
});

test("World omits the built-in System Status board", async ({ page }) => {
  await prepareWorldPage(page, "status-board-removed");
  await waitForWorld(page);

  const statusFeature = await page.locator("forkmesh-world").evaluate((shell) => ({
    board: Boolean(
      shell.world.scene.getObjectByName("forkmesh-status-banner"),
    ),
    refreshMethod: typeof shell.refreshSystemStatusBoard,
    timerMethod: typeof shell.syncSystemStatusBoardTimer,
    adminLight: Boolean(shell.querySelector("[data-world-admin-status-light]")),
  }));

  expect(statusFeature).toEqual({
    board: false,
    refreshMethod: "undefined",
    timerMethod: "undefined",
    adminLight: false,
  });
});

test("Town Square placement is contextual, tracking-free, and collapses safely", async ({
  page,
}) => {
  const placementRequests = [];
  page.on("request", (request) => {
    if (request.url().includes("/api/world/community-ads/placements")) {
      placementRequests.push(request.url());
    }
  });
  await prepareWorldPage(page, "community-placement");
  await waitForWorld(page);
  await pauseWorld(page);
  await expandWorldRightRail(page);

  const rail = page.locator(".world-right-rail");
  const map = page.locator(".world-map");
  const placement = page.locator("[data-world-community-placement]");
  await expect(placement).toBeVisible();
  await expect(placement).toContainText("Community-reviewed placement");
  await expect(placement).toContainText("No behavioral tracking");
  await expect(placement.getByRole("link", { name: /Visit sponsor/ }))
    .toHaveAttribute("rel", "sponsored noopener noreferrer");
  await expect(placement.getByRole("link", { name: /Visit sponsor/ }))
    .toHaveAttribute("referrerpolicy", "no-referrer");
  expect(placementRequests.map((value) => {
    const url = new URL(value);
    return `${url.pathname}${url.search}`;
  })).toEqual([
    "/api/world/community-ads/placements?context=town-square",
  ]);
  expect(
    await rail.evaluate((element, nodes) => {
      const mapNode = document.querySelector(nodes.map);
      const placementNode = document.querySelector(nodes.placement);
      return (
        element.contains(mapNode) &&
        element.contains(placementNode) &&
        Boolean(
          mapNode.compareDocumentPosition(placementNode) &
            Node.DOCUMENT_POSITION_FOLLOWING,
        )
      );
    }, { map: ".world-map", placement: "[data-world-community-placement]" }),
  ).toBe(true);

  await page.locator("forkmesh-world").evaluate((shell) =>
    shell.openLandmark("fountain"),
  );
  await expect(placement).toBeHidden();
  await page.locator("[data-world-detail-close]").click();
  await page.setViewportSize({ width: 900, height: 900 });
  await expect(map).toBeHidden();
  await expect(placement).toBeHidden();
});

test("verified public feedback requires manual preview and remains pending", async ({
  page,
}) => {
  const createBodies = [];
  await page.addInitScript(() => {
    localStorage.setItem(
      "forkmesh.session",
      JSON.stringify({
        sessionToken: "playwright-owner-session",
        nodeName: "owner",
      }),
    );
  });
  page.on("request", (request) => {
    if (request.url().endsWith("/create")) {
      createBodies.push(request.postDataJSON());
    }
  });
  await prepareWorldPage(page, "fediverse-manual-review");
  await waitForWorld(page);
  await pauseWorld(page);
  await expandWorldRightRail(page);

  const feed = page.locator("[data-world-fediverse-activity]");
  await expect(feed).toBeVisible();
  await expect(feed).toContainText("Verified public feedback");
  await expect(feed).toContainText("Manual review");
  await feed.getByRole("button", { name: "Preview" }).click();
  await expect(page.getByRole("heading", { name: "Review public feedback" }))
    .toBeVisible();
  await expect(page.locator("[data-world-fediverse-followup]")).not.toBeChecked();

  await page.locator("[data-world-fediverse-create]").click();
  expect(createBodies).toHaveLength(0);
  await expect(page.locator("[data-world-fediverse-result]")).toContainText(
    "explicit authorization",
  );

  await page.locator("[data-world-fediverse-confirm]").check();
  await page.locator("[data-world-fediverse-followup]").check();
  await page.locator("[data-world-fediverse-create]").click();
  await expect(page.locator("[data-world-fediverse-result]")).toContainText(
    "Pending owner-node materialization",
  );
  expect(createBodies).toEqual([
    {
      confirm: true,
      title: "Save button crashes",
      body: "The save button crashes on large projects.",
      publishFollowup: true,
    },
  ]);
  await expect(page.getByText("created", { exact: true })).toHaveCount(0);
});

test("tracked public replies open a separate read-only fediverse thread", async ({
  page,
}) => {
  await prepareWorldPage(page, "fediverse-thread", { includeThread: true });
  await waitForWorld(page);
  await pauseWorld(page);
  await expandWorldRightRail(page);

  const feed = page.locator("[data-world-fediverse-activity]");
  await expect(feed).toContainText("Tracked reply");
  await feed.getByRole("button", { name: "Thread" }).click();

  await expect(
    page.getByRole("heading", { name: "Issue #17 fediverse thread" }),
  ).toBeVisible();
  const reply = page.locator("[data-world-federated-reply]");
  await expect(reply).toContainText("Nested reply from a public Lemmy thread.");
  await expect(reply).toContainText("not a signed ForkMesh native event");
  await expect(reply).toHaveAttribute("data-native-event", "false");
  await expect(reply.getByRole("link", { name: /Open original/ }))
    .toHaveAttribute("href", "https://lemmy.example.org/comment/77");
  await expect(reply.getByRole("link", { name: /Open original/ }))
    .toHaveAttribute("referrerpolicy", "no-referrer");
});

test("sound button enables short effects and the sign opens the ForkMesh song", async ({ page }) => {
  const mediaRequests = [];
  page.on("request", (request) => {
    if (
      request.resourceType() === "media" ||
      /\.(?:mp3|m4a|ogg|wav|m3u8)(?:[?#]|$)/i.test(request.url())
    ) {
      mediaRequests.push(request.url());
    }
  });
  await page.addInitScript(() => {
    window.__forkmeshEffectStarts = 0;
    window.__forkmeshSongOpens = [];
    window.__forkmeshAudioBuilds = [];
    class AudioParam {
      setValueAtTime() {}
      exponentialRampToValueAtTime() {}
      linearRampToValueAtTime() {}
      cancelScheduledValues() {}
      setTargetAtTime() {}
    }
    class Node {
      constructor() {
        this.gain = new AudioParam();
        this.frequency = new AudioParam();
        this.detune = new AudioParam();
      }
      connect() {}
      start() {
        window.__forkmeshEffectStarts += 1;
      }
      stop() {}
    }
    class FakeAudioContext {
      constructor() {
        this.currentTime = 0;
        this.destination = {};
        this.state = "running";
      }
      createGain() {
        return new Node();
      }
      createOscillator() {
        return new Node();
      }
      async resume() {}
      async close() {
        this.state = "closed";
      }
    }
    Object.defineProperty(window, "AudioContext", {
      configurable: true,
      value: FakeAudioContext,
    });
    class ForbiddenAudio {
      constructor(src) {
        window.__forkmeshAudioBuilds.push(String(src || ""));
      }
      addEventListener() {}
      pause() {}
      async play() {}
    }
    Object.defineProperty(window, "Audio", {
      configurable: true,
      value: ForbiddenAudio,
    });
    Object.defineProperty(window, "open", {
      configurable: true,
      value: (url) => {
        window.__forkmeshSongOpens.push(String(url || ""));
        return null;
      },
    });
  });
  await prepareWorldPage(page, "short-effects");
  await waitForWorld(page);

  await page.locator("[data-world-sound-toggle]").evaluate((button) =>
    button.click(),
  );
  await expect(page.locator("[data-world-sound-toggle]")).toHaveAttribute(
    "aria-pressed",
    "true",
  );
  const startsAfterEnable = await page.evaluate(
    () => window.__forkmeshEffectStarts,
  );
  expect(startsAfterEnable).toBeGreaterThan(0);

  await page.locator("forkmesh-world").evaluate((shell) =>
    shell.playCountryJoinSound("US", true),
  );
  expect(await page.evaluate(() => window.__forkmeshEffectStarts)).toBeGreaterThan(
    startsAfterEnable,
  );
  await page.locator("forkmesh-world").evaluate((shell) =>
    shell.openForkmeshSong(),
  );
  expect(await page.evaluate(() => window.__forkmeshSongOpens)).toEqual([
    "https://www.youtube.com/watch?v=EsQMZuS6lbE",
  ]);
  // The World never fetches or decodes the track itself.
  expect(await page.evaluate(() => window.__forkmeshAudioBuilds)).toEqual([]);
  expect(mediaRequests).toEqual([]);

  await page.locator("[data-world-sound-toggle]").evaluate((button) =>
    button.click(),
  );
  await expect(page.locator("[data-world-sound-toggle]")).toHaveAttribute(
    "aria-pressed",
    "false",
  );
  expect(
    await page.locator("forkmesh-world").evaluate((shell) => ({
      soundEnabled: shell.soundEnabled,
      soundContext: shell.soundContext,
      hasFocusPlayer: typeof shell.playFocusMusic === "function",
    })),
  ).toEqual({
    soundEnabled: false,
    soundContext: null,
    hasFocusPlayer: false,
  });
});

test("@critical portrait coarse-pointer thumbstick and visual viewport remain usable", async ({
  browser,
}) => {
  test.setTimeout(60_000);
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
  });
  const page = await context.newPage();
  const hiddenFocusWarnings = [];
  const rendererRequests = [];
  page.on("request", (request) => {
    const url = new URL(request.url());
    if (url.pathname.includes("three") || url.hostname === "cdn.jsdelivr.net") {
      rendererRequests.push(url);
    }
  });
  page.on("console", (message) => {
    if (message.text().includes("Blocked aria-hidden on an element")) {
      hiddenFocusWarnings.push(message.text());
    }
  });
  await prepareWorldPage(page, "portrait-touch");
  await waitForWorld(page, "/world/?quality=mobile-mid&adaptive=0");
  await expect(page.locator(".world-webgl-fallback")).toHaveCount(0);

  const compactRaster = await page
    .locator("forkmesh-world")
    .evaluate((shell) => {
      const diagnostics = shell.world.getDiagnostics();
      let quarterScaleTextures = 0;
      shell.world.scene.traverse((object) => {
        const materials = Array.isArray(object.material)
          ? object.material
          : [object.material];
        materials.filter(Boolean).forEach((material) => {
          if (material.map?.image?.dataset?.textureScale === "0.25") {
            quarterScaleTextures += 1;
          }
        });
      });
      return {
        compact: diagnostics.output.compactRenderer,
        canvasTextureScale: diagnostics.output.canvasTextureScale,
        quarterScaleTextures,
      };
    });
  expect(compactRaster).toMatchObject({
    compact: true,
    canvasTextureScale: 0.25,
  });
  expect(compactRaster.quarterScaleTextures).toBeGreaterThan(0);
  expect(
    rendererRequests.some(
      (url) => url.pathname ===
        "/world/vendor/three-0.184.0/three.module.min.js",
    ),
  ).toBe(true);
  expect(
    rendererRequests.some(
      (url) => url.pathname ===
        "/world/vendor/three-0.184.0/three.core.min.js",
    ),
  ).toBe(true);
  expect(
    rendererRequests.some((url) => url.hostname === "cdn.jsdelivr.net"),
  ).toBe(false);

  await expect(page.locator("[data-world-thumbstick]")).toBeVisible();
  await page.locator("[data-world-settings-open]").first().click();
  await expect(page.locator("[data-world-settings]")).toHaveAttribute(
    "data-open",
    "true",
  );
  const statusNoteInput = page.locator("[data-world-status-note]");
  await statusNoteInput.focus();
  const before = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getPosition(),
  );
  await page.keyboard.press("w");
  const afterInput = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getPosition(),
  );
  expect(afterInput).toEqual(before);

  const stableHeight = await page.locator("forkmesh-world").evaluate(
    (shell) => shell.lastStableViewportHeight,
  );
  await page.setViewportSize({ width: 390, height: 520 });
  await page.waitForTimeout(300);
  expect(
    await page.locator("forkmesh-world").evaluate(
      (shell) => shell.lastStableViewportHeight,
    ),
  ).toBe(stableHeight);
  await page.setViewportSize({ width: 520, height: 390 });
  await page.waitForFunction(
    () =>
      document.querySelector("forkmesh-world")?.style
        .getPropertyValue("--world-viewport-height") === "390px",
  );
  await page.locator("[data-world-settings-close]").click();
  await expect(page.locator("[data-world-settings]")).toHaveAttribute(
    "data-open",
    "false",
  );
  expect(hiddenFocusWarnings).toEqual([]);

  const control = page.locator("[data-world-thumbstick]");
  // Leave enough time for more than one animation frame even when the release
  // gate is sharing a loaded CI host; the assertion still requires real
  // position movement from an actual CDP touch sequence.
  await dragThumbstick(page, control, { durationMs: 600 });
  const afterTouch = await page.locator("forkmesh-world").evaluate((shell) =>
    shell.world.getPosition(),
  );
  expect(
    Math.hypot(afterTouch.x - before.x, afterTouch.z - before.z),
  ).toBeGreaterThan(0.05);
  await context.close();
});

test("phone HUD keeps tools readable and combines avatar actions", async ({
  browser,
}) => {
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    hasTouch: true,
    isMobile: true,
  });
  const page = await context.newPage();
  await prepareWorldPage(page, "mobile-readable-hud");
  await waitForWorld(page);
  await pauseWorld(page);

  const launcher = page.locator("[data-world-shirt-badge]");
  const actions = page.locator("[data-world-avatar-actions]");
  const actionsToggle = page.locator("[data-world-avatar-actions-toggle]");
  const actionButtons = page.locator(
    "[data-world-avatar-action-menu] button",
  );
  await expect(actionsToggle).toBeVisible();
  await expect(actionsToggle).toHaveAttribute("aria-expanded", "false");
  await expect(actionButtons.first()).toBeHidden();

  const collapsedGeometry = await page.evaluate(() => {
    const avatar = document.querySelector("[data-world-shirt-badge]");
    const toggle = document.querySelector(
      "[data-world-avatar-actions-toggle]",
    );
    const avatarRect = avatar?.getBoundingClientRect();
    const toggleRect = toggle?.getBoundingClientRect();
    return {
      avatar: avatarRect && {
        top: avatarRect.top,
        right: avatarRect.right,
        width: avatarRect.width,
        height: avatarRect.height,
        radius: getComputedStyle(avatar).borderTopLeftRadius,
      },
      toggle: toggleRect && {
        center: toggleRect.left + toggleRect.width / 2,
        bottom: toggleRect.bottom,
      },
      width: window.innerWidth,
      height: window.innerHeight,
    };
  });
  expect(collapsedGeometry.avatar.top).toBeGreaterThan(0);
  expect(collapsedGeometry.avatar.right).toBeLessThan(
    collapsedGeometry.width,
  );
  expect(collapsedGeometry.avatar.width).toBe(
    collapsedGeometry.avatar.height,
  );
  expect(collapsedGeometry.avatar.radius).toBe("50%");
  expect(collapsedGeometry.toggle.center).toBeCloseTo(
    collapsedGeometry.width / 2,
    0,
  );
  expect(collapsedGeometry.toggle.bottom).toBeLessThan(
    collapsedGeometry.height,
  );

  // Touch uses the first avatar tap to expose the HUD without opening the
  // account sheet. The tool row now lives below the status pills and summon
  // stays off the phone surface.
  await launcher.click();
  await expect(page.locator("[data-world-hud]")).toHaveAttribute(
    "data-hud-expanded",
    "true",
  );
  await expect(page.locator("[data-world-agent-summon]")).toBeHidden();
  const expandedGeometry = await page.evaluate(() => {
    const rect = (selector) =>
      document.querySelector(selector)?.getBoundingClientRect();
    const status = rect(".world-status-actions");
    const tools = [...document.querySelectorAll(".world-top-link")]
      .filter((element) => getComputedStyle(element).display !== "none")
      .map((element) => element.getBoundingClientRect());
    const rail = document.querySelector("[data-world-right-rail]");
    const map = document.querySelector(".world-map");
    const saved = document.querySelector(".world-saved-views");
    const labels = [
      ...document.querySelectorAll(
        ".world-map-label-copy, .world-share-view-button > span:last-child, .world-remember-view > span:last-child",
      ),
    ];
    const railButtons = [
      ...document.querySelectorAll(
        ".world-map-button, .world-share-view-button, .world-remember-view",
      ),
    ];
    return {
      statusBottom: status?.bottom || 0,
      toolTop: Math.min(...tools.map((item) => item.top)),
      toolsInsideViewport: tools.every(
        (item) => item.left >= 0 && item.right <= window.innerWidth,
      ),
      railRight: rail?.getBoundingClientRect().right || 0,
      readableLabels: labels.every((label) => {
        const style = getComputedStyle(label);
        return (
          style.display !== "none" &&
          style.visibility !== "hidden" &&
          label.getBoundingClientRect().width > 20
        );
      }),
      wideRailButtons: railButtons.every(
        (button) => button.getBoundingClientRect().width >= 120,
      ),
      mapChrome: map && {
        background: getComputedStyle(map).backgroundColor,
        border: getComputedStyle(map).borderTopWidth,
      },
      savedChrome: saved && {
        background: getComputedStyle(saved).backgroundColor,
        border: getComputedStyle(saved).borderTopWidth,
      },
      width: window.innerWidth,
    };
  });
  expect(expandedGeometry.toolTop).toBeGreaterThan(
    expandedGeometry.statusBottom,
  );
  expect(expandedGeometry.toolsInsideViewport).toBe(true);
  expect(expandedGeometry.railRight).toBeLessThan(
    expandedGeometry.width,
  );
  expect(expandedGeometry.readableLabels).toBe(true);
  expect(expandedGeometry.wideRailButtons).toBe(true);
  expect(expandedGeometry.mapChrome).toEqual({
    background: "rgba(0, 0, 0, 0)",
    border: "0px",
  });
  expect(expandedGeometry.savedChrome).toEqual({
    background: "rgba(0, 0, 0, 0)",
    border: "0px",
  });

  await actionsToggle.click();
  await expect(actions).toHaveAttribute("data-expanded", "true");
  await expect(actionsToggle).toHaveAttribute("aria-expanded", "true");
  await expect(actionButtons).toHaveCount(10);
  await expect(actionButtons.first()).toBeVisible();
  const expandedActions = await page.evaluate(() => {
    const toggle = document
      .querySelector("[data-world-avatar-actions-toggle]")
      ?.getBoundingClientRect();
    const menu = document
      .querySelector("[data-world-avatar-action-menu]")
      ?.getBoundingClientRect();
    const buttons = [
      ...document.querySelectorAll("[data-world-avatar-action-menu] button"),
    ].map((button) => button.getBoundingClientRect());
    return {
      menuTop: menu?.top || 0,
      menuBottom: menu?.bottom || 0,
      toggleTop: toggle?.top || 0,
      centered: buttons.every(
        (button) =>
          Math.abs(
            button.left + button.width / 2 - window.innerWidth / 2,
          ) <= 1,
      ),
    };
  });
  expect(expandedActions.menuTop).toBeGreaterThanOrEqual(0);
  expect(expandedActions.menuBottom).toBeLessThanOrEqual(
    expandedActions.toggleTop,
  );
  expect(expandedActions.centered).toBe(true);

  await actionButtons.first().click();
  await expect(actions).toHaveAttribute("data-expanded", "false");
  await expect(actionsToggle).toHaveAttribute("aria-expanded", "false");
  await context.close();
});

for (const viewport of [
  {
    name: "desktop",
    options: { viewport: { width: 1440, height: 900 } },
  },
  {
    name: "portrait",
    options: {
      viewport: { width: 390, height: 844 },
      hasTouch: true,
      isMobile: true,
    },
  },
  {
    name: "landscape",
    options: {
      viewport: { width: 844, height: 390 },
      hasTouch: true,
      isMobile: true,
    },
  },
]) {
  test(`visual acceptance ${viewport.name}`, async ({ browser }) => {
    const context = await browser.newContext(viewport.options);
    const page = await context.newPage();
    await page.routeWebSocket(
      "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
      () => {},
    );
    await prepareWorldPage(page, `visual-${viewport.name}`, {
      chatPassphrase: `visual-${viewport.name}-office-passphrase`,
      session: {
        kind: "user",
        nodeName: "visual-member",
        email: "visual-member@example.test",
        sessionToken: `visual-${viewport.name}-office-token`,
      },
    });
    await waitForWorld(page);
    await freezeWorld(page);
    const townState = await renderedWorldState(page);
    expect(townState).toMatchObject({
      cssWidth: viewport.options.viewport.width,
      cssHeight: viewport.options.viewport.height,
      contextLost: false,
      cameraFinite: true,
      space: "town-square",
    });
    expect(townState.bufferWidth).toBeGreaterThan(0);
    expect(townState.bufferHeight).toBeGreaterThan(0);
    expect(townState.frame).toBeGreaterThan(0);
    expect(townState.pixel[3]).toBe(255);
    expect(townState.pixel.slice(0, 3).some((channel) => channel > 0)).toBe(true);
    expect(await page.evaluate(
      () => document.documentElement.scrollWidth <= window.innerWidth,
    )).toBe(true);
    await openOfficeForVisual(page);
    const dock = page.locator("[data-world-office-lobby]");
    await expect(dock).toBeHidden();
    await expect(page.locator("canvas.world-canvas")).toBeVisible();
    await expect.poll(() => officeSceneState(page)).toEqual({
      slidingDoorCount: 2,
      hasHingedDoor: false,
      hasKeypad: false,
      campusVisible: true,
      interiorVisible: true,
      space: "office-lobby",
    });
    const officeState = await renderedWorldState(page);
    expect(officeState).toMatchObject({
      cssWidth: viewport.options.viewport.width,
      cssHeight: viewport.options.viewport.height,
      contextLost: false,
      cameraFinite: true,
      space: "office-lobby",
    });
    expect(officeState.pixel[3]).toBe(255);
    expect(officeState.pixel.slice(0, 3).some((channel) => channel > 0)).toBe(true);
    expect(await page.evaluate(
      () => document.documentElement.scrollWidth <= window.innerWidth,
    )).toBe(true);
    await context.close();
  });
}

test("ForkMesh Office remains a continuous World at 320 CSS pixels", async ({
  browser,
}) => {
  const context = await browser.newContext({
    viewport: { width: 320, height: 640 },
    hasTouch: true,
    isMobile: true,
  });
  const page = await context.newPage();
  await page.routeWebSocket(
    "**/api/repo/mainnode/forkmesh/rooms/world-general/ws",
    () => {},
  );
  await prepareWorldPage(page, "office-320", {
    chatPassphrase: "playwright-office-320-passphrase",
    session: {
      kind: "user",
      nodeName: "alice",
      email: "alice@example.test",
      sessionToken: "alice-office-token",
    },
  });
  await waitForWorld(page);
  await openOfficeForVisual(page);
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(320);
  await expect(page.locator("[data-world-office-lobby]")).toBeHidden();
  await expect(page.locator("canvas.world-canvas")).toBeVisible();
  await context.close();
});

test("the HUD online pill counts everyone here and drops the roster down on hover", async ({
  page,
}) => {
  let relay = null;
  await prepareWorldPage(page, "roster-host", {
    worldSocketHandler(socket, socketId) {
      relay = socket;
      socket.send(
        JSON.stringify({
          type: "welcome",
          id: socketId,
          peers: [
            {
              id: "peer-zoe",
              name: "Zoe",
              status: "away",
              countryCode: "SE",
              x: 3,
              y: 0.38,
              z: 4,
              yaw: 0,
              space: "town-square",
            },
            {
              id: "peer-ada",
              name: "Ada",
              status: "online",
              countryCode: "GB",
              x: -2,
              y: 0.38,
              z: 1,
              yaw: 0,
              space: "town-square",
            },
          ],
        }),
      );
    },
  });
  await waitForWorld(page);

  const pill = page.locator("[data-world-online-toggle]");
  const roster = page.locator("[data-world-online-list]");
  await expect(page.locator("[data-world-online-count]")).toHaveText("3");
  await expect(pill).toHaveAttribute(
    "aria-label",
    "3 people online right now — show who is here",
  );

  // The roster stays out of the way until the pointer asks for it.
  const rosterOpacity = () =>
    roster.evaluate((node) => getComputedStyle(node).opacity);
  expect(await rosterOpacity()).toBe("0");
  await pill.hover();
  await expect.poll(rosterOpacity).toBe("1");

  // You are listed first; every other visitor follows in name order.
  await expect(roster.locator(".world-online-name")).toHaveText([
    /\(you\)$/,
    "Ada",
    "Zoe",
  ]);
  await expect(roster.locator(".world-online-activity").nth(2)).toHaveText(
    "Away",
  );

  // A roster row is the same door the avatar is: it opens the public profile.
  await roster.getByRole("button", { name: /^Ada/ }).click();
  await expect(page.locator("[data-world-detail]")).toHaveAttribute(
    "data-open",
    "true",
  );
  await expect(page.locator("#world-detail-title")).toContainText("Ada");

  // A departure is reflected without a reload.
  relay.send(JSON.stringify({ type: "leave", id: "peer-zoe" }));
  await expect(page.locator("[data-world-online-count]")).toHaveText("2");
  await expect(roster.locator(".world-online-name")).toHaveText([
    /\(you\)$/,
    "Ada",
  ]);
});

test("World logo opens a dashboard switcher instead of silently reloading", async ({
  page,
}) => {
  test.setTimeout(60_000);
  await prepareWorldPage(page, "world-brand-nav");
  await waitForWorld(page);

  const menu = page.locator("[data-world-brand-menu]");
  const trigger = page.getByRole("button", {
    name: "ForkMesh menu — go to a dashboard page",
  });
  const nav = page.locator("[data-world-brand-nav]");

  // Closed by default, and closed means out of the tab order — not merely
  // transparent — so the HUD's keyboard path is unchanged for anyone who
  // never opens the menu.
  await expect(trigger).toHaveAttribute("aria-expanded", "false");
  await expect(nav).toBeHidden();

  await trigger.click();
  await expect(trigger).toHaveAttribute("aria-expanded", "true");
  await expect(menu).toHaveAttribute("data-open", "true");
  await expect(nav).toBeVisible();

  await expect(nav.locator(".world-brand-nav-label")).toHaveText([
    "Overview",
    "Profile",
    "Repositories",
    "Tasks",
    "Network",
    "Chat",
    "Settings",
  ]);
  expect(
    await nav
      .locator("[data-world-brand-nav-link]")
      .evaluateAll((links) =>
        links.map((link) => [link.getAttribute("href"), link.target]),
      ),
  ).toEqual([
    ["/dashboard", "_top"],
    ["/dashboard/profile", "_top"],
    ["/dashboard/repos", "_top"],
    ["/dashboard/tasks", "_top"],
    ["/dashboard/network", "_top"],
    ["/dashboard/chat", "_top"],
    ["/dashboard/settings", "_top"],
    // Public pages ride along as chips under the dashboard list.
    ["/", "_top"],
    ["/docs", "_top"],
    ["/network", "_top"],
    ["/status", "_top"],
  ]);

  // The logo's original job survives as an explicit row.
  await expect(nav.locator("[data-world-logo-refresh]")).toContainText(
    "Reload World",
  );

  // Escape closes and hands focus back to the logo.
  await page.keyboard.press("Escape");
  await expect(menu).toHaveAttribute("data-open", "false");
  await expect(nav).toBeHidden();
  await expect(trigger).toBeFocused();

  // Clicking back into the world dismisses it as well.
  await trigger.click();
  await expect(menu).toHaveAttribute("data-open", "true");
  await page.locator("[data-world-canvas-wrap]").evaluate((wrap) => wrap.click());
  await expect(menu).toHaveAttribute("data-open", "false");
  await expect(nav).toBeHidden();

  // A keyboard opening lands on the first destination, but a pointer opening
  // leaves focus on the logo so a mouse user is never pulled out of the
  // canvas. This is why the panel's visibility is stepped rather than
  // transitioned: focus() on a still-hidden element is a silent no-op.
  await trigger.focus();
  await page.keyboard.press("Enter");
  await expect(nav.locator("[data-world-brand-nav-link]").first()).toBeFocused();
  await page.keyboard.press("Escape");
  await trigger.click();
  await expect(trigger).toBeFocused();
});
