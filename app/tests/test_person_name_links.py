import json
import re
import subprocess
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
SOURCES = {path.name: path.read_text(encoding="utf-8") for path in JS.glob("*.js")}
DOCS = (ROOT.parent / "docs" / "app" / "profile-links.md").read_text(encoding="utf-8")
SSR = (ROOT / "host" / "forkmesh_host" / "repo_ssr.py").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()
BOOT = "  void bootDashboard();"
EXPORTS = "issuePersonNameHtml, issueActorAccount, hydrateIssuePersonLink, hydrateIssueActorAvatars, state"

STUBS = """
const vm = require("vm");
const accounts = {
  alice: { exists: true, available: false, kind: "user" },
  node7: { exists: true, available: false, kind: "node" },
  hidden: { exists: true, available: false, kind: "user", profilePrivate: true },
  pending: { exists: true, available: true, kind: "user" },
  held: { exists: true, available: false },
  "helper-bot": { exists: true, available: false, kind: "user" },
};
const fetched = [];
global.fetch = async (url) => {
  fetched.push(String(url));
  const name = decodeURIComponent(String(url).split("/").pop());
  return { ok: true, json: async () => accounts[name] || { exists: false, available: true } };
};
global.location = { origin: "https://code.example.org", hostname: "code.example.org", pathname: "/dashboard", search: "", hash: "" };
global.localStorage = { getItem() { return null; }, setItem() {}, removeItem() {} };
const emptyClassList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
global.document = {
  body: null,
  documentElement: { dataset: {}, style: {} },
  cookie: "",
  querySelector() { return null; },
  querySelectorAll() { return []; },
  addEventListener() {},
  createElement() {
    const attrs = new Map();
    return {
      attrs, dataset: {}, className: "", children: [],
      setAttribute(name, value) { attrs.set(name, String(value)); },
      replaceChildren(...nodes) { this.children = nodes; },
      set href(value) { attrs.set("href", value); },
    };
  },
  createTextNode: (text) => ({ textContent: text }),
};
global.window = {
  matchMedia() { return { addEventListener() {}, addListener() {} }; },
  addEventListener() {},
  dispatchEvent() { return true; },
  history: { state: null, pushState() {}, replaceState() {} },
  lucide: { createIcons() {} },
  setTimeout() {}, setInterval() {}, clearInterval() {},
};
global.navigator = {};
vm.runInThisContext(SOURCE);
const api = global.__dashboardExports;
const out = {};
function link(person, git = false) {
  const attrs = new Map([["data-issue-person", person]]);
  if (!git) attrs.set("href", "https://code.example.org/@" + person);
  else attrs.set("data-issue-person-git", "");
  return {
    dataset: { issuePerson: person }, attrs, textContent: person,
    setAttribute(name, value) { attrs.set(name, value); },
    removeAttribute(name) { attrs.delete(name); },
    hasAttribute(name) { return attrs.has(name); },
    replaceWith() {},
  };
}
"""


def run(body):
    transformed = DASHBOARD_JS.replace(
        BOOT, "  globalThis.__dashboardExports = { " + EXPORTS + " };")
    assert transformed != DASHBOARD_JS
    script = ("const SOURCE = " + json.dumps(transformed) + ";\n" + STUBS
              + "(async () => {\n" + body
              + "\nconsole.log(JSON.stringify(out));\n})();")
    result = subprocess.run(["node"], input=script, check=True,
                            capture_output=True, text=True)
    assert result.stderr == ""
    return json.loads(result.stdout.strip().splitlines()[-1])


