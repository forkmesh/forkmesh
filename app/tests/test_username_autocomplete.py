import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
PUBLIC = ROOT / "public"
SCRIPT = PUBLIC / "username-autocomplete.js"
CHAT_HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
CHAT_JS = (PUBLIC / "chat.js").read_text(encoding="utf-8")
DASHBOARD_CHAT = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
SHELL = (PUBLIC / "dashboard" / "shell.html").read_text(encoding="utf-8")
JS = PUBLIC / "dashboard" / "js"

_HARNESS = r"""
const fs = require("fs");
const vm = require("vm");
const fetched = [];
const storage = { "forkmesh.session": JSON.stringify({ nodeName: "Me", sessionToken: "cookie" }) };
const directory = ["me", "alice", "alina", "malia", "bob", "ali", "bad name"];
const sandbox = {
  console,
  Promise,
  Set,
  Map,
  JSON,
  addEventListener() {},
  localStorage: { getItem: (key) => storage[key] ?? null },
  document: { addEventListener() {}, getElementById() { return null; } },
  fetch: async (url, options) => {
    fetched.push({ url, auth: options.headers.authorization });
    const query = decodeURIComponent(url.split("query=")[1]);
    return { ok: true, json: async () => ({ users: directory.filter((name) => name.includes(query)).map((name) => ({ name })) }) };
  },
};
sandbox.window = sandbox;
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(process.argv[1], "utf8"), sandbox);
const U = sandbox.ForkMeshUsernames;
(async () => {
%s
})().catch((error) => { console.error(error); process.exit(1); });
"""


def _node(body):
    result = subprocess.run(
        ["node", "-e", _HARNESS % body, str(SCRIPT)],
        capture_output=True, text=True, check=True, timeout=30,
    )
    return json.loads(result.stdout.strip().splitlines()[-1])


def test_list_fields_complete_the_name_under_the_caret():
    out = _node("""
const value = "alice, bo, carol";
const token = U.tokenAt(value, 9, "list");
console.log(JSON.stringify({
  token,
  inserted: U.insertName(value, token, "bob", "list"),
  tail: U.insertName("alice,al", U.tokenAt("alice,al", 8, "list"), "alina", "list"),
  single: U.insertName("@Al", U.tokenAt("@Al", 3, "single"), "alice", "single"),
  singleToken: U.tokenAt("@Al", 3, "single"),
}));
""")
    assert out["token"] == {"start": 6, "end": 9, "query": "bo"}
    assert out["inserted"] == {"value": "alice, bob, carol", "caret": len("alice, bob, ")}
    assert out["tail"] == {"value": "alice, alina, ", "caret": len("alice, alina, ")}
    assert out["single"] == {"value": "alice", "caret": 5}
    assert out["singleToken"]["query"] == "al"


def test_ranking_puts_prefix_matches_first_and_skips_excluded_names():
    out = _node("""
console.log(JSON.stringify([
  U.rank(["malia", "alina", "alice", "ali", "bob"], "AL", ["alice"]),
  U.rank(Array.from({ length: 20 }, (_, i) => "al" + i), "al").length,
]));
""")
    assert out == [["ali", "alina", "malia"], 8]


def test_lookup_searches_accounts_once_and_never_suggests_yourself():
    out = _node("""
U.remember(["alina"]);
const first = U.lookup("al", null);
const later = await new Promise((resolve) => U.lookup("al", resolve, ["alice"]));
let again = 0;
U.lookup("al", () => { again += 1; });
await U.search("al");
console.log(JSON.stringify({
  first, later, again, fetched,
  me: U.lookup("me", null),
  blank: U.lookup("  ", null),
}));
""")
    assert out["first"] == ["alina"]
    assert out["later"] == ["ali", "alina", "malia"]
    assert out["again"] == 0
    assert out["fetched"] == [
        {"url": "/api/chat/direct-messages/users?query=al", "auth": "Bearer cookie"},
    ]
    assert out["me"] == []
    assert out["blank"] == []


def test_every_page_with_a_username_field_loads_the_picker():
    tag = '<script src="/username-autocomplete.js?v=${version}" defer></script>'
    assert tag in CHAT_HTML and tag in SHELL
    assert '"username-autocomplete.js",' in (ROOT / "src" / "dashboard_shell.py").read_text(encoding="utf-8")
    assert '"/username-autocomplete.js",' in (REPO / "www" / "worker.js").read_text(encoding="utf-8")
    for page in (PUBLIC / "dashboard").rglob("index.html"):
        text = page.read_text(encoding="utf-8")
        if "/dashboard.js?v=" in text:
            assert "/username-autocomplete.js?v=" in text, page


def test_channel_invites_and_new_channels_autocomplete_usernames():
    assert 'id="chat-channel-username"' in CHAT_HTML
    invite = CHAT_HTML[CHAT_HTML.index('<input id="chat-channel-username"'):]
    assert 'data-username-autocomplete="single"' in invite[:invite.index("/>")]
    create = CHAT_HTML[CHAT_HTML.index('<input id="chat-channel-initial-members"'):]
    assert 'data-username-autocomplete="list"' in create[:create.index("/>")]
    assert "channelUsernameInput.dataset.usernameExclude" in CHAT_JS
    assert "window.ForkMeshUsernames?.remember(registeredUsers.keys())" in CHAT_JS


def test_dashboard_username_fields_and_mentions_use_the_account_search():
    compose = (JS / "07-repo-compose-branch.js").read_text(encoding="utf-8")
    security = (JS / "07-repo-security.js").read_text(encoding="utf-8")
    content = (JS / "06-repo-content.js").read_text(encoding="utf-8")
    assert '<input data-repo-collaborator-name data-username-autocomplete="single"' in compose
    assert 'field.dataset.usernameExclude = names.join(",")' in compose
    assert security.count('name="user" data-username-autocomplete="single"') == 2
    assert "window.ForkMeshUsernames?.lookup(query" in content
    assert "mentionCandidatesFor(input, found.query)" in content
    for source in (CHAT_JS, DASHBOARD_CHAT):
        assert "window.ForkMeshUsernames?.lookup(partial" in source
