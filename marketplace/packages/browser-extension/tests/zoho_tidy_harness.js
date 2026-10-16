"use strict";
const fs = require("fs"), path = require("path"), vm = require("vm");

const source = fs.readFileSync(path.join(__dirname, "..", "content", "extension", "zoho-tidy.js"), "utf8");
const sandbox = { Intl };
sandbox.globalThis = sandbox;
vm.runInNewContext(source, sandbox);
const tidy = sandbox.ForkMeshZohoTidy;

let checks = 0;
function check(name, ok, detail = "") {
  checks += 1;
  if (!ok) { console.error(`FAIL ${name} ${detail}`); process.exit(1); }
  console.log("ok   " + name);
}
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

const rows = [
  ["Acme News", "Weekly digest"],
  ["Bob Example", "Re: Lunch on Friday"],
  ["\"Acme News\"", "Fwd: Weekly digest #2"],
  ["Café Bot (3)", "Your receipt"],
  ["bob example", "Invoice 12"],
  ["Acme News", "Sale ends today"],
  ["", "No sender here"],
].map(([sender, subject], index) => ({ index, sender: tidy.senderName(sender), subject: tidy.subjectText(subject) }));

check("sender names drop quotes and a thread count", rows[2].sender === "Acme News" && rows[3].sender === "Café Bot");
check("subjects drop reply and forward prefixes", rows[1].subject === "Lunch on Friday" && rows[2].subject === "Weekly digest #2");
check("sender keys ignore case and accents", tidy.senderKey("CAFE bot") === tidy.senderKey("Café Bot"));

const groups = tidy.groups(rows);
check("groups count each sender, largest first",
  same(groups.map((entry) => [entry.label, entry.count]),
    [["Acme News", 3], ["Bob Example", 2], ["(no sender)", 1], ["Café Bot", 1]]), JSON.stringify(groups));
check("a group's key is its folded sender", groups[1].key === "bob example");

const ids = (list) => list.map((item) => item.index);
check("a plain word matches the subject", same(ids(tidy.filtered(rows, { query: "digest" })), [0, 2]));
check("a plain word matches the sender too", same(ids(tidy.filtered(rows, { query: "bob" })), [1, 4]));
check("every word must match", same(ids(tidy.filtered(rows, { query: "acme sale" })), [5]));
check("from: looks in the sender only", same(ids(tidy.filtered(rows, { query: "from:acme" })), [0, 2, 5]));
check("subject: looks in the subject only", same(ids(tidy.filtered(rows, { query: "subject:acme" })), []));
check("a quoted phrase stays together", same(ids(tidy.filtered(rows, { query: '"lunch on"' })), [1]));
check("a quoted from: phrase", same(ids(tidy.filtered(rows, { query: 'from:"bob ex"' })), [1, 4]));
check("matching ignores accents", same(ids(tidy.filtered(rows, { query: "cafe" })), [3]));
check("the reply prefix is not searched", same(ids(tidy.filtered(rows, { query: "subject:fwd" })), []));
check("a sender filter keeps one group", same(ids(tidy.filtered(rows, { sender: "acme news" })), [0, 2, 5]));
check("a sender filter and a search combine", same(ids(tidy.filtered(rows, { sender: "acme news", query: "digest" })), [0, 2]));
check("an empty search keeps everything", tidy.filtered(rows, {}).length === rows.length);
check("shown returns the kept indexes", same([...tidy.shown(rows, { query: "invoice" })], [4]));

check("date order is Zoho's", same(ids(tidy.order(rows, {})), [0, 1, 2, 3, 4, 5, 6]));
check("date reversed", same(ids(tidy.order(rows, { descending: true })), [6, 5, 4, 3, 2, 1, 0]));
check("sender order is alphabetical and stable", same(ids(tidy.order(rows, { by: "sender" })), [6, 0, 2, 5, 1, 4, 3]));
check("subject order ignores prefixes", same(ids(tidy.order(rows, { by: "subject" })), [4, 1, 6, 5, 0, 2, 3]));
check("grouping puts the largest sender first, Zoho order inside",
  same(ids(tidy.order(rows, { group: true })), [0, 2, 5, 1, 4, 6, 3]));
check("grouping keeps the subject sort inside each group",
  same(ids(tidy.order(rows, { group: true, by: "subject" })), [5, 0, 2, 4, 1, 6, 3]));
check("order does not change the input", same(ids(rows), [0, 1, 2, 3, 4, 5, 6]));

check("a checked input is ticked", tidy.ticked({ checked: true }));
check("aria-checked true is ticked", tidy.ticked({ ariaChecked: "true" }));
check("aria-checked false wins over a class", !tidy.ticked({ ariaChecked: "false", rowClass: "selected" }));
check("a selected row class is ticked", tidy.ticked({ rowClass: "zmList zmLSelected" }));
check("unchecked and checkbox classes are not ticks", !tidy.ticked({ boxClass: "zmchkbox unchecked", rowClass: "unselected" }));

check("Zoho's Delete is the delete action", tidy.actionOf("Delete") === "delete" && tidy.actionOf("Move to Trash") === "delete");
check("a shortcut hint is ignored", tidy.actionOf("Delete (Del)") === "delete");
check("Archive and Mark as read", tidy.actionOf(" Archive ") === "archive" && tidy.actionOf("Mark as Read") === "read");
check("other buttons are no action",
  ["Mark as unread", "Delete forever", "Unarchive", "Reply", ""].every((label) => tidy.actionOf(label) === ""));

check("labels count messages", tidy.plural(1) === "1 message" && tidy.plural(12) === "12 messages");
check("the summary shows the filtered share", tidy.summary(4, 40) === "4 of 40 loaded" && tidy.summary(40, 40) === "40 loaded");
check("action buttons carry the count", tidy.verb("delete", 7) === "Delete 7" && tidy.verb("read", 2) === "Mark read 2");
check("the delete confirmation names the count", tidy.confirmText(37) === "Delete 37 messages? Zoho Mail moves them to Trash.");
check("done lines name the count", tidy.doneText("archive", 1) === "Archived 1 message.");

console.log(`${checks} checks passed`);
