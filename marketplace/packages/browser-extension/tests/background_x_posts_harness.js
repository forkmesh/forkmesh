"use strict";
const fs = require("fs"), path = require("path"), vm = require("vm");
const { spawn } = require("child_process");
const readline = require("readline");

const EXTENSION = path.join(__dirname, "..", "content", "extension");
const server = spawn(process.env.PYTHON || "python3", [path.join(__dirname, "marketing_stdio_server.py")],
  { stdio: ["pipe", "pipe", "inherit"] });
const replies = [];
readline.createInterface({ input: server.stdout }).on("line", (line) => replies.shift()(JSON.parse(line)));
const rpc = (message) => new Promise((resolve) => { replies.push(resolve); server.stdin.write(JSON.stringify(message) + "\n"); });

let checks = 0;
function check(name, ok, detail = "") {
  checks += 1;
  if (!ok) { console.error(`FAIL ${name} ${detail}`); server.kill(); process.exit(1); }
  console.log("ok   " + name);
}

const storage = {};
const session = {};
const liveTimers = [];
const xTabs = new Map();
const xPages = {};
const fetched = [];
let inflight = 0;
let listener = null;
const noop = { addListener() {} };
const chrome = {
  runtime: {
    id: "ext", getURL: (suffix) => `chrome-extension://ext/${suffix}`,
    onMessage: { addListener(callback) { listener = callback; } },
    onInstalled: noop, onStartup: noop,
  },
  permissions: { onRemoved: noop, contains: async () => false },
  scripting: {
    getRegisteredContentScripts: async () => [], registerContentScripts: async () => {}, unregisterContentScripts: async () => {},
    async executeScript({ target, files }) { return [{ result: files ? undefined : xPages[xTabs.get(target.tabId)?.url] || null }]; },
  },
  storage: { local: {
    async get(keys) { return Object.fromEntries(keys.filter((key) => key in storage).map((key) => [key, JSON.parse(JSON.stringify(storage[key]))])); },
    async set(values) { Object.assign(storage, JSON.parse(JSON.stringify(values))); },
    async remove(keys) { for (const key of keys) delete storage[key]; },
  }, session: {
    async get(keys) { return Object.fromEntries(keys.filter((key) => key in session).map((key) => [key, JSON.parse(JSON.stringify(session[key]))])); },
    async set(values) { Object.assign(session, JSON.parse(JSON.stringify(values))); },
    async remove(keys) { for (const key of [keys].flat()) delete session[key]; },
  } },
  tabs: { async create() { return { id: 1 }; }, async get(id) { return xTabs.get(id) || null; }, async sendMessage() {} },
  alarms: { onAlarm: noop, async create() {}, async clear() {} },
};
const backgroundContext = {
  chrome, URL, URLSearchParams, console, Promise, crypto: globalThis.crypto,
  setTimeout(callback, delay) {
    if (delay < 10000) return setTimeout(callback, delay);
    liveTimers.push({ callback, delay });
    return liveTimers.length;
  },
  clearTimeout(id) { if (liveTimers[id - 1]) liveTimers[id - 1].callback = () => {}; },
  importScripts() {},
  ForkMeshVaultBackground: { install: () => ({ forget: async () => {} }) },
  async fetch(url, options) {
    const target = new URL(url);
    const body = options.body === undefined ? null : JSON.parse(options.body);
    fetched.push({ method: options.method, path: target.pathname, body });
    inflight += 1;
    const reply = await rpc({ actor: "charlie", method: options.method, path: target.pathname + target.search, body });
    inflight -= 1;
    return { ok: reply.status < 400, status: reply.status, json: async () => reply.data };
  },
};
backgroundContext.self = backgroundContext;
vm.createContext(backgroundContext);
vm.runInContext(fs.readFileSync(path.join(EXTENSION, "background.js"), "utf8"), backgroundContext, { filename: "background.js" });

const POPUP = { id: "ext", url: "chrome-extension://ext/popup.html" };
const X_TAB = { id: "ext", url: "https://x.com/forkmesh", tab: { id: 7 } };
const OTHER_X_TAB = { id: "ext", url: "https://x.com/forkmesh", tab: { id: 8 } };
function message(request, sender) {
  return new Promise((resolve) => {
    if (!listener(request, sender, resolve)) setImmediate(() => resolve(undefined));
  });
}

