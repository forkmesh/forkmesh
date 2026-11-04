const path = require("path");
const S = require(path.join(__dirname, "..", "content", "web", "page.js"));
const C = S.crm;

let failures = 0, checks = 0;
function same(actual, expected, what) {
  checks += 1;
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) { failures += 1; console.error(`FAIL ${what}: expected ${e}, got ${a}`); }
}

const ADA = "a".repeat(32), BEA = "b".repeat(32), CY = "c".repeat(32), NEO = "d".repeat(32);
const COLUMNS = [
  { field: "name", label: "Name" }, { field: "email", label: "Email" }, { field: "stage", label: "Stage" },
  { field: "owner", label: "Owner" }, { field: "nextStep", label: "Next step" }, { field: "value", label: "Deal value" },
  { field: "tags", label: "Tags" }, { field: "lastContactedAt", label: "Last contacted" }, { field: "notes", label: "Notes" },
  { field: "fields.region", label: "Region" },
];
const CONTACTED = Date.UTC(2026, 9, 30, 14, 5, 37);
function contact(id, extra = {}) {
  return { id, name: "", email: "", stage: "lead", owner: "", nextStep: "", value: 0, tags: [], lastContactedAt: 0, notes: "", fields: {}, ...extra };
}
const PAYLOAD = {
  list: { id: "e".repeat(32), name: "Leads", flow: "customers", stages: ["lead", "qualified", "customer"], canAdd: true },
  columns: COLUMNS, members: ["alice", "charlie"], canWrite: true, readAt: 1000,
  contacts: [
    contact(ADA, { name: "Ada", email: "ada@example.com", owner: "alice", tags: ["vip", "beta"], value: 1200, lastContactedAt: CONTACTED, fields: { region: "EU" } }),
    contact(BEA, { name: "Bea", email: "bea@example.com", nextStep: "Call", notes: "Line one\nLine two" }),
    contact(CY, { name: "Cy", email: "cy@example.com", stage: "qualified" }),
  ],
};

function open(payload = PAYLOAD) {
  const wb = S.crm.workbook(JSON.parse(JSON.stringify(payload)), "acme");
  const round = S.deserializeWorkbook(JSON.parse(JSON.stringify(S.serializeWorkbook(wb, null))));
  const calc = new S.Calc(round);
  calc.recalc();
  return { wb: round, calc, sheet: round.sheets[0], link: round.crm };
}
function cellAt(doc, field, rowIndex) {
  const layout = C.layout(doc.wb, doc.sheet, doc.calc, doc.link);
  const col = field === "id" ? layout.idCol : layout.cols.find((x) => x.field === field).c;
  return { r: rowIndex, c: col };
}
function type(doc, field, rowIndex, text) {
  const { r, c } = cellAt(doc, field, rowIndex);
  const k = S.cellKey(r, c);
  const old = doc.sheet.cells.get(k);
  const style = S.styleOf(doc.wb, old);
  const parsed = S.parseInput(text, { textFormat: style.fmt === "@" });
  const cell = parsed.formula !== undefined ? S.makeCell(undefined, parsed.formula, old?.s || 0) : S.makeCell(parsed.value, undefined, old?.s || 0);
  if (cell) doc.sheet.cells.set(k, cell); else doc.sheet.cells.delete(k);
  doc.calc.recalc();
}
function rows(doc) { return C.readRows(doc.wb, doc.sheet, doc.calc, doc.link).rows; }

{
  const doc = open();
  same(doc.link.org, "acme", "the link names the organization");
  same(doc.link.list, "e".repeat(32), "the link names the list");
  same(doc.sheet.name, "Leads", "the sheet is named after the list");
  same(doc.sheet.hiddenCols.has(0), true, "the CRM ID column is hidden");
  same(doc.sheet.freezeRows, 1, "the header row is frozen");
  same(doc.sheet.validations.map((rule) => rule.values), [["lead", "qualified", "customer"], ["alice", "charlie"]], "stage and owner offer dropdowns");
  same(Object.keys(doc.link.snapshot), [ADA, BEA, CY], "the snapshot keeps every pulled contact");
  const read = rows(doc);
  same(read.map((row) => row.id), [ADA, BEA, CY], "one row per contact, keyed by its id");
  same(read[0].values.tags, ["vip", "beta"], "tags read back as a list");
  same(read[0].values.value, 1200, "deal value reads back as a number");
  same(read[0].values.lastContactedAt, CONTACTED, "the last-contacted time survives the date cell");
  same(read[0].values["fields.region"], "EU", "custom fields fill their column");
  same(read[1].values.notes, "Line one\nLine two", "notes keep their line breaks");
  const untouched = C.diff(read, doc.link);
  same([untouched.updates.length, untouched.creates.length, untouched.problems.length], [0, 0, 0], "an untouched sheet has nothing to sync");
}

