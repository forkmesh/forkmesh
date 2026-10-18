"""The web task workspace exposes the desktop task lifecycle safely."""

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from _dashboard_bundle import assembled_dashboard_js
from _dashboard_shell import assembled_dashboard_page


ROOT = Path(__file__).resolve().parents[1]
TASK_SOURCE = (
    ROOT / "public" / "dashboard" / "partials" / "views" / "tasks.html"
).read_text(encoding="utf-8")


def test_tasks_page_is_a_responsive_master_detail_workspace():
    page = assembled_dashboard_page("tasks")
    for marker in (
        'class="tasks-workspace"',
        "data-tasks-list",
        "data-tasks-detail",
        "data-tasks-stats",
        "data-tasks-new",
        "data-tasks-people",
        "data-task-editor",
        "data-task-action-dialog",
        '@media (max-width: 960px)',
        '@media (prefers-reduced-motion: reduce)',
    ):
        assert marker in page
    assert TASK_SOURCE in page


def test_tasks_page_can_filter_the_catalog_by_any_assignee():
    js = assembled_dashboard_js()
    assert 'assignee: "all"' in js
    assert "function taskMatchesAssignee(task, filter)" in js
    assert "taskMatchesAssignee(task, state.tasksView.assignee)" in js
    assert "function taskPeopleChoices()" in js
    assert "...members.map((name) => ({ value: `user:${name}`" in js
    assert 'const person = event.target.closest("[data-tasks-person]");' in js
    assert 'taskAvatarHtml({ assigneeKind: "user", assignee: choice.name }, "task-person-avatar")' in js


def test_tasks_page_status_tabs_are_icons_with_counts():
    js = assembled_dashboard_js()
    page = assembled_dashboard_page("tasks")
    hand = (ROOT / "public" / "dashboard" / "tasks" / "index.html").read_text(
        encoding="utf-8")
    for source in (TASK_SOURCE, page, hand):
        assert "data-tasks-assignee-filter" not in source
        assert 'data-tasks-people class="task-people"' in source
        assert ".task-person-avatar {" in source
        for key in ("open", "active", "mine", "qa", "done", "trash", "all"):
            assert f'data-tasks-filter-count="{key}"' in source, key
    assert "function taskFilterCounts()" in js
    assert "function taskPeopleOpenCounts()" in js
    assert 'badge.textContent = counted ? formatCount(value) : "";' in js


def test_tasks_page_uses_the_full_authorized_lifecycle_api():
    js = assembled_dashboard_js()
    for marker in (
        'taskRequest("GET", `/api/tasks/${taskId}${mail ? `?mail=${encodeURIComponent(mail)}` : ""}`)',
        'taskRequest(mode === "edit" ? "PATCH" : "POST"',
        'taskRequest("DELETE", `/api/tasks/${taskId}`, Object.keys(body).length ? body : null)',
        'taskRequest("POST", "/api/tasks/delete", { ids })',
        'taskRequest("POST", `/api/tasks/${taskId}/restore`, {})',
        '`/api/tasks?status=deleted${offset ? `&offset=${offset}` : ""}`',
        '`/api/tasks/${task.id}/${action}`',
        'const taskId = form.dataset.taskReplyForm;',
        'taskRequest("POST", `/api/tasks/${taskId}/replies`, { body })',
        'parentTaskId: taskId',
        'assigneeKind: "agent"',
        'completionNote',
        'howToTest',
    ):
        assert marker in js


def test_task_detail_shows_every_private_record_family():
    js = assembled_dashboard_js()
    detail = js[js.index("function taskEditablePropsHtml("):js.index("function renderTasksPage(")]
    for marker in (
        "task.details",
        "task.department",
        "task.team",
        "task.destination",
        "task.repository",
        "task.createdBy",
        "task.parentTaskId",
        "task.elapsedMs",
        "task.agentSessionId",
        "task.lastCheckin",
        "task.completionNote",
        "taskAttachmentsHtml(task)",
        "taskHistoryHtml(task)",
        "taskActivityHtml(task)",
        "qa.howToTest",
    ):
        assert marker in detail


