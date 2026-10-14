// The CRM page's own helpers, run for real: the handle parsers that decide
// which accounts a detail card reads, and the lines it shows them as.
//
// The page is a dashboard module, so it hands nothing back on its own. The
// checks lift its body out of the registerPage call and ask it for the
// helpers by name, which keeps them honest without exporting anything the
// browser does not need.
const fs = require("fs");
const path = require("path");

const source = fs.readFileSync(
  path.join(__dirname, "..", "content", "web", "page.js"), "utf8");
const body = source.slice(source.indexOf("=> {") + 4,
                          source.lastIndexOf("return initCrmPage();"));
const exported = [
  "crmMastodonKey", "crmBlueskyActor", "crmGithubUser", "crmLiveAccounts",
  "crmHttpsUrl", "crmPictureUrl", "crmPlainText", "crmGithubEventLine",
  "crmContactLabel", "crmContactUrl", "crmListUrl", "crmDeleteColumnItems", "crmVisibleColumns",
  "selectCrmTableView", "crmTableViewId", "state",
  "crmSetColumnHidden", "crmTableViewDraftFields", "saveCrmTableViewDraft",
  "crmOwnerMatches", "crmFocusMatches", "crmFlow", "crmInvestorFlow",
  "crmAccountChip", "crmAccountProof",
  "crmNextContact", "crmNextChannels", "crmZohoComposeUrl",
  "crmAiPrompt", "crmAiAngle", "crmAiTargetUrl", "crmAiMessageText", "crmAiDefaultFormat", "crmAiMode", "crmAiBrief",
];
const page = new Function(
  "$", "$$", "session", "escapeHtml", "relativeTimeLabel",
  `${body}\nreturn { ${exported.join(", ")} };`,
)(() => null, () => [], () => ({ name: "Alice" }), (value) => String(value), (value) => String(value));

let failures = 0;
function check(condition, what) {
  if (!condition) {
    failures += 1;
    console.error(`FAIL ${what}`);
  }
}