{
  const zone = (ms) => (ms > 0 ? 300 : 300);
  const cell = C.toCell("lastContactedAt", CONTACTED, zone);
  same(C.fromCell("lastContactedAt", cell.v, "", zone), CONTACTED, "a time zone shifts the cell and back");
  same(S.formatValue(cell.v, cell.fmt).text, "2026-10-30 09:05", "the cell shows the local wall time");
  same(C.norm("stage", " Qualified "), "qualified", "stages compare as the CRM stores them");
  same(C.norm("tags", "VIP; beta, vip"), "vip,beta", "tags compare after the CRM's clean-up");
  same(C.norm("value", "1,200.50"), "1200", "deal values compare as whole numbers");
  same(C.norm("email", " Ada@Example.com "), "ada@example.com", "emails compare without case");
  same(C.same("lastContactedAt", CONTACTED, CONTACTED + 20000), true, "a time within the same minute is not a change");
}

{
  const doc = open();
  type(doc, "owner", 1, "charlie");
  type(doc, "stage", 3, "Customer");
  type(doc, "tags", 1, "vip, beta");
  type(doc, "name", 4, "Neo");
  type(doc, "email", 4, "neo@example.com");
  type(doc, "nextStep", 4, "Intro");
  type(doc, "value", 2, "2500");
  const diff = C.diff(rows(doc), doc.link);
  same(diff.updates.map((item) => [item.id, item.set, item.expect]), [
    [ADA, { owner: "charlie" }, { owner: "alice" }],
    [BEA, { value: 2500 }, { value: 0 }],
    [CY, { stage: "Customer" }, { stage: "qualified" }],
  ], "only edited cells are sent, each with the value it was pulled at");
  same(diff.creates.map((item) => item.set), [{ name: "Neo", email: "neo@example.com", nextStep: "Intro" }], "a row without an id becomes a new contact");
  same(diff.removed, [], "no rows were removed");
}

{
  const doc = open();
  type(doc, "name", 2, "Somebody else");
  type(doc, "email", 2, "else@example.com");
  const { r, c } = cellAt(doc, "id", 3);
  doc.sheet.cells.set(S.cellKey(r, c), S.makeCell(ADA));
  const ghost = cellAt(doc, "id", 5);
  doc.sheet.cells.set(S.cellKey(ghost.r, ghost.c), S.makeCell("f".repeat(32)));
  doc.calc.recalc();
  const diff = C.diff(rows(doc), doc.link);
  same(diff.problems.map((p) => p.error), ["row_mismatch", "duplicate_id", "unknown_id"], "a row whose name and email both changed, a copied id and an unknown id are held back");
  same(diff.updates.length, 0, "nothing suspicious is sent");
  same(diff.removed, [CY], "the overwritten row's contact counts as removed, never deleted");
}

{
  const doc = open();
  type(doc, "nextStep", 2, "Demo");
  type(doc, "owner", 1, "charlie");
  const current = JSON.parse(JSON.stringify(PAYLOAD.contacts));
  current[1].nextStep = "Email";
  current[0].notes = "Met at the fair";
  current[2].stage = "customer";
  current.push(contact(NEO, { name: "Neo", email: "neo@example.com" }));
  const merged = C.merge(rows(doc), doc.link, current);
  same(merged.conflicts.map((x) => [x.id, x.field, x.mine, x.theirs, x.base]), [[BEA, "nextStep", "Demo", "Email", "Call"]], "a field changed on both sides is a conflict, never overwritten");
  same(merged.pulls.map((x) => [x.id, x.field, x.value]), [[ADA, "notes", "Met at the fair"], [CY, "stage", "customer"]], "CRM changes to untouched cells are pulled in");
  same(merged.snapshot[BEA].nextStep, "Call", "an unresolved conflict keeps the old base so the server checks it again");
  same(merged.snapshot[ADA].owner, "alice", "a local edit stays pending after a refresh");
  same(merged.added.map((x) => x.id), [NEO], "contacts new on the list are added");
  const resolved = C.resolve(merged.snapshot, merged.conflicts, ["mine"]);
  same([resolved.again, resolved.pulls.length, resolved.snapshot[BEA].nextStep], [true, 0, "Email"], "keeping mine re-bases on the CRM's value and syncs again");
  const taken = C.resolve(merged.snapshot, merged.conflicts, ["theirs"]);
  same([taken.again, taken.pulls], [false, [{ id: BEA, field: "nextStep", value: "Email" }]], "taking the CRM's value writes it into the cell");
}