def test_task_writes_keep_server_permissions_and_private_session_auth():
    js = assembled_dashboard_js()
    request = js[js.index("async function taskRequest("):js.index("function taskNoticeHtml(")]
    assert 'credentials: "same-origin"' in request
    assert 'headers.authorization = `Bearer ${token}`' in request
    assert 'cache: "no-store"' in request
    assert "payload.ok === false" in request

    actions = js[js.index("function taskActionsHtml("):js.index("function renderTaskDetail(")]
    for gate in (
        "state.tasksView.canManage",
        "mine && !done",
        "!done && (mine || manager)",
        "done && (manager || mine || creator || reviewer)",
    ):
        assert gate in actions


def test_task_images_are_bounded_and_loaded_through_authenticated_fetch():
    js = assembled_dashboard_js()
    for marker in (
        "files.length > 4",
        "file.size > 256 * 1024",
        "total > 1024 * 1024",
        "TASK_IMAGE_TYPES.has(file.type)",
        "async function taskAttachmentObjectUrl(path)",
        'credentials: "same-origin"',
        "URL.createObjectURL(await response.blob())",
    ):
        assert marker in js


def test_task_images_open_in_an_authenticated_full_size_preview():
    js = assembled_dashboard_js()
    for marker in (
        "data-task-attachment-link",
        "async function expandTaskAttachment(link)",
        "function openTaskImagePreview(src, name)",
        "dialog.showModal()",
        "task-image-modal",
    ):
        assert marker in js


_WORK_ITEMS_HARNESS = r"""
const fs = require("fs");
const source = fs.readFileSync(process.argv[1], "utf8");
function extract(name) {
  const start = source.indexOf(`function ${name}(`);
  if (start < 0) throw new Error(`missing ${name}`);
  let depth = 0;
  for (let index = source.indexOf("{", start); index < source.length; index += 1) {
    if (source[index] === "{") depth += 1;
    if (source[index] === "}" && --depth === 0) return source.slice(start, index + 1);
  }
  throw new Error(`unbalanced ${name}`);
}
const escapeHtml = (value) => String(value).replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
const names = ["taskDetailCard", "taskRepositoryHtml", "taskWorkItems", "taskWorkItemsHtml", "taskPromptText"];
const repoPathUrl = (repo) => `/${repo.owner}/${repo.name}`;
const repoPublicKey = (key) => String(key);
const api = new Function("escapeHtml", "repoPathUrl", "repoPublicKey", `${names.map(extract).join("\n")}; return { ${names.join(", ")} };`)(escapeHtml, repoPathUrl, repoPublicKey);
const task = {
  id: "a".repeat(32), title: "Ship", repository: "forkmesh/src",
  workItems: [
    { repository: "forkmesh/src", branch: "feat/<x>", worktree: "/w/one", node: "desk" },
    { repository: "", branch: "fix/two", worktree: "", node: "" },
    { repository: "", branch: "", worktree: "/w/none", node: "" },
    null,
  ],
};
process.stdout.write(JSON.stringify({
  items: api.taskWorkItems(task).length,
  none: api.taskWorkItemsHtml({ workItems: "bad" }),
  html: api.taskWorkItemsHtml(task),
  prompt: api.taskPromptText(task),
}));
"""


def test_task_detail_lists_work_items_escaped_with_repository_links():
    node = shutil.which("node")
    if node is None:
        pytest.skip("node is not installed")
    source = ROOT / "public" / "dashboard" / "js" / "09-tasks.js"
    result = subprocess.run(
        [node, "-e", _WORK_ITEMS_HARNESS, str(source)],
        capture_output=True, text=True, timeout=60, check=True)
    out = json.loads(result.stdout)
    assert out["items"] == 2
    assert out["none"] == ""
    html = out["html"]
    assert "Work items (2)" in html
    assert "feat/&#60;x&#62;" in html and "<x>" not in html
    assert 'href="/forkmesh/src"' in html
    assert "on desk · /w/one" in html
    assert "fix/two" in html and "/w/none" not in html
    assert "Branches: feat/<x>, fix/two" in out["prompt"]
    js = assembled_dashboard_js()
    assert "${taskWorkItemsHtml(task)}" in js
    assert "taskWorkItems(task).flatMap" in js
    assert ".task-work-item {" in TASK_SOURCE


