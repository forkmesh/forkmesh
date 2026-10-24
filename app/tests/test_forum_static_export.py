import hashlib
import json
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import forum_static_export as fse  # noqa: E402
import forum_static_publish as fsp  # noqa: E402

T0 = 1_700_000_000_000
SHA = "a" * 64
PNG = b"\x89PNG\r\n\x1a\n" + b"\0" * 32
MIRRORED = "https://github.com/user-attachments/assets/0f8fad5b-d9cb-469f-a165-70867728950e"
MISSING = "https://github.com/user-attachments/assets/11111111-2222-3333-4444-555555555555"


def event(kind, eid, ts, author="github:alice", **fields):
    return {"type": kind, "id": eid, "author": "key", "authorName": author, "ts": ts, **fields}


def write_issue(root, number, events, side="open", **record):
    folder = root / ".forkmesh" / "issues" / side / str(number)
    folder.mkdir(parents=True, exist_ok=True)
    doc = {"schema": "forkmesh-issue-v1", "number": number, "createdAt": events[0]["ts"],
           "author": "key", "authorName": events[0]["authorName"], "events": events, **record}
    (folder / ("issue-%d.json" % number)).write_text(json.dumps(doc), encoding="utf-8")


def make_forum(root):
    root.mkdir(parents=True, exist_ok=True)
    (root / "README.md").write_text(
        "# Test BBS\n\nWelcome. ![logo](logo.png) [rules](docs/rules.md)\n", encoding="utf-8")
    (root / "logo.png").write_bytes(PNG)
    write_issue(root, 1, [
        event("open", "o1", T0, title="First thread", body="Hello ![shot](%s) and ![gone](%s)\n\nSee #2 and #99."
              % (MIRRORED, MISSING)),
        event("comment", "c1", T0 + 1000, author="github:bob", body="Reply one by @carol"),
        event("comment", "c2", T0 + 2000, body="to be deleted"),
        event("delete", "d1", T0 + 3000, target="c2"),
        event("comment", "c3", T0 + 4000, body="original"),
        event("edit", "e1", T0 + 5000, target="c3", body="edited text"),
        event("labels", "l1", T0 + 6000, labels=["Iran", "reading group"]),
    ])
    write_issue(root, 2, [
        event("open", "o2", T0 + 10_000, author="github:bob", title="سلام",
              body="<script>alert(1)</script> [x](javascript:alert(1))"),
        event("status", "s2", T0 + 11_000, status="closed"),
    ], side="closed", labels=["Iran"])
    write_issue(root, 3, [
        event("open", "o3", T0 + 20_000, title="Spam", body="spam"),
        {"type": "delete", "id": "d3", "author": "key", "authorName": "github:alice",
         "ts": T0 + 21_000, "target": "self"},
    ])
    attachments = root / ".forkmesh" / "attachments"
    attachments.mkdir(parents=True)
    (attachments / (SHA + ".png")).write_bytes(PNG)
    (attachments / "index.json").write_text(json.dumps({
        "schema": "forkmesh-attachments-v1", "unavailable": {},
        "files": {MIRRORED: {"path": ".forkmesh/attachments/%s.png" % SHA, "type": "image/png",
                             "bytes": len(PNG)}}}), encoding="utf-8")
    contributors = root / ".forkmesh" / "contributors"
    contributors.mkdir(parents=True)
    (contributors / "github-bob.png").write_bytes(PNG)
    return root


def export(source, out, **options):
    options.setdefault("source_url", "https://forum.example/example/forum")
    options.setdefault("site_id", "example/forum")
    src = fse.open_source(source, options.pop("ref", ""))
    try:
        return fse.Exporter(src, out, **options).run()
    finally:
        src.close()


def tree_bytes(folder):
    return {p.relative_to(folder).as_posix(): p.read_bytes()
            for p in sorted(folder.rglob("*")) if p.is_file()}


def git(*args, cwd=None):
    return subprocess.run(["git", "-c", "user.email=t@example.com", "-c", "user.name=t", *args],
                          cwd=cwd, capture_output=True, text=True, check=True).stdout.strip()


