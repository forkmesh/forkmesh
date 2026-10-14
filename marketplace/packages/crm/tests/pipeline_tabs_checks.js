const fs = require("fs");
const path = require("path");

const source = fs.readFileSync(
  path.join(__dirname, "..", "content", "web", "page.js"), "utf8");
const body = source.slice(source.indexOf("=> {") + 4,
                          source.lastIndexOf("return initCrmPage();"));
const exported = [
  "state", "crmTabViews", "crmVirtualViews", "crmContactOnTab", "crmContactInView",
  "crmStripStages", "crmFirstStage", "crmStageColor", "crmStageInk", "crmStageLabel",
  "crmStageStripHtml", "crmMergePlan", "crmMoveToSheetEdits", "crmPipelineMoveItems",
  "crmPipelinePlan", "crmPipelineEditState", "crmRemovePipelineDraft", "crmFlowSlug",
  "crmStageSlug", "crmSheetShareSummary", "crmSheetShareItems", "crmCanDeleteList",
  "crmSyncActiveTab", "crmActiveView", "crmMembersAdded", "crmStoredColumns",
];

const stageSelect = {
  value: "",
  options: ["", "lead", "qualified", "customer", "new", "meeting", "committed", "applied", "hired"]
    .map((value) => ({ value })),
};
global.location = new URL("https://app.example.com/dashboard/crm");
global.history = { state: null, replaceState() {} };
const select = (selector) => (selector === "[data-crm-stage]" ? stageSelect : null);
const page = new Function(
  "$", "$$", "session", "escapeHtml", "relativeTimeLabel",
  `${body}\nreturn { ${exported.join(", ")} };`,
)(select, () => [], () => ({ name: "alice" }), (value) => String(value), (value) => String(value));

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

function reset() {
  view.organization = "acme";
  view.members = ["alice", "bob", "carol"];
  view.canManageSettings = true;
  view.activeShared = "";
  view.sharedOnly = false;
  view.activeView = "";
  view.stageTab = "";
  view.virtualColumns = {};
  view.settings = { flows: [
    { id: "customers", name: "Customers", icon: "customers", stages: ["lead", "qualified", "customer"] },
    { id: "investors", name: "Investors", icon: "investors", stages: ["new", "meeting", "committed"] },
    { id: "hiring", name: "Hiring", icon: "hiring", stages: ["applied", "hired"] },
  ] };
  view.views = [
    { id: "l1", name: "Seed", flow: "investors", canManage: true, owner: "alice", visibility: "organization",
      columns: [{ field: "name", hidden: false }, { field: "company", hidden: true }] },
    { id: "l2", name: "Series A", flow: "investors", canManage: true, owner: "alice", columns: [] },
    { id: "p1", name: "Press", flow: "", canManage: true, owner: "bob",
      columns: [{ field: "company", hidden: false }, { field: "region", label: "Region", custom: true, hidden: false }] },
  ];
  view.contacts = [
    { id: "a", flow: "investors", stage: "meeting", views: [] },
    { id: "b", flow: "investors", stage: "new", views: ["l2"] },
    { id: "c", flow: "customers", stage: "qualified", views: ["p1"] },
    { id: "d", flow: "gone", stage: "x-ray", views: [] },
    { id: "e", flow: "hiring", stage: "applied", views: [] },
  ];
}

function desktopContactInSheet(contact, sheet, views) {
  const lists = new Map();
  for (const item of views) {
    if (item.flow) lists.set(item.flow, [...(lists.get(item.flow) || []), item.id]);
  }
  const memberships = contact.views || [];
  if (!sheet.flow) return memberships.includes(sheet.id);
  if (contact.flow !== sheet.flow) return false;
  const bound = lists.get(sheet.flow) || [sheet.id];
  for (const list of bound) if (memberships.includes(list)) return list === sheet.id;
  return bound[0] === sheet.id;
}

reset();

same(page.crmTabViews().map((tab) => tab.id),
     ["l1", "l2", "p1", "pipeline:customers", "pipeline:hiring", "no-pipeline"], "tab order");
same(page.crmVirtualViews().map((tab) => [tab.name, tab.flow, tab.virtual]),
     [["Customers", "customers", true], ["Hiring", "hiring", true]], "virtual tabs are named after their flow");
check(!page.crmTabViews().some((tab) => tab.id === "pipeline:investors"),
      "a pipeline with a list bound to it has no tab of its own");