def test_the_helper_links_people_and_leaves_rows_bots_and_git_names_alone():
    out = run("""
out.person = api.issuePersonNameHtml("Alice", "font-semibold");
out.row = api.issuePersonNameHtml("alice", "", { inLink: true });
out.git = api.issuePersonNameHtml("alice", "", { git: true });
out.gitPlain = api.issuePersonNameHtml("Jane Doe", "", { git: true });
out.bot = api.issuePersonNameHtml("helper-bot", "");
out.botRow = api.issuePersonNameHtml("helper-bot", "", { inLink: true });
out.actions = api.issuePersonNameHtml("acme-actions", "");
out.tagged = api.issuePersonNameHtml("tasks[bot]", "");
out.hostile = api.issuePersonNameHtml('<img src=x onerror=alert(1)>', "");
""")
    assert out["person"] == (
        '<a href="https://code.example.org/@alice" data-issue-person="alice" '
        'class="font-semibold hover:text-[var(--dashboard-link)] hover:underline">'
        '<span data-user-name="alice">Alice</span></a>')
    assert out["row"] == '<span class=""><span data-user-name="alice">alice</span></span>'
    assert out["git"].startswith('<a data-issue-person="alice" data-issue-person-git=""')
    assert "href" not in out["git"]
    assert out["gitPlain"] == '<span class="">Jane Doe</span>'
    assert out["bot"] == '<span data-issue-bot-account="helper-bot" class="">helper-bot</span>'
    assert out["botRow"] == '<span class="">helper-bot</span>'
    assert out["actions"] == '<span class="">acme-actions</span>'
    assert out["tagged"] == '<span class="">tasks[bot]</span>'
    assert "<img" not in out["hostile"] and "&lt;img" in out["hostile"]


def test_only_public_taken_user_or_node_accounts_are_linkable():
    out = run("""
out.accounts = {};
for (const name of ["alice", "node7", "hidden", "pending", "held", "nobody"]) {
  out.accounts[name] = (await api.issueActorAccount(name)).linkable;
}
const plain = link("pending");
api.hydrateIssuePersonLink(plain, { account: await api.issueActorAccount("pending"), roster: null });
const person = link("alice");
api.hydrateIssuePersonLink(person, { account: await api.issueActorAccount("alice"), roster: null });
const gitPerson = link("alice", true);
api.hydrateIssuePersonLink(gitPerson, { account: await api.issueActorAccount("alice"), roster: null });
const gitRoster = link("pending", true);
api.hydrateIssuePersonLink(gitRoster, { account: await api.issueActorAccount("pending"), roster: { profile: "https://example.com/p" } });
out.plain = Object.fromEntries(plain.attrs);
out.person = Object.fromEntries(person.attrs);
out.gitPerson = Object.fromEntries(gitPerson.attrs);
out.gitRoster = Object.fromEntries(gitRoster.attrs);
""")
    assert out["accounts"] == {"alice": True, "node7": True, "hidden": False,
                               "pending": False, "held": False, "nobody": False}
    assert "href" not in out["plain"]
    assert out["person"]["href"] == "https://code.example.org/@alice"
    assert out["gitPerson"]["href"] == "https://code.example.org/@alice"
    assert "href" not in out["gitRoster"] and "target" not in out["gitRoster"]


def test_a_bot_with_an_account_page_becomes_a_link():
    out = run("""
const bot = {
  dataset: { issueBotAccount: "helper-bot" }, className: "font-medium", isConnected: true,
  childNodes: [{ textContent: "helper-bot" }], replaced: null,
  replaceWith(node) { this.replaced = node; },
};
const other = { ...bot, dataset: { issueBotAccount: "other-bot" }, replaced: null, replaceWith(node) { this.replaced = node; } };
api.hydrateIssueActorAvatars({ querySelectorAll: (sel) => sel === "[data-issue-bot-account]" ? [bot, other] : [] });
await new Promise((resolve) => setImmediate(resolve));
await new Promise((resolve) => setImmediate(resolve));
out.href = bot.replaced && bot.replaced.attrs.get("href");
out.person = bot.replaced && bot.replaced.attrs.get("data-issue-person");
out.className = bot.replaced && bot.replaced.className;
out.other = other.replaced;
""")
    assert out["href"] == "https://code.example.org/@helper-bot"
    assert out["person"] == "helper-bot"
    assert out["className"].startswith("font-medium ")
    assert out["other"] is None


