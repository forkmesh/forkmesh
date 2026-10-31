import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "host"))

import bot_avatars  # noqa: E402
from forkmesh_host import repo_ssr_issue  # noqa: E402
from test_account_avatar_world_roster import TINY_PNG, Harness, _records  # noqa: E402

JS = ROOT / "public" / "dashboard" / "js"
HELPERS = (JS / "02-helpers.js").read_text(encoding="utf-8")
CONTENT = (JS / "06-repo-content.js").read_text(encoding="utf-8")
ACTIONS = (JS / "06-repo-actions.js").read_text(encoding="utf-8")
NETWORK = (JS / "08-repo-detail-network.js").read_text(encoding="utf-8")
BUNDLE = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
DOC = (ROOT.parent / "docs" / "app" / "bot-avatars.md").read_text(encoding="utf-8")

CASES = [
    ("example-actions", "", "actions"),
    ("example-actions[bot]", "", "actions"),
    ("github-actions", "", "actions"),
    ("github-actions[bot]", "", "actions"),
    ("ForkMesh Deploy", "", "actions"),
    ("Release Robot", "actions@forkmesh.local", "actions"),
    ("tasks[bot]", "", "bot"),
    ("dependabot", "", "bot"),
    ("triage-bot", "", "bot"),
    ("Helper", "helper[bot]@users.noreply.example.com", "bot"),
    ("build-runner", "", "runner"),
    ("Docs Runner", "", "runner"),
    ("AI Review", "", "ai"),
    ("ForkBot", "", "ai"),
    ("ForkMesh", "", "node"),
    ("ForkMesh mirror", "", "node"),
    ("ForkMesh Mirror Reconciler", "", "node"),
    ("alice", "", ""),
    ("robotics-fan", "", ""),
    ("actionsman", "", ""),
    ("frontrunner", "", ""),
    ("", "", ""),
]


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


def _glyph_block():
    start = HELPERS.index("  const BOT_AVATAR_GLYPHS = {")
    return HELPERS[start:HELPERS.index("\n  };\n", start) + 5]