for (const tab of page.crmTabViews().filter((item) => item.id !== "no-pipeline")) {
  for (const contact of view.contacts) {
    same(page.crmContactInView(contact, tab), desktopContactInSheet(contact, tab, view.views),
         `contact ${contact.id} on ${tab.id} matches the desktop`);
  }
}
const onTab = (id) => view.contacts.filter((contact) =>
  page.crmContactOnTab(contact, page.crmTabViews().find((tab) => tab.id === id))).map((contact) => contact.id);
same(onTab("l1"), ["a"], "a pipeline's first list holds everyone not on a sibling");
same(onTab("l2"), ["b"], "a sibling list holds the contacts put on it");
same(onTab("pipeline:customers"), ["c"], "a pipeline tab shows every contact in its flow");
same(onTab("no-pipeline"), ["d"], "No pipeline holds contacts whose flow is gone");
view.contacts = view.contacts.filter((contact) => contact.id !== "d");
check(!page.crmTabViews().some((tab) => tab.id === "no-pipeline"), "No pipeline hides while it is empty");
reset();

page.crmSyncActiveTab();
same(view.activeView, "l1", "the first tab opens on first load");
same(stageSelect.value, "new", "a pipeline with a 'new' stage opens on it");
view.activeView = "no-pipeline";
page.crmSyncActiveTab();
same(view.activeView, "no-pipeline", "an open tab that still exists stays open");
view.contacts = view.contacts.filter((contact) => contact.id !== "d");
page.crmSyncActiveTab();
same(view.activeView, "l1", "a tab that disappears falls back to the first tab");
view.activeView = "pipeline:customers";
page.crmSyncActiveTab();
same(stageSelect.value, "lead", "a pipeline without 'new' opens on its first stage");
stageSelect.value = "";
page.crmSyncActiveTab();
same(stageSelect.value, "", "the stage filter is seeded once per tab opened, not on every render");
view.activeShared = "shared-1";
view.activeView = "";
page.crmSyncActiveTab();
same(view.activeView, "", "a shared tab keeps no organization tab open");
same(page.crmActiveView(), null, "no organization tab is active under a shared one");
reset();
same(page.crmFirstStage(["Lead", "NEW", "won"]), "NEW", "'new' is matched case-insensitively");
same(page.crmFirstStage(["lead", "won"]), "lead", "else the first stage");

{
  const tabs = page.crmTabViews();
  const strip = page.crmStripStages(tabs.find((tab) => tab.id === "l1"));
  same(strip.stages, ["new", "meeting", "committed"], "a pipeline list strips its flow's stages");
  same([...strip.counts], [["meeting", 1]], "only the list's own contacts are counted");
  view.contacts.push({ id: "f", flow: "investors", stage: "zeta", views: ["p1"] },
                     { id: "g", flow: "hiring", stage: "Alpha", views: ["p1"] },
                     { id: "h", flow: "customers", stage: "lead", views: ["p1"] });
  same(page.crmStripStages(tabs.find((tab) => tab.id === "p1")).stages,
       ["lead", "qualified", "Alpha", "zeta"], "a plain list strips its contacts' stages in pipeline order, the rest sorted");
  same(page.crmStripStages(tabs.find((tab) => tab.id === "p1"), "customers").stages,
       ["lead", "qualified", "customer"], "a plain list with the flow filter set strips that flow");
  same(page.crmStripStages(page.crmTabViews().find((tab) => tab.id === "no-pipeline")).stages,
       ["x-ray"], "No pipeline strips the stages its contacts are at");
  same(page.crmStripStages(null).stages, [], "no tab, no strip");
  const html = page.crmStageStripHtml(["new", "meeting"], new Map([["new", 2]]), "new");
  check(html.includes('aria-pressed="true"') && html.includes("Click to show every stage"),
        "the picked chip says a click shows every stage");
  check(html.includes("2 contacts at New") && html.includes("0 contacts at Meeting\nClick to show only this stage"),
        "each chip says how many contacts are at it");
  check(html.includes("crm-stage-empty"), "an empty stage is drawn muted");
  reset();
}

