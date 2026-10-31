import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
CONTENT = (ROOT / "app/public/dashboard/js/06-repo-content.js").read_text(encoding="utf-8")
IO = (ROOT / "app/public/dashboard/js/03-issues-profile-io.js").read_text(encoding="utf-8")

needs_node = pytest.mark.skipif(shutil.which("node") is None, reason="node is required")


def _function(source, name):
    match = re.search(r"\n  (?:async )?function %s\(" % re.escape(name), source)
    assert match, name
    end = source.index("\n  }\n", match.start())
    return source[match.start():end + 4]


FAKE_DOM = r"""
const camel = (name) => name.replace(/^data-/, "").replace(/-([a-z])/g, (_, c) => c.toUpperCase());
class El {
  constructor(tag, data = {}, className = "") {
    this.tagName = tag.toUpperCase();
    this.dataset = { ...data };
    this.className = className;
    this.children = [];
    this.parent = null;
    this.listeners = {};
    this.hidden = false;
    this.value = "";
  }
  get parentElement() { return this.parent; }
  get isConnected() { let n = this; while (n.parent) n = n.parent; return n === document.body; }
  append(...nodes) { for (const node of nodes) { node.parent = this; this.children.push(node); } return this; }
  insertBefore(node, ref) {
    node.parent = this;
    const at = ref ? this.children.indexOf(ref) : -1;
    if (at < 0) this.children.push(node); else this.children.splice(at, 0, node);
    return node;
  }
  remove() {
    const hadFocus = this.contains(document.activeElement);
    if (this.parent) this.parent.children = this.parent.children.filter((node) => node !== this);
    this.parent = null;
    if (hadFocus) document.activeElement = document.body;
  }
  contains(node) { while (node) { if (node === this) return true; node = node.parent; } return false; }
  matches(selector) {
    return selector.split(",").map((part) => part.trim()).some((part) => {
      if (part.startsWith("[")) return camel(part.slice(1, -1)) in this.dataset;
      if (part.startsWith(".")) {
        const have = this.className.split(/\s+/);
        return part.slice(1).split(".").every((name) => have.includes(name));
      }
      return this.tagName === part.toUpperCase();
    });
  }
  closest(selector) { let n = this; while (n && n.matches) { if (n.matches(selector)) return n; n = n.parent; } return null; }
  querySelectorAll(selector) {
    const out = [];
    for (const child of this.children) {
      if (child.matches(selector)) out.push(child);
      out.push(...child.querySelectorAll(selector));
    }
    return out;
  }
  querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
  getClientRects() {
    for (let n = this; n; n = n.parent) if (n.hidden || n.className.split(/\s+/).includes("hidden")) return [];
    return [{}];
  }
  focus() { if (this.isConnected && this.getClientRects().length) document.activeElement = this; }
  setSelectionRange() {}
  addEventListener(type, listener) { (this.listeners[type] ||= []).push(listener); }
  dispatch(type, init = {}) {
    const event = { ...init, defaultPrevented: false, preventDefault() { this.defaultPrevented = true; }, stopPropagation() {} };
    for (const listener of this.listeners[type] || []) listener(event);
  }
  click() { this.dispatch("click"); }
  set innerHTML(html) {
    this.children = [];
    if (html.includes("<textarea")) this.append(new El("textarea"));
    if (html.includes("data-repo-issue-comment-edit-cancel")) this.append(new El("button", { repoIssueCommentEditCancel: "" }));
  }
}
const body = new El("body");
globalThis.document = {
  body,
  activeElement: body,
  createElement: (tag) => new El(tag),
  querySelectorAll: (selector) => body.querySelectorAll(selector),
};
globalThis.window = { prompt: () => { throw new Error("fell back to window.prompt"); } };

function pullPage() {
  body.children = [];
  document.activeElement = body;
  const conversation = new El("div", { repoRecordPanel: "conversation" }, "hidden min-w-0");
  const hiddenEdit = new El("button", { repoPullCommentEdit: "c1" });
  const hiddenPanel = new El("div", {}, "min-w-0 overflow-hidden rounded-md border");
  hiddenPanel.append(new El("div").append(hiddenEdit), new El("div", { repoIssueCommentText: "" }));
  conversation.append(new El("div", { repoIssueComment: "" }).append(hiddenPanel));
  const files = new El("div", { repoRecordPanel: "files" }, "min-w-0");
  const edit = new El("button", { repoPullCommentEdit: "c1" });
  const column = new El("div", {}, "min-w-0");
  const text = new El("div", { repoPullInlineCommentText: "" });
  column.append(new El("div").append(edit), text);
  files.append(new El("article", { repoPullInlineCommentCard: "" }).append(column));
  body.append(conversation, files);
  return { conversation, files, edit, text };
}
const SELECTOR = "[data-repo-pull-comment-edit]";
const KEY = "repoPullCommentEdit";
const focusName = () => (document.activeElement === body ? "body" : document.activeElement.tagName.toLowerCase());
"""


