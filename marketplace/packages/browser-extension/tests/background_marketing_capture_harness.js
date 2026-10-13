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

const writes = [];
function element(tagName, text = "", attributes = {}, childElementCount = 0, closest = () => null, children = []) {
  const target = {
    tagName, textContent: text, childElementCount, offsetParent: {},
    getAttribute: (name) => (name in attributes ? String(attributes[name]) : null),
    getAttributeNames: () => Object.keys(attributes),
    closest, querySelector: () => null, querySelectorAll: () => children,
  };
  return new Proxy(target, { set(object, key, value) { writes.push(String(key)); object[key] = value; return true; } });
}

function readPage(url, { title = "", metas = {}, links = {}, elements = [], ldJson = [], times = [], password = false, nodes = {} }) {
  const location = new URL(url);
  const select = (selector) => {
    if (selector in nodes) return nodes[selector];
    const meta = selector.match(/^meta\[property="([^"]+)"\]/);
    if (meta) return metas[meta[1]] ? element("META", "", { content: metas[meta[1]] }) : null;
    if (selector.startsWith('link[rel="canonical"]')) return links.canonical ? element("LINK", "", { href: links.canonical }) : null;
    if (selector.startsWith('link[rel="alternate"]')) return links.feed ? element("LINK", "", { href: links.feed }) : null;
    return null;
  };
  const selectAll = (selector) => {
    if (selector.startsWith('script[type="application/ld+json"]')) return ldJson.map((value) => element("SCRIPT", JSON.stringify(value)));
    if (selector === "time[datetime]") return times.map(([datetime, href, heading]) =>
      element("TIME", "", { datetime }, 0, (query) => (query === "a[href]" ? element("A", heading, { href }) : null)));
    if (selector === 'input[type="password"]') return password ? [element("INPUT")] : [];
    return [];
  };
  const context = {
    URL, JSON, Date, Number, String, Set, Map, Object, Array, Math, RegExp,
    location: { href: location.href, pathname: location.pathname, hostname: location.hostname },
    document: {
      title, readyState: "complete",
      querySelector: select, querySelectorAll: selectAll,
      body: { getElementsByTagName: () => elements.map(([text, attributes = {}, count = 0]) => element("SPAN", text, attributes, count)) },
    },
  };
  context.globalThis = context;
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(path.join(EXTENSION, "marketing-capture.js"), "utf8"), context, { filename: "marketing-capture.js" });
  return JSON.parse(JSON.stringify(context.__forkmeshMarketingRead()));
}

const pages = {
  "https://www.instagram.com/acme/": readPage("https://www.instagram.com/acme/", {
    title: "Acme (@acme) • Instagram photos and videos",
    metas: { "og:description": "1,234 Followers, 56 Following, 78 Posts - See Instagram photos and videos from Acme (@acme)",
      "og:image": "https://cdn.example.com/acme.jpg" },
    links: { canonical: "https://www.instagram.com/acme/" },
    times: [[new Date(Date.now() - 2 * 86400000).toISOString(), "/p/abc/", "Launch day"],
      [new Date(Date.now() - 40 * 86400000).toISOString(), "/p/old/", "Older post"]],
  }),
  "https://www.youtube.com/@acme": readPage("https://www.youtube.com/@acme", {
    title: "Acme - YouTube",
    elements: [["Home"], ["@acme"], ["1.23K subscribers"], ["45 videos"], ["Subscribe"], ["12K views"]],
  }),
  "https://www.trustpilot.com/review/acme.example": readPage("https://www.trustpilot.com/review/acme.example", {
    title: "Acme Reviews | Read Customer Service Reviews",
    ldJson: [{ "@type": "Organization", aggregateRating: { ratingValue: "4.6", reviewCount: "321", bestRating: "5" } }],
  }),
  "https://www.linkedin.com/company/acme/": readPage("https://www.linkedin.com/company/acme/", {
    title: "Sign In | LinkedIn", password: true, elements: [["Sign in"], ["Email or phone"]],
  }),
  "https://www.reddit.com/r/acme/": readPage("https://www.reddit.com/r/acme/", {
    title: "r/acme", elements: [["1.2K upvotes"], ["45 comments"], ["Members 99"]],
    nodes: { "shreddit-subreddit-header": element("SHREDDIT-SUBREDDIT-HEADER", "",
      { subscribers: "5321", active: "12", "weekly-active-users": "1840", "weekly-contributions": "212", "prefixed-name": "r/acme" }) },
    times: [[new Date(Date.now() - 3 * 86400000).toISOString(), "/r/acme/comments/abc/launch/", "Launch thread"]],
  }),
  "https://www.reddit.com/r/visitors/": readPage("https://www.reddit.com/r/visitors/", {
    title: "r/visitors",
    nodes: { "shreddit-subreddit-header": element("SHREDDIT-SUBREDDIT-HEADER", "", { "prefixed-name": "r/visitors" }, 2, undefined,
      [element("DIV", "3.4K weekly visitors"), element("DIV", "120 weekly contributions")]) },
  }),
  "https://old.reddit.com/r/acme/": readPage("https://old.reddit.com/r/acme/", {
    title: "acme", elements: [["7 points"]],
    nodes: { ".side .subscribers .number": element("SPAN", "5,321") },
  }),
  "https://github.com/acme": readPage("https://github.com/acme", {
    title: "Acme · GitHub", elements: [["Followers 2.5k", {}, 1], ["Repositories 18", {}, 1]],
  }),
};