def test_fold_applies_edits_deletes_labels_and_status(tmp_path):
    forum = make_forum(tmp_path / "forum")
    src = fse.open_source(forum)
    threads, _ = fse.load_threads(src)
    by = {t["number"]: t for t in threads}
    assert sorted(by) == [1, 2]
    first = by[1]
    assert [c["body"] for c in first["comments"]] == ["Reply one by @carol", "edited text"]
    assert first["labels"] == ["Iran", "reading group"]
    assert by[2]["status"] == "closed"


def committed_forum(root):
    make_forum(root)
    git("init", "-q", str(root))
    git("add", "-A", cwd=root)
    git("commit", "-qm", "forum", cwd=root)
    return root


def test_the_site_is_static_relative_and_never_embeds_github_images(tmp_path):
    out = tmp_path / "site"
    result = export(committed_forum(tmp_path / "forum"), out)
    assert result["threads"] == 2 and result["comments"] == 2
    files = tree_bytes(out)
    for name in ("index.html", "all.html", "issues/1.html", "issues/2.html", "labels/iran.html",
                 "labels/reading-group.html", "feed.atom", "style.css", ".nojekyll", "manifest.json",
                 "files/%s.png" % SHA, "avatars/github-bob.png", "repo/logo.png"):
        assert name in files, name
    assert "issues/3.html" not in files
    for name, data in files.items():
        if name.endswith(".html"):
            text = data.decode()
            assert "<script" not in text.lower()
            assert 'href="/' not in text and 'src="/' not in text
            assert 'src="http' not in text
            assert "javascript:" not in text
    thread = files["issues/1.html"].decode()
    assert '<img src="../files/%s.png"' % SHA in thread
    assert '<a href="%s" rel="nofollow ugc noreferrer">gone (image on GitHub)</a>' % MISSING in thread
    assert '<a href="../issues/2.html">#2</a>' in thread
    assert 'href="https://forum.example/example/forum/issues/99"' in thread
    assert 'href="https://forum.example/@carol"' in thread
    assert "to be deleted" not in thread and "edited text" in thread
    assert 'src="../avatars/github-bob.png"' in thread
    assert 'href="../style.css"' in thread
    assert 'dir="auto"' in files["issues/2.html"].decode()
    assert "alert(1)" in files["issues/2.html"].decode()
    index = files["index.html"].decode()
    assert '<img src="repo/logo.png"' in index
    assert 'href="https://forum.example/example/forum/blob/docs/rules.md"' in index
    assert 'id="test-bbs"' in index and "<title>Test BBS</title>" in index
    manifest = json.loads(files["manifest.json"])
    assert manifest["files"]["issues/1.html"]["sha256"] == hashlib.sha256(files["issues/1.html"]).hexdigest()
    assert set(manifest["files"]) == set(files) - {"manifest.json"}
    assert fse.verify(out) == []


def test_the_feed_is_atom_with_threaded_replies(tmp_path):
    out = tmp_path / "site"
    export(make_forum(tmp_path / "forum"), out, base_url="https://mirror.example/forum")
    root = ET.fromstring((out / "feed.atom").read_bytes())
    ns = {"a": "http://www.w3.org/2005/Atom", "thr": "http://purl.org/syndication/thread/1.0"}
    assert root.get("{http://www.w3.org/XML/1998/namespace}base") == "https://mirror.example/forum/"
    entries = root.findall("a:entry", ns)
    assert len(entries) == 4
    replies = [e for e in entries if e.find("thr:in-reply-to", ns) is not None]
    assert len(replies) == 2
    parent = replies[0].find("thr:in-reply-to", ns)
    assert parent.get("ref") == "tag:forkmesh.com,2026:static/example/forum/issue/1"
    assert parent.get("href") == "issues/1.html"
    stamps = [e.find("a:updated", ns).text for e in entries]
    assert stamps == sorted(stamps, reverse=True)
    assert root.find("a:updated", ns).text == stamps[0]


