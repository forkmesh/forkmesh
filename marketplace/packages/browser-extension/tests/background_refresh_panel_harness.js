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

const area = (store) => ({
  async get(keys) { return Object.fromEntries([keys].flat().filter((key) => key in store).map((key) => [key, JSON.parse(JSON.stringify(store[key]))])); },
  async set(values) { Object.assign(store, JSON.parse(JSON.stringify(values))); },
  async remove(keys) { for (const key of [keys].flat()) delete store[key]; },
});
const storage = {};
const session = {};
const fetched = [];
const injected = [];
const broadcast = [];
const badges = new Map();
const tabs = new Map();
const tabEvents = { activated: [], updated: [] };
let listener = null;
const noop = { addListener() {} };
const chrome = {
  runtime: {
    id: "ext", getURL: (suffix) => `chrome-extension://ext/${suffix}`,
    onMessage: { addListener(callback) { listener = callback; } },
    onInstalled: noop, onStartup: noop,
    async sendMessage(request) { broadcast.push(request); },
  },
  alarms: { onAlarm: noop, async create() {}, async clear() {} },
  permissions: { onAdded: noop, onRemoved: noop, contains: async () => false },
  scripting: {
    async getRegisteredContentScripts() { return []; },
    async registerContentScripts(scripts) { injected.push(["register", scripts]); },
    async unregisterContentScripts() {},
    async executeScript(options) { injected.push(["execute", options]); return [{ result: null }]; },
    async insertCSS(options) { injected.push(["css", options]); },
  },
  action: {
    async setBadgeText({ tabId, text }) { badges.set(tabId, { ...badges.get(tabId), text }); },
    async setBadgeBackgroundColor({ tabId, color }) { badges.set(tabId, { ...badges.get(tabId), color }); },
  },
  storage: { local: area(storage), session: area(session) },
  tabs: {
    onActivated: { addListener(callback) { tabEvents.activated.push(callback); } },
    onUpdated: { addListener(callback) { tabEvents.updated.push(callback); } },
    async create() { return { id: 900 }; },
    async get(id) { return tabs.get(id) || null; },
    async update(id) { return { id }; },
    async remove() {},
    async sendMessage(id, request) { injected.push(["tab-message", id, request]); },
  },
};
const backgroundContext = {
  chrome, URL, URLSearchParams, console, Promise, crypto: globalThis.crypto,
  setTimeout: (callback) => setImmediate(callback), clearTimeout() {},
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

const PANEL = { id: "ext", url: "chrome-extension://ext/refresh-panel.html" };
const POPUP = { id: "ext", url: "chrome-extension://ext/popup.html" };
function tab(id, url) {
  tabs.set(id, { id, url, status: "complete" });
  return { id: "ext", url, tab: { id } };
}
function message(request, sender) {
  return new Promise((resolve) => {
    if (!listener(request, sender, resolve)) setImmediate(() => resolve(undefined));
  });
}
const plan = (url, sender = PANEL) => message({ type: "fm-refresh-plan", url }, sender);
const run = (url, tabId) => message({ type: "fm-refresh-run", url, tabId }, PANEL);
const labels = (result) => result.pages.map((page) => `${page.current ? ">" : ""}${page.done ? "+" : ""}${page.label}`).join("|");
const settle = async (test) => {
  for (let round = 0; round < 200 && !test(); round += 1) await new Promise((resolve) => setTimeout(resolve, 5));
  return test();
};

(async () => {
  const alice = (method, apiPath, body) => rpc({ actor: "alice", method, path: apiPath, body });
  await alice("PUT", "/api/marketing/acme/settings", {
    channels: [
      { id: "x", profileUrl: "https://x.com/acme" },
      { id: "reddit", profileUrl: "https://www.reddit.com/r/acme/" },
      { id: "mastodon", profileUrl: "https://mastodon.social/@acme" },
      { id: "instagram", profileUrl: "https://www.instagram.com/acme/" },
    ],
    reddit: { subreddits: ["selfhosted", "git"], queries: ["self-hosted git"] },
  });
  await rpc({ control: "outbound", url: "https://mastodon.social/api/v1/accounts/lookup?acct=acme",
    data: { id: "42", display_name: "Acme", followers_count: 253, url: "https://mastodon.social/@acme" } });
  await rpc({ control: "outbound", url: "https://mastodon.social/api/v1/accounts/42/statuses?limit=40&exclude_replies=true&exclude_reblogs=true",
    data: [] });
  storage.xStats = { handle: "acme" };

  check("signed out, nothing to refresh and no request", (await plan("https://x.com/acme")).show === false && fetched.length === 0);
  Object.assign(storage, { sessionToken: "token", account: "charlie" });
  check("a page's script cannot ask for the plan", (await plan("https://x.com/acme", tab(3, "https://x.com/acme"))).error === "forbidden_sender");
  check("nor run it", (await message({ type: "fm-refresh-run", url: "https://x.com/acme" }, tab(3, "https://x.com/acme"))).error === "forbidden_sender");
  check("the popup may ask too", (await plan("https://x.com/acme", POPUP)).show === true);

  const profile = await plan("https://x.com/acme");
  check("X: one button per page x.js reads, the profile marked as this tab",
    labels(profile) === ">@acme|Media|Mentions|Search @acme" && profile.title === "X · @acme" && profile.organization === "acme", labels(profile));
  check("on a page x.js reads, the main button syncs what it read", profile.here && profile.action === "Sync X stats"
    && profile.next.url === "https://x.com/acme/media" && profile.read === 0 && profile.unread === 4);
  check("the search button searches the handle", profile.pages[3].url === "https://x.com/search?q=%40acme&f=live");
  const fetchedAfterSetup = fetched.length;
  await plan("https://x.com/acme/media");
  check("the channel list is cached between tabs", fetched.length === fetchedAfterSetup);
  const home = await plan("https://x.com/home");
  check("elsewhere on X the main button goes to the next page", !home.here && home.next.url === "https://x.com/acme"
    && home.hint === "ForkMesh does not read this page.");
  check("and nothing runs there", (await run("https://x.com/home")).error === "refresh_page_not_read");

  tab(7, "https://x.com/acme/media");
  for (const callback of tabEvents.updated) callback(7, { status: "complete" });
  check("the icon counts the site's unread pages on its tabs", await settle(() => badges.get(7)?.text === "4")
    && badges.get(7).color === "#2f81f7", JSON.stringify(badges.get(7)));
  tab(8, "https://example.com/");
  for (const callback of tabEvents.activated) callback({ tabId: 8 });
  check("and is cleared on a site with nothing to refresh", await settle(() => badges.get(8)?.text === ""));

  const observation = { handle: "acme", page: "media", profile: { media: 12 }, posts: [], mentions: [] };
  await message({ type: "fm-x-observe", observation }, tab(7, "https://x.com/acme/media"));
  check("a page x.js read is marked read", Number(session.refreshRead?.["x:media"]) > 0);
  check("the panel and popup are told", broadcast.some((item) => item.type === "fm-refresh-changed"));
  check("and the icon drops to three", await settle(() => badges.get(7)?.text === "3"));
  const media = await plan("https://x.com/acme/media");
  check("the read page is ticked", labels(media) === "@acme|>+Media|Mentions|Search @acme" && media.read === 1);
  const synced = await run("https://x.com/acme/media", 7);
  const recorded = fetched.filter((item) => item.method === "POST" && item.path === "/api/marketing/acme/metrics");
  check("running it records the X stats", synced.ok && !synced.failed && synced.message === "X stats recorded in acme Marketing."
    && recorded.length === 1 && recorded[0].body.media === 12, JSON.stringify(synced));

  const subreddit = await plan("https://www.reddit.com/r/selfhosted/new/");
  check("Reddit: one button per watched subreddit, search and missing rules page",
    labels(subreddit) === ">r/selfhosted|r/git|Search “self-hosted git”|r/selfhosted rules|r/git rules", labels(subreddit));
  check("each opens the page reddit.js reads", subreddit.pages.map((page) => page.url).join(" ") ===
    ["https://www.reddit.com/r/selfhosted/new/", "https://www.reddit.com/r/git/new/",
      "https://www.reddit.com/search/?q=self-hosted%20git&sort=new", "https://old.reddit.com/r/selfhosted/about/rules",
      "https://old.reddit.com/r/git/about/rules"].join(" "));
  check("a watched page sends to the desk", subreddit.here && subreddit.action === "Send to Reddit desk");
  const other = await plan("https://www.reddit.com/r/linux/");
  check("an unwatched subreddit goes to the first unread page", !other.here && other.next.url === "https://www.reddit.com/r/selfhosted/new/");
  await message({ type: "fm-reddit-observe", observation: { kind: "thread", subreddit: "git", postId: "ccc333",
    posts: [{ id: "ccc333", subreddit: "git", title: "Mirror to two remotes", url: "https://www.reddit.com/r/git/comments/ccc333/mirror/" }] } },
  tab(9, "https://www.reddit.com/r/git/comments/ccc333/mirror/"));
  const thread = await plan("https://www.reddit.com/r/git/comments/ccc333/mirror/");
  check("a thread in a watched subreddit is this tab, read, one waiting",
    labels(thread).endsWith("|>+This thread") && thread.action === "Send to Reddit desk (1)", labels(thread) + thread.action);
  const sent = await run("https://www.reddit.com/r/git/comments/ccc333/mirror/", 9);
  check("running it sends the desk", sent.ok && !sent.failed
    && sent.message === "Sent to the Reddit desk: 1 new thread.", JSON.stringify(sent));
  const again = await run("https://www.reddit.com/r/git/comments/ccc333/mirror/", 9);
  check("running it again says why nothing was sent", again.ok && !again.failed
    && again.message === "Nothing new for the Reddit desk: 1 thread read so far is already on it.", JSON.stringify(again));
  await message({ type: "fm-reddit-observe", observation: { kind: "listing", subreddit: "selfhosted",
    posts: [{ id: "hhh888", subreddit: "selfhosted", title: "My homelab rack", url: "https://www.reddit.com/r/selfhosted/comments/hhh888/rack/" }] } },
  tab(10, "https://www.reddit.com/r/selfhosted/"));
  const listingRun = await run("https://www.reddit.com/r/selfhosted/", 10);
  check("on a listing it says why its posts were not sent", listingRun.ok && listingRun.message.startsWith(
    "Nothing new for the Reddit desk: the posts read here are not threads you opened"), JSON.stringify(listingRun));

  const mastodon = await plan("https://mastodon.social/@someone");
  check("Mastodon: the channel's page, refreshed by Marketing's server from anywhere on it",
    labels(mastodon) === "Mastodon" && mastodon.here && mastodon.action === "Refresh Mastodon"
    && mastodon.next.url === "https://mastodon.social/@acme", JSON.stringify(mastodon));
  const refreshed = await run("https://mastodon.social/@someone", 10);
  const refresh = fetched.filter((item) => item.path === "/api/marketing/acme/refresh").pop();
  check("it calls the server-side refresh", refreshed.ok && !refreshed.failed
    && refreshed.message === "Mastodon refreshed in acme Marketing." && JSON.stringify(refresh?.body) === JSON.stringify({ channel: "mastodon" }),
    JSON.stringify(refreshed));
  check("and the channel counts as read", refreshed.plan.read === 1 && refreshed.plan.pages[0].done);
  check("the update lands in the popup's history", storage.marketingCaptures?.[0]?.channel === "mastodon"
    && storage.marketingCaptures[0].state === "done");
  const instagram = await plan("https://www.instagram.com/acme/");
  check("a channel the server cannot refresh only gets its button", instagram.show && !instagram.here
    && labels(instagram) === ">Instagram" && /cannot refresh Instagram/.test(instagram.hint), JSON.stringify(instagram));
  const idle = await plan("https://example.com/");
  check("a site with no channel suggests the channel updated longest ago, skipping X and Mastodon updated this hour",
    idle.show && !idle.pages.length && !idle.here && idle.suggest?.channel === "reddit" && idle.suggest.label === "Reddit"
    && idle.suggest.url === "https://www.reddit.com/r/acme/" && !idle.suggest.current && idle.suggest.at === 0, JSON.stringify(idle));
  const hour = 60 * 60 * 1000;
  session.refreshRead["channel:reddit"] = Date.now() - hour / 2;
  check("a channel updated within the hour is skipped", (await plan("https://example.com/")).suggest?.channel === "instagram");
  session.refreshRead["channel:reddit"] = Date.now() - 3 * hour;
  session.refreshRead["channel:instagram"] = Date.now() - 2 * hour;
  const oldest = await plan("https://example.com/");
  check("the one updated furthest in the past comes first", oldest.suggest?.channel === "reddit"
    && oldest.suggest.at === session.refreshRead["channel:reddit"], JSON.stringify(oldest.suggest));
  for (const key of ["channel:reddit", "channel:instagram", "channel:x"]) session.refreshRead[key] = Date.now();
  session.refreshRead["channel:mastodon"] = 1;
  const covered = await plan("https://mastodon.social/@someone");
  check("on a site whose main button refreshes the suggestion, no second button is offered",
    covered.suggest?.channel === "mastodon" && covered.suggest.covered && covered.refreshes.join() === "mastodon", JSON.stringify(covered));
  check("a page's script cannot update the suggestion", (await message({ type: "fm-refresh-channel", channel: "mastodon",
    url: "https://example.com/" }, tab(3, "https://example.com/"))).error === "forbidden_sender");
  const anywhere = await message({ type: "fm-refresh-channel", channel: "mastodon", url: "https://example.com/" }, POPUP);
  check("a channel Marketing refreshes is updated from any page", anywhere.ok && !anywhere.failed
    && anywhere.message === "Mastodon refreshed in acme Marketing.", JSON.stringify(anywhere));
  check("and with every channel updated this hour nothing is suggested", anywhere.plan.show === false, JSON.stringify(anywhere.plan));
  check("nothing was ever injected, registered or messaged into a page",
    injected.length === 0, JSON.stringify(injected));
  session.refreshRead["channel:instagram"] = 1;
  check("one Marketing cannot refresh is not updated away from its page", (await message({ type: "fm-refresh-channel",
    channel: "instagram", url: "https://example.com/", tabId: 11 }, POPUP)).error === "refresh_page_not_read");
  const onInstagram = await plan("https://www.instagram.com/acme/");
  check("on its page the suggestion is this tab", onInstagram.suggest?.channel === "instagram" && onInstagram.suggest.current
    && !onInstagram.suggest.covered, JSON.stringify(onInstagram.suggest));
  tab(11, "https://www.instagram.com/acme/");
  const read = await message({ type: "fm-refresh-channel", channel: "instagram", url: "https://www.instagram.com/acme/", tabId: 11 }, POPUP);
  check("and its Update reads the page in this tab", read.ok && read.failed
    && injected.some(([kind, options]) => kind === "execute" && options.target.tabId === 11)
    && read.message === "Instagram: No follower, post, view or review counts were found on this page.", JSON.stringify(read));

  delete storage.sessionToken;
  check("signing out hides it", (await plan("https://www.reddit.com/r/git/new/")).show === false);

  const domChild = (item) => (item && typeof item === "object" && !Array.isArray(item) ? item : { tag: "#text", textContent: String(item) });
  const element = (tag) => ({
    tag, children: [], handlers: {}, hidden: false, className: "", textContent: "", title: "", disabled: false,
    append(...items) { this.children.push(...items.map(domChild)); },
    replaceChildren(...items) { this.children = items.map(domChild); },
    addEventListener(event, handler) { this.handlers[event] = handler; },
  });
  const find = (node, test) => {
    if (!node || typeof node !== "object") return null;
    if (test(node)) return node;
    for (const child of node.children || []) {
      const found = find(child, test);
      if (found) return found;
    }
    return null;
  };
  const roots = { "[data-refresh]": element("section"), "[data-refresh-empty]": element("p") };
  const panelSent = [];
  const navigated = [];
  const panelEvents = { activated: [], updated: [], message: [] };
  let activeTab = { id: 21, windowId: 4, url: "https://x.com/home" };
  let panelPlan = { ok: true, show: true, site: "x", title: "X · @acme", organization: "acme", here: false, action: "",
    hint: "ForkMesh does not read this page.", next: { label: "Media", url: "https://x.com/acme/media" }, read: 1, total: 2, unread: 1,
    pages: [{ key: "x:profile", label: "@acme", url: "https://x.com/acme", current: false, done: true },
      { key: "x:media", label: "Media", url: "https://x.com/acme/media", current: false, done: false }] };
  const panelChrome = {
    runtime: { lastError: undefined, onMessage: { addListener: (callback) => panelEvents.message.push(callback) },
      sendMessage(request, callback) {
        panelSent.push(request);
        const reply = request.type === "fm-refresh-plan" ? panelPlan
          : { ok: true, message: "X stats recorded in acme Marketing.", plan: { ...panelPlan, read: 2 } };
        setImmediate(() => callback(reply));
      } },
    tabs: {
      async query(filter) { return filter.windowId === 4 && filter.active ? [activeTab] : []; },
      async update(id, options) { navigated.push([id, options.url]); return { id }; },
      onActivated: { addListener: (callback) => panelEvents.activated.push(callback) },
      onUpdated: { addListener: (callback) => panelEvents.updated.push(callback) },
    },
    windows: { async getCurrent() { return { id: 4 }; } },
  };
  const panel = { chrome: panelChrome, document: { createElement: element, querySelector: (selector) => roots[selector] || null },
    Promise, Object, String, Boolean, JSON, setImmediate };
  panel.globalThis = panel;
  vm.createContext(panel);
  for (const file of ["refresh-view.js", "refresh-panel.js"]) {
    vm.runInContext(fs.readFileSync(path.join(EXTENSION, file), "utf8"), panel, { filename: file });
  }
  const panelFlush = async () => { for (let round = 0; round < 10; round += 1) await new Promise((resolve) => setImmediate(resolve)); };
  await panelFlush();
  const root = roots["[data-refresh]"];
  check("the panel asks for the active tab's plan", panelSent[0]?.type === "fm-refresh-plan" && panelSent[0].url === "https://x.com/home");
  const main = () => find(root, (node) => node.className === "primary");
  check("and shows the main button and one button per page", !root.hidden && roots["[data-refresh-empty]"].hidden
    && main().textContent === "Go to Media"
    && find(root, (node) => node.className === "refresh-pages").children.map((item) => item.children[0].textContent).join("|") === "✓ @acme|○ Media");
  check("with the read count", find(root, (node) => node.className === "refresh-title").children[1].textContent === "1/2 read");
  main().handlers.click();
  check("the main button navigates the active tab", JSON.stringify(navigated.at(-1)) === JSON.stringify([21, "https://x.com/acme/media"]));
  find(root, (node) => node.className === "refresh-pages").children[0].children[0].handlers.click();
  check("a page button navigates the active tab", JSON.stringify(navigated.at(-1)) === JSON.stringify([21, "https://x.com/acme"]));
  activeTab = { id: 22, windowId: 4, url: "https://x.com/acme/media" };
  panelPlan = { ...panelPlan, here: true, action: "Sync X stats", hint: "",
    pages: [panelPlan.pages[0], { ...panelPlan.pages[1], current: true }] };
  for (const callback of panelEvents.activated) callback({ tabId: 22, windowId: 9 });
  await panelFlush();
  check("another window's tab is ignored", panelSent.length === 1);
  for (const callback of panelEvents.activated) callback({ tabId: 22, windowId: 4 });
  await panelFlush();
  check("it follows the active tab", panelSent.at(-1).url === "https://x.com/acme/media" && main().textContent === "Sync X stats");
  main().handlers.click();
  await panelFlush();
  check("and runs the refresh for it", panelSent.some((item) => item.type === "fm-refresh-run" && item.tabId === 22)
    && find(root, (node) => node.className === "ok").textContent === "X stats recorded in acme Marketing.");
  activeTab = { id: 22, windowId: 4, url: "https://example.com/" };
  panelPlan = { ok: true, show: false };
  for (const callback of panelEvents.updated) callback(22, { url: "https://example.com/" });
  await panelFlush();
  check("a tab with nothing to refresh shows the empty note", root.hidden && !roots["[data-refresh-empty]"].hidden);
  panelPlan = { ok: true, show: true, site: "", title: "", organization: "acme", here: false, action: "", hint: "", next: null,
    read: 0, total: 0, unread: 0, refreshes: [], pages: [],
    suggest: { channel: "discord", label: "Discord", url: "https://discord.gg/acme", at: 0, current: false, covered: false } };
  for (const callback of panelEvents.updated) callback(22, { status: "complete" });
  await panelFlush();
  const text = (node) => [node.textContent, ...(node.children || []).map(text)].join(" ");
  check("elsewhere the panel suggests the next channel", !root.hidden && main().textContent === "Go to Discord"
    && text(root).includes("Discord, never updated.") && !find(root, (node) => node.className === "refresh-pages"), text(root));
  main().handlers.click();
  check("and brings it to the active tab", JSON.stringify(navigated.at(-1)) === JSON.stringify([22, "https://discord.gg/acme"]));
  activeTab = { id: 22, windowId: 4, url: "https://discord.gg/acme" };
  panelPlan = { ...panelPlan, suggest: { ...panelPlan.suggest, current: true } };
  for (const callback of panelEvents.updated) callback(22, { url: "https://discord.gg/acme" });
  await panelFlush();
  check("on its page the button updates it", main().textContent === "Update Discord" && text(root).includes("Discord is next to update"));
  main().handlers.click();
  await panelFlush();
  check("with one request for that channel", panelSent.some((item) => item.type === "fm-refresh-channel" && item.channel === "discord"
    && item.tabId === 22 && item.url === "https://discord.gg/acme"));
  panelPlan = { ...panelPlan, site: "discord.gg", title: "Discord", here: true, action: "Refresh Discord", refreshes: ["discord"], total: 1,
    pages: [{ key: "channel:discord", label: "Discord", url: "https://discord.gg/acme", current: true, done: false }],
    suggest: { ...panelPlan.suggest, covered: true } };
  for (const callback of panelEvents.updated) callback(22, { status: "complete" });
  await panelFlush();
  const buttons = [];
  find(root, (node) => { if (node.tag === "button") buttons.push(node.textContent); return false; });
  check("one card, one button when the page's refresh is the suggestion", buttons.join("|") === "Refresh Discord", buttons.join("|"));
  const here = element("div");
  panel.ForkMeshRefreshView.render(here, { ...panelPlan, title: "", here: false, pages: [], suggest: { ...panelPlan.suggest, covered: false } },
    { title: "Reddit · r/git", kind: "subreddit", lines: ["r/git is watched.", "Nothing waiting to send."], actions: [] });
  check("the popup's lines show as paragraphs, not object names", here.children.every((node) => node.tag !== "#text")
    && here.children.filter((node) => node.className === "muted").map((node) => node.textContent).join("|")
      === "r/git is watched.|Nothing waiting to send.", text(here));

  server.kill();
  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); server.kill(); process.exit(1); });