check("an Instagram profile's description gives followers, following and posts",
  pages["https://www.instagram.com/acme/"].metrics.followers === 1234
  && pages["https://www.instagram.com/acme/"].metrics.following === 56
  && pages["https://www.instagram.com/acme/"].metrics.posts === 78, JSON.stringify(pages["https://www.instagram.com/acme/"]));
check("dated posts give the last post and the 30-day count",
  pages["https://www.instagram.com/acme/"].postsLast30d === 1
  && pages["https://www.instagram.com/acme/"].recent[0].url === "https://www.instagram.com/p/abc/"
  && pages["https://www.instagram.com/acme/"].image === "https://cdn.example.com/acme.jpg");
check("abbreviated counts on the page are expanded",
  pages["https://www.youtube.com/@acme"].metrics.followers === 1230
  && pages["https://www.youtube.com/@acme"].metrics.posts === 45
  && pages["https://www.youtube.com/@acme"].metrics.views === 12000, JSON.stringify(pages["https://www.youtube.com/@acme"].metrics));
check("structured data gives the rating and the review count",
  pages["https://www.trustpilot.com/review/acme.example"].rating === 4.6
  && pages["https://www.trustpilot.com/review/acme.example"].metrics.reviews === 321);
check("a label before its number is read too",
  pages["https://github.com/acme"].metrics.followers === 2500
  && pages["https://github.com/acme"].metrics.posts === 18, JSON.stringify(pages["https://github.com/acme"].metrics));
check("a subreddit's header gives members, weekly visitors and weekly contributions, not post scores",
  JSON.stringify(pages["https://www.reddit.com/r/acme/"].metrics) === JSON.stringify({ followers: 5321, reach: 1840, engagements: 212 })
  && pages["https://www.reddit.com/r/acme/"].handle === "r/acme"
  && pages["https://www.reddit.com/r/acme/"].postsLast30d === 1, JSON.stringify(pages["https://www.reddit.com/r/acme/"]));
check("the subreddit header's visible labels are read when it has no count attributes",
  pages["https://www.reddit.com/r/visitors/"].metrics.reach === 3400
  && pages["https://www.reddit.com/r/visitors/"].metrics.engagements === 120, JSON.stringify(pages["https://www.reddit.com/r/visitors/"].metrics));
check("old Reddit's sidebar gives the reader count",
  JSON.stringify(pages["https://old.reddit.com/r/acme/"].metrics) === JSON.stringify({ followers: 5321 }), JSON.stringify(pages["https://old.reddit.com/r/acme/"].metrics));
check("a sign-in wall is reported, not recorded",
  pages["https://www.linkedin.com/company/acme/"].signInNeeded === true
  && Object.keys(pages["https://www.linkedin.com/company/acme/"].metrics).length === 0);
check("the reader never writes to the page", writes.length === 0, writes.join());