same(page.crmStageColor("closed-lost", 0), "#f85149", "lost wins over closed");
same(page.crmStageColor("disqualified", 0), "#f85149", "disqualified is not qualified");
same(page.crmStageColor("closed-won", 0), "#3fb950", "won is green");
same(page.crmStageColor("in-progress", 0), "#e3b341", "progress is yellow");
same(page.crmStageColor("qualified", 0), "#bc8cff", "qualified is purple");
same(page.crmStageColor("new", 0), "#58a6ff", "new is blue");
same(page.crmStageColor("xyz", 3), "#39c5cf", "others take the palette by position");
same(page.crmStageColor("xyz", 11), "#39c5cf", "the palette wraps");
same(page.crmStageInk("#e3b341"), "#1f2328", "a light chip gets dark ink");
same(page.crmStageInk("#f85149"), "#ffffff", "a dark chip gets white ink");
same(page.crmStageLabel("in-progress"), "In progress", "a slug reads as words");
same(page.crmStageLabel("closed__won"), "Closed won", "underscores read as spaces");
same(page.crmStageLabel(""), "No stage", "an empty stage has a name");

same(page.crmMoveToSheetEdits(["a"], "l2"), [{ id: "a", views: ["l2"], removeViews: ["l1"] }],
     "moving within a pipeline swaps the sibling list");
same(page.crmMoveToSheetEdits(["c"], "pipeline:hiring"), [{ id: "c", flow: "hiring", stage: "applied" }],
     "a virtual pipeline tab sets the flow and its first stage, no list");
same(page.crmMoveToSheetEdits(["c"], "l1"),
     [{ id: "c", flow: "investors", stage: "new", views: ["l1"], removeViews: ["l2"] }],
     "a stored pipeline list joins the flow and drops its siblings");
view.contacts.push({ id: "m", flow: "customers", stage: "meeting", views: [] });
same(page.crmMoveToSheetEdits(["m"], "l1")[0].stage, "meeting", "a stage the pipeline shares is kept");
reset();
same(page.crmMoveToSheetEdits(["e", "a"], "pipeline:hiring"), [{ id: "a", flow: "hiring", stage: "applied" }],
     "contacts already there are skipped");
same(page.crmMoveToSheetEdits(["a"], "p1"), [{ id: "a", views: ["p1"] }], "a plain list adds the membership");
same(page.crmMoveToSheetEdits(["d"], "no-pipeline"), [], "nothing moves into No pipeline");
{
  const items = page.crmPipelineMoveItems(["a"]);
  same(items.map((item) => item.label), ["Seed · Investors", "Series A · Investors", "Customers", "Hiring"],
       "lists sharing a pipeline are named with it");
  same(items.map((item) => item.disabled), [true, false, false, false], "the list a contact is on is disabled");
}

{
  const plan = page.crmMergePlan("p1", "l1");
  same(plan.edits, [{ id: "c", views: ["l1"], removeViews: ["l2", "p1"], flow: "investors", stage: "new" }],
       "one bulk edit moves the source's contacts onto the target pipeline");
  same(plan.columns.map((column) => [column.field, column.hidden]),
       [["name", false], ["company", false], ["region", false]],
       "every field the source shows is shown on the target");
  check(plan.columnsChanged, "the target's fields change");
  const plain = page.crmMergePlan("l2", "p1");
  same(plain.edits, [{ id: "b", views: ["p1"], removeViews: ["l2"] }], "merging into a plain list keeps the flow");
  view.views[2].columns = [];
  const seeded = page.crmMergePlan("l1", "p1");
  same(seeded.columns.slice(0, 2).map((column) => column.field), ["name", "email"],
       "a target showing nothing is seeded with the built-in columns");
  same(seeded.columns.length, 20, "the built-in columns are the twenty contact fields");
  same(page.crmMergePlan("l1", "l1"), null, "a list does not merge into itself");
  reset();
}

check(page.crmCanDeleteList(view.views[0]), "a pipeline list with a sibling can be deleted");
check(page.crmCanDeleteList(view.views[2]), "a plain list can be deleted");
view.views = view.views.filter((item) => item.id !== "l2");
check(!page.crmCanDeleteList(view.views[0]), "a pipeline's only list cannot be deleted");
check(!page.crmCanDeleteList({ ...view.views[1], canManage: false }), "deleting needs canManage");
check(!page.crmCanDeleteList(page.crmVirtualViews()[0]), "a virtual tab is not a list to delete");
reset();

same(page.crmSheetShareSummary({ visibility: "organization", owner: "alice", sharedWith: ["zed"] }),
     ["Open to 3 people in acme", "zed", "bob · member", "carol · member", "Owner: alice"],
     "an open list reaches its picked names and the roster");
same(page.crmSheetShareSummary({ visibility: "private", owner: "alice", sharedWith: [] }),
     ["Private to its owner", "Owner: alice"], "a private list reaches its owner only");