def _function(name):
    for text in SOURCES.values():
        match = re.search(r"^  (?:async )?function %s\(" % re.escape(name), text, re.M)
        if not match:
            continue
        cursor = text.index("{", text.index(")", match.start()))
        depth = 0
        while True:
            depth += {"{": 1, "}": -1}.get(text[cursor], 0)
            if depth == 0:
                return text[match.start():cursor + 1]
            cursor += 1
    raise AssertionError(name)


RENDERERS = {
    "issueCardAuthorHtml": "issuePersonNameHtml(label)",
    "renderIssueTimelineEvent": "issuePersonNameHtml(who)",
    "issueEventDescriptionHtml": "issuePersonNameHtml(name,",
    "renderRepoPullCommits": "issuePersonNameHtml(commit.authorName",
    "renderRepoCommitDetail": "issuePersonNameHtml(commit.author",
    "renderRepoInsights": "issuePersonNameHtml(item.author",
    "activityNameHtml": "issuePersonNameHtml(text,",
    "renderCommentReactionWho": "issuePersonNameHtml(person.name",
    "repoForkRowHtml": "issuePersonNameHtml(owner,",
    "repositoryOwnerNameHtml": "issuePersonNameHtml(text,",
    "homeFeedCard": "issuePersonNameHtml(`@${homeFeedActor(item)}`",
    "notificationLeadHeadHtml": "issuePersonNameHtml(lead.actor",
}

ROW_RENDERERS = {
    "renderRepoRecordList": 'issuePersonNameHtml(item.author, "", { inLink: true })',
    "renderDiscussionRows": 'issuePersonNameHtml(item.author, "", { inLink: true })',
    "loadRepoCommits": "{ inLink: true, git: true }",
    "notificationLeadHeadHtml": "{ inLink: true }",
    "homeFeedCard": "{ inLink: Boolean(href) }",
}


def test_the_main_renderers_draw_names_with_the_shared_helper():
    for name, call in RENDERERS.items():
        assert call in _function(name), name
    for name, call in ROW_RENDERERS.items():
        assert call in _function(name), name
    assert "repositoryOwnerNameHtml(displayOwner, false)" in _function("repositoryCard")
    assert "repositoryOwnerNameHtml(label.owner)" in _function("sidebarRepositoryRow")
    assert "repositoryForkOwnerHtml(entry, parentEntry)" in _function("homeRepositoryForkRow")
    assert "repositoryForkOwnerHtml(entry, parentEntry)" in _function("sidebarRepositoryForkRow")
    assert "homeRepositoryLabelHtml(" in _function("homeRepositoryRow")
    joined = "\n".join(SOURCES.values())
    assert "linkRepoInsightsContributors" not in joined
    assert "resolveContributorProfile" not in joined


def test_names_are_not_drawn_around_the_helper():
    allowed = {
        ("02-helpers.js", "issuePersonNameHtml"),
        ("02-helpers.js", "issueActorAvatarHtml"),
        ("04-account.js", "renderProfileReferrals"),
        ("06-repo-content.js", "renderFederatedReplies"),
    }
    callers = set()
    for file, text in SOURCES.items():
        for match in re.finditer(r"userNameHtml\(", text):
            head = text.rfind("\n  function ", 0, match.start())
            head = max(head, text.rfind("\n  async function ", 0, match.start()))
            name = re.match(r"\n  (?:async )?function (\w+)", text[head:]).group(1)
            if name != "userNameHtml":
                callers.add((file, name))
    assert callers <= allowed, callers - allowed
    for file, text in SOURCES.items():
        assert not re.search(r"escapeHtml\(shownAccountName\(commit\.author\)", text), file
        assert "escapeHtml(lead.head)}</span>${lead.where" not in text, file


def test_inline_names_never_nest_a_link_and_ssr_matches():
    helper = _function("issuePersonNameHtml")
    assert 'if (!handle || options?.inLink === true) return `<span class="${classes}">${shown}</span>`;' in helper
    assert "repo_ssr_issue.person_name_html(" in SSR and "git=True" in SSR
    for phrase in ("`{ git: true }`", "`{ inLink: true }`", "`linkable`",
                   "data-issue-bot-account"):
        assert phrase in DOCS, phrase