function same(actual, expected, what) {
  check(JSON.stringify(actual) === JSON.stringify(expected),
        `${what}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}

// A fediverse handle, however it was typed, names the instance to ask.
for (const typed of ["@grace@mastodon.social", "grace@mastodon.social",
                     "mastodon.social/@grace", "https://mastodon.social/@grace"]) {
  same(page.crmMastodonKey(typed), "grace@mastodon.social", `mastodon key of "${typed}"`);
}
for (const typed of ["grace", "", "@grace", "grace@localhost"]) {
  same(page.crmMastodonKey(typed), "", `no mastodon key from "${typed}"`);
}

// A Bluesky handle, a profile link, or the account's DID.
same(page.crmBlueskyActor("@Grace.bsky.social"), "grace.bsky.social", "bluesky handle");
same(page.crmBlueskyActor("https://bsky.app/profile/grace.bsky.social"),
     "grace.bsky.social", "bluesky profile link");
same(page.crmBlueskyActor("did:plc:abc123"), "did:plc:abc123", "bluesky DID");
same(page.crmBlueskyActor("grace"), "", "a bare word is no Bluesky account");

// A GitHub username, "@username", or a profile link.
same(page.crmGithubUser("@octocat"), "octocat", "github handle");
same(page.crmGithubUser("https://github.com/octocat"), "octocat", "github profile link");
same(page.crmGithubUser("not a user!"), "", "a phrase is no GitHub username");
same(page.crmGithubUser(`a${"b".repeat(40)}`), "", "an over-long username is refused");

// The accounts a record is read from, in the order the picture is tried.
const accounts = page.crmLiveAccounts({
  mastodon: "@grace@mastodon.social", bluesky: "grace.bsky.social",
  github: "octocat", twitter: "@grace", linkedin: "linkedin.com/in/grace",
});
same(accounts.map((account) => account.service), ["mastodon", "bluesky", "github"],
     "only the three services with a public read are listed");
same(accounts[0].url, "https://mastodon.social/@grace", "the instance profile is linked");
// GitHub serves a picture by username, so the card shows one before any read.
same(accounts[2].picture, "https://avatars.githubusercontent.com/octocat?s=192",
     "the GitHub picture is known up front");
check(!accounts[0].picture && !accounts[1].picture,
      "Mastodon and Bluesky pictures wait for the profile read");
same(page.crmLiveAccounts({ name: "Grace" }), [], "a record with no handles reads nothing");

// Only https addresses are ever shown, and never Mastodon's placeholder.
same(page.crmHttpsUrl("http://example.com"), "", "http is not a link");
same(page.crmHttpsUrl("javascript:alert(1)"), "", "a script URL is not a link");
same(page.crmHttpsUrl("https://example.com/post"), "https://example.com/post",
     "https is kept as it stands");
same(page.crmPictureUrl("https://mastodon.social/avatars/original/missing.png"), "",
     "the instance placeholder is not a picture");

// A post reads as one line. The HTML a Mastodon status carries is reduced to
// its words by crmMastodonText, which parses it in the browser's own inert
// document, so only the line itself is checked here.
same(page.crmPlainText("  Shipped v2\n\nand wrote it up  "), "Shipped v2 and wrote it up",
     "a post reads as one line");
check(page.crmPlainText("word ".repeat(80)).endsWith("…"),
      "a long post is cut with an ellipsis");
check(page.crmPlainText("word ".repeat(80)).length <= 220,
      "a long post is cut to the card's width");
same(page.crmPlainText("word ".repeat(80), 20).length, 20, "a caller can ask for less");

// A public event as one line.
same(page.crmGithubEventLine({
  type: "PushEvent", repo: { name: "acme/widget" },
  payload: { size: 3, commits: [{ message: "Fix the thing" }] },
}), "pushed 3 commits in acme/widget: Fix the thing", "a push names its commits");
same(page.crmGithubEventLine({
  type: "PullRequestEvent", repo: { name: "acme/widget" },
  payload: { action: "opened", number: 7, pull_request: { title: "Add a card" } },
}), "opened pull request #7 in acme/widget: Add a card", "a pull request names its title");
same(page.crmGithubEventLine({ type: "WatchEvent", repo: { name: "acme/widget" }, payload: {} }),
     "starred acme/widget", "a star takes the repository as its object");
same(page.crmGithubEventLine({ type: "ReleaseEvent", repo: { name: "acme/widget" },
                               payload: { release: { tag_name: "v2.1.0" } } }),
     "released v2.1.0 in acme/widget", "a release names its tag");
// An event type the page has never heard of still reads as a sentence.
same(page.crmGithubEventLine({ type: "SponsorshipEvent", repo: { name: "acme/widget" },
                               payload: {} }),
     "sponsorship in acme/widget", "an unknown event still reads as a line");

same(page.crmContactLabel({ email: "grace@example.com" }), "grace@example.com",
     "a record with no name is known by its address");
same(page.crmContactLabel({}), "Unnamed contact", "a record with neither is named plainly");

globalThis.location = new URL("https://app.forkmesh.com/dashboard/crm?section=inbox&org=old");
page.state.crmView.organization = "Acme & Co";
same(page.crmContactUrl("c 1"),
     "https://app.forkmesh.com/dashboard/crm?contact=c+1",
     "a contact link names the contact and nothing else");
same(page.crmListUrl("0123abcd"),
     "https://app.forkmesh.com/dashboard/crm?list=0123abcd",
     "a list link names the list and nothing else, not the organization");

page.state.crmView.canManageSettings = false;
same(page.crmDeleteColumnItems({ field: "email", custom: false }), [],
     "the email column cannot be deleted");
same(page.crmDeleteColumnItems({ field: "stage", custom: false }), [],
     "the stage column cannot be deleted");
same(page.crmDeleteColumnItems({ field: "region", custom: true }).map((item) => [item.label, Boolean(item.disabled)]),
     [["Delete column…", false]], "any member deletes a custom column");
same(page.crmDeleteColumnItems({ field: "phone", custom: false }).map((item) => [item.label, Boolean(item.disabled)]),
     [["Delete column… (owners and admins)", true]], "a member sees a built-in column's delete switched off");
page.state.crmView.canManageSettings = true;
same(page.crmDeleteColumnItems({ field: "phone", custom: false }).map((item) => [item.label, Boolean(item.disabled)]),
     [["Delete column…", false]], "an admin deletes a built-in column");

// Owner and follow-up filters: mine, unassigned, one teammate, and the
// three follow-up slices.
const owned = { owner: "alice" }, unowned = { owner: " " }, theirs = { owner: "Kaif" };
check(page.crmOwnerMatches(owned, "@me") && !page.crmOwnerMatches(theirs, "@me"), "my contacts are the ones I own");
check(page.crmOwnerMatches(unowned, "@none") && !page.crmOwnerMatches(owned, "@none"), "unassigned means no owner");
check(page.crmOwnerMatches(theirs, "kaif") && !page.crmOwnerMatches(owned, "kaif"), "a teammate's contacts");
const daysAgo = (days) => Date.now() - days * 86400000;
check(page.crmFocusMatches({ nextStep: "Call" }, "next-step") && !page.crmFocusMatches({ nextStep: "" }, "next-step"), "next step set");
check(page.crmFocusMatches({ lastContactedAt: 0 }, "uncontacted") && !page.crmFocusMatches({ lastContactedAt: daysAgo(1) }, "uncontacted"), "never contacted");
check(page.crmFocusMatches({ lastContactedAt: daysAgo(40), stage: "lead" }, "quiet"), "quiet after 30 days");
check(!page.crmFocusMatches({ lastContactedAt: daysAgo(40), stage: "inactive" }, "quiet"), "a closed contact is never quiet");
check(!page.crmFocusMatches({ lastContactedAt: daysAgo(5), stage: "lead" }, "quiet"), "a recent contact is not quiet");

page.state.crmView.emails = [
  { contactId: "w1", direction: "outbound", sentAt: 100 },
  { contactId: "w1", direction: "inbound", sentAt: 200 },
  { contactId: "w2", direction: "inbound", sentAt: 100 },
  { contactId: "w2", direction: "outbound", sentAt: 200 },
  { contactId: "w3", direction: "inbound", sentAt: 300 },
];
check(page.crmFocusMatches({ id: "w1", stage: "lead" }, "waiting"), "a reply we have not answered waits on us");
check(!page.crmFocusMatches({ id: "w2", stage: "lead" }, "waiting"), "an answered reply does not wait");
check(!page.crmFocusMatches({ id: "w3", stage: "passed" }, "waiting"), "a closed contact does not wait");
check(page.crmFocusMatches({ id: "w1", owner: "alice", stage: "lead" }, "my-queue"), "my waiting contact is in my queue");
check(page.crmFocusMatches({ id: "q1", owner: "alice", stage: "lead", nextStep: "Call" }, "my-queue"), "my next step is in my queue");
check(!page.crmFocusMatches({ id: "w1", owner: "kaif", stage: "lead" }, "my-queue"), "a teammate's contact is not in my queue");
check(!page.crmFocusMatches({ id: "q2", owner: "alice", stage: "lead" }, "my-queue"), "nothing to do keeps it out of my queue");
page.state.crmView.emails = [];

const day = Date.UTC(2026, 9, 1);
const payer = { id: "p1", stage: "lead", account: { account: "payer", signedUpAt: day - 86400000 * 60,
  paid: { tier: "pro", since: day - 86400000 * 30, until: day + 86400000, active: true },
  trial: null, promotion: null, suggestedStage: "customer", recordedAt: day } };
check(page.crmFocusMatches(payer, "paying"), "an active paid membership is a paying member");
check(page.crmFocusMatches(payer, "evidence-ahead"), "a payer still at lead sits behind the evidence");
check(!page.crmFocusMatches({ ...payer, stage: "customer" }, "evidence-ahead"), "a payer at customer is caught up");
check(!page.crmFocusMatches({ id: "s1", account: { account: "plain", paid: null } }, "paying"), "a signup alone is not paying");
check(page.crmAccountChip(payer).includes("Pro member") && page.crmAccountChip(payer).includes('data-crm-account-advance="p1"'),
      "the chip names the membership and offers the suggested move");
check(page.crmAccountProof(payer).includes("ForkMesh account payer, signed up 2026-08-02")
      && page.crmAccountProof(payer).includes("Pro membership paid since 2026-09-01, runs 2026-10-02"),
      "the proof line says which account and which dates");
same(page.crmAccountChip({ id: "n1", account: { account: "" } }), "", "no account, no chip");
check(page.crmAccountChip({ id: "s2", stage: "lead", account: { account: "plain", signedUpAt: day, suggestedStage: "" } }) === '<span class="rounded-full border border-border px-2 py-0.5 text-[10px] font-semibold text-muted-foreground" title="ForkMesh account plain, signed up 2026-10-01. Checked .">Signed up 2026-10-01</span>',
      "a plain signup shows only its signup date");

// A pipeline shared for editing brings its own stages: the board and the
// add-contact form use them, not the reader's organization's flows.
page.state.crmView.settings = { flows: [{ id: "customers", name: "Customers", stages: ["lead"] }] };
page.state.crmView.activeShared = "shared-1";
page.state.crmView.sharedCache = { "shared-1": { view: { id: "shared-1", name: "Ugeh", flow: "partnerships",
  stages: ["prospect", "contacted"], canEdit: true }, contacts: [] } };
same(page.crmFlow("partnerships").stages, ["prospect", "contacted"], "a shared pipeline's stages");
same(page.crmFlow("customers").stages, ["lead"], "the reader's own flows are unchanged");
page.state.crmView.activeShared = "";
same(page.crmFlow("partnerships").stages, ["lead"], "outside the shared tab an unknown flow falls back");
page.state.crmView.sharedCache = {};

// The investor deck is for investor pipelines; anyone else needs a confirm.
page.state.crmView.settings = { flows: [
  { id: "customers", name: "Customers", icon: "customers", stages: ["lead"] },
  { id: "backers", name: "Backers", icon: "investors", stages: ["new"] },
  { id: "investment", name: "Investment", stages: ["sourced"] }] };
check(!page.crmInvestorFlow("customers"), "a customer is not an investor");
check(page.crmInvestorFlow("backers") && page.crmInvestorFlow("investment"), "investor pipelines by icon or id");

page.state.crmView.contacts = [{ id: "c1", fields: { commits: "3", region: "EU" } }];
page.state.crmView.views = [{ id: "list", name: "Contributors", columns: [
  { field: "name" }, { field: "email" }, { field: "commits", custom: true }] }];
page.state.crmView.activeView = "list";
page.state.crmView.activeShared = "";
page.state.crmView.settings = { removedColumns: ["twitter"], tableViews: [
  { id: "view-a", name: "Outreach", columns: ["stage", "name", "mastodon-latest"] }] };
const shown = () => page.crmVisibleColumns().map((column) => column.field);
same(page.crmTableViewId(), "default", "a list opens in the Default view");
same(shown(), ["name", "email", "mastodon", "bluesky", "linkedin", "github", "lastContactedAt"],
     "the Default view, less a deleted column");
page.selectCrmTableView("all");
check(["name", "phone", "notes", "lastContactedAt", "commits"].every((field) => shown().includes(field)),
      "the All view shows every column the list carries");
check(!shown().includes("region") && !shown().includes("twitter"),
      "the All view leaves out another list's column and a deleted one");
page.selectCrmTableView("view-a");
same(shown(), ["stage", "name"], "a custom view shows its own columns in its own order");
page.selectCrmTableView("view-gone");
same(page.crmTableViewId(), "default", "a deleted view falls back to Default");

(async () => {
  globalThis.prompt = () => { check(false, "a column change on a built-in view asks for nothing"); return ""; };
  const puts = [];
  globalThis.fetch = async (url, init) => {
    puts.push(JSON.parse(init.body));
    return { ok: true, status: 200, json: async () => ({ settings: JSON.parse(init.body) }) };
  };
  page.crmSetColumnHidden("github", true);
  same(shown(), ["name", "email", "mastodon", "bluesky", "linkedin", "lastContactedAt"],
       "hiding a column on Default takes it off the table at once");
  same(page.crmTableViewDraftFields(), ["name", "email", "mastodon", "bluesky", "linkedin", "lastContactedAt"],
       "Default is marked modified with the columns on screen");
  same(puts, [], "a modified Default saves nothing until Save view");
  page.crmSetColumnHidden("github", false);
  page.crmSetColumnHidden("linkedin", true);
  page.crmSetColumnHidden("linkedin", false);
  check(page.crmTableViewDraftFields() !== null, "a re-ordered Default is still modified");
  page.selectCrmTableView("all");
  page.selectCrmTableView("default");
  same(page.crmTableViewDraftFields(), null, "picking another view drops the unsaved change");
  page.crmSetColumnHidden("mastodon", true);
  await page.saveCrmTableViewDraft();
  same(puts.length, 1, "Save view stores the view once");
  const saved = puts[0].tableViews.at(-1);
  same([saved.name, saved.columns], ["View 1", ["name", "email", "bluesky", "linkedin", "github", "lastContactedAt"]],
       "Save view names the new view on its own");
  same(page.crmTableViewId(), saved.id, "the saved view is the one open");
  same(page.crmTableViewDraftFields(), null, "a saved view is no longer modified");

  {
    const day = 86400000; const now = 1_800_000_000_000;
    const view = page.state.crmView;
    const saved = { contacts: view.contacts, emails: view.emails, settings: view.settings };
    view.settings = { zoho: { region: "eu" } };
    same(page.crmZohoComposeUrl("ada@example.com"), "https://mail.zoho.eu/zm/comp.do?ct=mailto%3Aada%40example.com",
         "Email with Zoho opens a compose window in the organization's data center");
    same(page.crmNextChannels({ email: "ada@example.com", mastodon: "@ada@mastodon.social", twitter: "@ada" }).map((c) => [c.kind, c.url]),
         [["zoho", "https://mail.zoho.eu/zm/comp.do?ct=mailto%3Aada%40example.com"],
          ["mastodon", "https://mastodon.social/@ada"], ["twitter", "https://x.com/ada"]],
         "every channel the record names gets a button");
    same(page.crmNextChannels({ email: "not an address" }), [], "a record with nothing to reach gets no button");
    view.contacts = [
      { id: "fresh", email: "f@example.com", createdAt: now - 3 * day },
      { id: "quiet", email: "q@example.com", lastContactedAt: now - 40 * day },
      { id: "recent", email: "r@example.com", lastContactedAt: now - 2 * day },
      { id: "won", email: "w@example.com", lastContactedAt: now - 90 * day, stage: "customer" },
      { id: "unreachable", lastContactedAt: now - 90 * day },
    ];
    view.emails = [];
    let follow = page.crmNextContact("follow", now);
    same([follow.contact?.id, follow.due], ["quiet", 1], "follow up picks someone contacted and quiet for 14+ days");
    check(follow.reason.includes("40 days ago"), "the reason says how long it has been");
    let reach = page.crmNextContact("reach", now);
    same([reach.contact?.id, reach.due], ["fresh", 1], "reach out picks someone never contacted");
    view.contacts.push(
      { id: "ranked", mastodon: "@r@mastodon.social", createdAt: now - 1 * day, fields: { priority_rank: "4" } },
      { id: "ranked-better", email: "rb@example.com", createdAt: now - 1 * day, fields: { rank: "2", coding_fit_score: "70" } },
      { id: "valued", email: "v@example.com", createdAt: now - 1 * day, value: 5000 },
      { id: "lost", email: "l@example.com", stage: "lost" });
    reach = page.crmNextContact("reach", now);
    same([reach.contact?.id, reach.due], ["valued", 4], "a deal value qualifies a new lead first, and an ended stage is left out");
    check(reach.reason.includes("deal value"), "the reason says what qualifies them");
    view.nextSkipped.add("valued");
    reach = page.crmNextContact("reach", now);
    same(reach.contact?.id, "ranked-better", "then the lowest rank in a rank column");
    check(reach.reason.includes("ranked 2 (rank)") && reach.reason.includes("score 70"), "the reason names the rank and score columns");
    view.nextSkipped.clear();
    view.contacts.push({ id: "replied", twitter: "@rep", stage: "customer", lastContactedAt: now - 5 * day });
    view.emails = [{ contactId: "replied", direction: "inbound", subject: "Pricing", sentAt: now - day },
                   { contactId: "replied", direction: "outbound", status: "draft", sentAt: now }];
    follow = page.crmNextContact("follow", now);
    same(follow.contact?.id, "replied", "an unanswered reply wins at any stage, and an unsent draft is no answer");
    check(follow.reason.includes("Pricing"), "the reason names the message");
    view.contacts.push({ id: "new-reply", email: "n@example.com", createdAt: now - day });
    view.emails.push({ contactId: "new-reply", direction: "inbound", subject: "Hello", sentAt: now - 1000 });
    same(page.crmNextContact("follow", now).contact?.id, "new-reply", "a never-contacted lead who wrote in is a follow-up");
    check(page.crmNextContact("reach", now).contact?.id !== "new-reply", "and not a new lead");
    view.contacts = saved.contacts; view.emails = saved.emails; view.settings = saved.settings;
  }

  {
    const view = page.state.crmView;
    const saved = { organization: view.organization, settings: view.settings };
    const now = Date.parse("2026-10-13T12:00:00Z"); const day = 86400000;
    const contact = {
      id: "ada", name: "Ada Lovelace", email: "ada@example.com", company: "Engines Ltd", title: "CTO",
      mastodon: "@ada@hachyderm.io", github: "ada", stage: "contacted", nextStep: "Send the deck",
      notes: "Met at FOSDEM.\nLikes self-hosting.", lastContactedAt: now - 20 * day, createdAt: now - 40 * day,
      fields: { fund_size: "$40M", empty: "" },
    };
    const input = {
      contact, format: "email", mode: "follow", reason: "Last contacted 20 days ago.",
      brief: "We build ForkMesh.", organization: "forkmesh", sender: "jett", flowName: "Investors",
      messages: [
        { at: now - 2 * day, direction: "inbound", via: "Zoho Mail", subject: "Re: hello", text: "Sounds good, send more.\n\nOn Mon, Oct 1, 2026 jett wrote:\n> hello" },
        { at: now - 20 * day, direction: "outbound", via: "Zoho Mail", sender: "jett", subject: "hello", text: "Hi Ada" },
        { at: now - day, direction: "outbound", status: "draft", via: "Mailtrap", subject: "unsent" },
      ],
      updates: [
        { label: "Mastodon", handle: "@ada@hachyderm.io", url: "https://hachyderm.io/@ada", posts: [{ at: now - day, text: "Shipped a mirror", url: "https://hachyderm.io/@ada/1" }] },
        { label: "GitHub", handle: "@ada", url: "https://github.com/ada", posts: [], error: "the service answered 403" },
      ],
      deckUrl: "https://forkmesh.com/pitch-deck/abc123def456", thread: "",
    };
    const prompt = page.crmAiPrompt(input);
    check(prompt.startsWith("You are helping jett at forkmesh write an email to Ada Lovelace."), "the prompt opens with who writes what to whom");
    check(prompt.includes('"Subject:" line'), "an email asks for a subject line");
    check(prompt.includes("pick up where the conversation below left off"), "a follow-up continues the conversation");
    check(prompt.includes("## About us: forkmesh\nWe build ForkMesh."), "the brief is in the prompt");
    for (const line of ["- Title: CTO", "- Company: Engines Ltd", "- Pipeline: Investors, stage Contacted",
                        "- Next step on our record: Send the deck", "- Mastodon: https://hachyderm.io/@ada",
                        "- GitHub: https://github.com/ada", "- Their investor deck link: https://forkmesh.com/pitch-deck/abc123def456",
                        "- Fund Size: $40M", "Met at FOSDEM.\nLikes self-hosting."]) {
      check(prompt.includes(line), `the record line ${JSON.stringify(line)} is in the prompt`);
    }
    check(!prompt.includes("Empty:"), "an empty custom field is left out");
    check(prompt.includes("- 2026-10-12: Shipped a mirror (https://hachyderm.io/@ada/1)"), "their recent posts are in the prompt");
    check(prompt.includes("GitHub @ada (https://github.com/ada): could not be read (the service answered 403)"), "an unreadable account says so");
    const hello = prompt.indexOf("Subject: hello"); const reply = prompt.indexOf("Subject: Re: hello");
    check(hello > 0 && reply > hello, "past messages run oldest first");
    check(prompt.includes("We wrote (jett) via Zoho Mail") && prompt.includes("They wrote via Zoho Mail"), "each message says who wrote it");
    check(prompt.includes("  > Sounds good, send more.") && !prompt.includes("> hello"), "a quoted reply tail is cut");
    check(prompt.includes("[draft, not delivered]"), "an unsent draft is marked");
    check(prompt.endsWith("## Why now\nLast contacted 20 days ago."), "the reason closes the prompt");
    const toot = page.crmAiPrompt({ ...input, format: "toot", mode: "reach", thread: "https://hachyderm.io/@ada/9", messages: [], updates: [] });
    check(toot.includes("mentions @ada@hachyderm.io at the start") && toot.includes("at most 500 characters"), "a toot mentions them and fits Mastodon");
    check(toot.includes("reply in our existing thread: https://hachyderm.io/@ada/9"), "a toot replies in the thread we have");
    check(toot.includes("This is our first contact") && toot.includes("Nothing on record yet"), "a first contact says so");
    check(prompt.includes('They are on our "Investors" list as an investor'), "an investor pipeline sets the investor angle");
    check(page.crmAiPrompt({ ...input, group: "Press list" }).includes('on our "Press list" list as a journalist'), "the open list sets the angle first");
    check(page.crmAiPrompt({ ...input, group: "", flowName: "" }).split("\n")[3] === "", "no list and no pipeline give no angle");
    check(page.crmAiPrompt({ ...input, updates: null }).includes("Not read yet: open their profiles linked above."), "posts still being read are not reported missing");
    check(page.crmAiPrompt({ ...input, format: "message" }).includes("under 80 words, no subject line"), "a direct message is short");
    same(page.crmAiMessageText("Thanks!\n> old\nMore"), "Thanks!\nMore", "quoted lines drop out of a message");
    check(page.crmAiMessageText("x".repeat(5000)).length === 900, "a long message is cut");
    const short = page.crmAiTargetUrl("claude", "Hello there");
    same(short, { url: "https://claude.ai/new?q=Hello%20there", filled: true }, "Claude opens with the prompt filled in");
    same(page.crmAiTargetUrl("chatgpt", "a".repeat(20000)), { url: "https://chatgpt.com/", filled: false }, "a prompt too long for a link opens the bare site");
    same(page.crmAiDefaultFormat({ email: "a@b.co" }), "email", "an email address picks an email");
    same(page.crmAiDefaultFormat({ mastodon: "@a@hachyderm.io" }), "toot", "a Mastodon handle picks a toot");
    same(page.crmAiDefaultFormat({ twitter: "@a" }), "message", "otherwise a direct message");
    same(page.crmAiMode({ lastContactedAt: 0 }, []), "reach", "never contacted is a first contact");
    same(page.crmAiMode({ lastContactedAt: 0 }, [{ direction: "outbound", status: "draft" }]), "reach", "an unsent draft is not contact");
    same(page.crmAiMode({ lastContactedAt: 0 }, [{ direction: "inbound" }]), "follow", "a message from them is a conversation");
    view.settings = {}; view.organization = "forkmesh";
    check(page.crmAiBrief().includes("https://forkmesh.com/changelog"), "ForkMesh starts from its own brief with links");
    view.organization = "acme";
    same(page.crmAiBrief(), "", "another organization starts from its own saved brief only");
    view.settings = { messageBrief: "  Acme makes anvils.  " };
    same(page.crmAiBrief(), "Acme makes anvils.", "a saved brief wins");
    view.organization = saved.organization; view.settings = saved.settings;
  }

  if (failures) {
    console.error(`${failures} check(s) failed`);
    process.exit(1);
  }
  console.log("contact detail checks passed");
})();
