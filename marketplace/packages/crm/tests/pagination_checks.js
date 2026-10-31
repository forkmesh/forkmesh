const fs = require("fs");
const path = require("path");

const source = fs.readFileSync(
  path.join(__dirname, "..", "content", "web", "page.js"), "utf8");
const body = source.slice(source.indexOf("=> {") + 4,
                          source.lastIndexOf("return initCrmPage();"));
const exported = ["state", "crmRowPage", "crmRowPageSlice", "crmSort", "crmLoadRemainingContacts",
  "crmCountCustomers", "CRM_FIRST_PAGE", "CRM_ROW_PAGE_SIZES"];
const fields = {};
const page = new Function(
  "$", "$$", "session", "escapeHtml", "relativeTimeLabel",
  `${body}\nreturn { ${exported.join(", ")}, generation: () => crmContactsGeneration, bump: () => ++crmContactsGeneration };`,
)((selector) => {
  const name = /data-crm-([a-z-]+)/.exec(selector)?.[1];
  return name in fields ? { value: fields[name] } : null;
}, () => [], () => ({ name: "Alice" }), (value) => String(value), (value) => String(value));

let failures = 0;
function check(condition, what) {
  if (!condition) { failures += 1; console.error(`FAIL ${what}`); }
}
function same(actual, expected, what) {
  check(JSON.stringify(actual) === JSON.stringify(expected),
        `${what}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}

const view = page.state.crmView;
view.organization = "acme";
const people = Array.from({ length: 230 }, (_, index) => ({ id: `c${index}`, name: `P${index}`, stage: index % 10 ? "lead" : "customer" }));

(async () => {
  same(page.crmRowPage.size, 100, "a browser without a saved size shows 100 rows a page");
  let slice = page.crmRowPageSlice(people);
  same([slice.start, slice.rows.length, slice.pages], [0, 100, 3], "the first page holds the first 100 rows");
  page.crmRowPage.index = 2;
  slice = page.crmRowPageSlice(people);
  same([slice.start, slice.rows.length, slice.rows[0].id], [200, 30, "c200"], "the last page holds the rest");
  slice = page.crmRowPageSlice(people.slice(0, 120));
  same([page.crmRowPage.index, slice.start], [1, 100], "a page past the end clamps to the last page");
  fields.search = "p1";
  slice = page.crmRowPageSlice(people);
  same(page.crmRowPage.index, 0, "changing the search starts again from the first page");
  page.crmRowPage.index = 1; page.crmRowPageSlice(people);
  page.crmSort.field = "name";
  page.crmRowPageSlice(people);
  same(page.crmRowPage.index, 0, "sorting by a column starts again from the first page");
  same(page.crmRowPageSlice([]).pages, 1, "an empty view still has one page");

  const pages = { "": people.slice(0, 2), x1: people.slice(2, 4), x2: people.slice(4, 5) };
  const next = { x1: "x2", x2: "" };
  const asked = [];
  global.fetch = async (url) => {
    const query = new URL(url, "https://example.test").searchParams;
    asked.push(`${query.get("limit")}:${query.get("cursor")}`);
    const cursor = query.get("cursor");
    return { ok: true, status: 200, json: async () => ({ ok: true, contacts: pages[cursor].concat(people.slice(0, 1)), next: next[cursor] }) };
  };
  view.contacts = pages[""].slice(); view.contactsTotal = 5; view.contactsLoading = true;
  await page.crmLoadRemainingContacts("x1", page.bump());
  same(asked, ["1000:x1", "1000:x2"], "the rest is read 1000 at a time, following each cursor");
  same(view.contacts.map((contact) => contact.id), ["c0", "c1", "c2", "c3", "c4"], "pages append once each, no duplicates");
  same(view.contactsLoading, false, "loading ends after the last page");
  same(view.summary.customers, 1, "customers are counted from every loaded contact");

  view.contacts = pages[""].slice(); view.revision = "r1"; asked.length = 0;
  const stale = page.bump();
  const loading = page.crmLoadRemainingContacts("x1", stale);
  page.bump();
  await loading;
  same(view.contacts.length, 2, "a load superseded by a newer read drops its pages");

  global.fetch = async () => ({ ok: false, status: 400, json: async () => ({ error: "bad_request" }) });
  view.contactsLoading = true; view.revision = "r2";
  await page.crmLoadRemainingContacts("x1", page.bump());
  same([view.contactsLoading, view.revision], [false, ""], "a failed page stops loading and forces a full re-read next time");

  if (failures) { console.error(`${failures} pagination check(s) failed`); process.exit(1); }
  console.log("pagination checks passed");
})().catch((error) => { console.error(error); process.exit(1); });