def _run(body):
    script = "\n".join([
        FAKE_DOM,
        _function(CONTENT, "visibleCommentEditButton"),
        _function(CONTENT, "restoreCommentEditFocus"),
        _function(CONTENT, "openIssueCommentEditor"),
        "(async () => {", body, "})().catch((error) => { console.error(error); process.exit(1); });",
    ])
    result = subprocess.run(["node", "-e", script], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


@needs_node
def test_editing_an_inline_comment_opens_in_the_visible_files_tab():
    out = _run(r"""
      const page = pullPage();
      page.edit.focus();
      const pending = openIssueCommentEditor("c1", "old", SELECTOR, KEY);
      const form = document.querySelectorAll("[data-repo-issue-comment-editor]")[0];
      const result = {
        panel: form.closest("[data-repo-record-panel]").dataset.repoRecordPanel,
        visible: form.getClientRects().length > 0,
        focus: focusName(),
        textHidden: page.text.hidden,
        hiddenTabUntouched: page.conversation.querySelectorAll("[data-repo-issue-comment-editor]").length === 0,
      };
      form.querySelector("textarea").dispatch("keydown", { key: "Escape" });
      await pending;
      console.log(JSON.stringify(result));
    """)
    assert out == {
        "panel": "files",
        "visible": True,
        "focus": "textarea",
        "textHidden": True,
        "hiddenTabUntouched": True,
    }


@needs_node
def test_closing_the_editor_returns_focus_to_its_edit_button():
    out = _run(r"""
      const results = {};
      for (const how of ["escape", "cancel", "save"]) {
        const page = pullPage();
        page.conversation.remove();
        page.edit.focus();
        const pending = openIssueCommentEditor("c1", "old", SELECTOR, KEY);
        const form = document.querySelectorAll("[data-repo-issue-comment-editor]")[0];
        const input = form.querySelector("textarea");
        const typing = document.activeElement === input;
        input.value = "new";
        if (how === "escape") input.dispatch("keydown", { key: "Escape" });
        if (how === "cancel") form.querySelector("[data-repo-issue-comment-edit-cancel]").click();
        if (how === "save") input.dispatch("keydown", { key: "Enter", ctrlKey: true });
        const value = await pending;
        results[how] = {
          typing,
          value,
          backOnEdit: document.activeElement === page.edit,
          editorGone: document.querySelectorAll("[data-repo-issue-comment-editor]").length === 0,
          textShown: !page.text.hidden,
        };
      }
      console.log(JSON.stringify(results));
    """)
    closed = {"typing": True, "backOnEdit": True, "editorGone": True, "textShown": True}
    assert out == {
        "escape": dict(closed, value=None),
        "cancel": dict(closed, value=None),
        "save": dict(closed, value="new"),
    }


@needs_node
def test_after_the_saved_rerender_focus_lands_on_the_new_visible_edit_button():
    out = _run(r"""
      pullPage();
      const rerendered = pullPage();
      restoreCommentEditFocus("c1", SELECTOR, KEY);
      const landed = document.activeElement === rerendered.edit;
      const elsewhere = new El("input");
      body.append(elsewhere);
      elsewhere.focus();
      restoreCommentEditFocus("c1", SELECTOR, KEY);
      console.log(JSON.stringify({ landed, keptElsewhere: document.activeElement === elsewhere }));
    """)
    assert out == {"landed": True, "keptElsewhere": True}


def test_both_save_paths_restore_focus_after_rerendering():
    pull = _function(IO, "handleWebPullCommentAction")
    assert pull.index("refreshRepoPullDiff();") < pull.index(
        'restoreCommentEditFocus(target, "[data-repo-pull-comment-edit]", "repoPullCommentEdit")')
    issue = _function(IO, "handleWebIssueAction")
    assert issue.index("renderCurrentWebIssueDetail();") < issue.index("restoreCommentEditFocus(target)")
