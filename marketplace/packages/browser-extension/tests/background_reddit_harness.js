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
function node(attributes = {}, children = {}, content = "", kids = [], tag = "") {
  const target = {
    getAttribute: (name) => (name in attributes ? String(attributes[name]) : null),
    querySelector: (selector) => children[selector] || null,
    matches: (selector) => selector === tag,
    textContent: content, innerText: content, firstElementChild: null, children: kids,
  };
  return new Proxy(target, { set(object, key, value) { writes.push(String(key)); object[key] = value; return true; } });
}

function readPage(url, nodes, watch, readyState = "complete") {
  const sent = [];
  const timers = [];
  const listeners = [];
  const location = new URL(url);
  const context = {
    URL, URLSearchParams, JSON, Date, Number, String, Set, Map, Promise, Object, Array, Math,
    decodeURIComponent,
    location: { href: location.href, pathname: location.pathname, search: location.search, hostname: location.hostname, origin: location.origin },
    document: {
      visibilityState: "visible", readyState,
      querySelectorAll: (selector) => nodes[selector] || [],
      querySelector: (selector) => (nodes[selector] || [])[0] || null,
    },
    setTimeout: (callback) => { timers.push(callback); return timers.length; },
    chrome: { runtime: { id: "ext", lastError: undefined, onMessage: { addListener: (callback) => listeners.push(callback) }, sendMessage(message, callback) {
      sent.push(message);
      callback(message.type === "fm-reddit-watch" ? { ok: true, ...watch } : { ok: true });
    } } },
  };
  context.window = context;
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(path.join(EXTENSION, "reddit.js"), "utf8"), context, { filename: "reddit.js" });
  return new Promise((resolve) => setImmediate(() => setImmediate(() => resolve({ sent, timers, listeners }))));
}

const storage = {};
const session = {};
const fetched = [];
let inflight = 0;
const tabsCreated = [];
const tabsRemoved = [];
const tabsVisited = [];
const tabMessages = [];
const injected = [];
const alarms = [];
let tabReply = () => null;
let browserActor = "charlie";
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
    getRegisteredContentScripts: async () => [], registerContentScripts: async () => {},
    unregisterContentScripts: async () => {},
    async executeScript(options) {
      injected.push(options.files ? options.files.join() : "func");
      return [{ result: options.files ? undefined : { ok: true, bursts: 4 } }];
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
    async create(options) { tabsCreated.push(options.url); return { id: 40 + tabsCreated.length, active: options.active }; },
    async get(id) { return { id, status: "complete" }; },
    async update(id, options) { if (options.url) tabsVisited.push(options.url); return { id }; },
    async query() { return []; },
    async remove(id) { tabsRemoved.push(id); },
    async sendMessage(id, request) { tabMessages.push([id, request]); return tabReply(request, id); },
  },
  alarms: {
    onAlarm: noop,
    async create(name, options) { alarms.push([name, options]); },
    async clear(name) { alarms.push([name, null]); },
  },
};
let walkDelay = 0;
const backgroundContext = {
  chrome, URL, URLSearchParams, console, Promise,
  crypto: { randomUUID: () => "walk-id" },
  setTimeout: (callback, ms) => setTimeout(callback, Math.min(Number(ms) || 0, walkDelay)),
  importScripts() {},
  ForkMeshVaultBackground: { install: () => ({ forget: async () => {} }) },
  async fetch(url, options) {
    const target = new URL(url);
    const body = options.body === undefined ? null : JSON.parse(options.body);
    fetched.push({ method: options.method, path: target.pathname, body });
    inflight += 1;
    const reply = await rpc({ actor: browserActor, method: options.method, path: target.pathname + target.search, body });
    inflight -= 1;
    return { ok: reply.status < 400, status: reply.status, json: async () => reply.data };
  },
};
backgroundContext.self = backgroundContext;
vm.createContext(backgroundContext);
vm.runInContext(fs.readFileSync(path.join(EXTENSION, "background.js"), "utf8"), backgroundContext, { filename: "background.js" });

const POPUP = { id: "ext", url: "chrome-extension://ext/popup.html" };
const TAB = { id: "ext", url: "https://www.reddit.com/r/selfhosted/", tab: { id: 3 } };
async function settled() {
  let count;
  do {
    count = fetched.length;
    await new Promise((resolve) => setTimeout(resolve, 40));
  } while (inflight || count !== fetched.length);
}
const writesSince = (start) => fetched.slice(start).filter((item) => item.method !== "GET");
const deskWritesSince = (start) => writesSince(start).filter((item) => !item.path.endsWith("/activity"));
const activitySince = (start) => writesSince(start).filter((item) => item.path === "/api/marketing/acme/activity");

function message(request, sender) {
  return new Promise((resolve) => {
    if (!listener(request, sender, resolve)) setImmediate(() => resolve(undefined));
  });
}

