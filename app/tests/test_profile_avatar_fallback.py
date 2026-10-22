import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
PROFILE_IO = (JS / "03-issues-profile-io.js").read_text(encoding="utf-8")
HELPERS = (JS / "02-helpers.js").read_text(encoding="utf-8")
EXPLORER = (JS / "05-repo-list-explorer.js").read_text(encoding="utf-8")
BUNDLE = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
DOC = (ROOT.parent / "docs" / "app" / "profile-avatar.md").read_text(encoding="utf-8")


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


def _node(body):
    script = """
const state = { repositories: [] };
const lookups = [];
const rosterReads = [];
const accounts = { alice: "iVBORw0KGgo=" };
const rosters = { "example/project": { exampleuser: "/raw/example/project/exampleuser.png" } };
function validNodeName(value) { return /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(String(value || "")); }
function platformName() { return "Example Code"; }
function repoPublicOwner(repo) { return repo.owner; }
function repositoryViewKey(repo) { return `${repo.owner}/${repo.name}`.toLowerCase(); }
function repoForkParent(repo) { const v = String(repo?.forkedFrom || "").toLowerCase(); if (!v.includes("/")) return null; const [owner, name] = v.split("/"); return { owner, name, key: v }; }
function findRepository(key) { return state.repositories.find((r) => `${r.owner}/${r.name}`.toLowerCase() === String(key).toLowerCase()) || null; }
async function issueActorAccount(name) {
  lookups.push(name);
  return { exists: true, png: accounts[name] || "" };
}
async function repoPeopleRosterMatch(repo, handle) {
  rosterReads.push(`${repo.owner}/${repo.name}`);
  const url = rosters[`${repo.owner}/${repo.name}`]?.[handle];
  return url ? { avatarUrl: url } : null;
}
function classList(owner) {
  return {
    set: new Set(owner.initialClasses || []),
    add(name) { this.set.add(name); }, remove(name) { this.set.delete(name); },
    toggle(name, on) { if (on) this.set.add(name); else this.set.delete(name); },
    contains(name) { return this.set.has(name); },
  };
}
function avatarContainer() {
  const image = { attrs: {}, hidden: true, alt: "", listeners: {},
    set src(value) { this.attrs.src = value; }, get src() { return this.attrs.src || ""; },
    removeAttribute(name) { delete this.attrs[name]; },
    addEventListener(name, fn) { this.listeners[name] = fn; } };
  image.classList = classList({ initialClasses: ["hidden"] });
  const initial = { textContent: "" };
  initial.classList = classList({});
  initial.innerHTML = "";
  const container = { dataset: {}, image, initial,
    querySelector(selector) { return selector === "img" ? image : selector === "[data-avatar-initial]" ? initial : null; } };
  container.classList = classList({});
  return container;
}
const containers = [];
globalThis.document = { querySelectorAll(selector) {
  return selector === "[data-profile-avatar-for]" ? containers.filter((c) => c.dataset.profileAvatarFor) : [];
} };
const settle = () => new Promise((resolve) => setTimeout(resolve, 0));
const view = (c) => ({ src: c.image.attrs.src ?? null, imageHidden: c.image.hidden || c.image.classList.contains("hidden"),
  initialHidden: c.initial.classList.contains("hidden"), initial: c.initial.textContent, state: c.dataset.profileAvatarState || "" });
%s
%s
""" % ("\n".join(_function(PROFILE_IO, name) for name in (
        "applyAvatar", "profileAvatarHandle", "profileAvatarRosterRepos", "profileAvatarPicture",
        "showProfileAvatarPicture", "showProfileAvatarBot", "hydrateProfileAvatar",
        "hydrateProfileAvatars"))
       + "\n" + "\n".join(_function(HELPERS, name) for name in ("botAvatarKind", "botAvatarSvg"))
       + "\n" + HELPERS[HELPERS.index("  const BOT_AVATAR_GLYPHS = {"):
                        HELPERS.index("\n  };\n", HELPERS.index("  const BOT_AVATAR_GLYPHS = {")) + 5]
       + "\nconst PROFILE_AVATAR_ROSTER_REPOS = 4;", body)
    result = subprocess.run(["node", "--input-type=module"], input=script,
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stderr[-2000:]
    return json.loads(result.stdout)


def test_an_account_without_an_uploaded_avatar_gets_the_roster_picture():
    out = _node("""
state.repositories = [
  { owner: "someone", name: "other" },
  { owner: "ExampleUser", name: "fork" },
  { owner: "example", name: "project" },
  { owner: "example", name: "secret", isPrivate: true },
];
const page = avatarContainer();
const header = avatarContainer();
containers.push(page, header);
applyAvatar(page, { nodeName: "ExampleUser", avatarPng: "" });
applyAvatar(header, { nodeName: "exampleuser", avatarPng: "" });
const before = view(page);
await settle(); await settle();
console.log(JSON.stringify({ before, page: view(page), header: view(header), lookups, rosterReads }));
""")
    assert out["before"]["src"] is None and out["before"]["imageHidden"] is True
    assert out["before"]["initialHidden"] is False and out["before"]["initial"] == "E"
    for key in ("page", "header"):
        assert out[key]["src"] == "/raw/example/project/exampleuser.png"
        assert out[key]["imageHidden"] is False and out[key]["initialHidden"] is True
        assert out[key]["state"] == "image"
    assert set(out["lookups"]) == {"exampleuser"}
    assert list(dict.fromkeys(out["rosterReads"])) == ["ExampleUser/fork", "someone/other", "example/project"]
    assert "example/secret" not in out["rosterReads"]


def test_an_uploaded_avatar_comes_from_the_account_when_the_session_has_none():
    out = _node("""
const page = avatarContainer();
containers.push(page);
applyAvatar(page, { nodeName: "Alice", avatarPng: "" });
await settle(); await settle();
const resolved = view(page);
applyAvatar(page, { nodeName: "alice", avatarPng: "c2Vzc2lvbg==" });
console.log(JSON.stringify({ resolved, fromSession: view(page), lookups }));
""")
    assert out["resolved"]["src"] == "data:image/png;base64,iVBORw0KGgo="
    assert out["resolved"]["imageHidden"] is False
    assert out["fromSession"]["src"] == "data:image/png;base64,c2Vzc2lvbg=="
    assert out["fromSession"]["state"] == ""
    assert out["lookups"] == ["alice"]


def test_no_picture_anywhere_keeps_initials_and_never_an_empty_src_then_retries_with_the_catalog():
    out = _node("""
const page = avatarContainer();
containers.push(page);
applyAvatar(page, { nodeName: "exampleuser", avatarPng: "" });
await settle(); await settle();
const empty = view(page);
hydrateProfileAvatars();
await settle();
const unchanged = rosterReads.length;
state.repositories = [{ owner: "example", name: "project" }];
hydrateProfileAvatars();
await settle(); await settle();
const org = avatarContainer();
applyAvatar(org, { nodeName: "example", kind: "organization", avatarPng: "" });
console.log(JSON.stringify({ empty, unchanged, later: view(page), org: org.dataset }));
""")
    assert out["empty"] == {"src": None, "imageHidden": True, "initialHidden": False,
                            "initial": "E", "state": "initial"}
    assert out["unchanged"] == 0
    assert out["later"]["src"] == "/raw/example/project/exampleuser.png"
    assert out["org"] == {}


def test_a_system_account_without_a_picture_gets_its_glyph_and_an_upload_wins():
    out = _node("""
accounts["upload-actions"] = "iVBORw0KGgo=";
const bot = avatarContainer();
const uploaded = avatarContainer();
containers.push(bot, uploaded);
applyAvatar(bot, { nodeName: "example-actions", avatarPng: "" });
applyAvatar(uploaded, { nodeName: "upload-actions", avatarPng: "" });
await settle(); await settle();
console.log(JSON.stringify({ bot: view(bot), glyph: bot.initial.innerHTML, uploaded: view(uploaded) }));
""")
    assert out["bot"]["state"] == "bot" and out["bot"]["src"] is None
    assert '<svg data-bot-avatar="actions"' in out["glyph"]
    assert out["uploaded"]["src"] == "data:image/png;base64,iVBORw0KGgo="


def test_wiring_bundle_and_docs():
    assert "hydrateProfileAvatars();" in _function(EXPLORER, "renderProfileRepositories")
    assert "function hydrateProfileAvatar(" in BUNDLE
    for needle in ("repoPeopleRosterMatch", "issueActorAccount", ".forkmesh/contributors",
                   "PROFILE_AVATAR_ROSTER_REPOS"):
        assert needle in DOC


def test_a_fork_owner_gets_the_picture_from_the_fork_parent_roster():
    out = _node("""
state.repositories = [
  { owner: "a", name: "one" }, { owner: "b", name: "two" }, { owner: "c", name: "three" },
  { owner: "d", name: "four" },
  { owner: "exampleuser", name: "project", forkedFrom: "example/project" },
  { owner: "example", name: "project" },
];
console.log(JSON.stringify(profileAvatarRosterRepos("exampleuser").map((r) => `${r.owner}/${r.name}`)));
""".replace("console.log(JSON.stringify(", "console.log(JSON.stringify(") )
    assert out[:2] == ["exampleuser/project", "example/project"]