function element(attributes = {}, children = {}, content = "", tagName = "DIV") {
  const node = {
    tagName,
    owner: null,
    children: [],
    getAttribute: (name) => (name in attributes ? String(attributes[name]) : null),
    matches: (selector) => selector.split(",").some((one) => {
      const match = one.trim().match(/^\[([\w-]+)="([^"]*)"\]$/);
      return Boolean(match && attributes[match[1]] === match[2]);
    }),
    querySelector: (selector) => children[selector] || null,
    querySelectorAll: (selector) => children[`all:${selector}`] || (children[selector] ? [children[selector]] : []),
    closest: (selector) => (selector === "article"
      ? (tagName === "ARTICLE" ? node : node.owner) : children.closest || null),
    contains: (other) => (children.inside || []).includes(other),
    cloneNode: () => ({ ...node, querySelectorAll: () => [] }),
    textContent: content,
  };
  return node;
}

function article(attributes, children) {
  const node = element(attributes, children, "", "ARTICLE");
  for (const [key, value] of Object.entries(children)) {
    if (key === "closest" || key === "inside") continue;
    for (const child of Array.isArray(value) ? value : [value]) child.owner ||= node;
  }
  return node;
}

function tweet(author, id, at, text, counts, media = {}) {
  const link = element({ href: `/${author}/status/${id}` });
  const time = element({ datetime: new Date(at).toISOString() }, { closest: link });
  const quote = element({ role: "link" });
  const images = [
    ...(media.avatar ? [element({ src: media.avatar }, {}, "", "IMG")] : []),
    ...(media.photos || []).map((src) => element({ src }, {}, "", "IMG")),
    ...(media.posters || []).map((poster) => element({ poster }, {}, "", "VIDEO")),
    ...(media.quoted || []).map((src) => element({ src }, { closest: quote }, "", "IMG")),
  ];
  return article({ "data-testid": "tweet" }, {
    "all:img[src], video[poster]": images,
    inside: [quote],
    'a[href*="/status/"] time': time,
    "time[datetime]": time,
    '[data-testid="tweetText"]': element({}, {}, text),
    '[data-testid="reply"], [data-engagement-action="reply"]': element({ "aria-label": `${counts.replies} Replies. Reply` }),
    '[data-testid="retweet"], [data-testid="unretweet"], [data-engagement-action="retweet"]': element({ "aria-label": `${counts.reposts} reposts. Repost` }),
    '[data-testid="like"], [data-testid="unlike"], [data-engagement-action="like"]': element({ "aria-label": `${counts.likes} Likes. Like` }),
    'a[href$="/analytics"], [data-engagement-action="generic"]': element({ "aria-label": `${counts.views} views. View post analytics` }),
  });
}

function signedOutPost(author, id, text, counts, quotedText = "") {
  const outer = {};
  const inner = quotedText ? article({}, {
    'a[href*="/status/"]': element({ href: `/someone/status/1800000000000000000` }),
    'div[dir="auto"]': element({}, {}, quotedText),
  }) : null;
  const texts = [...(inner ? [inner.querySelector('div[dir="auto"]')] : []), element({}, {}, text)];
  const post = article({}, Object.assign(outer, {
    'a[href*="/status/"]': element({ href: `/${author}/status/${id}` }),
    'all:div[dir="auto"]': texts.slice(inner ? 1 : 0),
    '[data-testid="reply"], [data-engagement-action="reply"]': element({ "aria-label": "Reply" }, {}, counts.replies),
    '[data-testid="retweet"], [data-testid="unretweet"], [data-engagement-action="retweet"]': element({ "aria-label": "Repost" }, {}, counts.reposts),
    '[data-testid="like"], [data-testid="unlike"], [data-engagement-action="like"]': element({ "aria-label": "Like" }, {}, counts.likes),
    'a[href$="/analytics"], [data-engagement-action="generic"]': element({ "aria-label": "View count" }, {}, counts.views),
  }));
  post.querySelectorAll = (selector) => (selector === 'div[dir="auto"]' ? texts
    : outer[`all:${selector}`] || (outer[selector] ? [outer[selector]] : []));
  if (inner) inner.parentElement = { closest: (selector) => (selector === "article" ? post : null) };
  return inner ? [post, inner] : [post];
}