{
  const doc = open();
  const gone = JSON.parse(JSON.stringify(PAYLOAD.contacts)).filter((x) => x.id !== CY && x.id !== BEA);
  type(doc, "nextStep", 2, "Still mine");
  const merged = C.merge(rows(doc), doc.link, gone);
  same(merged.gone.map((x) => [x.id, x.edited]), [[BEA, true], [CY, false]], "rows that left the list are reported, edited ones kept");
  same(Boolean(merged.snapshot[BEA]) && !merged.snapshot[CY], true, "an untouched row that left the list drops out of the snapshot");
}

{
  const doc = open();
  type(doc, "nextStep", 1, "Send deck");
  type(doc, "nextStep", 2, "Demo");
  type(doc, "name", 4, "Neo");
  const read = rows(doc);
  const diff = C.diff(read, doc.link);
  diff.creates[0].requestId = NEO;
  const batches = C.batches(diff, 2);
  same(batches.map((b) => [b.updates.length, b.creates.length]), [[2, 0], [0, 1]], "batches hold at most the row limit");
  same(batches[1].creates[0], { ref: NEO, requestId: NEO, set: { name: "Neo" } }, "a create carries its request id so a retry cannot duplicate it");
  const theirsBea = { ...PAYLOAD.contacts[1], nextStep: "Email", owner: "charlie" };
  const results = [
    { id: ADA, status: "updated", contact: { ...PAYLOAD.contacts[0], nextStep: "Send deck" } },
    { id: BEA, status: "conflict", fields: ["nextStep"], contact: theirsBea },
    { id: NEO, ref: NEO, status: "created", contact: contact(NEO, { name: "Neo", stage: "lead" }) },
  ];
  const outcome = C.apply({ ...doc.link, pending: { [NEO]: 1 } }, diff, results, read);
  same([outcome.updated, outcome.created], [1, 1], "saved and created rows are counted");
  same(outcome.snapshot[ADA].nextStep, "Send deck", "a saved edit becomes the new base");
  same(outcome.snapshot[NEO].name, "Neo", "a created contact joins the snapshot");
  same(outcome.pending, {}, "a created contact is no longer pending");
  same(outcome.conflicts.map((x) => [x.id, x.field, x.mine, x.theirs]), [[BEA, "nextStep", "Demo", "Email"]], "a server conflict comes back for the user to settle");
  same(outcome.pulls, [{ id: BEA, field: "owner", value: "charlie" }], "the CRM's other change on a conflicting row is pulled in");
  const failed = C.apply(doc.link, diff, [{ id: ADA, status: "missing" }, { id: BEA, status: "error", error: "unknown_stage", stages: ["lead"] }], read);
  same(failed.problems.map((p) => p.error), ["deleted_in_crm", "unknown_stage"], "rejected rows are reported with their reason");
  same(failed.snapshot[ADA].nextStep, "", "a rejected row keeps its old base");
}

{
  same(C.cleanLink({ kind: "crm", org: "Acme!", list: "x" }), null, "a malformed link is dropped");
  const doc = open();
  const copy = S.deserializeWorkbook(JSON.parse(JSON.stringify({ ...S.serializeWorkbook(doc.wb, null), crm: { ...doc.link, snapshot: { nope: {}, [ADA]: { name: "Ada" } } } })));
  same(Object.keys(copy.crm.snapshot), [ADA], "snapshot keys must be contact ids");
  const plain = S.deserializeWorkbook(S.serializeWorkbook(S.createWorkbook(), null));
  same(plain.crm, null, "an unlinked workbook stays unlinked");
}

if (failures) { console.error(`${failures} of ${checks} checks failed`); process.exit(1); }
console.log(`crm link checks passed (${checks})`);
