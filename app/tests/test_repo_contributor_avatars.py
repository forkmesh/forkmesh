import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FRAGMENT = ROOT / "public" / "dashboard" / "js" / "06-repo-content.js"

STUBS = """
let watched = 0;
function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#039;",
  })[char]);
}
function validNodeName(value) { return /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(String(value || "")); }
function watchIssueActorAvatars() { watched += 1; }
"""


def _function(text, name):
    start = text.index("  function %s(" % name)
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


def _avatars(contributors):
    text = FRAGMENT.read_text(encoding="utf-8")
    script = (STUBS + _function(text, "repoContributorAvatar")
              + _function(text, "repoContributorAvatarUrl")
              + "\nconsole.log(JSON.stringify({html: %s.map(repoContributorAvatar), watched}));\n"
              % json.dumps(contributors))
    result = subprocess.run(["node", "--input-type=module", "-e", script],
                            check=True, text=True, capture_output=True)
    return json.loads(result.stdout)


def test_contributors_show_their_avatar_url_over_initials():
    out = _avatars([{"name": "Alice Example", "commits": 3,
                     "avatarUrl": "https://avatars.githubusercontent.com/u/1?s=56&v=4"}])
    html = out["html"][0]
    assert 'src="https://avatars.githubusercontent.com/u/1?s=56&amp;v=4"' in html
    assert 'referrerpolicy="no-referrer"' in html and 'loading="lazy"' in html
    assert 'width="28" height="28"' in html
    assert "<span>AE</span>" in html
    assert "data-repo-contributor-avatar" in html


def test_account_names_hydrate_from_the_instance_and_others_keep_initials():
    out = _avatars([{"name": "AliceDev", "commits": 1}, {"name": "Bob Smith", "commits": 1}])
    assert 'data-issue-actor-avatar="alicedev"' in out["html"][0]
    assert "data-issue-actor-avatar" not in out["html"][1]
    assert "<img" not in out["html"][1] and "<span>BS</span>" in out["html"][1]
    assert out["watched"] == 1


def test_only_plain_https_avatar_urls_are_used():
    out = _avatars([
        {"name": "a b", "avatarUrl": "javascript:alert(1)"},
        {"name": "c d", "avatarUrl": "http://example.test/a.png"},
        {"name": "e f", "avatarUrl": "https://user:pw@example.test/a.png"},
        {"name": "g h", "avatarUrl": 'https://example.test/a.png" onerror="x'},
    ])
    for html in out["html"][:3]:
        assert "<img" not in html
    assert 'onerror="' not in out["html"][3]
    assert "javascript:" not in "".join(out["html"])


def test_broken_contributor_avatars_fall_back_to_initials():
    text = FRAGMENT.read_text(encoding="utf-8")
    render = _function(text, "renderRepoAboutContributors")
    assert "dropBrokenContributorAvatars(list)" in render
    drop = _function(text, "dropBrokenContributorAvatars")
    assert 'addEventListener("error", () => image.remove()' in drop