async function scrollProfile(articles, links = [], sender = X_TAB) {
  const timers = [];
  const context = {
    URL, URLSearchParams, JSON, Date, Number, String, Object, Array, Promise, Math,
    location: { pathname: "/forkmesh", search: "", href: "https://x.com/forkmesh" },
    document: {
      visibilityState: "visible",
      querySelectorAll: (selector) => (selector === 'article[data-testid="tweet"], article:not([data-testid])' ? articles
        : selector === 'a[href^="/"]' ? links : []),
      querySelector: () => null,
    },
    setTimeout: (callback) => { timers.push(callback); return timers.length; },
    chrome: { runtime: { id: "ext", lastError: undefined, sendMessage(request, callback) {
      void message(request, sender).then((reply) => callback(reply));
    } } },
  };
  context.window = context;
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(path.join(EXTENSION, "x.js"), "utf8"), context, { filename: "x.js" });
  for (let wait = 0; wait < 50 && !timers.length; wait += 1) await new Promise((resolve) => setTimeout(resolve, 10));
  return timers.length;
}

(async () => {
  Object.assign(storage, { sessionToken: "token", account: "charlie" });
  const day = 86400000;
  const now = Date.now();
  const ticked = await scrollProfile([
    tweet("forkmesh", "1900000000000000001", now - 2 * day, "Launch day: mirrors for every repo",
      { replies: 4, reposts: 6, likes: 40, views: "1.2K" }, {
        avatar: "https://pbs.twimg.com/profile_images/1/forkmesh_normal.jpg",
        photos: ["https://pbs.twimg.com/media/GlaunchA?format=jpg&name=900x900", "https://abs.twimg.com/emoji/v2/svg/1f680.svg"],
        posters: ["https://pbs.twimg.com/ext_tw_video_thumb/19/pu/img/demo.jpg"],
        quoted: ["https://pbs.twimg.com/media/Gquoted?format=jpg&name=small"],
      }),
    tweet("someone", "1900000000000000009", now - day, "Not ours", { replies: 1, reposts: 1, likes: 1, views: 10 }),
  ]);
  check("x.js reads the profile once and waits for the next tick", ticked === 1);
  for (let count = -1; inflight || count !== fetched.length;) {
    count = fetched.length;
    await new Promise((resolve) => setTimeout(resolve, 40));
  }
  const readWrites = fetched.filter((call) => call.method !== "GET");
  check("reading the profile sends only its activity line", readWrites.length === 1
    && readWrites[0].path === "/api/marketing/acme/activity" && readWrites[0].body.kind === "read"
    && readWrites[0].body.text.startsWith("Read @forkmesh on X (profile)"), JSON.stringify(fetched));
  fetched.length = 0;
  const kept = storage.xStats?.posts?.["1900000000000000001"] || {};
  check("the post's text and counts are kept", kept.text === "Launch day: mirrors for every repo"
    && kept.views === 1200 && kept.likes === 40, JSON.stringify(kept));
  check("another account's post is not kept", !storage.xStats.posts["1900000000000000009"]);
  check("the post's own photo and video poster are kept, not the avatar, emoji or quoted post",
    JSON.stringify(kept.images) === JSON.stringify(["https://pbs.twimg.com/media/GlaunchA?format=jpg&name=small",
      "https://pbs.twimg.com/ext_tw_video_thumb/19/pu/img/demo.jpg"]), JSON.stringify(kept.images));

  await scrollProfile([
    tweet("forkmesh", "1900000000000000002", now - 5 * day, "Older post, read after scrolling",
      { replies: 0, reposts: 1, likes: 9, views: 300 }),
  ]);
  check("scrolling adds the next posts", Object.keys(storage.xStats.posts).length === 2);
  const stats = await message({ type: "fm-x-stats" }, POPUP);
  check("the popup gets the most recent posts first, with their images",
    stats.recent.map((post) => post.id).join() === "1900000000000000001,1900000000000000002"
    && stats.recent[0].images.length === 2 && stats.recent[0].url === "https://x.com/forkmesh/status/1900000000000000001"
    && stats.recent[1].images.length === 0, JSON.stringify(stats.recent));

  const synced = await message({ type: "fm-x-sync" }, POPUP);
  check("Sync now succeeds", synced.ok, JSON.stringify(synced));
  check("Sync now adds both post links", synced.lastSync.linksAdded === 2 && synced.lastSync.linksUpdated === 0,
    JSON.stringify(synced.lastSync));
  const sent = fetched.filter((call) => call.path === "/api/marketing/acme/posts");
  check("posts go to Marketing in one request", sent.length === 1 && sent[0].body.posts.length === 2);
  const listed = await rpc({ actor: "alice", method: "GET", path: "/api/marketing/acme/posts" });
  const byUrl = Object.fromEntries(listed.data.posts.map((post) => [post.url, post]));
  const launch = byUrl["https://x.com/forkmesh/status/1900000000000000001"] || {};
  check("the desktop sees the X post with its stats", launch.channel === "x" && launch.source === "extension"
    && launch.title === "Launch day: mirrors for every repo" && launch.views === 1200 && launch.engagements === 50,
    JSON.stringify(launch));
  check("the older post is there too", byUrl["https://x.com/forkmesh/status/1900000000000000002"]?.likes === 9);
  check("the X post's images reach Marketing", launch.images?.length === 2
    && launch.images[0] === "https://pbs.twimg.com/media/GlaunchA?format=jpg&name=small", JSON.stringify(launch.images));

  fetched.length = 0;
  const again = await message({ type: "fm-x-sync" }, POPUP);
  check("a second sync sends no post that was not read again", again.ok
    && !fetched.some((call) => call.path === "/api/marketing/acme/posts"));

  await new Promise((resolve) => setTimeout(resolve, 5));
  await scrollProfile([
    tweet("forkmesh", "1900000000000000001", now - 2 * day, "Launch day: mirrors for every repo",
      { replies: 5, reposts: 6, likes: 55, views: "2K" }),
  ]);
  const later = await message({ type: "fm-x-sync" }, POPUP);
  check("reading a post again updates its link", later.ok && later.lastSync.linksUpdated === 1
    && later.lastSync.linksAdded === 0, JSON.stringify(later.lastSync));
  const updated = await rpc({ actor: "alice", method: "GET", path: `/api/marketing/acme/posts/${launch.id}` });
  check("the newer numbers replace the old ones", updated.data.post.views === 2000 && updated.data.post.likes === 55,
    JSON.stringify(updated.data.post));
  check("a read before the images load keeps the images already seen",
    storage.xStats.posts["1900000000000000001"].images.length === 2 && updated.data.post.images.length === 2,
    JSON.stringify(updated.data.post.images));

  const runLive = async () => {
    const pending = liveTimers.splice(0);
    for (const timer of pending) timer.callback();
    const before = fetched.length;
    for (let round = 0; round < 100; round += 1) {
      await new Promise((resolve) => setTimeout(resolve, 10));
      if (fetched.length > before && round > 5) break;
    }
    return pending.length;
  };
  await rpc({ actor: "alice", method: "PUT", path: "/api/marketing/acme/settings",
    body: { channels: [{ id: "x", profileUrl: "https://x.com/forkmesh" }] } });
  xTabs.set(7, { id: 7, url: "https://x.com/forkmesh", status: "complete" });
  xPages["https://x.com/forkmesh"] = { url: "https://x.com/forkmesh", title: "ForkMesh (@forkmesh) / X",
    handle: "forkmesh", metrics: { followers: 1400, following: 80 } };
  await new Promise((resolve) => setTimeout(resolve, 5));
  await scrollProfile([
    tweet("forkmesh", "1900000000000000003", now - day / 2, "Newest post, read before the click",
      { replies: 1, reposts: 2, likes: 12, views: 400 }),
  ]);
  check("reading the profile before the click starts nothing live", liveTimers.length === 0 && !session.xLive);
  fetched.length = 0;
  const clicked = await message({ type: "fm-marketing-capture-tab", organization: "acme", channel: "x", tabId: 7 }, POPUP);
  check("Update X stats from this page records the page", clicked.ok && clicked.job.state === "done"
    && clicked.job.result.metrics.followers === 1400, JSON.stringify(clicked));
  check("the click adds the latest posts read on the profile", clicked.job.result.posts?.linksAdded === 1
    && clicked.job.result.posts.linksUpdated === 0, JSON.stringify(clicked.job.result.posts));
  check("the click records one snapshot, from the page", fetched.filter((call) => call.method === "POST").map((call) => call.path).join()
    === "/api/marketing/acme/capture,/api/marketing/acme/posts", JSON.stringify(fetched));
  const newest = await rpc({ actor: "alice", method: "GET", path: "/api/marketing/acme/posts" });
  check("the newest post is in Marketing", newest.data.posts.some((post) =>
    post.url === "https://x.com/forkmesh/status/1900000000000000003" && post.likes === 12));
  check("this tab is now live", session.xLive?.tabId === 7 && session.xLive.organization === "acme"
    && (await message({ type: "fm-x-stats" }, POPUP)).live?.tabId === 7, JSON.stringify(session.xLive));

  fetched.length = 0;
  await new Promise((resolve) => setTimeout(resolve, 5));
  await scrollProfile([
    tweet("forkmesh", "1900000000000000004", now - day / 4, "Posted while the page was open",
      { replies: 0, reposts: 0, likes: 3, views: 90 }),
  ], [element({ href: "/forkmesh/verified_followers" }, {}, "1,500 Followers")]);
  check("a new post on the live tab is queued, not sent at once", liveTimers.length === 1 && fetched.length === 0);
  await runLive();
  const live = await rpc({ actor: "alice", method: "GET", path: "/api/marketing/acme/posts" });
  check("the new post reaches Marketing without another click", live.data.posts.some((post) =>
    post.url === "https://x.com/forkmesh/status/1900000000000000004" && post.views === 90), JSON.stringify(fetched));
  check("a follower change within a minute of the last snapshot waits for the next one",
    !fetched.some((call) => call.path === "/api/marketing/acme/metrics" && call.method === "POST")
    && liveTimers.length === 1 && liveTimers[0].delay <= 60000, JSON.stringify(liveTimers));
  check("the live count adds up", session.xLive.linksAdded === 2, JSON.stringify(session.xLive));

  storage.xStats.lastSync.recordedAt -= 60000;
  fetched.length = 0;
  await runLive();
  const metrics = await rpc({ actor: "alice", method: "GET", path: "/api/marketing/acme/metrics" });
  check("the new follower count is recorded once the minute is up",
    fetched.filter((call) => call.path === "/api/marketing/acme/metrics" && call.method === "POST").length === 1
    && metrics.data.metrics.find((metric) => metric.channel === "x").followers === 1500, JSON.stringify(fetched));

  await new Promise((resolve) => setTimeout(resolve, 5));
  await scrollProfile([
    tweet("forkmesh", "1900000000000000005", now - day / 8, "Read in another tab",
      { replies: 0, reposts: 0, likes: 1, views: 5 }),
  ], [], OTHER_X_TAB);
  check("another X tab is not live", liveTimers.length === 0);

  const stopped = await message({ type: "fm-x-live-stop" }, POPUP);
  check("Stop live updates ends it", stopped.ok && stopped.live === null && !session.xLive);
  check("only the popup may stop it", (await message({ type: "fm-x-live-stop" }, X_TAB)).error === "forbidden_sender");
  await new Promise((resolve) => setTimeout(resolve, 5));
  await scrollProfile([
    tweet("forkmesh", "1900000000000000006", now - day / 16, "After stopping",
      { replies: 0, reposts: 0, likes: 1, views: 5 }),
  ]);
  check("after stopping, reading queues nothing", liveTimers.length === 0);

  await new Promise((resolve) => setTimeout(resolve, 5));
  const postedAt = now - day;
  const signedOutId = String(BigInt(postedAt - 1288834974657) << 22n);
  await scrollProfile(signedOutPost("forkmesh", signedOutId, "Code healthy with forkmesh.com",
    { replies: "", reposts: "2", likes: "1", views: "1.2K" }, "A quoted post"));
  const signedOut = storage.xStats.posts[signedOutId] || {};
  check("the signed-out profile's post is read, dated by its id", signedOut.at === postedAt
    && signedOut.text === "Code healthy with forkmesh.com" && signedOut.likes === 1 && signedOut.reposts === 2
    && signedOut.replies === 0 && signedOut.views === 1200, JSON.stringify(signedOut));
  check("the quoted post inside it is not read as a post", !storage.xStats.posts["1800000000000000000"]);
  const signedOutSync = await message({ type: "fm-x-sync" }, POPUP);
  const signedOutLinks = await rpc({ actor: "alice", method: "GET", path: "/api/marketing/acme/posts" });
  const signedOutLink = signedOutLinks.data.posts.find((post) => post.url === `https://x.com/forkmesh/status/${signedOutId}`) || {};
  check("Sync now adds the signed-out profile's post as a link", signedOutSync.ok
    && signedOutLink.title === "Code healthy with forkmesh.com" && signedOutLink.postedAt === postedAt
    && signedOutLink.views === 1200, JSON.stringify(signedOutLink));

  server.kill();
  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); server.kill(); process.exit(1); });