const storage = {};
const session = {};
const fetched = [];
const created = [];
const removed = [];
const updated = [];
const notified = [];
const granted = new Set();
const injected = [];
const tabs = new Map();
let nextTab = 100;
let listener = null;
const noop = { addListener() {} };
const chrome = {
  runtime: {
    id: "ext", getURL: (suffix) => `chrome-extension://ext/${suffix}`,
    onMessage: { addListener(callback) { listener = callback; } },
    onInstalled: noop, onStartup: noop,
  },
  alarms: { onAlarm: noop, async create() {}, async clear() {} },
  permissions: { onRemoved: noop, contains: async ({ origins }) => origins.every((origin) => granted.has(origin)) },
  scripting: {
    getRegisteredContentScripts: async () => [], registerContentScripts: async () => {}, unregisterContentScripts: async () => {},
    async executeScript({ target, files }) {
      injected.push(tabs.get(target.tabId)?.url || "");
      if (files) return [{ result: undefined }];
      return [{ result: pages[tabs.get(target.tabId)?.url] || null }];
    },
  },
  storage: {
    local: {
      async get(keys) { return Object.fromEntries(keys.filter((key) => key in storage).map((key) => [key, JSON.parse(JSON.stringify(storage[key]))])); },
      async set(values) { Object.assign(storage, JSON.parse(JSON.stringify(values))); },
      async remove(keys) { for (const key of keys) delete storage[key]; },
    },
    session: {
      async get(keys) { return Object.fromEntries(keys.filter((key) => key in session).map((key) => [key, JSON.parse(JSON.stringify(session[key]))])); },
      async set(values) { Object.assign(session, JSON.parse(JSON.stringify(values))); },
    },
  },
  tabs: {
    async create({ url }) { const id = nextTab++; tabs.set(id, { id, url, status: "complete" }); created.push(url); return { id }; },
    async get(id) { return tabs.get(id) || null; },
    async update(id, options) { updated.push([id, options]); if (options.url) tabs.set(id, { id, url: options.url, status: "complete" }); return { id }; },
    async remove(id) { removed.push(id); tabs.delete(id); },
    async sendMessage(id, message) { notified.push([id, message]); },
  },
};
const backgroundContext = {
  chrome, URL, URLSearchParams, console, Promise, crypto: globalThis.crypto,
  setTimeout: (callback) => setImmediate(callback),
  importScripts() {},
  ForkMeshVaultBackground: { install: () => ({ forget: async () => {} }) },
  async fetch(url, options) {
    const target = new URL(url);
    const body = options.body === undefined ? null : JSON.parse(options.body);
    fetched.push({ method: options.method, path: target.pathname, body });
    const reply = await rpc({ actor: "charlie", method: options.method, path: target.pathname + target.search, body });
    return { ok: reply.status < 400, status: reply.status, json: async () => reply.data };
  },
};
backgroundContext.self = backgroundContext;
vm.createContext(backgroundContext);
vm.runInContext(fs.readFileSync(path.join(EXTENSION, "background.js"), "utf8"), backgroundContext, { filename: "background.js" });

