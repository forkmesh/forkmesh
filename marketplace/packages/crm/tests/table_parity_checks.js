const fs = require("fs");
const path = require("path");

const source = fs.readFileSync(
  path.join(__dirname, "..", "content", "web", "page.js"), "utf8");
const body = source.slice(source.indexOf("=> {") + 4,
                          source.lastIndexOf("return initCrmPage();"));
const exported = [
  "state", "crmMergeTargets", "crmMergeLabel", "crmGithubContributorsCsv", "crmGithubRepositoryPath",
  "crmMaskedGitEmail", "crmCoAuthors", "crmOutreachPrompt", "crmMailComposeUrl", "crmNextChannels",
  "crmSortContacts", "crmContactMatchesQuery", "crmCustomColumnKey", "crmCustomColumnLabel",
  "crmForkMeshUsersCsv", "crmLinkedCellText", "crmLinkedLine", "crmTimestamp", "crmPictureSources",
  "crmPictureFor", "crmPictureCache", "crmPictures", "crmWithContactEdit", "crmPatchContacts",
  "crmMarkContacted", "crmSelection", "crmSelectContact", "crmMenuSelection", "crmSort", "crmFilteredContacts",
  "crmVisibleColumns", "crmColumnLabel", "crmDeleteColumnItems", "crmListColumns",
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

const view = page.state.crmView;
view.settings = { flows: [
  { id: "press", name: "Press", stages: ["new", "pitched"] },
  { id: "customers", name: "Customers", stages: ["lead", "customer"] },
] };

{
  const duplicate = { id: "d", name: "Ada Lovelace", email: "ada@x.org", company: "Analytical" };
  const contacts = [
    duplicate,
    { id: "z", name: "Zed" },
    { id: "c", name: "Charles Babbage", company: "analytical", flow: "press" },
    { id: "a", name: "Ada Lovelace", email: "ADA@x.org", company: "Analytical", flow: "customers" },
    { id: "b", name: "Ada King", company: "Other" },
    { id: "o", name: "Bob" },
  ];
  const targets = page.crmMergeTargets(duplicate, contacts);
  same(targets.map((target) => [target.id, target.score]),
       [["a", 9], ["b", 2], ["c", 1], ["o", 0], ["z", 0]],
       "merge targets: same email +4, each shared name word +2, same company +1, then by name");
  same(targets[0].label, "Ada Lovelace — ADA@x.org", "a target reads as name and email");
  same(targets[0].detail, "Analytical · Customers", "its company and pipeline follow");
  same(page.crmMergeLabel({ email: "x@y.z" }), "x@y.z", "a target without a name is its email");
  same(page.crmMergeLabel({}), "(unnamed contact)", "and without either says so");
}

{
  const commit = (author, committer, message, date) => ({ commit: {
    author: { name: author[0], email: author[1], date },
    committer: { name: committer[0], email: committer[1], date }, message } });
  const commits = [
    commit(["Ada", "ada@example.org"], ["GitHub", "noreply@github.com"],
      "Fix the loom\n\nCo-authored-by: Grace Hopper <grace@navy.mil>\nCo-authored-by: dependabot[bot] <49699333+dependabot[bot]@users.noreply.github.com>",
      "2026-10-01T10:00:00Z"),
    commit(["Ada", "ADA@example.org"], ["Ada", "ada@example.org"], "Tidy", "2026-09-01T10:00:00Z"),
    commit(["dependabot[bot]", "49699333+dependabot[bot]@users.noreply.github.com"], ["GitHub", "noreply@github.com"], "Bump", "2026-09-02T10:00:00Z"),
    commit(["Masked", "1234+masked@users.noreply.github.com"], ["Masked", "1234+masked@users.noreply.github.com"], "x", "2026-09-03T10:00:00Z"),
    commit(["Local", "dev@laptop.local"], ["Local", "dev@laptop.local"], "x", "2026-09-04T10:00:00Z"),
    commit(["Release Bot", "do-not-reply@corp.example.com"], ["Robot", "bot@ci.internal"], "x", "2026-09-05T10:00:00Z"),
    commit(["Linus", "linus@kernel.org"], ["Linus", "not an address"], "x", "2026-08-01T10:00:00Z"),
    commit(['Smith, "J"', "j@smith.dev"], ["Smith", "j@smith.dev"], "x", "2026-07-01T10:00:00Z"),
  ];
  const { csv, found } = page.crmGithubContributorsCsv(commits);
  same(csv, [
    "Name,Email,Commits,Last commit",
    "Ada,ada@example.org,2,2026-10-01",
    "Grace Hopper,grace@navy.mil,1,2026-10-01",
    "Linus,linus@kernel.org,1,2026-08-01",
    '"Smith, ""J""",j@smith.dev,1,2026-07-01',
  ].join("\r\n") + "\r\n", "contributors: trailers counted, masked, bot and unmailable addresses left out, ranked by commits then newest");
  same(found, 4, "four contributors have a mailable address");
  const capped = page.crmGithubContributorsCsv(commits, 1);
  same([capped.found, capped.csv.trim().split("\r\n").length], [4, 2], "the cap keeps the most active and still counts the rest");
  same(page.crmCoAuthors("x\n\nco-authored-by:  Grace <g@n.mil>\nCo-authored-by: nobody"), [["Grace", "g@n.mil"]],
       "a Co-authored-by trailer names a co-author; one with no address is skipped");
  for (const masked of ["noreply@github.com", "1+a@users.noreply.github.com", "do-not-reply@x.com", "donotreply@x.com", "no-reply@x.com"]) {
    check(page.crmMaskedGitEmail(masked), `masked: ${masked}`);
  }
  for (const open of ["replyto@x.com", "ada@noreplyhost.com", "ada@example.org"]) {
    check(!page.crmMaskedGitEmail(open), `not masked: ${open}`);
  }
}

for (const [typed, expected] of [
  ["forkmesh/forkmesh", "forkmesh/forkmesh"],
  ["https://github.com/octo-org/my.repo.git", "octo-org/my.repo"],
  ["github.com/a/b/tree/main/src", "a/b"],
  ["git@github.com:a/b.git", "a/b"],
  ["https://www.github.com/a/b?tab=readme", "a/b"],
  ["a/..", ""], ["notarepo", ""], ["https://gitlab.com/a/b", ""],
]) {
  same(page.crmGithubRepositoryPath(typed), expected, `repository path of "${typed}"`);
}

{
  const contact = { name: "Grace", mastodon: "@grace@mastodon.social", title: "Editor", company: "Tech Weekly",
    website: "https://tw.example", location: "Berlin", flow: "press" };
  same(page.crmOutreachPrompt(contact, "Journalists"), [
    "Tell me about ForkMesh (https://forkmesh.com).",
    "Then tell me about Grace (Mastodon: @grace@mastodon.social (https://mastodon.social/@grace); Editor at Tech Weekly; https://tw.example; Berlin).",
    "Then write a short toot (under 500 characters) I can send them on Mastodon about ForkMesh. They are on my “Journalists” sheet as a journalist, so pitch a story: why decentralized code hosting matters now and what is new about ForkMesh, and offer an interview or a demo. No sales talk. Mention them by their handle, make it specific to their work, and use at most two hashtags.",
  ].join("\n\n"), "the outreach prompt pitches a journalist a story");
  const plain = page.crmOutreachPrompt({ email: "f@x.org", title: "Founder", flow: "customers" }, "No pipeline");
  check(plain.includes("Then tell me about f@x.org (no Mastodon handle saved, so look it up; Founder)."),
        "no handle asks for one to be looked up, and the email stands in for a name");
  check(plain.includes("They are on my “Customers” sheet as a potential user,"), "the No pipeline tab falls back to the pipeline");
  const linked = page.crmOutreachPrompt({ name: "M", mastodon: "https://mastodon.social/@m" }, "");
  check(linked.includes("(Mastodon: https://mastodon.social/@m)."), "a profile link is not repeated");
  check(!linked.includes("They are on my"), "with no list and no pipeline there is no angle");
}

{
  view.settings = { ...view.settings, zoho: { region: "eu" } };
  same(page.crmMailComposeUrl(" ada@example.com "), "https://mail.zoho.eu/zm/comp.do?ct=mailto%3Aada%40example.com",
       "Zoho is the default compose window");
  view.settings = { ...view.settings, mailProvider: "gmail" };
  same(page.crmMailComposeUrl("ada@example.com"), "https://mail.google.com/mail/?view=cm&fs=1&to=ada%40example.com",
       "Gmail composes with view=cm");
  same(page.crmNextChannels({ email: "ada@example.com" }).map((channel) => [channel.kind, channel.label]),
       [["gmail", "Email with Gmail"]], "the next-contact card follows the provider");
  view.settings = { ...view.settings, mailProvider: "zoho" };
}

{
  const rows = [{ id: "1", name: "bob" }, { id: "2", name: "" }, { id: "3", name: "Alice" }, { id: "4", name: "carol" }, { id: "5", name: "Bob" }];
  same(page.crmSortContacts(rows, "name", false).map((row) => row.id), ["3", "1", "5", "4", "2"],
       "text sorts ignore case, keep ties in order, and sink empty cells");
  same(page.crmSortContacts(rows, "name", true).map((row) => row.id), ["4", "1", "5", "3", "2"],
       "descending still sinks empty cells");
  const dated = [{ id: "a", lastContactedAt: 10 }, { id: "b" }, { id: "c", lastContactedAt: 5 }];
  same(page.crmSortContacts(dated, "lastContactedAt", false).map((row) => row.id), ["b", "c", "a"], "dates sort as numbers");
  same(page.crmSortContacts(dated, "", false).map((row) => row.id), ["a", "b", "c"], "no sort column keeps the worker's order");
  view.emails = [
    { contactId: "a", provider: "gmail", sentAt: 100 },
    { contactId: "c", provider: "gmail", sentAt: 300 },
    { contactId: "b", provider: "mastodon", sentAt: 900 },
  ];
  same(page.crmSortContacts(dated, "linked-emails", true).map((row) => row.id), ["c", "a", "b"],
       "linked emails sort by the newest message, toots apart");
  same(page.crmSortContacts([{ id: "x", fields: { rank: "b" } }, { id: "y", fields: { rank: "a" } }], "rank", false).map((row) => row.id),
       ["y", "x"], "a custom column sorts by its value");
}

{
  const contact = { phone: "+44 20", notes: "Met at FOSDEM", nextStep: "Send deck", source: "Referral",
    flow: "press", fields: { region: "Nordics" }, tags: ["vip"], website: "anvil.example" };
  for (const query of ["+44", "fosdem", "send deck", "referral", "nordics", "vip", "press", "anvil"]) {
    check(page.crmContactMatchesQuery(contact, query), `search finds "${query}"`);
  }
  check(!page.crmContactMatchesQuery(contact, "zzz"), "search leaves out what is not there");
}

same(page.crmCustomColumnKey(" Deal Size (€) "), "deal_size", "a column name keys like a CSV header");
same(page.crmCustomColumnKey("__Next Step__"), "next_step", "underscores at the ends go");
same(page.crmCustomColumnKey("!!!"), "", "a name with no letters or digits has no key");
same(page.crmCustomColumnKey("x".repeat(80)).length, 64, "keys stop at 64 characters");
same(page.crmCustomColumnLabel("deal_size"), "Deal size", "the key reads back as a label");

{
  const users = [
    { name: "old", email: "Old@Example.com", createdAt: Date.UTC(2025, 0, 2), emailNotifications: false },
    { name: "new", email: "new@example.com", createdAt: Date.UTC(2026, 5, 7, 23, 30) },
    { name: "dup", email: "new@example.com", createdAt: Date.UTC(2026, 5, 8) },
    { name: "gone", email: "gone@example.com", status: "deleted" },
    { name: "org", email: "org@example.com", kind: "organization" },
    { name: "", email: "nameless@example.com" },
    { name: "private", email: "" },
  ];
  const result = page.crmForkMeshUsersCsv(users);
  same(result.csv, [
    "Name,Email,Signed up,Email notifications",
    "new,new@example.com,2026-06-07,On",
    "old,old@example.com,2025-01-02,Off",
  ].join("\r\n") + "\r\n", "users: active people with an address, newest first, one row per address");
  same([result.found, result.withoutEmail], [2, 1], "users without an address are counted apart");
}

{
  view.emails = [
    { contactId: "c", provider: "gmail", subject: "Hi", direction: "outbound", sentAt: Date.UTC(2026, 9, 1, 8, 0) },
    { contactId: "c", provider: "zoho", subject: "", direction: "inbound", createdAt: Date.UTC(2026, 9, 2, 9, 5) },
    { contactId: "c", provider: "mastodon", subject: "toot", direction: "outbound", sentAt: Date.UTC(2026, 9, 3, 7, 0) },
  ];
  same(page.crmTimestamp(0), "—", "an unset stamp is a dash");
  same(page.crmTimestamp(Date.UTC(2026, 9, 14, 9, 5)), "2026-10-14 09:05", "stamps read like the desktop's");
  same(page.crmLinkedCellText("c", false), "↙ (no subject) · 2026-10-02 09:05  (+1 more)",
       "the Linked emails cell is the newest message and how many more");
  same(page.crmLinkedCellText("c", true), "↗ toot · 2026-10-03 07:00", "toots have their own column");
  same(page.crmLinkedCellText("nobody", false), "", "no messages, an empty cell");
}

{
  const contact = { email: "Ada@X.org", mastodon: "@Ada@Mastodon.Social", bluesky: "ada.bsky.social", github: "@AdaL" };
  same(page.crmPictureSources(contact),
       ["mastodon:ada@mastodon.social", "bluesky:ada.bsky.social", "github:adal", "gravatar:ada@x.org"],
       "pictures are tried in the desktop's order, the email's Gravatar last");
  page.crmPictures.users = new Map([["ada@x.org", "data:image/png;base64,AAAA"]]);
  same(page.crmPictureSources(contact)[0], "forkmesh:ada@x.org", "a ForkMesh account's own picture comes first");
  same(page.crmPictureFor(page.crmPictureSources(contact)).url, "data:image/png;base64,AAAA", "and wins when it is known");
  page.crmPictures.users = new Map();
  const cache = page.crmPictureCache();
  same(page.crmPictureFor(page.crmPictureSources(contact)), { url: "", done: false, source: "mastodon:ada@mastodon.social" },
       "an unread source is the next one to read");
  cache.set("mastodon:ada@mastodon.social", { url: "", at: Date.now() });
  cache.set("bluesky:ada.bsky.social", { url: "https://cdn.bsky.app/a.jpg", at: Date.now() });
  same(page.crmPictureFor(page.crmPictureSources(contact)).url, "https://cdn.bsky.app/a.jpg", "a cached miss falls through to the next");
  cache.set("bluesky:ada.bsky.social", { url: "https://cdn.bsky.app/a.jpg", at: Date.now() - 8 * 86400000 });
  same(page.crmPictureFor(page.crmPictureSources(contact)).done, false, "a picture older than a week is read again");
  same(page.crmPictureSources({ name: "No one" }), [], "a record with no account or address has no picture");
}

{
  const contact = { id: "c", stage: "new", views: ["a"] };
  same(page.crmWithContactEdit(contact, { lastContactedAt: 7, owner: "bob", views: ["b"], removeViews: ["a"] }),
       { id: "c", stage: "new", views: ["b"], lastContactedAt: 7, owner: "bob" },
       "a bulk edit applies to the page's copy before the reply");
}

(async () => {
  view.organization = "acme";
  view.activeView = "";
  view.contacts = [{ id: "a", name: "A" }, { id: "b", name: "B" }, { id: "c", name: "C" }];
  const sent = [];
  globalThis.fetch = async (url, init) => {
    sent.push({ url, method: init.method, body: JSON.parse(init.body || "null") });
    const edits = JSON.parse(init.body).edits;
    return { ok: true, status: 200, json: async () => ({ ok: true, contacts: edits.map((edit) => ({ ...edit, name: "saved" })), missing: [] }) };
  };
  await page.crmMarkContacted(["a", "c"], 1234);
  same(sent.map((item) => [item.url, item.method]), [["/api/crm/acme/contacts/bulk", "POST"]], "Mark contacted now is one bulk request");
  same(sent[0].body, { edits: [{ id: "a", lastContactedAt: 1234 }, { id: "c", lastContactedAt: 1234 }] }, "with an edit per selected contact");
  same(view.contacts.map((contact) => [contact.id, contact.lastContactedAt, contact.name]),
       [["a", 1234, "saved"], ["b", undefined, "B"], ["c", 1234, "saved"]], "the reply's copies replace the rows");
  globalThis.fetch = async () => ({ ok: false, status: 400, json: async () => ({ error: "bad" }) });
  let reloaded = false;
  view.contacts = [{ id: "a", stage: "new" }];
  await page.crmPatchContacts([{ id: "a", stage: "won" }], "moved", "to the won stage").then(() => { reloaded = true; });
  check(reloaded, "a failed bulk edit settles after reloading");

  page.crmSelection.shown = ["a", "b", "c", "d"];
  page.crmSelectContact("b");
  page.crmSelectContact("d", { range: true });
  same([...page.crmSelection.ids], ["b", "c", "d"], "Shift+click selects the range from the last click");
  page.crmSelectContact("c", { toggle: true });
  same([...page.crmSelection.ids], ["b", "d"], "Ctrl+click toggles one row");
  view.contacts = ["a", "b", "c", "d"].map((id) => ({ id }));
  same(page.crmMenuSelection("d"), ["b", "d"], "a right-click inside the selection acts on all of it");
  same(page.crmMenuSelection("a"), ["a"], "a right-click outside it selects just that row");

  view.contacts = [{ id: "a", name: "A" }];
  view.views = []; view.activeShared = "";
  view.settings = { tableViews: [{ id: "view-x", name: "X", columns: ["name", "linked-emails", "linked-toots", "mastodon-latest"] }] };
  same(page.crmListColumns().filter((column) => column.computed).map((column) => column.field),
       ["mastodon-latest", "linked-emails", "linked-toots"], "the computed columns are always on offer");
  same(page.crmColumnLabel("mastodon-latest"), "Mastodon last update", "with the desktop's labels");
  same(page.crmDeleteColumnItems(page.crmListColumns().find((column) => column.field === "linked-emails")), [],
       "a computed column cannot be deleted");

  if (failures) {
    console.error(`${failures} check(s) failed`);
    process.exit(1);
  }
  console.log("table parity checks passed");
})();
