"use strict";
const fs = require("fs"), path = require("path"), vm = require("vm");
const { spawn } = require("child_process");
const readline = require("readline");

const EXTENSION = path.join(__dirname, "..", "content", "extension");
const server = spawn(process.env.PYTHON || "python3", [path.join(__dirname, "crm_stdio_server.py")],
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

const storage = { sessionToken: "token", account: "charlie", organization: "acme" };
const session = {};
const fetched = [];
let listener = null;
const noop = { addListener() {} };
const area = (store) => ({
  async get(keys) { return Object.fromEntries([keys].flat().filter((key) => key in store).map((key) => [key, JSON.parse(JSON.stringify(store[key]))])); },
  async set(values) { Object.assign(store, JSON.parse(JSON.stringify(values))); },
  async remove(keys) { for (const key of [keys].flat()) delete store[key]; },
});
const chrome = {
  runtime: {
    id: "ext", getURL: (suffix) => `chrome-extension://ext/${suffix}`,
    onMessage: { addListener(callback) { listener = callback; } },
    onInstalled: noop, onStartup: noop,
  },
  permissions: { onRemoved: noop, contains: async () => false },
  scripting: { getRegisteredContentScripts: async () => [], registerContentScripts: async () => {}, unregisterContentScripts: async () => {} },
  storage: { local: area(storage), session: area(session) },
  tabs: { async create() { return { id: 1 }; }, async get() { return null; }, async sendMessage() {} },
  alarms: { onAlarm: noop, async create() {}, async clear() {} },
};
const backgroundContext = {
  chrome, URL, URLSearchParams, console, Promise, crypto: globalThis.crypto, setTimeout, clearTimeout,
  importScripts() {},
  ForkMeshVaultBackground: { install: () => ({ forget: async () => {} }) },
  ForkMeshRepoBackground: { install: () => {} },
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

const POPUP = { id: "ext", url: "chrome-extension://ext/popup.html" };
const MASTODON_TAB = { id: "ext", url: "https://mastodon.social/@forkmesh/113489000000000001", tab: { id: 4 } };
function message(request, sender) {
  return new Promise((resolve) => {
    if (!listener(request, sender, resolve)) setImmediate(() => resolve(undefined));
  });
}

const toot = (statusId, author, mine, people) => ({
  statusId, threadId: statusId, author: { handle: author, name: "" }, mine, people,
  subject: "Shipping the mesh today", sentAt: 1_760_000_000_000,
  providerUrl: `https://mastodon.social/@${author.split("@")[0]}/${statusId}`,
});

(async () => {
  const ada = await rpc({ actor: "alice", method: "POST", path: "/api/crm/acme/contacts",
    body: { name: "Ada Lovelace", mastodon: "@ada@mathstodon.xyz" } });
  check("a known contact exists before the post is opened", ada.status === 201);

  const mine = toot("113489000000000001", "forkmesh@mastodon.social", "forkmesh@mastodon.social", [
    { handle: "grace@hachyderm.io", name: "Grace Hopper" },
    { handle: "ada@mathstodon.xyz", name: "" },
    { handle: "forkmesh@mastodon.social", name: "" },
    { handle: "#hashtag", name: "" },
  ]);
  const logged = await message({ type: "fm-track-toot", toot: mine }, MASTODON_TAB);
  check("your own post logs everyone it mentions", logged.ok && logged.tracked && logged.logged === 2
    && logged.created === 1, JSON.stringify(logged));
  const links = fetched.filter((call) => call.path === "/api/crm/acme/emails/link");
  check("one link per person, the first by the status id and the next by status/handle",
    links.map((call) => call.body.messageId).join() === "113489000000000001,113489000000000001/ada@mathstodon.xyz"
    && links.every((call) => call.body.provider === "mastodon" && call.body.direction === "outbound"
      && call.body.createContact === true && call.body.account === "forkmesh@mastodon.social"),
    JSON.stringify(links.map((call) => call.body)));
  check("the signed-in account never asks the CRM settings who you are",
    !fetched.some((call) => call.path === "/api/crm/acme/settings"));

  const contacts = await rpc({ actor: "alice", method: "GET", path: "/api/crm/acme/contacts" });
  const grace = contacts.data.contacts.find((contact) => contact.mastodon === "@grace@hachyderm.io") || {};
  check("a person the CRM did not know becomes a contact", grace.name === "Grace Hopper"
    && contacts.data.contacts.length === 2, JSON.stringify(contacts.data.contacts));

  const pings = (await rpc({ control: "pings" })).data.pings.filter((ping) => ping.recipient === "charlie");
  check("a ping for each person logged", pings.map((ping) => ping.title).join("|")
    === "🎉 New CRM contact from Mastodon|🎉 Mastodon post tracked in the CRM", JSON.stringify(pings));

  const before = fetched.length;
  const again = await message({ type: "fm-track-toot", toot: mine }, MASTODON_TAB);
  check("the same post opened again sends nothing", again.ok && !again.tracked && again.seen
    && fetched.length === before);

  const settings = await message({ type: "fm-toot-settings" }, POPUP);
  check("the popup shows who was logged last", settings.track && settings.last.contacts.join() === "Grace Hopper,Ada Lovelace"
    && settings.last.created === 1 && settings.last.error === "", JSON.stringify(settings));

  const theirs = toot("113489000000000002", "bob@mastodon.social", "forkmesh@mastodon.social",
    [{ handle: "grace@hachyderm.io", name: "" }]);
  const skipped = await message({ type: "fm-track-toot", toot: theirs }, MASTODON_TAB);
  check("someone else's post is not logged", skipped.ok && !skipped.tracked && skipped.mine === false
    && !fetched.slice(before).some((call) => call.path.endsWith("/emails/link")), JSON.stringify(skipped));

  const connected = await rpc({ actor: "alice", method: "PUT", path: "/api/crm/acme/settings",
    body: { mastodon: { account: "@forkmesh@mastodon.social", token: "secret-token" } } });
  check("the organization connects its Mastodon account", connected.status === 200);
  delete session.mastodonTootAccount;
  const signedOut = toot("113489000000000003", "forkmesh@mastodon.social", "",
    [{ handle: "linus@fosstodon.org", name: "Linus" }]);
  const byOrg = await message({ type: "fm-track-toot", toot: signedOut }, MASTODON_TAB);
  check("signed out of Mastodon, the CRM's connected account still makes the post yours",
    byOrg.ok && byOrg.tracked && byOrg.created === 1, JSON.stringify(byOrg));

  const off = await message({ type: "fm-toot-set-track", track: false }, MASTODON_TAB);
  check("a Mastodon page cannot switch the logging", off.ok === false && off.error === "forbidden_sender");
  await message({ type: "fm-toot-set-track", track: false }, POPUP);
  const quiet = fetched.length;
  const paused = await message({ type: "fm-track-toot",
    toot: toot("113489000000000004", "forkmesh@mastodon.social", "forkmesh@mastodon.social",
      [{ handle: "grace@hachyderm.io", name: "" }]) }, MASTODON_TAB);
  check("switched off in the popup, nothing is sent", paused.ok && !paused.tracked && fetched.length === quiet);

  server.kill();
  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); server.kill(); process.exit(1); });
