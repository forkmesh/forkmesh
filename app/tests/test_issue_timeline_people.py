import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
HELPERS = (JS / "02-helpers.js").read_text(encoding="utf-8")
CONTENT = (JS / "06-repo-content.js").read_text(encoding="utf-8")
COMPOSE = (JS / "07-repo-compose-branch.js").read_text(encoding="utf-8")
SHELL = (ROOT / "public" / "dashboard" / "shell.html").read_text(encoding="utf-8")


def _function(text, name):
    match = re.search(r"^  (?:async )?function %s\(" % re.escape(name), text, re.M)
    start = match.start()
    cursor = text.index("{", text.index(")", start))
    depth = 0
    while True:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                return text[start:cursor + 1]
        cursor += 1


def _const(text, name):
    start = text.index("  const %s =" % name)
    return text[start:text.index(";\n", start) + 1]


HELPER_NAMES = (
    "escapeHtml", "userNameKey", "userDisplayName", "userNameHtml", "issuePersonHandle",
    "botAvatarKind", "botAvatarSvg", "issueActorIsBot", "issueActorPixels", "issuePersonProfileHref", "issueActorAvatarHtml",
    "issuePersonNameHtml", "hydrateIssuePersonLink", "issueActorPortraitPng", "issueActorAccount",
    "forgeIdentity", "forgeLinkAttrs",
)
CONTENT_NAMES = (
    "repoContributorIsBot", "repoContributorProfileUrl", "repoRosterPerson", "issueCardAuthorHtml",
    "issueEventDescription", "issueEventDescriptionHtml", "renderIssueTimelineEvent",
    "renderIssueTimelineComment", "mentionQueryAt", "mentionInsertion", "mentionRecordPeople",
    "buildMentionCandidates", "rankMentionCandidates", "mentionKeyAction",
)


