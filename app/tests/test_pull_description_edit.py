import json
import re
import shutil
import subprocess

import pytest

from _dashboard_bundle import assembled_dashboard_js


DASHBOARD_JS = assembled_dashboard_js()


def _js_function(name):
    match = re.search(r"\n  (?:async )?function " + name + r"\(.*?\n  }\n", DASHBOARD_JS, re.S)
    assert match, name
    return match.group(0)


def _constant(name):
    match = re.search(r"\n  const " + name + r" = .*?;\n", DASHBOARD_JS, re.S)
    assert match, name
    return match.group(0)


def _run(script):
    result = subprocess.run(["node"], input=script, capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_only_the_pull_authors_newest_description_edit_applies():
    script = _constant("PULL_DESCRIPTION_TARGET") + _constant("PULL_EDITABLE_COMMENT_TYPES") + "".join(
        _js_function(name) for name in (
            "pullTimestamp", "pullDescriptionEditApplies", "pullDescriptionEdit",
            "pullCommentChangeApplies", "pullReviewKeepsVerdict", "foldPullCommentChanges")) + """
const values = { author: "KA", authorName: "ann" };
const conversation = [
  { id: "c1", type: "comment", author: "KB", authorName: "bob", ts: 1, body: "hi" },
  { id: "e1", type: "comment-edit", author: "KA", authorName: "ann", ts: 3, target: "description", body: "newest" },
  { id: "e2", type: "comment-edit", author: "KA", authorName: "ann", ts: 2, target: "description", body: "older" },
  { id: "e3", type: "comment-edit", author: "KB", authorName: "bob", ts: 9, target: "description", body: "hijack" },
  { id: "e4", type: "comment-edit", author: "KZ", authorName: "ANN", ts: 4, target: "c1", body: "not the description" },
];
const byName = [{ id: "e5", type: "comment-edit", author: "KD", authorName: "Ann", ts: 1, target: "description", body: "desktop key" }];
process.stdout.write(JSON.stringify({
  edit: pullDescriptionEdit(values, conversation),
  byName: pullDescriptionEdit(values, byName)?.body || null,
  none: pullDescriptionEdit(values, conversation.slice(3)),
  timeline: foldPullCommentChanges(conversation).map((ev) => ev.id),
}));
"""
    out = _run(script)
    assert out["edit"]["id"] == "e1" and out["edit"]["body"] == "newest"
    assert out["byName"] == "desktop key"
    assert out["none"] is None
    assert out["timeline"] == ["c1"]


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_only_the_signed_in_author_of_a_live_pull_may_edit_it():
    script = "".join(_js_function(name) for name in ("webSessionAuthored", "pullDescriptionEditable")) + """
const state = { session: { nodeName: "ann" } };
const webIssueKeyPublic = () => "KA";
process.stdout.write(JSON.stringify([
  pullDescriptionEditable({ author: "KX", authorName: "Ann", status: "open" }),
  pullDescriptionEditable({ author: "KA", authorName: "", status: "merged" }),
  pullDescriptionEditable({ author: "KB", authorName: "bob", status: "open" }),
  pullDescriptionEditable({ author: "KA", authorName: "ann", status: "deleted" }),
]));
"""
    assert _run(script) == [True, True, False, False]


def test_the_pull_page_offers_the_editor_and_saves_a_description_edit():
    detail = DASHBOARD_JS.split("  function renderRepoRecordDetail(repo, kind, number, parsed) {", 1)[1].split(
        "\n  function ", 1)[0]
    assert 'const descriptionEdit = kind === "pulls" ? pullDescriptionEdit(values, parsed.pullConversation) : null;' in detail
    assert 'const canEditDescription = kind === "pulls" && pullDescriptionEditable(values);' in detail
    assert '${canEditDescription ? `<button type="button" data-repo-pull-description-edit="${PULL_DESCRIPTION_TARGET}"' in detail
    assert '${isPulls ? " data-repo-pull-description-card" : ""}' in detail
    editor = _js_function("openIssueCommentEditor")
    assert '"[data-repo-issue-comment], [data-repo-pull-description-card]"' in editor
    assert "[data-repo-record-opening-text]" in editor
    handler = _js_function("handleWebPullDescriptionEdit")
    assert "submitWebPullCommentEdit(repo, number, PULL_DESCRIPTION_TARGET, trimmed)" in handler
    assert "paintPullDescription(repo, trimmed);" in handler
    assert 'if (event.target.closest("[data-repo-pull-description-edit]")) {\n        void handleWebPullDescriptionEdit();' in DASHBOARD_JS


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_shared_editor_opens_inside_the_description_card():
    from test_comment_editor_focus_frontend import FAKE_DOM
    script = "\n".join([FAKE_DOM] + [_js_function(name) for name in (
        "visibleCommentEditButton", "restoreCommentEditFocus", "openIssueCommentEditor")] + [r"""
(async () => {
  body.children = [];
  const edit = new El("button", { repoPullDescriptionEdit: "description" });
  const text = new El("div", { repoRecordBody: "", repoRecordOpeningText: "" });
  const panel = new El("div", {}, "relative min-w-0 overflow-hidden rounded-md border border-border");
  panel.append(new El("div").append(edit), text);
  body.append(new El("div", { repoRecordDescription: "body", repoPullDescriptionCard: "" }).append(new El("span"), panel));
  edit.focus();
  const pending = openIssueCommentEditor("description", "old", "[data-repo-pull-description-edit]", "repoPullDescriptionEdit");
  const form = document.querySelectorAll("[data-repo-issue-comment-editor]")[0];
  const opened = { inPanel: form.parent === panel, textHidden: text.hidden, typing: document.activeElement === form.querySelector("textarea") };
  form.querySelector("textarea").value = "new";
  form.querySelector("textarea").dispatch("keydown", { key: "Enter", metaKey: true });
  const value = await pending;
  process.stdout.write(JSON.stringify({ ...opened, value, textShown: !text.hidden, backOnEdit: document.activeElement === edit }));
})().catch((error) => { console.error(error); process.exit(1); });
"""])
    assert _run(script) == {"inPanel": True, "textHidden": True, "typing": True,
                            "value": "new", "textShown": True, "backOnEdit": True}


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_saving_sends_one_signed_description_edit_and_repaints():
    script = _constant("PULL_DESCRIPTION_TARGET") + "".join(_js_function(name) for name in (
        "pullTimestamp", "pullDescriptionEditApplies", "pullDescriptionEdit",
        "webSessionAuthored", "pullDescriptionEditable", "handleWebPullDescriptionEdit")) + r"""
const webIssueKeyPublic = () => "KA";
const location = { href: "", pathname: "/o/r/pull/7", search: "" };
let reply = "";
const sent = [];
const painted = [];
async function openIssueCommentEditor(target, body) { sent.push(["opened", target, body]); return reply; }
async function submitWebPullCommentEdit(repo, number, target, body) {
  sent.push(["edit", number, target, body]);
  return { event: { id: "e9", type: "comment-edit", author: "KA", authorName: "ann", ts: 5, target, body } };
}
function paintPullDescription(repo, body) { painted.push(body); }
function restoreCommentEditFocus() {}
function showErrorNotice(message) { sent.push(["error", message]); }
function webIssueActionError(error) { return String(error); }
const detail = { repo: { owner: "o", name: "r" }, kind: "pulls", number: 7,
  parsed: { body: "Original", values: { author: "KA", authorName: "ann", status: "open" }, pullConversation: [] } };
const state = { session: { nodeName: "ann" }, repoRecordDetail: detail };
(async () => {
  reply = "Original\n";
  await handleWebPullDescriptionEdit();
  reply = "Fixed text\n\n";
  await handleWebPullDescriptionEdit();
  reply = "Fixed again";
  await handleWebPullDescriptionEdit();
  detail.parsed.values = { author: "KB", authorName: "bob", status: "open" };
  state.session.nodeName = "carol";
  await handleWebPullDescriptionEdit();
  process.stdout.write(JSON.stringify({ sent, painted, events: detail.parsed.pullConversation.length }));
})().catch((error) => { console.error(error); process.exit(1); });
"""
    out = _run(script)
    assert out["sent"] == [
        ["opened", "description", "Original"],
        ["opened", "description", "Original"],
        ["edit", 7, "description", "Fixed text"],
        ["opened", "description", "Fixed text"],
        ["edit", 7, "description", "Fixed again"],
    ]
    assert out["painted"] == ["Fixed text", "Fixed again"]
    assert out["events"] == 2