(async () => {
  const alice = (method, apiPath, body) => rpc({ actor: "alice", method, path: apiPath, body });
  await alice("PUT", "/api/marketing/acme/settings", {
    reddit: { subreddits: ["selfhosted", "git"], queries: ["self-hosted git"] } });
  Object.assign(storage, { sessionToken: "token", account: "charlie" });

  const empty = await message({ type: "fm-reddit-watch" }, TAB);
  check("the cached watch list starts empty", empty.ok && empty.subreddits.length === 0);
  check("a content script never reaches ForkMesh", fetched.length === 0);
  const forbidden = await message({ type: "fm-reddit-sync" }, TAB);
  check("a page cannot press Send", forbidden.error === "forbidden_sender" && fetched.length === 0);
  const refused = await message({ type: "fm-reddit-refresh" }, TAB);
  check("a page cannot fetch the watch list", refused.error === "forbidden_sender" && fetched.length === 0);

  const refreshed = await message({ type: "fm-reddit-refresh" }, POPUP);
  check("Refresh loads the Marketing watch list", refreshed.ok && refreshed.subreddits === 2
    && refreshed.queries === 1 && refreshed.organization === "acme", JSON.stringify(refreshed));
  check("subreddits without rules link to old Reddit's rules pages",
    refreshed.rulesMissing.map((item) => item.url).join() ===
    "https://old.reddit.com/r/selfhosted/about/rules,https://old.reddit.com/r/git/about/rules");
  const watch = await message({ type: "fm-reddit-watch" }, TAB);
  check("the content script gets the cached list", watch.subreddits.join() === "selfhosted,git"
    && watch.queries.join() === "self-hosted git");

  const shreddit = (id, sub, title, extra = {}) => node({
    id: `t3_${id}`, permalink: `/r/${sub}/comments/${id}/${title.toLowerCase().replace(/[^a-z]+/g, "_")}/`,
    "post-title": title, author: "grace_h", "subreddit-prefixed-name": `r/${sub}`,
    "comment-count": "12", score: "-3", "created-timestamp": "2026-10-04T12:34:56.789000+0000", ...extra });
  const listing = await readPage("https://www.reddit.com/r/selfhosted/new/", {
    "shreddit-post": [shreddit("aaa111", "selfhosted", "Best self-hosted Git server?"),
      shreddit("bbb222", "selfhosted", "My homelab rack")],
  }, watch);
  const listed = listing.sent.find((item) => item.type === "fm-reddit-observe");
  check("a watched listing is read", listed && listed.observation.kind === "listing"
    && listed.observation.posts.length === 2, JSON.stringify(listing.sent));
  check("new Reddit's post attributes are read", listed.observation.posts[0].comments === 12
    && listed.observation.posts[0].score === -3 && listed.observation.posts[0].author === "grace_h"
    && listed.observation.posts[0].postedAt === Date.parse("2026-10-04T12:34:56.789Z")
    && listed.observation.posts[0].url === "https://www.reddit.com/r/selfhosted/comments/aaa111/best_self_hosted_git_server_/");
  check("the reader ticks again", listing.timers.length === 1);

  const oldThing = node({ "data-fullname": "t3_ccc333", "data-permalink": "/r/git/comments/ccc333/mirror_two_remotes/",
    "data-subreddit": "git", "data-author": "ada", "data-comments-count": "4", "data-score": "7",
    "data-timestamp": "1790000000000" }, { "a.title": node({}, {}, "Mirror to two remotes") });
  const thread = await readPage("https://old.reddit.com/r/git/comments/ccc333/mirror_two_remotes/", {
    'div.thing[data-fullname^="t3_"]': [oldThing],
  }, watch);
  const opened = thread.sent.find((item) => item.type === "fm-reddit-observe");
  check("an old Reddit thread is read", opened && opened.observation.kind === "thread"
    && opened.observation.posts[0].title === "Mirror to two remotes" && opened.observation.posts[0].comments === 4);

  const search = await readPage("https://www.reddit.com/search/?q=Self-Hosted%20%20Git&sort=new", {
    'a[href*="/comments/"]': [node({ href: "/r/linux/comments/ddd444/selfhosted_git_on_a_pi/" }, {}, "Self-hosted git on a Pi"),
      node({ href: "/r/linux/comments/ddd444/selfhosted_git_on_a_pi/eee555/" }, {}, "A comment link")],
  }, watch);
  const searched = search.sent.find((item) => item.type === "fm-reddit-observe");
  check("a search for a watched query is read", searched && searched.observation.kind === "search"
    && searched.observation.posts.length === 1 && searched.observation.posts[0].subreddit === "linux");

  const unwatched = await readPage("https://www.reddit.com/r/linux/new/", {
    "shreddit-post": [shreddit("fff666", "linux", "Self-hosted git again")] }, watch);
  check("an unwatched subreddit is left alone", !unwatched.sent.some((item) => item.type === "fm-reddit-observe"));
  const otherSearch = await readPage("https://www.reddit.com/search/?q=gitea", {
    'a[href*="/comments/"]': [node({ href: "/r/git/comments/ggg777/x/" }, {}, "Gitea or Forgejo")] }, watch);
  check("a search for another query is left alone", !otherSearch.sent.some((item) => item.type === "fm-reddit-observe"));

  const rule = (title, description) => node({}, { ".rule-title": node({}, {}, title), ".rule-description": node({}, {}, description) });
  const rulesPage = await readPage("https://old.reddit.com/r/selfhosted/about/rules", {
    ".subreddit-rule-item": [rule("No spam", "Self-promotion only in the\n\n\n\nmegathread."), rule("Be kind", "")],
  }, watch);
  const rules = rulesPage.sent.find((item) => item.type === "fm-reddit-observe");
  check("old Reddit's rules are read", rules && rules.observation.kind === "rules"
    && rules.observation.rules.length === 2 && rules.observation.rules[0].description === "Self-promotion only in the\n\nmegathread.");
  const noRules = await readPage("https://old.reddit.com/r/git/about/rules", {
    ".subreddit-rules-page": [node()] }, watch);
  const emptyRules = noRules.sent.find((item) => item.type === "fm-reddit-observe");
  check("an old Reddit rules page without rules says so", emptyRules && emptyRules.observation.rules.length === 0);
  check("the reader never writes to the page", writes.length === 0, writes.join());

  check("the first read after a page loads is marked fresh", [listed, opened, searched, rules, emptyRules].every((item) => item.fresh === true));
  for (const observed of [listed, opened, searched, rules, emptyRules]) {
    const before = fetched.length;
    const ok = (await message(observed, TAB)).ok;
    await settled();
    const activity = activitySince(before);
    check(`observing ${observed.observation.kind} sends only its activity line`, ok && !deskWritesSince(before).length
      && activity.length === 1 && activity[0].body.kind === "read" && activity[0].body.site === "reddit",
    JSON.stringify(fetched.slice(before)));
  }
  const listingActivity = (await rpc({ actor: "charlie", method: "GET", path: "/api/marketing/acme/activity" })).data.browserActivity;
  check("the activity names the page and what waits for the desk", listingActivity.some((item) =>
    item.text === "Read r/selfhosted: 2 posts. 1 waiting for the Reddit desk."), JSON.stringify(listingActivity.map((item) => item.text)));
  const repeatBefore = fetched.length;
  await message({ ...listed, fresh: false }, TAB);
  await settled();
  check("a re-read of a page already loaded sends nothing", fetched.length === repeatBefore);
  const statsBefore = fetched.length;
  await message({ type: "fm-reddit-observe", observation: { kind: "listing", subreddit: "linux",
    posts: [{ id: "fff666", subreddit: "linux", title: "Self-hosted git again", url: "https://www.reddit.com/r/linux/comments/fff666/x/" }] } }, TAB);

  const stats = await message({ type: "fm-reddit-stats" }, POPUP);
  check("three candidates and two rule sets wait", stats.waiting === 3 && stats.rulesWaiting === 2 && stats.seen === 4,
    JSON.stringify(stats));
  check("stats do not refetch a fresh watch list", fetched.length === statsBefore);

  await alice("POST", "/api/marketing/acme/reddit", { url: "https://www.reddit.com/r/git/comments/ccc333/" });
  const synced = await message({ type: "fm-reddit-sync" }, POPUP);
  check("Send logs the new threads and counts the logged one", synced.ok && synced.lastSync.threads === 2
    && synced.lastSync.already === 1 && synced.lastSync.rules === 2 && !synced.lastSync.error, JSON.stringify(synced.lastSync));
  check("nothing waits after Send", synced.waiting === 0 && synced.rulesWaiting === 0 && synced.rulesMissing.length === 0);
  const posted = fetched.filter((item) => item.method === "POST");
  check("only the Marketing API is written to", posted.every((item) => /^\/api\/marketing\/acme\/(reddit(-rules)?|activity)$/.test(item.path)));
  check("no human attestation is ever sent", posted.every((item) =>
    !("rulesChecked" in item.body) && !("disclosed" in item.body) && !("replyUrl" in item.body)));
  const threads = (await rpc({ control: "threads" })).data.rows;
  const byId = Object.fromEntries(threads.map((item) => [item.postId, item]));
  check("threads arrive to review with what the browser read", byId.aaa111.status === "to_review"
    && byId.aaa111.source === "extension" && byId.aaa111.author === "grace_h" && byId.aaa111.comments === 12
    && byId.aaa111.score === -3 && byId.ddd444.subreddit === "linux" && !byId.bbb222 && !byId.fff666
    && byId.aaa111.rulesChecked === false && byId.aaa111.disclosed === false, JSON.stringify(threads));
  const desk = (await alice("GET", "/api/marketing/acme/reddit")).data.redditDesk;
  const selfhosted = desk.subreddits.find((item) => item.name === "selfhosted");
  check("the rules are on file for the team", selfhosted.rulesRead && selfhosted.rules[0].title === "No spam"
    && selfhosted.rulesReadBy === "charlie" && desk.rulesOnFile === 2);

  const before = fetched.filter((item) => item.method === "POST").length;
  const again = await message({ type: "fm-reddit-sync" }, POPUP);
  check("a second Send posts nothing", again.ok && fetched.filter((item) => item.method === "POST").length === before);

  await message({ type: "fm-open-reddit-desk" }, POPUP);
  check("Open Reddit desk opens the Marketing page", tabsCreated.pop() === "https://app.forkmesh.com/dashboard/marketing?org=acme&page=reddit");

  const patches = () => fetched.filter((item) => item.method === "PATCH");
  check("a page cannot add a monitored thread", (await message({ type: "fm-reddit-monitor-add",
    url: "https://www.reddit.com/r/devops/comments/1wy1g5i/" }, TAB)).error === "forbidden_sender");
  check("a page cannot start a check", (await message({ type: "fm-reddit-check" }, TAB)).error === "forbidden_sender");
  check("a link that is not a thread is refused", (await message({ type: "fm-reddit-monitor-add",
    url: "https://www.reddit.com/r/devops/" }, POPUP)).error === "invalid_reddit_thread_url");
  const added = await message({ type: "fm-reddit-monitor-add",
    url: "https://old.reddit.com/r/devops/comments/1wy1g5i/comment/pec00w1/?utm_source=share" }, POPUP);
  check("a comment link becomes a monitored thread on the desk", added.ok && added.monitored.length === 1
    && added.monitored[0].url === "https://www.reddit.com/r/devops/comments/1wy1g5i/"
    && added.monitored[0].commentUrl === "https://www.reddit.com/r/devops/comments/1wy1g5i/comment/pec00w1/"
    && added.monitored[0].checkedAt === 0, JSON.stringify(added.monitored));
  const addPost = fetched.filter((item) => item.method === "POST").pop();
  check("adding sends the link and the monitor flag only", addPost.body.monitored === true
    && addPost.body.url === "https://www.reddit.com/r/devops/comments/1wy1g5i/comment/pec00w1/"
    && !("rulesChecked" in addPost.body) && !("disclosed" in addPost.body) && !("replyUrl" in addPost.body));
  const existing = await message({ type: "fm-reddit-monitor-add",
    url: "https://www.reddit.com/r/git/comments/ccc333/mirror_two_remotes/" }, POPUP);
  check("an already logged thread is switched to monitored", existing.ok && existing.monitored.length === 2
    && patches().pop().body.monitored === true);
  const monitoredWatch = await message({ type: "fm-reddit-watch" }, TAB);
  const monitoredFirst = JSON.stringify(monitoredWatch.monitored.slice(0, 2));
  check("the reader learns every logged thread, monitored ones and their comments first", (monitoredFirst
    === JSON.stringify([{ postId: "ccc333", commentId: "" }, { postId: "1wy1g5i", commentId: "pec00w1" }])
    || monitoredFirst === JSON.stringify([{ postId: "1wy1g5i", commentId: "pec00w1" }, { postId: "ccc333", commentId: "" }]))
    && JSON.stringify(monitoredWatch.monitored.slice(2).map((item) => item.postId).sort()) === JSON.stringify(["aaa111", "ddd444"]),
    JSON.stringify(monitoredWatch.monitored));

  const viewsLeaf = node({}, {}, "45 views");
  const reply = node({}, {}, "", [node({}, {}, "Nice tool, 999 views"), node({}, {}, "", [viewsLeaf])]);
  const nested = node({ score: "7" }, {}, "", [node({}, {}, "2 views")], "shreddit-comment");
  const ownComment = node({ thingid: "t1_pec00w1", score: "3" }, {}, "", [nested, reply], "shreddit-comment");
  const ownPost = node({ id: "t3_1wy1g5i", permalink: "/r/devops/comments/1wy1g5i/weekly_self_promotion_thread/",
    "post-title": "Weekly Self Promotion Thread", author: "AutoModerator", score: "8", "comment-count": "40",
    "view-count": "1.2K" });
  const commentPage = await readPage("https://www.reddit.com/r/devops/comments/1wy1g5i/comment/pec00w1/", {
    "shreddit-post": [ownPost],
    'shreddit-post[id="t3_1wy1g5i"]': [ownPost],
    'shreddit-comment[thingid="t1_pec00w1"]': [ownComment],
  }, monitoredWatch);
  const seenMonitor = commentPage.sent.find((item) => item.type === "fm-reddit-observe");
  check("a monitored thread in an unwatched subreddit is read", seenMonitor
    && JSON.stringify(seenMonitor.observation.monitor) === JSON.stringify({ postId: "1wy1g5i",
      title: "Weekly Self Promotion Thread", views: 1200, score: 8, comments: 40, commentId: "pec00w1",
      commentViews: 45, commentScore: 3 }), JSON.stringify(seenMonitor?.observation));
  const answered = await new Promise((resolve) => commentPage.listeners[0](
    { type: "fm-reddit-read", postId: "1wy1g5i" }, { id: "ext" }, resolve));
  check("the reader answers the extension's read request", answered.ok && answered.monitor.views === 1200);
  check("a page cannot ask the reader", commentPage.listeners[0](
    { type: "fm-reddit-read", postId: "1wy1g5i" }, { id: "ext", tab: { id: 2 } }, () => {}) === false);
  const fetchedBeforeReading = fetched.length;
  await message(seenMonitor, TAB);
  await settled();
  const sentByItself = await message({ type: "fm-reddit-stats" }, POPUP);
  const readingPatch = patches().pop();
  check("a reading on a logged thread is sent to the desk by itself", deskWritesSince(fetchedBeforeReading).length === 1
    && sentByItself.readingsWaiting === 0 && readingPatch.body.views === 1200 && readingPatch.body.commentScore === 3
    && readingPatch.body.checkedAt > 0 && readingPatch.body.title === "Weekly Self Promotion Thread", JSON.stringify(readingPatch));
  const sentReading = await message({ type: "fm-reddit-sync" }, POPUP);
  check("Send finds no reading left to send", sentReading.lastSync.readings === 0 && sentReading.readingsWaiting === 0);
  const devops = () => rpc({ control: "threads" }).then((reply) => reply.data.rows.find((item) => item.postId === "1wy1g5i"));
  const stored = await devops();
  check("the desk keeps the views and a history point", stored.views === 1200 && stored.commentViews === 45
    && stored.history.length === 1 && stored.monitored === true, JSON.stringify(stored));

  tabReply = (request) => ({ ok: true, monitor: { postId: request.postId, title: "", views: 1500, score: 9,
    comments: 41, commentId: request.postId === "1wy1g5i" ? "pec00w1" : "", commentViews: 60, commentScore: 4 } });
  const createdBefore = tabsCreated.length;
  const checked = await message({ type: "fm-reddit-check" }, POPUP);
  const checkTabs = tabsCreated.slice(createdBefore);
  check("Check now opens each monitored thread and closes it", checked.ok && checkTabs.length === 2
    && checkTabs.includes("https://www.reddit.com/r/devops/comments/1wy1g5i/comment/pec00w1/")
    && tabsRemoved.length === 2 && checked.lastCheck.read === 2 && checked.lastCheck.sent === 2, JSON.stringify(checked.lastCheck));
  check("Check now asks only the reader in the tab it opened", tabMessages.every(([id, request]) =>
    id > 40 && request.type === "fm-reddit-read"));
  const rechecked = await devops();
  check("the check updates the desk", rechecked.views === 1500 && rechecked.commentScore === 4);
  const other = (await rpc({ control: "threads" })).data.rows.find((item) => item.postId === "ccc333");
  check("a thread without a comment keeps no comment counts", other.views === 1500 && other.commentScore === undefined,
    JSON.stringify(other));

  const auto = await message({ type: "fm-reddit-auto-check", on: true }, POPUP);
  check("the six-hour check is opt-in", auto.autoCheck === true
    && JSON.stringify(alarms.pop()) === JSON.stringify(["fm-reddit-monitor-check", { periodInMinutes: 360 }]));
  check("and can be switched off", (await message({ type: "fm-reddit-auto-check", on: false }, POPUP)).autoCheck === false
    && JSON.stringify(alarms.pop()) === JSON.stringify(["fm-reddit-monitor-check", null]));
  const stoppedId = existing.monitored.find((item) => item.url.includes("ccc333")).deskId;
  const stopped = await message({ type: "fm-reddit-monitor-remove", deskId: stoppedId }, POPUP);
  check("Stop leaves the thread on the desk unmonitored", stopped.ok && stopped.monitored.length === 1
    && patches().pop().body.monitored === false);

  check("a page cannot start a walk", (await message({ type: "fm-reddit-walk" }, TAB)).error === "forbidden_sender");
  tabReply = (request) => (request.type === "fm-reddit-scan" ? { ok: true, posts: 2 } : null);
  const walkCreatedBefore = tabsCreated.length;
  const started = await message({ type: "fm-reddit-walk" }, POPUP);
  check("the walk plans every watched subreddit and search, rules already on file",
    started.ok && started.walk.stops.map((stop) => stop.url).join("\n") === [
      "https://www.reddit.com/r/selfhosted/new/",
      "https://www.reddit.com/search/?q=self-hosted%20git&sort=new&t=week",
      "https://www.reddit.com/r/git/new/",
    ].join("\n"), JSON.stringify(started.walk?.stops));
  check("the walk opens exactly one tab", tabsCreated.length - walkCreatedBefore === 1
    && tabsCreated[tabsCreated.length - 1] === "https://www.reddit.com/r/selfhosted/new/");
  const finished = await new Promise((resolve) => {
    const poll = async () => {
      const state = await message({ type: "fm-reddit-walk-state" }, POPUP);
      if (state.walk && state.walk.finishedAt) resolve(state.walk);
      else setTimeout(poll, 5);
    };
    void poll();
  });
  check("the walk reads every stop and closes its tab", finished.state === "done"
    && finished.stops.every((stop) => stop.state === "read") && finished.posts === 6
    && tabsRemoved.includes(41 + walkCreatedBefore), JSON.stringify(finished));
  check("the walk reuses the one tab for the rest of the plan",
    tabsVisited.slice(-2).join() === "https://www.reddit.com/search/?q=self-hosted%20git&sort=new&t=week,https://www.reddit.com/r/git/new/",
    tabsVisited.join());
  check("each stop injects the pacing script and then asks the reader",
    injected.slice(-6).join("|") === "reddit-walk.js|func|reddit-walk.js|func|reddit-walk.js|func"
    && tabMessages.slice(-3).every(([, request]) => request.type === "fm-reddit-scan"), injected.join("|"));
  check("the walk ends by sending what it found to the desk", finished.synced && !finished.synced.error);
  check("a second walk starts clean", (await message({ type: "fm-reddit-walk" }, POPUP)).ok);
  const running = await message({ type: "fm-reddit-walk" }, POPUP);
  check("two walks never run at once", running.error === "reddit_walk_running");
  check("a walk stops when asked", (await message({ type: "fm-reddit-walk-stop" }, POPUP)).ok);
  await new Promise((resolve) => {
    const poll = async () => {
      const state = await message({ type: "fm-reddit-walk-state" }, POPUP);
      if (!state.walking) resolve();
      else setTimeout(poll, 5);
    };
    void poll();
  });
  check("a stopped walk says so", ["stopped", "done"].includes(
    (await message({ type: "fm-reddit-walk-state" }, POPUP)).walk.state));
  const rulesOnly = await message({ type: "fm-reddit-walk", rulesOnly: true }, POPUP);
  check("a rules-only walk has nothing to read once every rule set is on file",
    rulesOnly.error === "reddit_rules_on_file", JSON.stringify(rulesOnly));

  const jettPost = (id, sub, title, extra = {}) => node({
    id: `t3_${id}`, permalink: `/r/${sub}/comments/${id}/${title.toLowerCase().replace(/[^a-z]+/g, "_")}/`,
    "post-title": title, author: "ada_lovelace", "comment-count": "5", score: "21",
    "created-timestamp": "2026-10-09T08:00:00.000000+0000", ...extra });
  const oldComment = node({ "data-fullname": "t1_kkk001", "data-author": "ada_lovelace",
    "data-permalink": "/r/git/comments/hhh888/worktrees_for_agents/kkk001/" },
  { "a.title": node({}, {}, "Worktrees for agents"), ".tagline .score.unvoted": node({ title: "4" }), "time[datetime]": node({ datetime: "2026-10-10T10:00:00Z" }) });
  const profileNodes = {
    "shreddit-post": [jettPost("jjj111", "selfhosted", "I built a Git forge that runs on your desk", { "view-count": "3.4K" }),
      jettPost("jjj222", "devops", "Mirrors without a server"),
      node({ id: "t3_zzz999", permalink: "/r/git/comments/zzz999/someone_else/", "post-title": "Someone else", author: "grace_h" })],
    'div.thing[data-fullname^="t3_"], div.thing[data-fullname^="t1_"]': [oldComment],
  };
  const unlisted = await readPage("https://www.reddit.com/user/ada_lovelace/", profileNodes, watch);
  check("a profile that is not one of your accounts is not read on its own",
    !unlisted.sent.some((item) => item.type === "fm-reddit-observe"));
  const asked = await new Promise((resolve) => unlisted.listeners[0]({ type: "fm-reddit-profile-read" }, { id: "ext" }, resolve));
  check("Update reads the profile you are on: your posts and comments, nobody else's", asked.ok
    && asked.account === "ada_lovelace" && JSON.stringify(asked.items.map((item) => [item.kind, item.postId, item.commentId]))
      === JSON.stringify([["post", "jjj111", ""], ["post", "jjj222", ""], ["comment", "hhh888", "kkk001"]]), JSON.stringify(asked));
  check("a post's link, counts and the views Reddit shows its author are read", asked.items[0].views === 3400
    && asked.items[0].score === 21 && asked.items[0].comments === 5
    && asked.items[0].url === "https://www.reddit.com/r/selfhosted/comments/jjj111/i_built_a_git_forge_that_runs_on_your_desk/"
    && asked.items[0].postedAt === Date.parse("2026-10-09T08:00:00Z"));
  check("a comment keeps its own permalink, score and thread title", asked.items[2].url
    === "https://www.reddit.com/r/git/comments/hhh888/worktrees_for_agents/kkk001/" && asked.items[2].score === 4
    && asked.items[2].title === "Worktrees for agents" && asked.items[2].comments === null);
  check("a page cannot ask for a profile read", unlisted.listeners[0]({ type: "fm-reddit-profile-read" }, { id: "ext", tab: { id: 2 } }, () => {}) === false);
  check("a page cannot press Update on a profile", (await message({ type: "fm-reddit-profile-update", tabId: 7 }, TAB)).error === "forbidden_sender");

  const PROFILE_TAB = 7;
  tabReply = (request) => {
    if (request.type === "fm-reddit-profile-read") return { ok: true, account: asked.account, items: asked.items };
    if (request.type !== "fm-reddit-read") return null;
    return { ok: true, monitor: { postId: request.postId, title: "", views: request.postId === "jjj222" ? 880 : 3500,
      score: 22, comments: 6, commentId: request.commentId, commentViews: request.commentId ? 130 : null, commentScore: request.commentId ? 5 : null } };
  };
  browserActor = "alice";
  const profileTabsBefore = tabsCreated.length;
  const updated = await message({ type: "fm-reddit-profile-update", tabId: PROFILE_TAB }, POPUP);
  check("Update on your profile adds the account and records every link", updated.ok && updated.profile.added
    && updated.profile.found === 3 && updated.profile.linksAdded === 3 && updated.accounts.join() === "ada_lovelace"
    && updated.profile.checking === 3, JSON.stringify(updated.profile));
  check("Update says what it did", /^u\/ada_lovelace is now one of the Reddit desk's accounts\. Read 3 posts and comments by u\/ada_lovelace: 3 new links and 0 updated in Marketing posts\. Reading the views of 3 links in background tabs/.test(updated.text), updated.text);
  const viewsRead = await new Promise((resolve) => {
    const poll = async () => {
      const stats = await message({ type: "fm-reddit-stats" }, POPUP);
      if (stats.mineCheck?.done && !stats.mineChecking) resolve(stats);
      else setTimeout(poll, 5);
    };
    void poll();
  });
  check("each link is opened in a background tab", viewsRead.mineCheck.read === 3 && viewsRead.mineCheck.total === 3
    && tabsCreated.slice(profileTabsBefore).sort().join() === [
      "https://www.reddit.com/r/devops/comments/jjj222/",
      "https://www.reddit.com/r/git/comments/hhh888/worktrees_for_agents/kkk001/",
      "https://www.reddit.com/r/selfhosted/comments/jjj111/"].join(), JSON.stringify(tabsCreated.slice(profileTabsBefore)));
  check("the background reads ask for the comment too", tabMessages.some(([, request]) =>
    request.type === "fm-reddit-read" && request.postId === "hhh888" && request.commentId === "kkk001"));
  const marketingPosts = (await alice("GET", "/api/marketing/acme/posts")).data.posts.filter((item) => item.channel === "reddit");
  const byUrl = Object.fromEntries(marketingPosts.map((item) => [item.url, item]));
  const forge = byUrl["https://www.reddit.com/r/selfhosted/comments/jjj111"];
  const ownReply = byUrl["https://www.reddit.com/r/git/comments/hhh888/worktrees_for_agents/kkk001"];
  check("Marketing posts hold each link with its views, score and comments", marketingPosts.length === 3
    && forge.views === 3500 && forge.likes === 22 && forge.replies === 6 && forge.title === "I built a Git forge that runs on your desk"
    && forge.postedAt === Date.parse("2026-10-09T08:00:00Z") && forge.history.length >= 1
    && byUrl["https://www.reddit.com/r/devops/comments/jjj222"].views === 880
    && ownReply.views === 130 && ownReply.likes === 5 && ownReply.title === "Comment in r/git: Worktrees for agents", JSON.stringify(marketingPosts));

  const fresh = await message({ type: "fm-reddit-profile-update", tabId: PROFILE_TAB }, POPUP);
  check("a second Update right away re-reads the page but leaves fresh views alone", fresh.ok && !fresh.profile.added
    && fresh.profile.checking === 0 && fresh.profile.linksAdded === 0 && / read in the last 30 minutes\.$/.test(fresh.text), fresh.text);

  tabReply = (request) => (request.type === "fm-reddit-profile-read"
    ? { ok: true, account: "forkmesh", items: [{ kind: "post", postId: "fff001", commentId: "", subreddit: "forkmesh",
      url: "https://www.reddit.com/r/forkmesh/comments/fff001/welcome/", title: "Welcome", score: 3, comments: 1, views: null, postedAt: 0 }] }
    : request.type === "fm-reddit-read" ? { ok: true, monitor: { postId: request.postId, title: "", views: 75, score: 3, comments: 1, commentId: "" } } : null);
  const second = await message({ type: "fm-reddit-profile-update", tabId: PROFILE_TAB }, POPUP);
  check("a second account joins the first", second.ok && second.profile.added && second.accounts.join() === "ada_lovelace,forkmesh");
  const deskAccounts = (await alice("GET", "/api/marketing/acme/reddit")).data.redditDesk.accounts;
  check("the desk lists both accounts with their profile links", JSON.stringify(deskAccounts) === JSON.stringify([
    { name: "ada_lovelace", url: "https://www.reddit.com/user/ada_lovelace/submitted/" },
    { name: "forkmesh", url: "https://www.reddit.com/user/forkmesh/submitted/" }]), JSON.stringify(deskAccounts));
  const accountsWatch = await message({ type: "fm-reddit-watch" }, TAB);
  check("the reader learns both accounts", accountsWatch.accounts.join() === "ada_lovelace,forkmesh");
  const listedPage = await readPage("https://www.reddit.com/user/ada_lovelace/comments/", profileNodes, accountsWatch);
  const listedProfile = listedPage.sent.find((item) => item.type === "fm-reddit-observe");
  check("one of your profiles is read as you browse it", listedProfile && listedProfile.observation.kind === "profile"
    && listedProfile.observation.posts.length === 3);
  await new Promise((resolve) => {
    const poll = async () => {
      if (!(await message({ type: "fm-reddit-stats" }, POPUP)).mineChecking) resolve();
      else setTimeout(poll, 5);
    };
    void poll();
  });

  browserActor = "charlie";
  tabReply = (request) => (request.type === "fm-reddit-profile-read"
    ? { ok: true, account: "someone_else", items: [] } : null);
  const member = await message({ type: "fm-reddit-profile-update", tabId: PROFILE_TAB }, POPUP);
  check("a member cannot add an account and is told so", member.ok && !member.profile.added
    && member.profile.addError === "organization_admin_required" && member.accounts.length === 2
    && /^Only organization owners and admins can add u\/someone_else/.test(member.text), member.text);
  tabReply = () => null;
  check("a page the reader cannot answer says so", (await message({ type: "fm-reddit-profile-update", tabId: PROFILE_TAB }, POPUP)).error
    === "reddit_profile_not_read");

  const DASHBOARD = { id: "ext", url: "https://app.forkmesh.com/dashboard/marketing?org=acme&page=reddit", tab: { id: 9 } };
  check("a Reddit page cannot ask for every account's posts", (await message({ type: "fm-reddit-accounts-update" }, TAB)).error === "forbidden_sender");
  check("the dashboard of another organization is refused", (await message({ type: "fm-reddit-accounts-update", organization: "other" }, DASHBOARD)).error
    === "reddit_organization_mismatch");
  browserActor = "alice";
  const tabUrl = (id) => tabsCreated[id - 41] || "";
  tabReply = (request, id) => {
    const url = tabUrl(id);
    if (request.type === "fm-reddit-read") return { ok: true, monitor: { postId: request.postId, title: "", views: 60, score: 2, comments: 0,
      commentId: request.commentId, commentViews: request.commentId ? 12 : null, commentScore: request.commentId ? 1 : null } };
    if (request.type !== "fm-reddit-profile-read" || !url.includes("/user/forkmesh/")) return null;
    return { ok: true, account: "forkmesh", items: url.endsWith("/submitted/")
      ? [{ kind: "post", postId: "fff002", commentId: "", subreddit: "forkmesh", url: "https://www.reddit.com/r/forkmesh/comments/fff002/release_notes/",
        title: "Release notes", score: 6, comments: 2, views: null, postedAt: 0 }]
      : [{ kind: "comment", postId: "hhh888", commentId: "kkk777", subreddit: "git", url: "https://www.reddit.com/r/git/comments/hhh888/worktrees_for_agents/kkk777/",
        title: "Worktrees for agents", score: 3, comments: null, views: null, postedAt: 0 }] };
  };
  const accountTabsBefore = tabsCreated.length;
  const everyone = await message({ type: "fm-reddit-accounts-update", organization: "acme" }, DASHBOARD);
  const profileTabs = tabsCreated.slice(accountTabsBefore);
  check("Update from the dashboard opens each account's posts and comments in a background tab", everyone.ok
    && ["ada_lovelace", "forkmesh"].every((name) => profileTabs.includes(`https://www.reddit.com/user/${name}/submitted/`)
      && profileTabs.includes(`https://www.reddit.com/user/${name}/comments/`)), JSON.stringify(profileTabs));
  check("it says what each account gave", everyone.accounts.length === 2 && !everyone.accounts[0].read
    && everyone.accounts[1].read && everyone.accounts[1].found === 2 && everyone.accounts[1].linksAdded === 2
    && /^u\/ada_lovelace: the profile did not load.*u\/forkmesh: 2 posts and comments read, 2 new links and 0 updated\./.test(everyone.text), everyone.text);
  await new Promise((resolve) => {
    const poll = async () => {
      const stats = await message({ type: "fm-reddit-stats" }, POPUP);
      if (stats.mineCheck?.done && !stats.mineChecking) resolve();
      else setTimeout(poll, 5);
    };
    void poll();
  });
  const allPosts = (await alice("GET", "/api/marketing/acme/posts")).data.posts.filter((item) => item.channel === "reddit");
  const release = allPosts.find((item) => item.url === "https://www.reddit.com/r/forkmesh/comments/fff002");
  check("the new links land in Marketing posts with their views", release && release.views === 60
    && allPosts.some((item) => item.url.endsWith("/kkk777") && item.views === 12), JSON.stringify(allPosts.map((item) => [item.url, item.views])));
  check("every profile tab is closed again", profileTabs.every((url, index) => tabsRemoved.includes(41 + accountTabsBefore + index)));

  check("a Reddit page cannot ask to update the logged threads", (await message({ type: "fm-reddit-threads-update" }, TAB)).error === "forbidden_sender");
  check("the dashboard of another organization cannot update threads", (await message({ type: "fm-reddit-threads-update", organization: "other" }, DASHBOARD)).error
    === "reddit_organization_mismatch");
  const logged = (await alice("GET", "/api/marketing/acme/reddit")).data.redditThreads;
  const linux = logged.find((item) => item.postId === "ddd444");
  const threadTabsBefore = tabsCreated.length;
  const one = await message({ type: "fm-reddit-threads-update", organization: "acme", threadIds: [linux.id] }, DASHBOARD);
  const threadTabs = tabsCreated.slice(threadTabsBefore);
  check("Update next to a thread reads only that thread in a background tab and closes it", one.ok && one.done && one.total === 1
    && one.read === 1 && one.sent === 1 && JSON.stringify(threadTabs) === JSON.stringify(["https://www.reddit.com/r/linux/comments/ddd444/"])
    && tabsRemoved.includes(41 + threadTabsBefore), JSON.stringify([one, threadTabs]));
  const updatedLinux = (await rpc({ control: "threads" })).data.rows.find((item) => item.postId === "ddd444");
  check("an unmonitored logged thread gets its numbers and read time", updatedLinux.views === 60 && updatedLinux.score === 2
    && updatedLinux.comments === 0 && updatedLinux.checkedAt > 0 && updatedLinux.monitored === false, JSON.stringify(updatedLinux));
  const every = await message({ type: "fm-reddit-threads-update", organization: "acme" }, DASHBOARD);
  check("Update all reads every logged thread, least recently read first", every.ok && every.total === logged.length
    && every.read === logged.length && every.threadIds[every.threadIds.length - 1] === linux.id, JSON.stringify(every));
  check("an unknown thread id is refused", (await message({ type: "fm-reddit-threads-update", organization: "acme", threadIds: ["nope"] }, DASHBOARD)).error
    === "reddit_threads_empty");

  await message({ type: "fm-reddit-error", message: "boom", page: "/r/git/" }, TAB);
  check("errors land in the widget", (await message({ type: "fm-reddit-stats" }, POPUP)).errors[0].message === "/r/git/ boom");
  check("errors clear", (await message({ type: "fm-reddit-clear-errors" }, POPUP)).errors.length === 0);

  server.kill();
  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); server.kill(); process.exit(1); });