def _node(body):
    script = """
const state = { session: { nodeName: "me" }, userNameCases: new Map() };
let watched = 0;
function watchIssueActorAvatars() { watched += 1; }
function validNodeName(value) { return /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(String(value || "")); }
function publicProfileUrl(name) { return `https://code.example.test/@${encodeURIComponent(String(name).toLowerCase())}`; }
function platformName() { return "Acme Code"; }
function renderIssueLabelChip(label) { return `<span data-label>${escapeHtml(label)}</span>`; }
function formatRecordDateTime() { return { iso: "2026-10-20T00:00:00.000Z", label: "now" }; }
function formatTimeAgo() { return "1 minute ago"; }
function renderRecordMarkdown(body) { return `<p>${escapeHtml(body)}</p>`; }
function repoRosterAvatarUrl(repo, row) { return row.slug ? `/raw/${row.slug}.png` : ""; }
const MD_BODY_CLASS = "md";
const fetched = [];
globalThis.fetch = async (url) => {
  fetched.push(url);
  const name = decodeURIComponent(String(url).split("/").pop());
  const exists = name === "alice";
  return { ok: true, json: async () => ({ exists, avatarPng: exists ? "iVBORw0KGgo=" : "" }) };
};
const issueActorAccounts = new Map();
const issueActorAccountsSettled = new Map();
%s
%s
%s
%s
%s
""" % (
        _const(HELPERS, "USER_NAME_RE"),
        "\n".join([_const(HELPERS, "ISSUE_PERSON_HANDLE_RE"), _const(HELPERS, "FORGE_PROFILES"),
                   _const(HELPERS, "FORGE_IDENTITY_RE")]) + "\n" + HELPERS[
            HELPERS.index("  const BOT_AVATAR_GLYPHS = {"):
            HELPERS.index("\n  };\n", HELPERS.index("  const BOT_AVATAR_GLYPHS = {")) + 5],
        "\n".join(_function(HELPERS, name) for name in HELPER_NAMES),
        "\n".join([_const(CONTENT, "MD_MENTION"), _const(CONTENT, "MENTION_SUGGESTION_LIMIT"),
                   _const(CONTENT, "MENTION_LOGIN_RE")]
                  + [_function(CONTENT, name) for name in CONTENT_NAMES]),
        body,
    )
    result = subprocess.run(["node", "--input-type=module"], input=script,
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stderr[-2000:]
    return json.loads(result.stdout)


def test_people_get_linked_avatars_and_bots_get_a_bot_mark():
    out = _node("""
console.log(JSON.stringify({
  person: issueActorAvatarHtml("vandana42", "", "h-6 w-6"),
  mixed: issueActorAvatarHtml("Kofi-Mensah", "", "h-5 w-5"),
  digit: issueActorAvatarHtml("2Fast-Dev", "", "h-5 w-5"),
  importer: issueActorAvatarHtml("gitlab-import[bot]", "", "h-5 w-5"),
  actions: issueActorAvatarHtml("acme-actions", "", "h-10 w-10"),
  name: issuePersonNameHtml("vandana42"),
  mixedName: issuePersonNameHtml("Kofi-Mensah"),
  digitName: issuePersonNameHtml("2Fast-Dev"),
  botName: issuePersonNameHtml("tasks[bot]"),
  hostile: issuePersonNameHtml('<img src=x onerror=alert(1)>'),
  hostileAvatar: issueActorAvatarHtml('"><script>alert(1)</script>', "", "h-5 w-5"),
}));
""")
    assert out["person"].startswith('<a href="https://code.example.test/@vandana42" data-issue-person="vandana42"')
    assert 'data-issue-actor-avatar="vandana42"' in out["person"] and "h-6 w-6" in out["person"]
    assert out["mixed"].startswith('<a href="https://code.example.test/@kofi-mensah" data-issue-person="kofi-mensah"')
    assert 'data-issue-actor-avatar="kofi-mensah"' in out["mixed"]
    assert out["digit"].startswith('<a data-issue-person="2fast-dev"') and 'data-issue-actor-avatar="2fast-dev"' in out["digit"]
    for bot in (out["importer"], out["actions"]):
        assert bot.startswith("<span") and "<svg data-bot-avatar=" in bot
        assert "data-issue-person" not in bot and "href" not in bot
    assert '<svg data-bot-avatar="bot"' in out["importer"]
    assert "data-issue-actor-avatar" not in out["importer"]
    assert '<svg data-bot-avatar="actions"' in out["actions"] and 'class="h-5 w-5 text-muted-foreground"' in out["actions"]
    assert 'data-issue-actor-avatar="acme-actions"' in out["actions"]
    assert 'href="https://code.example.test/@vandana42"' in out["name"] and ">vandana42</span></a>" in out["name"]
    assert ">Kofi-Mensah</span></a>" in out["mixedName"] and 'href="https://code.example.test/@kofi-mensah"' in out["mixedName"]
    assert out["digitName"].startswith('<a data-issue-person="2fast-dev"') and ">2Fast-Dev</a>" in out["digitName"]
    assert out["botName"] == '<span class="font-semibold text-foreground">tasks[bot]</span>'
    assert "<img" not in out["hostile"] and "&lt;img src=x onerror=alert(1)&gt;" in out["hostile"]
    assert "<script" not in out["hostileAvatar"] and "href" not in out["hostileAvatar"]


def test_mirrored_github_authors_link_to_their_github_profile_without_loading_github():
    out = _node("""
console.log(JSON.stringify({
  name: issuePersonNameHtml("github:Gulp-Run"),
  avatar: issueActorAvatarHtml("github:wkrp", "", "h-6 w-6"),
  comment: renderIssueTimelineComment({ type: "comment", authorName: "github:immartian", body: "hi", ts: 1 }),
  sync: issuePersonNameHtml("github:~sync"),
  hostile: issuePersonNameHtml('github:x"onmouseover=alert(1)'),
  doubleDash: issuePersonNameHtml("github:a--b"),
  other: issuePersonNameHtml("gitea:alice"),
  row: userNameHtml("github:JohnWatsoon"),
  watched,
}));
""")
    assert out["name"].startswith('<a href="https://github.com/Gulp-Run" target="_blank" rel="noopener noreferrer nofollow"')
    assert 'title="Gulp-Run on GitHub"' in out["name"] and ">@Gulp-Run</a>" in out["name"]
    assert out["avatar"].startswith('<a href="https://github.com/wkrp"') and ">W</span></a>" in out["avatar"]
    assert "data-issue-actor-avatar" not in out["avatar"] and "<img" not in out["avatar"]
    assert out["comment"].count('href="https://github.com/immartian"') == 3
    assert "data-issue-actor-avatar" not in out["comment"] and "data-issue-person" not in out["comment"]
    for plain in (out["sync"], out["hostile"], out["doubleDash"], out["other"]):
        assert "github.com" not in plain and "<a" not in plain and '"onmouseover' not in plain
    assert out["watched"] == 0
    assert out["row"] == '<span title="JohnWatsoon on GitHub">@JohnWatsoon</span>'


def test_event_rows_and_comment_cards_name_people_with_links():
    out = _node("""
console.log(JSON.stringify({
  assign: renderIssueTimelineEvent("users", "tasks[bot]", "", issueEventDescriptionHtml({ type: "assignees", assignees: ["vandana42", "2Fast-Dev"] }), 1),
  label: renderIssueTimelineEvent("tag", "gitlab-import[bot]", "", issueEventDescriptionHtml({ type: "labels", labels: ["next"] }), 1),
  comment: renderIssueTimelineComment({ type: "comment", authorName: "vandana42", body: "/assign", ts: 1 }),
  bot: renderIssueTimelineComment({ type: "comment", authorName: "acme-actions", body: "hi", ts: 1 }),
}));
""")
    assign = out["assign"]
    assert '<span class="font-semibold text-foreground">tasks[bot]</span>' in assign
    assert '<svg data-bot-avatar="bot"' in assign
    assert "set assignees: <a href=\"https://code.example.test/@vandana42\"" in assign
    assert '<a data-issue-person="2fast-dev"' in assign and ">2Fast-Dev</a>" in assign
    assert '<svg data-bot-avatar="bot"' in out["label"] and ">G<" not in out["label"]
    comment = out["comment"]
    assert comment.count('data-issue-actor-avatar="vandana42"') == 2
    assert '<span class="hidden sm:block">' in comment and '<span class="mr-1 inline-flex align-middle sm:hidden">' in comment
    assert 'class="relative flex h-6 w-6 shrink-0' in comment and 'class="relative flex h-10 w-10 shrink-0' in comment
    assert '<a href="https://code.example.test/@vandana42" data-issue-person="vandana42" class="font-semibold' in comment
    assert out["bot"].count('<svg data-bot-avatar="actions"') == 2 and "data-issue-person" not in out["bot"]


def test_links_resolve_to_the_forge_then_the_roster_profile_then_plain_text():
    out = _node("""
function link(person, mention = false) {
  const attrs = new Map([["href", "https://code.example.test/@" + person], ["data-issue-person", person]]);
  if (mention) attrs.set("data-issue-mention", "");
  const node = {
    dataset: { issuePerson: person }, textContent: "@" + person, replaced: null,
    setAttribute(name, value) { attrs.set(name, value); },
    removeAttribute(name) { attrs.delete(name); },
    hasAttribute(name) { return attrs.has(name); },
    replaceWith(text) { node.replaced = text.textContent; },
    attrs,
  };
  return node;
}
globalThis.document = { createTextNode: (text) => ({ textContent: text }) };
const forge = link("alice");
hydrateIssuePersonLink(forge, { account: { exists: true }, roster: { profile: "https://example.com/alice" } });
const roster = link("bob");
hydrateIssuePersonLink(roster, { account: { exists: false }, roster: { profile: "https://example.com/bob" } });
const nobody = link("carol");
hydrateIssuePersonLink(nobody, { account: { exists: false }, roster: null });
const mention = link("dave", true);
hydrateIssuePersonLink(mention, { account: { exists: false }, roster: null });
const accounts = [await issueActorAccount("alice"), await issueActorAccount("ALICE"), await issueActorAccount("2Fast-Dev")];
const rows = [{ name: "Vandana", login: "", github: "Vandana42", gitlab: "vandana_gl", slug: "vandana42", profile: "https://github.com/vandana42", commits: 3 }];
const byLogin = new Map([["vandana42", rows[0]], ["vandana_gl", rows[0]]]);
const repo = { owner: "acme", name: "space" };
console.log(JSON.stringify({
  forge: Object.fromEntries(forge.attrs), roster: Object.fromEntries(roster.attrs),
  nobody: Object.fromEntries(nobody.attrs), mention: mention.replaced, accounts, fetched,
  match: repoRosterPerson(repo, { byLogin }, "VANDANA42"),
  gitlab: repoRosterPerson(repo, { byLogin }, "@vandana_gl")?.login,
  miss: repoRosterPerson(repo, { byLogin }, "someone"),
}));
""")
    assert out["forge"]["href"] == "https://code.example.test/@alice" and "target" not in out["forge"]
    assert out["roster"]["href"] == "https://example.com/bob"
    assert out["roster"]["target"] == "_blank" and out["roster"]["rel"] == "noopener noreferrer nofollow ugc"
    assert "href" not in out["nobody"]
    assert out["mention"] == "@dave"
    assert out["accounts"][0] == {"exists": True, "png": "iVBORw0KGgo=", "linkable": False} == out["accounts"][1]
    assert out["accounts"][2] == {"exists": False, "png": "", "linkable": False}
    assert out["fetched"] == ["/api/accounts/alice"]
    assert out["match"] == {"name": "Vandana", "login": "Vandana42", "profile": "https://github.com/vandana42",
                            "avatarUrl": "/raw/vandana42.png", "commits": 3}
    assert out["gitlab"] == "Vandana42" and out["miss"] is None


def test_avatar_sizes_are_fixed_and_pictures_load_lazily():
    hydrate = _function(HELPERS, "issueActorAvatarImage")
    assert 'image.loading = "lazy"' in hydrate and "image.width = pixels" in hydrate and "image.height = pixels" in hydrate
    assert "absolute inset-0 h-full w-full object-cover" in hydrate
    resolve = _function(HELPERS, "hydrateIssueActorAvatars")
    assert "found.account.png" in resolve and "found.roster?.avatarUrl" in resolve
    assert ".fm-mention-avatar { position: relative; display: inline-flex; flex: none;" in SHELL
    assert "width: 24px; height: 24px;" in SHELL and "min-height: 44px;" in SHELL


def test_mention_query_is_found_only_at_a_word_boundary():
    out = _node("""
const cases = ["@va", "hi @va", "(@Va", "mail me@va", "x @va-b", "@", "a@", "`@va", "@va ", "@@va", "see /@va"];
console.log(JSON.stringify(cases.map((text) => mentionQueryAt(text, text.length))
  .concat([mentionQueryAt("hi @vans there", 7), mentionQueryAt("hi @vans", 3)])));
""")
    assert out == [
        {"start": 0, "query": "va"}, {"start": 3, "query": "va"}, {"start": 1, "query": "Va"}, None,
        {"start": 2, "query": "va-b"}, {"start": 0, "query": ""}, None, None, None, None, None,
        {"start": 3, "query": "van"}, None,
    ]


def test_picking_a_mention_inserts_the_login_at_the_caret():
    out = _node("""
console.log(JSON.stringify([
  mentionInsertion("@va", 0, 3, "vandana42"),
  mentionInsertion("hi @va thanks", 3, 6, "vandana42"),
  mentionInsertion("hi @vanXX done", 3, 7, "vandana42"),
  mentionInsertion("ping @va", 5, 8, "Kofi-Mensah"),
]));
""")
    assert out[0] == {"start": 0, "end": 3, "text": "@vandana42 ", "value": "@vandana42 ", "caret": 11}
    assert out[1]["value"] == "hi @vandana42 thanks" and out[1]["caret"] == len("hi @vandana42 ")
    assert out[1]["start"] == 3 and out[1]["end"] == 7
    assert out[2]["value"] == "hi @vandana42 done" and out[2]["end"] == 10
    assert out[3]["value"] == "ping @Kofi-Mensah "


def test_suggestions_rank_people_in_the_thread_before_the_roster():
    out = _node("""
const detail = { parsed: {
  values: { authorName: "gitlab-import[bot]", createdAt: 1000, assignees: ["vandana42"], updatedAt: 3000 },
  issueEvents: [
    { type: "comment", authorName: "valerie", ts: 2000, body: "cc @Vaughn and me@vanmail.com" },
    { type: "assignees", authorName: "tasks[bot]", ts: 3000, assignees: ["vandana42"] },
    { type: "comment", authorName: "acme-actions", ts: 4000, body: "@vandana42 you're assigned" },
    { type: "comment", authorName: "me", ts: 5000, body: "x" },
  ],
} };
const roster = [
  { name: "Vandana", github: "vandana42", commits: 3, slug: "vandana42", avatarUrl: "/raw/v.png" },
  { name: "Ivan Petrov", login: "ipetrov", commits: 50 },
  { name: "Vance Big", login: "vbig", github: "vbig", commits: 900 },
  { name: "Lab", gitlab: "only_gitlab", commits: 1000 },
  { name: "Bad", login: "bad name", commits: 2000 },
];
const all = buildMentionCandidates(mentionRecordPeople(detail), roster, "me");
const many = buildMentionCandidates([], Array.from({ length: 20 }, (_, i) => ({ login: `va${i}`, commits: i })));
console.log(JSON.stringify({
  logins: all.map((c) => [c.login, c.tier, c.name]),
  va: rankMentionCandidates(all, "va").map((c) => c.login),
  VA: rankMentionCandidates(all, "VA").map((c) => c.login),
  van: rankMentionCandidates(all, "van").map((c) => c.login),
  pet: rankMentionCandidates(all, "pet").map((c) => c.login),
  word: rankMentionCandidates([{ login: "zed", name: "Vance Zed" }, { login: "ava", name: "" }, { login: "vab", name: "" }], "va").map((c) => c.login),
  empty: rankMentionCandidates(all, "").map((c) => c.login),
  limit: rankMentionCandidates(many, "va").length,
  avatar: all[0].avatarUrl,
}));
""")
    assert out["logins"] == [
        ["vandana42", 0, "Vandana"], ["valerie", 0, ""], ["Vaughn", 0, ""],
        ["vbig", 1, "Vance Big"], ["ipetrov", 1, "Ivan Petrov"],
    ]
    assert out["va"] == ["vandana42", "valerie", "Vaughn", "vbig", "ipetrov"]
    assert out["VA"] == out["va"]
    assert out["van"] == ["vandana42", "vbig", "ipetrov"]
    assert out["pet"] == ["ipetrov"]
    assert out["word"] == ["vab", "zed", "ava"]
    assert out["empty"] == ["vandana42", "valerie", "Vaughn", "vbig", "ipetrov"]
    assert out["limit"] == 8
    assert out["avatar"] == "/raw/v.png"


def test_account_search_results_follow_the_thread_and_roster():
    out = _node("""
const roster = [{ name: "Vance Big", login: "vbig", commits: 9 }];
const all = buildMentionCandidates([{ login: "valerie", ts: 1 }], roster, "me",
  ["vbig", "me", "vera-x", "valerie", "bad name", "dependabot"]);
console.log(JSON.stringify(all.map((c) => [c.login, c.tier, c.name])));
""")
    assert out == [["valerie", 0, ""], ["vbig", 1, "Vance Big"], ["vera-x", 2, ""]]


def test_mention_keys_move_pick_and_close():
    out = _node("""
console.log(JSON.stringify([
  mentionKeyAction("ArrowDown", 0, 3), mentionKeyAction("ArrowDown", 2, 3),
  mentionKeyAction("ArrowUp", 0, 3), mentionKeyAction("Enter", 1, 3),
  mentionKeyAction("Tab", 2, 3), mentionKeyAction("Escape", 0, 3),
  mentionKeyAction("a", 0, 3), mentionKeyAction("Enter", 0, 0),
]));
""")
    assert out == [
        {"action": "move", "index": 1}, {"action": "move", "index": 0},
        {"action": "move", "index": 2}, {"action": "pick", "index": 1},
        {"action": "pick", "index": 2}, {"action": "close"}, None, None,
    ]


def test_mention_autocomplete_is_wired_into_every_record_editor():
    for attribute in ("data-repo-issue-comment-body", "data-repo-pull-review-body",
                      "data-repo-issue-description-input", "data-repo-discussion-reply-body",
                      "data-repo-pull-inline-body"):
        assert f'<textarea {attribute} data-mention-input="record"' in CONTENT
    for attribute in ("data-repo-issue-body", "data-repo-pull-body"):
        assert f'<textarea {attribute} data-mention-input="repo"' in COMPOSE
    bind = _function(CONTENT, "bindMentionAutocomplete")
    assert 'window.visualViewport?.addEventListener("resize", reposition)' in bind
    assert "event.stopImmediatePropagation()" in bind and "}, true);" in bind
    render = _function(CONTENT, "renderMentionOptions")
    assert 'role="option"' in render and 'input.setAttribute("aria-activedescendant"' in render
    assert 'list.setAttribute("role", "listbox")' in _function(CONTENT, "mentionListElement")
    position = _function(CONTENT, "positionMentionList")
    assert "view.offsetTop" in position and "view.height" in position
    roster = _function(CONTENT, "mentionRosterRows")
    assert "repoContributorRosterCached(repo)" in roster and "fetch(" not in roster


def test_editing_a_comment_uses_an_inline_editor_with_suggestions():
    io = (JS / "03-issues-profile-io.js").read_text(encoding="utf-8")
    assert "await openIssueCommentEditor(target," in io
    assert 'window.prompt("Edit comment"' not in io
    editor = _function(CONTENT, "openIssueCommentEditor")
    assert 'data-mention-input="record"' in editor and "input.value = String(body" in editor
    assert 'container.querySelector("[data-repo-issue-comment-editor]")' in CONTENT