def test_output_is_deterministic_and_incremental(tmp_path):
    forum = make_forum(tmp_path / "forum")
    first = export(forum, tmp_path / "a")
    export(forum, tmp_path / "b")
    assert tree_bytes(tmp_path / "a") == tree_bytes(tmp_path / "b")
    assert first["rendered"] == 2
    again = export(forum, tmp_path / "a")
    assert again["rendered"] == 0 and again["reused"] == 2 and again["written"] == 0
    assert tree_bytes(tmp_path / "a") == tree_bytes(tmp_path / "b")
    path = forum / ".forkmesh/issues/closed/2/issue-2.json"
    doc = json.loads(path.read_text())
    doc["events"].append(event("comment", "c9", T0 + 30_000, body="new reply"))
    path.write_text(json.dumps(doc))
    changed = export(forum, tmp_path / "a")
    assert changed["rendered"] == 1 and changed["reused"] == 1
    assert "new reply" in (tmp_path / "a/issues/2.html").read_text()
    assert (tmp_path / ("a/files/%s.png" % SHA)).is_file()
    assert fse.verify(tmp_path / "a") == []
    path.unlink()
    gone = export(forum, tmp_path / "a")
    assert gone["removed"] >= 1
    assert not (tmp_path / "a/issues/2.html").exists()
    assert "issues/2.html" not in (tmp_path / "a/labels/iran.html").read_text()
    assert fse.verify(tmp_path / "a") == []


def test_a_bare_repository_and_ref_gives_the_same_site(tmp_path):
    forum = make_forum(tmp_path / "forum")
    git("init", "-q", "-b", "main", cwd=forum)
    git("add", "-A", cwd=forum)
    git("commit", "-q", "-m", "forum", cwd=forum)
    commit = git("rev-parse", "HEAD", cwd=forum)
    git("clone", "-q", "--bare", str(forum), str(tmp_path / "bare.git"))
    from_checkout = export(forum, tmp_path / "a")
    from_bare = export(tmp_path / "bare.git", tmp_path / "b", ref="main")
    assert from_checkout["commit"] == from_bare["commit"] == commit
    assert tree_bytes(tmp_path / "a") == tree_bytes(tmp_path / "b")


def test_the_fallback_renderer_is_safe_and_small(tmp_path):
    out = tmp_path / "site"
    export(make_forum(tmp_path / "forum"), out, renderer=None)
    text = (out / "issues/2.html").read_text()
    assert "<script" not in text and "javascript:" not in text
    thread = (out / "issues/1.html").read_text()
    assert '<img src="../files/%s.png"' % SHA in thread
    assert "[image: gone]" in thread


def test_label_slugs_are_unique_and_ascii():
    slugs = fse.label_slugs(["Iran", "iran", "reading group", "中国"])
    assert len(set(slugs.values())) == 4
    assert slugs["reading group"] == "reading-group"
    assert all(s.isascii() for s in slugs.values())


def test_the_tarball_is_reproducible(tmp_path):
    out = tmp_path / "site"
    export(make_forum(tmp_path / "forum"), out)
    fse.write_tarball(out, tmp_path / "one.tar.gz")
    fse.write_tarball(out, tmp_path / "two.tar.gz")
    assert (tmp_path / "one.tar.gz").read_bytes() == (tmp_path / "two.tar.gz").read_bytes()


def test_publish_commits_on_top_and_never_rewrites(tmp_path):
    forum = make_forum(tmp_path / "forum")
    site = tmp_path / "site"
    export(forum, site)
    remote = tmp_path / "pages.git"
    git("init", "-q", "--bare", str(remote))
    spec = ["--site", str(site), "--target", "pages=%s#gh-pages" % remote, "--cache", str(tmp_path / "cache")]
    assert fsp.main(spec + ["--dry-run"]) == 0
    assert git("--git-dir", str(remote), "branch", "--list") == ""
    assert fsp.main(spec) == 0
    first = git("--git-dir", str(remote), "rev-parse", "gh-pages")
    assert fsp.main(spec) == 0
    assert git("--git-dir", str(remote), "rev-parse", "gh-pages") == first
    path = forum / ".forkmesh/issues/open/1/issue-1.json"
    doc = json.loads(path.read_text())
    doc["events"].append(event("comment", "c8", T0 + 40_000, body="later"))
    path.write_text(json.dumps(doc))
    export(forum, site)
    assert fsp.main(spec) == 0
    second = git("--git-dir", str(remote), "rev-parse", "gh-pages")
    assert git("--git-dir", str(remote), "rev-parse", second + "^") == first
    names = git("--git-dir", str(remote), "ls-tree", "-r", "--name-only", "gh-pages").split("\n")
    assert set(names) == set(json.loads((site / "manifest.json").read_text())["files"]) | {"manifest.json"}
    assert ".nojekyll" in names and "issues/1.html" in names