def _node(body, platform="Example Code", git_node="examplenode"):
    script = """
function platformName() { return %s; }
function platformAccount(name) { return String(name || "").trim().toLowerCase() === %s; }
const state = { session: null };
function watchIssueActorAvatars() {}
function validNodeName(value) { return /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(String(value || "")); }
function publicProfileUrl(name) { return `/@${name}`; }
function repoContributorAvatarUrl() { return ""; }
const ISSUE_PERSON_HANDLE_RE = /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?$/;
%s
%s
%s
""" % (json.dumps(platform), json.dumps(git_node), _glyph_block(),
       "\n".join(line for line in HELPERS.splitlines()
                 if line.startswith(("  const FORGE_PROFILES", "  const FORGE_IDENTITY_RE")))
       + "\n" + "\n".join(_function(HELPERS, name) for name in (
           "escapeHtml", "botAvatarKind", "botAvatarSvg", "issueActorIsBot", "issuePersonHandle",
           "issueActorPixels", "issuePersonProfileHref", "forgeIdentity", "forgeLinkAttrs",
           "issueActorAvatarHtml"))
       + "\n" + "\n".join(_function(CONTENT, name) for name in (
           "repoContributorIsBot", "repoContributorAvatar")),
       body)
    result = subprocess.run(["node", "--input-type=module"], input=script,
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stderr[-2000:]
    return json.loads(result.stdout)


def test_server_kinds():
    for name, email, kind in CASES:
        assert bot_avatars.bot_avatar_kind(name, email) == kind, (name, email)


def test_dashboard_kinds_match_the_server_and_know_the_instance():
    out = _node("console.log(JSON.stringify(%s.map(([n, e]) => botAvatarKind(n, e))));"
                % json.dumps([[n, e] for n, e, _ in CASES]))
    assert out == [kind for _, _, kind in CASES]
    extra = _node("""console.log(JSON.stringify([
  botAvatarKind("Example Code"), botAvatarKind("Example Code mirror"),
  botAvatarKind("Example Code Actions"), botAvatarKind("ExampleNode"), botAvatarKind("someone"),
]));""")
    assert extra == ["node", "node", "actions", "node", ""]


def test_server_rendered_issue_kinds_match_for_names_without_email():
    for name, email, kind in CASES:
        if not email:
            assert repo_ssr_issue.bot_avatar_kind(name) == kind, name


def test_glyphs_are_the_same_everywhere_and_cover_each_kind():
    js = dict(re.findall(r'^    (\w+): ("(?:[^"\\]|\\.)*"),$', _glyph_block(), re.M))
    assert {key: json.loads(value) for key, value in js.items()} == bot_avatars.GLYPHS
    assert repo_ssr_issue.BOT_AVATAR_GLYPHS == bot_avatars.GLYPHS
    assert set(bot_avatars.GLYPHS) == {"actions", "node", "runner", "ai", "bot"}
    for kind in bot_avatars.KINDS:
        root = ET.fromstring(bot_avatars.bot_avatar_svg(kind))
        assert root.tag.endswith("svg") and root.get("viewBox") == "0 0 24 24"
        ET.fromstring(repo_ssr_issue.bot_avatar_svg(kind))


def test_dashboard_avatars_use_the_glyphs_and_allow_an_account_override():
    out = _node("""console.log(JSON.stringify({
  actions: issueActorAvatarHtml("example-actions", "", "h-10 w-10"),
  tasks: issueActorAvatarHtml("tasks[bot]", "", "h-5 w-5"),
  agent: issueActorAvatarHtml("codex", "", "h-5 w-5", true),
  review: issueActorAvatarHtml("ForkBot", "", "h-10 w-10", true),
  runner: issueActorAvatarHtml("build-runner", "", "h-6 w-6"),
  person: issueActorAvatarHtml("alice", "", "h-6 w-6"),
  contributorBot: repoContributorAvatar({ name: "github-actions[bot]", commits: 3 }),
  contributorNode: repoContributorAvatar({ name: "examplenode", commits: 9 }),
  contributorPerson: repoContributorAvatar({ name: "Alice Example", commits: 2 }),
}));""")
    assert '<svg data-bot-avatar="actions"' in out["actions"]
    assert 'data-issue-actor-avatar="example-actions"' in out["actions"]
    assert "data-issue-person" not in out["actions"] and "href" not in out["actions"]
    assert '<svg data-bot-avatar="bot"' in out["tasks"] and "data-issue-actor-avatar" not in out["tasks"]
    assert '<svg data-bot-avatar="ai"' in out["agent"] and "data-issue-actor-avatar" not in out["agent"]
    assert '<svg data-bot-avatar="ai"' in out["review"]
    assert '<svg data-bot-avatar="runner"' in out["runner"]
    assert "<svg" not in out["person"] and 'data-issue-actor-avatar="alice"' in out["person"]
    assert '<svg data-bot-avatar="actions"' in out["contributorBot"]
    assert '<svg data-bot-avatar="node"' in out["contributorNode"]
    assert 'data-issue-actor-avatar="examplenode"' in out["contributorNode"]
    assert "<svg" not in out["contributorPerson"] and "<span>AE</span>" in out["contributorPerson"]
    for html in (out["actions"], out["contributorBot"]):
        assert "text-muted-foreground" in html and "stroke=\"currentColor\"" in html


def test_avatar_endpoint_serves_a_glyph_for_system_names_only():
    records = _records(TINY_PNG)
    records["example-actions"] = {"name": "example-actions", "kind": "user",
                                  "status": "active", "created_at": 7,
                                  "avatar_png": TINY_PNG}
    harness = Harness(records)
    for name, kind in (("ci-actions", "actions"), ("tasks%5Bbot%5D", "bot"),
                       ("build-runner", "runner"), ("amy-bot", "bot")):
        response = harness.avatar(name)
        assert response.status == 200, name
        assert response.headers["content-type"] == "image/svg+xml"
        assert response.headers["x-content-type-options"] == "nosniff"
        assert "default-src 'none'" in response.headers["content-security-policy"]
        assert response.body.decode() == bot_avatars.bot_avatar_svg(kind)
    override = harness.avatar("example-actions")
    assert override.status == 200 and override.headers["content-type"] == "image/png"
    assert harness.avatar("nobody").status == 404
    assert harness.avatar("amy").status == 404


def test_other_surfaces_use_the_helpers_and_the_bundle_is_built():
    assert 'issueActorAvatarHtml(run.actor, run.actor, "h-4 w-4", false)' in ACTIONS
    assert 'issueActorAvatarHtml(run.actor, run.actor, "h-4 w-4", false, false)' not in ACTIONS
    assert "botAvatarSvg(botAvatarKind(lead.actor" in NETWORK
    assert 'avatar.innerHTML = botAvatarSvg(botKind, "h-3.5 w-3.5")' in CONTENT
    assert "function botAvatarKind(" in BUNDLE and "const BOT_AVATAR_GLYPHS = {" in BUNDLE
    for needle in ("botAvatarKind", "bot_avatars.py", "/api/accounts/<name>/avatar",
                   "## Override"):
        assert needle in DOC
