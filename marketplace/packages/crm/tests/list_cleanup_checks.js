const fs = require("fs");
const path = require("path");

const source = fs.readFileSync(
  path.join(__dirname, "..", "content", "web", "page.js"), "utf8");
const body = source.slice(source.indexOf("=> {") + 4,
                          source.lastIndexOf("return initCrmPage();"));
const exported = ["state", "crmCleanup", "crmCleanupPeople", "crmCleanupSorted",
  "crmCleanupPlan", "keepCrmPeopleOnList", "CRM_SECTIONS"];
const page = new Function(
  "$", "$$", "session", "escapeHtml", "relativeTimeLabel",
  `${body}\nreturn { ${exported.join(", ")} };`,
)(() => null, () => [], () => ({ name: "Alice", sessionToken: "t" }),
  (value) => String(value), (value) => String(value));

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
view.views = [
  { id: "inv1", name: "Investors", flow: "investors" },
  { id: "inv2", name: "Angels", flow: "investors" },
  { id: "a", name: "Conference" },
  { id: "b", name: "Newsletter" },
];
const contacts = () => [
  { id: "one", name: "One List", flow: "investors", stage: "new", views: ["a"], updatedAt: 1 },
  { id: "two", name: "Two Lists", flow: "investors", stage: "new", views: ["a", "b"], updatedAt: 5, lastContactedAt: 9 },
  { id: "p1", name: "Pat", flow: "investors", stage: "new", views: ["inv2"], updatedAt: 3, personKeys: ["email:pat@example.org"] },
  { id: "p2", name: "", email: "pat@example.org", flow: "customers", stage: "lead", views: ["a"], updatedAt: 4, personKeys: ["email:pat@example.org"] },
  { id: "s1", name: "Sam", flow: "investors", stage: "new", views: [], updatedAt: 2, personKeys: ["x:sam"] },
  { id: "s2", name: "Sam", flow: "investors", stage: "new", views: [], updatedAt: 2, personKeys: ["x:sam"] },
];
view.contacts = contacts();

(async () => {
  check(page.CRM_SECTIONS.includes("cleanup"), "the cleanup page is a section");
  const rows = page.crmCleanupPeople();
  same(rows.map((row) => row.id).sort(), ["p1", "two"],
       "one record on two plain lists and one person on two lists are found; a pipeline plus one list, or two records on one list, are not");
  const pat = rows.find((row) => row.id === "p1");
  same([pat.name, pat.email, pat.records.length, pat.lists.map((list) => list.id)],
       ["Pat", "pat@example.org", 2, ["inv2", "a"]], "a person row names the records and every list they show on");
  const two = rows.find((row) => row.id === "two");
  same(two.lists.map((list) => list.id), ["inv1", "a", "b"], "a record's pipeline list counts as one of its lists");

  same(page.crmCleanupSorted(rows, "records", true).map((row) => row.id), ["p1", "two"], "sorts by the number of records");
  same(page.crmCleanupSorted(rows, "name", false).map((row) => row.id), ["p1", "two"], "sorts by name");
  same(page.crmCleanupSorted(rows, "contacted", true).map((row) => row.id), ["two", "p1"], "sorts by last contact");
  same(page.crmCleanupSorted(rows, "lists", false).map((row) => row.id), ["p1", "two"], "sorts by the lists' names");

  same(page.crmCleanupPlan(two, "b"), {
    list: view.views[3], keep: two.lead, edit: { id: "two", setViews: ["b"] }, merge: [],
  }, "keeping one record on a plain list drops its other plain lists and stays in its pipeline");
  let plan = page.crmCleanupPlan(pat, "inv2");
  same([plan.keep.id, plan.merge, plan.edit], ["p1", ["p2"], { id: "p1", setViews: ["inv2"] }],
       "a pipeline list keeps the record already on it and merges the rest into it");
  plan = page.crmCleanupPlan(pat, "a");
  same([plan.keep.id, plan.merge, plan.edit], ["p2", ["p1"], { id: "p2", setViews: ["a"] }],
       "a plain list keeps the record on that list");
  same(page.crmCleanupPlan(pat, "b"), null, "a list the person is not on has no plan");
  view.contacts.find((contact) => contact.id === "p1").views = ["inv2", "b"];
  plan = page.crmCleanupPlan(page.crmCleanupPeople().find((row) => row.id === "p1"), "b");
  same(plan.edit, { id: "p1", setViews: ["b", "inv2"] }, "a plain list keeps the pipeline list the record was put on");
  view.contacts = contacts();

  const asked = [];
  global.confirm = (text) => { asked.push(text); return true; };
  const requests = [];
  global.fetch = async (url, options) => {
    const body = JSON.parse(options.body || "null");
    requests.push([options.method, url, body]);
    if (url.endsWith("/contacts/merge")) {
      return { ok: true, status: 200, json: async () => ({ ok: true, merged: body.from, contact: { id: body.into, name: "Pat", views: ["inv2", "a"], flow: "investors" } }) };
    }
    const edit = body.edits[0];
    return { ok: true, status: 200, json: async () => ({ ok: true, missing: [], contacts: [{ id: edit.id, name: "Pat", views: edit.setViews, flow: edit.flow || "investors", stage: "new" }] }) };
  };
  await page.keepCrmPeopleOnList(["p1"], "inv2");
  same(requests, [
    ["POST", "/api/crm/acme/contacts/merge", { from: "p2", into: "p1" }],
    ["POST", "/api/crm/acme/contacts/bulk", { edits: [{ id: "p1", setViews: ["inv2"] }] }],
  ], "a move merges the other records, then sets the kept record's lists");
  check(asked.length === 1 && asked[0].includes("of this person merges into"), "the move is confirmed first and says records merge");
  same(view.contacts.map((contact) => contact.id), ["one", "two", "p1", "s1", "s2"], "the merged record leaves the page");
  same(view.contacts.find((contact) => contact.id === "p1").views, ["inv2"], "the kept record shows the saved lists");
  same(page.crmCleanupPeople().map((row) => row.id), ["two"], "the person is no longer on more than one list");

  view.contacts = contacts(); requests.length = 0;
  view.contacts.find((contact) => contact.id === "one").views = ["a", "b"];
  view.contactsLoading = true;
  await page.keepCrmPeopleOnList(["one", "two"], "a");
  same(requests.length, 0, "nothing moves while contacts are still loading");
  view.contactsLoading = false;
  global.confirm = () => false;
  await page.keepCrmPeopleOnList(["one", "two"], "a");
  same(requests.length, 0, "a declined confirm moves nothing");
  global.confirm = (text) => { asked.push(text); return true; };
  await page.keepCrmPeopleOnList(["one", "two"], "a");
  same(requests.map((request) => request[2].edits[0]), [{ id: "one", setViews: ["a"] }, { id: "two", setViews: ["a"] }],
       "keeping everyone shown on a list moves each of them");
  check(asked[asked.length - 1].startsWith("Keep 2 people"), "a group move is confirmed once");
  same(page.crmCleanup.busy, false, "the page is free again after a move");

  if (failures) { console.error(`${failures} list cleanup check(s) failed`); process.exit(1); }
  console.log("list cleanup checks passed");
})().catch((error) => { console.error(error); process.exit(1); });