def test_publish_refuses_a_branch_it_did_not_make_and_keeps_the_pages_domain(tmp_path):
    site = tmp_path / "site"
    export(make_forum(tmp_path / "forum"), site)
    remote = tmp_path / "code.git"
    work = tmp_path / "work"
    git("init", "-q", "-b", "main", str(work))
    (work / "app.py").write_text("print(1)\n")
    (work / "CNAME").write_text("forum.example.org\n")
    git("add", "-A", cwd=work)
    git("commit", "-qm", "code", cwd=work)
    git("clone", "-q", "--bare", str(work), str(remote))
    before = git("--git-dir", str(remote), "rev-parse", "main")
    spec = ["--site", str(site), "--target", "pages=%s#main" % remote, "--cache", str(tmp_path / "cache")]
    assert fsp.main(spec) == 1
    assert git("--git-dir", str(remote), "rev-parse", "main") == before
    assert fsp.main(spec + ["--init"]) == 0
    names = git("--git-dir", str(remote), "ls-tree", "-r", "--name-only", "main").split("\n")
    assert "CNAME" in names and "app.py" not in names and "manifest.json" in names
    assert fsp.main(spec) == 0
    with pytest.raises(fsp.PublishError):
        fsp.publish_target(site, {"name": "x", "url": "--upload-pack=touch /tmp/x", "branch": "b"},
                           tmp_path / "cache", dry_run=True, message="m", author=("a", "a@b"),
                           environ={})


def test_publish_credentials_stay_out_of_the_command_line(tmp_path):
    token_file = tmp_path / "token"
    token_file.write_text("ghp_secretvalue\n")
    target = {"name": "github", "url": "https://github.com/example/forum-mirror.git",
              "branch": "gh-pages", "tokenFile": str(token_file)}
    env, secrets = fsp.credentials(target, {})
    assert "ghp_secretvalue" in secrets
    keys = {env["GIT_CONFIG_KEY_%d" % i]: env["GIT_CONFIG_VALUE_%d" % i]
            for i in range(int(env["GIT_CONFIG_COUNT"]))}
    header = keys["http.https://github.com/.extraHeader"]
    assert header.startswith("Authorization: Basic ")
    assert env["GIT_TERMINAL_PROMPT"] == "0"
    env2, _ = fsp.credentials(target, {"FORKMESH_PUBLISH_TOKEN_GITHUB": "other"})
    assert env2 != env
    with pytest.raises(fsp.PublishError):
        fsp.load_targets(None, ["bad=https://u:p@github.com/x/y.git#gh-pages"])
    with pytest.raises(fsp.PublishError):
        fsp.credentials({"name": "x", "url": "git@github.com:x/y.git", "tokenFile": str(token_file)}, {})
    source = (ROOT / "tools" / "forum_static_publish.py").read_text()
    assert "--force" not in source and "+refs/heads/%s:%s" in source


def test_a_post_can_only_pull_committed_files_into_the_site(tmp_path):
    forum = committed_forum(tmp_path / "forum")
    secret = tmp_path / "outside.txt"
    secret.write_text("HOSTSECRET\n", encoding="utf-8")
    (forum / "hostlink").symlink_to(secret)
    (forum / "tracked-link").symlink_to(secret)
    git("add", "tracked-link", cwd=forum)
    git("commit", "-qm", "link", cwd=forum)
    write_issue(forum, 4, [event("open", "o4", T0 + 30_000, title="Leak",
                                 body="![a](.env) ![b](hostlink) ![c](tracked-link) ![d](logo.png)")])
    git("add", "-A", cwd=forum)
    git("commit", "-qm", "issue", cwd=forum)
    (forum / ".env").write_text("TOPSECRET\n", encoding="utf-8")
    for ref in ("", "HEAD"):
        out = tmp_path / ("site" + ref)
        export(forum, out, ref=ref)
        files = tree_bytes(out)
        assert "repo/logo.png" in files
        for name in ("repo/.env", "repo/hostlink", "repo/tracked-link"):
            assert name not in files, (ref, name)
        assert not any(b"SECRET" in data for data in files.values())