def _task_row_thumbnails(task):
    import json
    import subprocess
    js = assembled_dashboard_js()
    fn = js[js.index("function taskRowThumbnailsHtml(task)"):js.index("function taskDetailCard(")]
    script = (
        "const escapeHtml = (value) => String(value).replace(/[&<>\"']/g, (c) => `&#${c.charCodeAt(0)};`);\n"
        "%s\nconsole.log(JSON.stringify(taskRowThumbnailsHtml(%s)));" % (fn, json.dumps(task)))
    result = subprocess.run(["node", "--input-type=module", "-e", script],
                            capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def test_task_rows_show_attached_image_thumbnails():
    thumb = "data:image/jpeg;base64,AAAA"
    html = _task_row_thumbnails({"attachments": [
        {"name": "one.png", "thumbnail": thumb},
        {"name": "two.gif", "url": "/api/tasks/t/attachments/a"},
        {"name": "three.png", "thumbnail": "javascript:alert(1)"},
        {"name": "four.png", "thumbnail": thumb},
    ]})
    assert html.count('class="task-row-thumb"') == 3
    assert html.count('<img src="%s"' % thumb) == 1
    assert "javascript:" not in html
    assert html.count('data-lucide="image"') == 2
    assert '<span class="task-row-thumb-more">+1</span>' in html
    assert 'aria-label="4 images attached"' in html
    assert "data-task-attachment-preview" not in html
    assert _task_row_thumbnails({"attachments": []}) == ""
    assert _task_row_thumbnails({}) == ""
    assert "${taskRowThumbnailsHtml(task)}" in assembled_dashboard_js()
    for marker in (".task-row-thumbs {", ".task-row-thumb {", ".task-row-thumb-more {"):
        assert marker in TASK_SOURCE


def test_web_task_uploads_carry_a_list_thumbnail():
    js = assembled_dashboard_js()
    upload = js[js.index("async function taskEditorAttachments()"):js.index("async function saveTaskEditor()")]
    assert "const thumbnail = await taskImageThumbnail(file);" in upload
    assert "if (thumbnail) attachment.thumbnail = thumbnail;" in upload
    assert 'file.type === "image/gif"' in upload
    assert "const side = 48;" in upload
    assert "thumbnail.length <= 10_000" in upload


def test_tasks_page_trashes_with_undo_and_restores_from_a_trash_filter():
    js = assembled_dashboard_js()
    page = assembled_dashboard_page("tasks")
    hand = (ROOT / "public" / "dashboard" / "tasks" / "index.html").read_text(
        encoding="utf-8")
    for source in (TASK_SOURCE, page, hand):
        assert 'data-tasks-filter="trash"' in source
        assert '<p data-tasks-notice class="task-notice" role="status" hidden></p>' in source
    for marker in (
        'taskActionHtml("delete", "trash-2", "Delete", manager || mine || creator, "danger")',
        'taskActionHtml("restore", "archive-restore", "Restore"',
        'setTaskListNotice("Task moved to trash.", "good", [taskId])',
        "data-tasks-notice-undo",
        "void undoTaskDelete(",
        'delete: ["Move task to trash"',
        "Restore it from Trash within ${TASK_TRASH_DAYS} days",
        "const TASK_TRASH_DAYS = 30;",
        "const TASK_DELETE_BATCH = 50;",
        'error?.message === "task_delete_blocked"',
        'dialog.dataset.force === "1" ? { force: true } : {}',
        "state.tasksView.items.filter(taskClearable)",
        'state.tasksView.filter === "trash" ? state.tasksView.trash : state.tasksView.items',
    ):
        assert marker in js, marker
    assert "Delete permanently" not in js
    assert "This permanently removes" not in js


_NOTICE_HARNESS = r"""
const fs = require("fs");
const source = fs.readFileSync(process.argv[1], "utf8");
function extract(name) {
  const start = source.indexOf(`function ${name}(`);
  if (start < 0) throw new Error(`missing ${name}`);
  let depth = 0;
  for (let index = source.indexOf("{", start); index < source.length; index += 1) {
    if (source[index] === "{") depth += 1;
    if (source[index] === "}" && --depth === 0) return source.slice(start, index + 1);
  }
  throw new Error(`unbalanced ${name}`);
}
const escapeHtml = (value) => String(value).replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
const line = { hidden: true, dataset: {}, innerHTML: "" };
const $ = (selector) => (selector === "[data-tasks-notice]" ? line : null);
const names = ["taskNoticeHtml", "renderTaskListNotice", "setTaskListNotice"];
const api = new Function("escapeHtml", "$", `let taskDetailNotice = null; let taskListNotice = null;
${names.map(extract).join("\n")}
return { ${names.join(", ")}, setDetail(value) { taskDetailNotice = value; } };`)(escapeHtml, $);
const snapshot = () => ({ hidden: line.hidden, tone: line.dataset.tone || "", html: line.innerHTML });
api.setTaskListNotice("Task moved to trash.", "good", ["a".repeat(32), "b".repeat(32)]);
const undo = snapshot();
api.setTaskListNotice("Could not load the trash: <offline>", "bad");
const failed = snapshot();
api.setTaskListNotice("");
const cleared = snapshot();
api.setDetail({ taskId: "t1", place: "progress", message: "Progress set to <65>%.", tone: "good" });
process.stdout.write(JSON.stringify({
  undo, failed, cleared,
  shown: api.taskNoticeHtml({ id: "t1" }, "progress"),
  otherPlace: api.taskNoticeHtml({ id: "t1" }, "actions"),
  otherTask: api.taskNoticeHtml({ id: "t2" }, "progress"),
}));
"""


def test_task_feedback_is_inline_next_to_the_action_not_a_toast():
    node = shutil.which("node")
    if node is None:
        pytest.skip("node is not installed")
    source = ROOT / "public" / "dashboard" / "js" / "09-tasks.js"
    result = subprocess.run(
        [node, "-e", _NOTICE_HARNESS, str(source)],
        capture_output=True, text=True, timeout=60, check=True)
    out = json.loads(result.stdout)
    assert out["undo"]["hidden"] is False
    assert out["undo"]["tone"] == "good"
    assert "<span>Task moved to trash.</span>" in out["undo"]["html"]
    assert 'data-tasks-notice-undo="%s,%s"' % ("a" * 32, "b" * 32) in out["undo"]["html"]
    assert ">Undo</button>" in out["undo"]["html"]
    assert out["failed"]["tone"] == "bad"
    assert "&#60;offline&#62;" in out["failed"]["html"]
    assert "data-tasks-notice-undo" not in out["failed"]["html"]
    assert out["cleared"] == {"hidden": True, "tone": "bad", "html": ""}
    assert out["shown"] == ('<p data-task-notice="progress" class="task-notice" role="status" '
                            'data-tone="good">Progress set to &#60;65&#62;%.</p>')
    assert out["otherPlace"] == '<p data-task-notice="actions" class="task-notice" role="status" hidden></p>'
    assert out["otherTask"] == '<p data-task-notice="progress" class="task-notice" role="status" hidden></p>'

    js = assembled_dashboard_js()
    for place in ("actions", "activity", "progress", "persistent", "checkin", "remind"):
        assert 'taskNoticeHtml(task, "%s")' % place in js, place
    page = assembled_dashboard_page("tasks")
    hand = (ROOT / "public" / "dashboard" / "tasks" / "index.html").read_text(encoding="utf-8")
    for text in (js, TASK_SOURCE, page, hand):
        assert "toast" not in text.lower().replace("fm-toast", "")
    for rule in (".task-notice {", '.task-notice[data-tone="bad"]', ".task-notice-undo {"):
        assert rule in TASK_SOURCE
    assert "position: fixed" not in TASK_SOURCE[TASK_SOURCE.index(".task-notice {"):TASK_SOURCE.index("[data-task-action-submit]")]