view.members = Array.from({ length: 20 }, (_, index) => `member${index}`);
{
  const items = page.crmSheetShareItems({ visibility: "organization", owner: "nobody" });
  same(items.length, 1 + 15 + 1 + 1, "fifteen names, then a count of the rest, then the owner");
  same(items[16].label, "…and 5 more", "the rest are counted");
}
reset();
same(page.crmMembersAdded({ owner: "alice", sharedWith: ["bob"] }, ["carol"], "@Dave, bob alice  eve"),
     ["carol", "dave", "eve"], "added members drop the owner and names already on the list");

view.activeView = "pipeline:customers";
same(page.crmStoredColumns(), [], "a virtual tab starts with no stored layout");
view.virtualColumns["pipeline:customers"] = [{ field: "email", hidden: false }];
same(page.crmStoredColumns(), [{ field: "email", hidden: false }], "its layout is kept for the session");
reset();

same(page.crmFlowSlug("Investors 2026!"), "investors-2026", "a pipeline id is a slug");
same(page.crmFlowSlug("2026 hiring"), "pipeline-2026-hiring", "a pipeline id starts with a letter");
same(page.crmStageSlug("  In progress "), "in-progress", "a stage slug");
{
  const edit = page.crmPipelineEditState();
  same(edit.drafts.map((draft) => draft.stages.map((stage) => stage.label)),
       [["Lead", "Qualified", "Customer"], ["New", "Meeting", "Committed"], ["Applied", "Hired"]],
       "drafts read each stage as words");
  same(edit.lists.map((list) => [list.id, Boolean(list.key)]), [["l1", true], ["l2", true], ["p1", false]],
       "lists start on the pipeline they show");
  edit.drafts[1].stages = [edit.drafts[1].stages[2], { original: "meeting", label: "Pitch meeting" }, edit.drafts[1].stages[0],
                           { original: "", label: "9 lives" }];
  edit.drafts.push({ key: edit.key(), id: "", name: "Customers", icon: "sales",
                     stages: [{ original: "", label: "New" }, { original: "", label: "In progress" }, { original: "", label: "Complete" }] });
  edit.lists[2].key = edit.drafts[3].key;
  const plan = page.crmPipelinePlan(edit, view.contacts);
  same(plan.flows.map((flow) => flow.id), ["customers", "investors", "hiring", "customers-2"],
       "a new pipeline whose id is taken gets a numbered one");
  same(plan.flows[1].stages, ["committed", "pitch-meeting", "new"], "reordering moves no one; a bad slug is dropped");
  same(plan.flows[3].stages, ["new", "in-progress", "complete"], "a new pipeline's stages");
  same(plan.flows[3].icon, "sales", "the type is kept");
  same(plan.stageEdits, [{ id: "a", stage: "pitch-meeting" }], "contacts at a renamed stage follow it by its original slug");
  same(plan.bindings, [{ id: "p1", flow: "customers-2" }], "a list rebound on the Lists page is saved");
}
{
  const edit = page.crmPipelineEditState();
  edit.drafts[0].stages = [{ original: "lead", label: "Won" }, { original: "qualified", label: "Lead" },
                           { original: "customer", label: "Customer" }];
  const plan = page.crmPipelinePlan(edit, [{ id: "x", flow: "customers", stage: "lead" },
                                           { id: "y", flow: "customers", stage: "qualified" }]);
  same(plan.stageEdits, [{ id: "y", stage: "lead" }],
       "a stage renamed onto a name the pipeline still uses leaves its contacts where they are");
}
{
  const edit = page.crmPipelineEditState();
  edit.drafts[2].stages.splice(0, 1);
  same(page.crmPipelinePlan(edit, view.contacts).stageEdits, [], "removing a stage moves nobody");
  page.crmRemovePipelineDraft(edit, 1);
  same(edit.lists.map((list) => list.key), ["", "", ""], "lists on a removed pipeline become plain lists");
  const plan = page.crmPipelinePlan(edit, view.contacts);
  same(plan.flows.map((flow) => flow.id), ["customers", "hiring"], "the removed pipeline is gone");
  same(plan.bindings, [{ id: "l1", flow: "" }, { id: "l2", flow: "" }],
       "no list is left pointing at a removed pipeline");
  edit.drafts.forEach((draft) => { draft.name = " "; });
  same(page.crmPipelinePlan(edit, view.contacts).error, "Keep at least one pipeline, with a name.",
       "an organization keeps a named pipeline");
}

if (failures) {
  console.error(`${failures} check(s) failed`);
  process.exit(1);
}
console.log("pipeline tab checks passed");
