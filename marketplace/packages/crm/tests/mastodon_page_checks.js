const fs = require("fs");
const path = require("path");

const source = fs.readFileSync(
  path.join(__dirname, "..", "content", "web", "page.js"), "utf8");
const body = source.slice(source.indexOf("=> {") + 4,
                          source.lastIndexOf("return initCrmPage();"));
const exported = [
  "state", "CRM_SECTIONS", "selectCrmSection", "crmMastodon", "crmTootLength",
  "renderCrmMastodon", "loadCrmMastodon", "sendCrmMastodon", "skipCrmMastodon",
  "prepareCrmMastodon", "onCrmMastodonInput", "onCrmMastodonClick",
  "crmLeaderboardLine", "renderCrmLeaderboardStrip", "openCrmLeaderboard",
];

function element(dataset = {}) {
  const node = {
    innerHTML: "", textContent: "", hidden: false, disabled: false, value: "", dataset,
    attributes: {}, classes: new Set(), focused: false,
    setAttribute(name, value) { this.attributes[name] = value; },
    classList: {
      toggle(name, on) { if (on) node.classes.add(name); else node.classes.delete(name); },
      add(name) { node.classes.add(name); }, remove(name) { node.classes.delete(name); },
    },
    focus() { this.focused = true; },
    querySelector() { return null; },
  };
  return node;
}

const hosts = {};
const all = {};
const page = new Function(
  "$", "$$", "session", "escapeHtml", "relativeTimeLabel",
  `${body}\nreturn { ${exported.join(", ")} };`,
)((selector) => hosts[selector] || null, (selector) => all[selector] || [],
  () => ({ name: "jett", sessionToken: "t" }),
  (value) => String(value).replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;").replaceAll('"', "&quot;"),
  (value) => String(value));