const DASHBOARD = { id: "ext", url: "https://app.forkmesh.com/dashboard/marketing?org=acme", tab: { id: 5 } };
const OTHER = { id: "ext", url: "https://evil.example/dashboard/marketing", tab: { id: 6 } };
const CAPTURE = { id: "ext", url: "chrome-extension://ext/capture.html?job=x", tab: { id: 7 } };
function message(request, sender) {
  return new Promise((resolve) => {
    if (!listener(request, sender, resolve)) setImmediate(() => resolve(undefined));
  });
}
async function settle(channel) {
  for (let round = 0; round < 1000; round += 1) {
    const last = notified.filter(([, item]) => item.job.channel === channel).pop();
    if (last && ["done", "error"].includes(last[1].job.state)) return last[1].job;
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  return null;
}

(async () => {
  const alice = (method, apiPath, body) => rpc({ actor: "alice", method, path: apiPath, body });
  await alice("PUT", "/api/marketing/acme/settings", { channels: [
    { id: "instagram", profileUrl: "https://www.instagram.com/acme/" },
    { id: "youtube", profileUrl: "https://www.youtube.com/@acme" },
    { id: "linkedin", profileUrl: "https://www.linkedin.com/company/acme/" },
    { id: "trustpilot", profileUrl: "" },
    { id: "discord", profileUrl: "https://discord.gg/acme" },
  ] });
  await rpc({ control: "outbound", url: "https://discord.com/api/v10/invites/acme?with_counts=true",
    data: { guild: { name: "Acme" }, approximate_member_count: 420, approximate_presence_count: 37 } });
  Object.assign(storage, { sessionToken: "token", account: "charlie" });

  const foreign = await message({ type: "fm-marketing-capture", organization: "acme", channel: "instagram" }, OTHER);
  check("only the Marketing dashboard may start an update", foreign.error === "forbidden_sender" && fetched.length === 0);
  const pageOnly = await message({ type: "fm-marketing-capture-continue", job: "x", granted: true }, DASHBOARD);
  check("only the extension's own page may grant", pageOnly.error === "forbidden_sender");

  granted.add("https://www.instagram.com/*");
  const started = await message({ type: "fm-marketing-capture", organization: "acme", channel: "instagram" }, DASHBOARD);
  check("Update opens the channel's own page", started.ok && started.job.state === "reading"
    && created.at(-1) === "https://www.instagram.com/acme/", JSON.stringify(started));
  const done = await settle("instagram");
  check("the page is read and recorded", done && done.state === "done" && done.result.metrics.followers === 1234,
    JSON.stringify(done));
  check("the dashboard tab is told and brought back, the channel tab closed",
    notified.every(([id]) => id === 5) && updated.some(([id, options]) => id === 5 && options.active) && removed.length === 1);
  check("only the capture endpoint and the activity line are written to", fetched.filter((item) => item.method === "POST")
    .every((item) => ["/api/marketing/acme/capture", "/api/marketing/acme/activity"].includes(item.path)));

  const asked = await message({ type: "fm-marketing-capture", organization: "acme", channel: "youtube" }, DASHBOARD);
  check("a site without access opens the permission page first", asked.ok && asked.job.state === "permission"
    && created.at(-1).startsWith("chrome-extension://ext/capture.html?job="));
  const jobId = new URL(created.at(-1)).searchParams.get("job");
  const job = await message({ type: "fm-marketing-capture-job", job: jobId }, CAPTURE);
  check("the permission page asks for that one site", job.ok && job.origin === "https://www.youtube.com/*");
  granted.add("https://www.youtube.com/*");
  const continued = await message({ type: "fm-marketing-capture-continue", job: jobId, granted: true }, CAPTURE);
  check("granting continues in the same tab", continued.ok && continued.job.state === "reading"
    && updated.some(([id, options]) => id === 7 && options.url === "https://www.youtube.com/@acme"));
  const youtube = await settle("youtube");
  check("the channel is updated after the grant", youtube && youtube.state === "done" && youtube.result.metrics.followers === 1230);

  granted.add("https://www.linkedin.com/*");
  await message({ type: "fm-marketing-capture", organization: "acme", channel: "linkedin" }, DASHBOARD);
  const wall = await settle("linkedin");
  check("a sign-in wall fails with a reason and keeps the tab", wall && wall.state === "error"
    && wall.error === "capture_sign_in_needed" && removed.length === 2, JSON.stringify(wall));
  const noLink = await message({ type: "fm-marketing-capture", organization: "acme", channel: "trustpilot" }, DASHBOARD);
  check("a channel without a link asks for one", noLink.ok === false && noLink.error === "channel_url_required");

  const tabsBefore = created.length;
  const discordStart = await message({ type: "fm-marketing-capture", organization: "acme", channel: "discord" }, DASHBOARD);
  const discord = await settle("discord");
  check("Discord updates from its public invite without opening, reading or closing a Discord tab",
    discordStart.ok && discord && discord.state === "done" && discord.result.metrics.followers === 420
    && created.length === tabsBefore && removed.length === 2
    && !injected.some((url) => /discord/.test(url))
    && fetched.filter((item) => item.method === "POST" && !item.path.endsWith("/activity")).at(-1).path === "/api/marketing/acme/refresh"
    && fetched.filter((item) => item.method === "POST" && !item.path.endsWith("/activity")).at(-1).body.channel === "discord", JSON.stringify(discord));
  const discordTabId = nextTab++;
  tabs.set(discordTabId, { id: discordTabId, url: "https://discord.com/channels/1/2", status: "complete" });
  const discordTab = await message({ type: "fm-marketing-capture-tab", organization: "acme", channel: "discord", tabId: discordTabId }, { id: "ext", url: "chrome-extension://ext/popup.html" });
  check("the popup never reads an open Discord tab either", discordTab.ok && discordTab.job.state === "done"
    && !injected.some((url) => /discord/.test(url)), JSON.stringify(discordTab));
  const setup = await message({ type: "fm-marketing-setup", organization: "acme" }, { id: "ext", url: "chrome-extension://ext/popup.html" });
  check("the popup does not ask for Discord site access", setup.ok
    && setup.channels.find((item) => item.id === "discord").server === true
    && setup.channels.filter((item) => item.server).length === 1, JSON.stringify(setup.channels));

  const overview = (await alice("GET", "/api/marketing/acme")).data;
  const instagram = overview.summary.channels.find((item) => item.channel === "instagram");
  check("Marketing shows the recorded numbers and their source", instagram.followers === 1234
    && instagram.posts === 78 && instagram.health.source === "extension" && instagram.health.checks.find((item) => item.id === "fresh").ok);
  const setting = overview.settings.channels.find((item) => item.id === "instagram");
  check("the channel keeps what the page showed", setting.capture && setting.capture.title.startsWith("Acme")
    && setting.capture.recent.length === 2 && setting.capture.by === "charlie" && setting.capture.feedUrl === "", JSON.stringify(setting));
  check("the popup sees the history", (storage.marketingCaptures || []).length === 5);

  server.kill();
  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); server.kill(); process.exit(1); });