let failures = 0;
function check(condition, what) {
  if (!condition) { failures += 1; console.error(`FAIL ${what}`); }
}
function same(actual, expected, what) {
  check(JSON.stringify(actual) === JSON.stringify(expected),
        `${what}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}
const flush = async () => { for (let i = 0; i < 20; i += 1) await new Promise((resolve) => setImmediate(resolve)); };

for (const hook of ["mastodon", "mastodon-unavailable", "mastodon-body", "mastodon-prepare",
                    "mastodon-stats", "mastodon-hint", "mastodon-queue", "mastodon-empty",
                    "mastodon-more", "mastodon-strategy", "mastodon-strategy-empty", "leaderboard"]) {
  hosts[`[data-crm-${hook}]`] = element();
}
const host = (hook) => hosts[`[data-crm-${hook}]`];
const cards = () => (host("mastodon-queue").innerHTML.match(/data-crm-mastodon-card="/g) || []).length;
const card = (id) => (host("mastodon-queue").innerHTML.split(`data-crm-mastodon-card="${id}"`)[1] || "").split("</article>")[0];

globalThis.location = new URL("https://app.forkmesh.com/dashboard/crm?org=acme");
globalThis.history = { state: null, replaceState(_state, _title, url) { globalThis.location = new URL(String(url)); } };
globalThis.CSS = { escape: (value) => String(value) };
const opened = [];
globalThis.window = { open: (...args) => { opened.push(args); return null; } };
const copied = [];
Object.defineProperty(globalThis, "navigator", {
  value: { clipboard: { writeText: async (text) => { copied.push(text); } } }, configurable: true,
});

const requests = [];
let answer = () => ({ status: 200, body: {} });
globalThis.fetch = async (url, init = {}) => {
  const request = { url: String(url), method: init.method || "GET", body: init.body ? JSON.parse(init.body) : null };
  requests.push(request);
  const { status, body: payload } = answer(request);
  return { ok: status >= 200 && status < 300, status, json: async () => payload };
};

const queueItem = (id, extra = {}) => ({
  id, name: `Name ${id}`, handle: `@${id}@mastodon.social`, stage: "new", flow: "customers",
  mastodon_draft: `@${id}@mastodon.social hello`, mastodon_status: "ready",
  mastodon_why: `Why ${id}`, mastodon_segment: "tor", mastodon_ask: "interview",
  mastodon_prepared: "2026-11-04 jett", ...extra,
});
const payload = (queue, extra = {}) => ({
  queue,
  stats: {
    today: { sent: 3, replied: 1 }, week: { sent: 12, replied: 2 },
    segments: [{ key: "tor", prepared: 5, sent: 4, replied: 1 }, { key: "researcher", prepared: 2, sent: 0, replied: 0 }],
    asks: [{ key: "interview", prepared: 7, sent: 4, replied: 1 }],
  },
  dailyTarget: 25, dailyLimit: 40, canSend: true, prompt: "Prepare more Mastodon toots", ...extra,
});

const view = page.state.crmView;
view.organization = "acme";
view.installed = true;

(async () => {
  same(page.crmTootLength("hello"), 5, "plain text counts its characters");
  same(page.crmTootLength("https://forkmesh.com/a/very/long/path/that/goes/on/and/on"), 23,
       "a link counts 23 however long it is");
  same(page.crmTootLength("see https://forkmesh.com/interview."), 4 + 23 + 1,
       "a sentence's full stop after a link still counts");
  same(page.crmTootLength("@ada@mastodon.social hi"), "@ada hi".length,
       "a remote mention counts only its user part");
  same(page.crmTootLength("Hi @ada@social.example.org and @bob, read https://example.com/x?y=1"),
       "Hi @ada and @bob, read ".length + 23, "mentions and links together");
  same(page.crmTootLength("mail ada@example.com"), "mail ada@example.com".length,
       "an email address is not a mention");
  same(page.crmTootLength("é👍"), 2, "a character is a character, not a UTF-16 unit");

  check(page.CRM_SECTIONS.includes("mastodon"), "Mastodon is a section");
  const tabs = page.CRM_SECTIONS.map((section) => element({ crmSectionTab: section }));
  const panels = page.CRM_SECTIONS.map((section) => element({ crmSection: section }));
  all["[data-crm-section-tab]"] = tabs; all["[data-crm-section]"] = panels;
  answer = () => ({ status: 200, body: payload([queueItem("c1"), queueItem("c2")]) });
  page.selectCrmSection("mastodon", true);
  same(view.section, "mastodon", "the Mastodon tab opens");
  same(location.searchParams.get("section"), "mastodon", "and is in the address");
  check(!panels.find((panel) => panel.dataset.crmSection === "mastodon").hidden
        && panels.filter((panel) => panel.dataset.crmSection !== "mastodon").every((panel) => panel.hidden),
        "only its panel shows");
  check(tabs.find((tab) => tab.dataset.crmSectionTab === "mastodon").focused, "the tab takes focus");
  await flush();
  check(/^\/api\/crm\/acme\/mastodon\?tz=-?\d+$/.test(requests[0]?.url || ""),
        `opening the tab reads the queue with the time zone (${requests[0]?.url})`);
  same(cards(), 2, "the queue renders one card per contact");
  const first = card("c1");
  check(first.includes(">Name c1<"), "a card names the contact");
  check(first.includes('href="https://mastodon.social/@c1"') && first.includes('target="_blank"'),
        "the handle links the profile on its instance");
  check(first.includes("data-crm-mastodon-segment") && first.includes(">tor<")
        && first.includes("data-crm-mastodon-ask") && first.includes(">interview<"), "segment and ask chips");
  check(first.includes("Why c1"), "the hook it opens with");
  check(first.includes('data-crm-mastodon-draft="c1"') && first.includes("@c1@mastodon.social hello</textarea>"),
        "an editable draft prefilled with the prepared toot");
  check(first.includes(`>${"@c1 hello".length} / 500<`), "with a live Mastodon character count");
  check(first.includes('data-crm-mastodon-send="c1"') && first.includes('data-crm-mastodon-skip="c1"'), "Send and Skip");
  check(!first.includes("bg-cta"), "Send is not the page's create button");
  check(host("mastodon-stats").innerHTML.includes(">3</span> / 25 sent today")
        && host("mastodon-stats").innerHTML.includes(">2</span> replies this week"),
        "the header shows today's sent against the target and replies this week");
  check(!host("mastodon-body").hidden && host("mastodon-unavailable").hidden, "the queue shows, not the unavailable line");
  const strategy = host("mastodon-strategy").innerHTML;
  check(strategy.indexOf('"segment:tor"') < strategy.indexOf('"segment:researcher"')
        && strategy.indexOf('"segment:researcher"') < strategy.indexOf('"ask:interview"'), "segments then asks");
  check(strategy.split('"segment:tor"')[1].split("</tr>")[0].includes(">25%<"), "reply rate is replies per toot sent");
  check(strategy.split('"segment:researcher"')[1].split("</tr>")[0].includes(">–<"), "nothing sent reads as a dash");

  const count = element();
  let replaced = "";
  Object.defineProperty(count, "outerHTML", { set(value) { replaced = value; } });
  hosts['[data-crm-mastodon-count="c1"]'] = count;
  const sendButton = element();
  hosts['[data-crm-mastodon-send="c1"]'] = sendButton;
  const long = `@c1@mastodon.social ${"x".repeat(480)}`;
  const textarea = { value: long, dataset: { crmMastodonDraft: "c1" } };
  page.onCrmMastodonInput({ target: { closest: () => textarea } });
  check(replaced.includes(`>${("@c1 ".length + 480).toLocaleString()} / 500<`) && !replaced.includes("text-destructive"),
        "typing recounts with the mention's user part");
  textarea.value = `${long} ${"y".repeat(30)}`;
  page.onCrmMastodonInput({ target: { closest: () => textarea } });
  check(replaced.includes("text-destructive") && sendButton.disabled, "over 500 turns red and Send is off");
  textarea.value = "@c1@mastodon.social edited https://forkmesh.com/interview";
  page.onCrmMastodonInput({ target: { closest: () => textarea } });
  check(!sendButton.disabled, "back under the limit Send is on again");

  requests.length = 0;
  answer = () => ({ status: 200, body: { ok: true } });
  await page.sendCrmMastodon("c1");
  same(requests.map((request) => [request.method, request.url, request.body]),
       [["POST", "/api/crm/acme/mastodon/send", { contactId: "c1", text: "@c1@mastodon.social edited https://forkmesh.com/interview" }]],
       "Send posts the edited text for the contact");
  same(cards(), 1, "a sent card leaves the queue");
  check(host("mastodon-stats").innerHTML.includes(">4</span> / 25 sent today"), "and today's count goes up");
  same(page.crmMastodon.data.stats.segments[0].sent, 5, "the segment's sent count goes up too");

  answer = () => ({ status: 409, body: { error: "follow_first" } });
  await page.sendCrmMastodon("c2");
  const locked = card("c2");
  check(locked.includes("data-crm-mastodon-locked") && locked.includes("data-crm-mastodon-follow")
        && locked.includes('href="https://mastodon.social/@c2"'), "409 follow_first turns the card into the locked state with Open profile");
  check(!locked.includes("data-crm-mastodon-send="), "a locked card has no Send");
  same(page.crmMastodon.data.queue[0].mastodon_status, "follow-first", "the card's status is follow-first");

  page.crmMastodon.data.queue.push(queueItem("c3"));
  answer = () => ({ status: 429, body: { error: "mastodon_daily_limit" } });
  await page.sendCrmMastodon("c3");
  check(card("c3").includes("limit of 40 toots in 24 hours") && card("c3").includes("text-destructive"),
        "429 mastodon_daily_limit shows the limit on the card");
  same(cards(), 2, "a refused card stays in the queue");

  requests.length = 0;
  answer = (request) => ({ status: 200, body: { contact: { id: "c3", fields: request.body.fields } } });
  await page.skipCrmMastodon("c3");
  same(requests.map((request) => [request.method, request.url, request.body]),
       [["PATCH", "/api/crm/acme/contacts/c3", { fields: { mastodon_status: "skipped" } }]],
       "Skip sets the contact's mastodon_status field to skipped");
  check(!card("c3"), "a skipped card leaves the queue");

  requests.length = 0;
  page.crmMastodon.data.canSend = false;
  page.crmMastodon.data.queue.push(queueItem("c4", { mastodon_draft: "@c4@mastodon.social hi & bye" }));
  page.renderCrmMastodon();
  check(host("mastodon-stats").innerHTML.includes("share page"), "the header says Send opens the share page");
  await page.sendCrmMastodon("c4");
  same(requests, [], "without an organization token nothing is posted");
  same(opened[0], [`https://mastodon.social/share?text=${encodeURIComponent("@c4@mastodon.social hi & bye")}`, "_blank", "noopener,noreferrer"],
       "Send opens Mastodon's share page with the text in a new tab");
  check(card("c4").includes("browser extension logs"), "and says the browser extension logs it");

  await page.prepareCrmMastodon();
  same(copied, ["Prepare more Mastodon toots"], "Prepare more copies the prompt");
  same(host("mastodon-hint").textContent, "Prompt copied: paste it into an agent session", "and says so");

  answer = () => ({ status: 200, body: payload(Array.from({ length: 60 }, (_, index) => queueItem(`q${index}`))) });
  await page.loadCrmMastodon();
  same(cards(), 50, "at most 50 cards show");
  check(!host("mastodon-more").hidden && host("mastodon-more").textContent === "Show 10 more of 10", "with a show more");
  page.onCrmMastodonClick({ target: { closest: (selector) => (selector === "[data-crm-mastodon-more]" ? {} : null) } });
  same(cards(), 60, "show more shows the rest");
  check(host("mastodon-more").hidden, "and then hides");

  answer = () => ({ status: 404, body: { error: "not_found" } });
  await page.loadCrmMastodon();
  check(!host("mastodon-unavailable").hidden && host("mastodon-unavailable").innerHTML.startsWith("Not available yet"),
        "a 404 reads as not available yet");
  check(host("mastodon-body").hidden && host("mastodon-prepare").disabled, "with no queue and no Prepare more");

  const day = (offset) => `2026-11-${String(offset).padStart(2, "0")}`;
  view.activity = {
    days: Array.from({ length: 10 }, (_, index) => day(index + 1)),
    members: [
      { name: "kaif", emails: [9, 9, 9, 0, 0, 0, 0, 0, 2, 0], outreach: [0, 0, 0, 0, 0, 0, 0, 0, 0, 3] },
      { name: "jett", emails: [0, 0, 0, 10, 10, 0, 0, 0, 0, 0], outreach: [0, 0, 0, 0, 0, 0, 0, 0, 0, 1], toots: [5, 0, 0, 5, 5, 5, 5, 5, 5, 0] },
      { name: "lee", emails: [0, 0, 0, 0, 0, 0, 0, 0, 0, 1] },
      { name: "max", emails: [0, 0, 0, 0, 0, 0, 0, 0, 0, 1] },
      { name: "quiet", emails: [4, 4, 4, 0, 0, 0, 0, 0, 0, 0] },
    ],
  };
  same(page.crmLeaderboardLine(), "7 days · jett 51 · kaif 5 · lee 1",
       "the strip sums emails, outreach and toots over the last 7 days, top three");
  page.renderCrmLeaderboardStrip();
  check(!host("leaderboard").hidden && host("leaderboard").textContent === "7 days · jett 51 · kaif 5 · lee 1", "and shows it");
  view.activity = { days: view.activity.days, members: [{ name: "quiet", emails: [4, 4, 4, 0, 0, 0, 0, 0, 0, 0] }] };
  page.renderCrmLeaderboardStrip();
  check(host("leaderboard").hidden && page.crmLeaderboardLine() === "", "nobody active in 7 days hides the strip");
  view.activity = { days: [], kinds: [], members: [] };
  page.renderCrmLeaderboardStrip();
  check(host("leaderboard").hidden, "no activity hides the strip");
  view.teamPeriod = 30;
  page.openCrmLeaderboard();
  same([view.section, view.teamPeriod], ["team", 7], "the strip opens the Team leaderboard on 7 days");

  if (failures) { console.error(`${failures} check(s) failed`); process.exit(1); }
  console.log("mastodon page checks passed");
})().catch((error) => { console.error(error); process.exit(1); });
