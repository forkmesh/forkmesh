import importlib.util
import json
import shutil
import subprocess
from pathlib import Path

import pytest

import changelog
import changelog_draft
from changelog_draft import Commit

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "www" / "public" / "changelog.js"


def load_stamp():
    spec = importlib.util.spec_from_file_location("changelog_stamp", ROOT / "native" / "ios" / "tools" / "changelog_stamp.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def repo(tmp_path, monkeypatch):
    for key in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR"):
        monkeypatch.delenv(key, raising=False)
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", str(tmp_path / "gitconfig"))
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")
    for role in ("AUTHOR", "COMMITTER"):
        monkeypatch.setenv(f"GIT_{role}_NAME", "Test")
        monkeypatch.setenv(f"GIT_{role}_EMAIL", "test@example.com")
    path = tmp_path / "repo"
    subprocess.run(["git", "init", "-q", "-b", "main", str(path)], check=True)
    return path


def commit(repo: Path, subject: str, *files: str, when: int = 1_790_000_000) -> str:
    for name in files:
        target = repo / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(target.read_text() + "x" if target.exists() else "x")
    subprocess.run(["git", "-C", str(repo), "add", "-A"], check=True)
    env_date = f"@{when} +0000"
    subprocess.run(["git", "-C", str(repo), "commit", "-q", "--allow-empty", "-m", subject],
                   check=True, env={**__import__("os").environ, "GIT_AUTHOR_DATE": env_date,
                                    "GIT_COMMITTER_DATE": env_date})
    return subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], capture_output=True,
                          text=True, check=True).stdout.strip()


def items(sections):
    return {section["title"]: section["items"] for section in sections}


def notes(date, *sections, version=""):
    return [{"date": date, "version": version, "area": area, "text": text} for area, lines in sections for text in lines]


def node_changelog(script: str, *args: str) -> str:
    if not shutil.which("node"):
        pytest.skip("node is not installed")
    source = f"const changelog = require({json.dumps(str(SCRIPT))});\n{script}"
    return subprocess.run(["node", "-e", source, "--", *args], capture_output=True, text=True, check=True).stdout


def test_noise_is_dropped():
    assert changelog_draft.is_noise(Commit("a", "12 files changed", ["x.py"]))
    assert changelog_draft.is_noise(Commit("a", "Merge main into 1990-thing", ["x.py"]))
    assert changelog_draft.is_noise(Commit("a", "Docs: the iOS Market redesign", ["docs/a.md"]))
    assert changelog_draft.is_noise(Commit("a", "Market: cards look nicer", ["docs/a.md", "app/tests/test_x.py"]))
    assert changelog_draft.is_noise(Commit("a", "chore(ios): bump a pin", ["native/ios/a.swift"]))
    assert changelog_draft.is_noise(Commit("a", "Add tests for the market list", ["app/src/a.py"]))
    assert not changelog_draft.is_noise(Commit("a", "iOS Market: one-tap install", ["native/ios/a.swift"]))


def test_areas_come_from_the_subject_then_the_paths():
    area = changelog_draft.area_of
    assert area(Commit("a", "Android Market: sort and filter chips", [])) == "Market"
    assert area(Commit("a", "Desktop: a project with no instance lists nothing", ["desktop/src/a.cpp"])) == "Projects"
    assert area(Commit("a", "Calls: blur the background behind you", [])) == "Calls"
    assert area(Commit("a", "Real photo face swap with mesh warp", [])) == "Calls"
    assert area(Commit("a", "iOS: walking scene in the top bar", ["native/ios/a.swift"])) == "iPhone"
    assert area(Commit("a", "Faster startup on slow phones", ["native/android/a.kt"])) == "Android"
    assert area(Commit("a", "Something nice for everyone", ["native/ios/a.swift", "desktop/b.cpp"])) == "Other improvements"


def test_subjects_become_short_sentences():
    assert changelog_draft.sentence("iOS Market: flat cards, sort bar") == "Flat cards, sort bar."
    assert changelog_draft.sentence("Fix the crash when a call ends (#123)") == "Fixed the crash when a call ends."
    assert changelog_draft.sentence("feat(desktop): open the palette with Cmd+P") == "Opens the palette with Cmd+P."
    long = changelog_draft.sentence("Market: " + "word " * 60)
    assert len(long) <= changelog_draft.MAX_ITEM_CHARS and long.endswith("…")


def test_code_heavy_subjects_are_left_out():
    readable = changelog_draft.readable
    assert not readable(Commit("a", "iOS Market: format card counts with PackagesFormat.count"))
    assert not readable(Commit("a", "Rename fetch_rows in market_packages"))
    assert readable(Commit("a", "Projects: hide app.forkmesh.com repos until an instance is chosen on iOS"))


def test_other_platform_changes_are_hidden(repo):
    base = commit(repo, "Initial import of the tree", "README")
    commit(repo, "Android Market: compact divided cards", "native/android/a.kt")
    commit(repo, "iOS Market: one-tap install from the list", "native/ios/a.swift")
    commit(repo, "Desktop: a project with no instance lists no network repositories", "desktop/a.cpp")
    commit(repo, "Docs: explain the market", "docs/a.md")
    commit(repo, "3 files changed", "app/a.py")
    commits = changelog_draft.commits_between(repo, "main", base)
    ios = items(changelog_draft.sections_for(commits, "ios"))
    assert ios == {"Market": ["One-tap install from the list."],
                   "Projects": ["A project with no instance lists no network repositories."]}
    android = items(changelog_draft.sections_for(commits, "android"))
    assert android["Market"] == ["Compact divided cards."]


def test_duplicates_and_long_sections_are_capped():
    commits = [Commit(str(i), f"Market: change number {i} to the list", ["app/a.py"]) for i in range(20)]
    commits.append(Commit("d", "Market: change number 1 to the list.", ["app/a.py"]))
    sections = changelog_draft.sections_for(commits)
    assert len(sections[0]["items"]) == changelog.MAX_ITEMS_PER_SECTION
    assert len(set(sections[0]["items"])) == len(sections[0]["items"])


def test_draft_lists_changes_since_a_date_or_a_commit(repo):
    base = commit(repo, "Initial import of the tree", "README", when=1_790_000_000)
    commit(repo, "Chat: send voice notes in a channel", "app/chat.py", when=1_790_000_100)
    commit(repo, "Merge 12-thing into main", when=1_790_000_200)
    commit(repo, "Calls: blur the background behind you", "app/calls.py", when=1_790_400_000)
    assert items(changelog_draft.sections_since(repo, base, "main")) == {
        "Calls": ["Blurs the background behind you."], "Chat": ["Sends voice notes in a channel."]}
    assert items(changelog_draft.sections_since(repo, "2026-09-24", "main")) == {
        "Calls": ["Blurs the background behind you."]}


def test_the_checked_in_changelog_is_valid():
    data = changelog.load()
    assert changelog.problems(data) == []
    assert changelog.layout_problems(changelog.CHANGELOG.read_text(encoding="utf-8"), data) == []
    assert all(entry["sections"] for entry in changelog.export(data)["entries"])


def test_every_note_and_release_is_one_line():
    lines = changelog.CHANGELOG.read_text(encoding="utf-8").splitlines()
    data = changelog.load()
    rows = [line for line in lines if line.startswith("  {")]
    assert len(rows) == len(data["notes"]) + len(data["releases"])
    assert all(line.endswith(("},", "}")) for line in rows)
    assert (ROOT / ".gitattributes").read_text().splitlines().count("changelog/changelog.json merge=union") == 1


def test_problems_name_bad_notes_and_releases():
    data = {"schema": 3, "releases": [{"date": "2026-10-02", "title": "Two"}, {"date": "2026-10-02", "title": "Again"},
                                      {"date": "2026-11-01", "title": "Nothing"}, {"date": "2026-06", "version": "x y"}],
            "notes": [*notes("2026-10-01", ("Chat", ["A.", "**a**"])), *notes("2026-10-02", ("Nope", ["B."])),
                      {"date": "soon", "area": "Chat", "text": "C."}, {"date": "2026-10-03", "area": "Chat", "text": " "}]}
    found = changelog.problems(data)
    assert "note 2: the same note appears twice on 2026-10-01" in found
    assert any(problem.startswith("note 3: area 'Nope'") for problem in found)
    assert "note 4: date must be YYYY-MM-DD or YYYY-MM" in found
    assert "note 5: text must be a non-empty sentence" in found
    assert "release 2: 2026-10-02 has two releases" in found
    assert "release 3: no notes have date '2026-11-01'" in found
    assert "release 4: bad version 'x y'" in found
    assert changelog.problems({"schema": 2, "entries": []}) == ["schema must be 3", "notes must be a non-empty list"]


def test_notes_group_into_entries_in_any_file_order():
    data = {"schema": 3, "releases": [{"date": "2026-06", "version": "0.4.0", "title": "Four"},
                                      {"date": "2026-10-21", "title": "Stickers", "pitch": "Pitch."}],
            "notes": [*notes("2026-06", ("Chat", ["Old four."]), version="0.4.0"),
                      *notes("2026-10-21", ("World", ["Walk."]), ("Chat", ["Stick.", "  Stick. "])),
                      *notes("2026-06", ("Chat", ["Old ten."]), version="0.10.0"),
                      *notes("2026-10-22", ("Chat", ["Newest."]))]}
    assert [(e["date"], e["version"], e["title"]) for e in changelog.entries(data)] == [
        ("2026-10-22", "", ""), ("2026-10-21", "", "Stickers"), ("2026-06", "0.10.0", ""), ("2026-06", "0.4.0", "Four")]
    assert changelog.entries(data)[1]["sections"] == [{"title": "Chat", "items": ["Stick."]},
                                                      {"title": "World", "items": ["Walk."]}]


def test_the_apps_get_plain_text_without_the_other_phone():
    data = {"schema": 3, "releases": [{"date": "2026-10-21", "version": "1.4.1", "title": "Stickers"}],
            "notes": [*notes("2026-10-21", ("Chat", ["**Stickers.** Stick them on messages."]), ("iPhone", ["AUTO DJ."]),
                             ("Android", ["Back gesture."]), version="1.4.1"),
                      *notes("2026-10-20", ("Android", ["Only Android."])) ]}
    ios = changelog.export(data, "ios")
    assert ios["schema"] == 1
    assert ios["entries"] == [{"version": "1.4.1", "build": "", "date": "2026-10-21", "title": "Stickers", "pitch": "",
                               "sections": [{"title": "Chat", "items": ["Stickers. Stick them on messages."]},
                                            {"title": "iPhone", "items": ["AUTO DJ."]}]}]
    android = changelog.export(data, "android")
    assert [items(e["sections"]) for e in android["entries"]] == [
        {"Chat": ["Stickers. Stick them on messages."], "Android": ["Back gesture."]}, {"Android": ["Only Android."]}]


def test_long_entries_are_capped_for_the_apps():
    lines = [f"Change number {i}." for i in range(30)]
    exported = changelog.export({"notes": notes("2026-10-21", ("Chat", lines), ("Market", lines[::-1]))})
    chat = exported["entries"][0]["sections"]
    assert len(chat[0]["items"]) == changelog.MAX_ITEMS_PER_SECTION


def test_the_website_reads_the_changelog_json():
    template = (ROOT / "www" / "templates" / "pages" / "changelog.html").read_text()
    assert '<script src="/changelog.js?v=${version}" defer></script>' in template
    assert 'id="changelog-releases" data-source="/changelog.json"' in template
    assert "<li>" not in template
    page = (ROOT / "www" / "public" / "changelog.html").read_text()
    assert 'data-source="/changelog.json"' in page


def test_the_website_groups_notes_like_the_apps():
    shown = json.loads(node_changelog(
        "const fs = require('fs');"
        "process.stdout.write(JSON.stringify(changelog.entries(JSON.parse(fs.readFileSync(process.argv[1], 'utf8')))));",
        str(changelog.CHANGELOG)))
    data = changelog.load()
    assert shown == changelog.entries(data)
    for exported, entry in zip(changelog.export(data)["entries"], shown):
        assert (exported["date"], exported["version"]) == (entry["date"], entry["version"])


def test_the_website_marks_up_versions_dates_and_bold_leads():
    data = {"releases": [{"date": "2026-07", "version": "0.7.0", "title": "City"}],
            "notes": [*notes("2026-07", ("Highlights", ["**Everyone enters.** Guests & members <b>."]), version="0.7.0"),
                      *notes("2026-10-21", ("Chat", ["A."]), ("World", ["B."]))]}
    rendered = node_changelog("process.stdout.write(changelog.pageHtml(JSON.parse(process.argv[1])));", json.dumps(data))
    assert rendered.index('id="2026-10-21"') < rendered.index('id="v0.7.0"')
    assert '<section class="release" id="v0.7.0">' in rendered
    assert '<span class="tag">v0.7.0</span>' in rendered
    assert '<time class="when" datetime="2026-07">July 2026</time>' in rendered
    assert "<li><strong>Everyone enters.</strong> Guests &amp; members &lt;b&gt;.</li>" in rendered
    assert "<h3>Highlights</h3>" not in rendered
    assert '<time class="when" datetime="2026-10-21">October 21, 2026</time>' in rendered
    assert "<h3>Chat</h3>" in rendered and "<h3>World</h3>" in rendered


def test_add_puts_a_dated_note_on_the_first_line(tmp_path):
    path = tmp_path / "changelog.json"
    changelog.save({"schema": 3, "releases": [], "notes": notes("2026-10-20", ("Chat", ["Old."]))}, path)
    before = path.read_text().splitlines()
    for area, text in (("World", "walk into a neighbour's Office"), ("Chat", "Stick stickers on messages."),
                       ("Chat", "stick stickers on messages")):
        assert changelog.main(["--file", str(path), "add", "--area", area, "--date", "2026-10-21", text]) == 0
    after = path.read_text().splitlines()
    assert after[:4] == before[:4] and after[-3:] == before[-3:]
    data = changelog.load(path)
    assert [(n["date"], n["area"], n["text"]) for n in data["notes"]] == [
        ("2026-10-21", "Chat", "Stick stickers on messages."), ("2026-10-21", "World", "walk into a neighbour's Office."),
        ("2026-10-20", "Chat", "Old.")]
    assert changelog.problems(data) == []
    assert changelog.main(["--file", str(path), "check"]) == 0


def test_two_branches_adding_notes_merge_without_a_conflict(repo, tmp_path):
    (repo / ".gitattributes").write_text((ROOT / ".gitattributes").read_text())
    path = repo / "changelog" / "changelog.json"
    path.parent.mkdir()
    changelog.save({"schema": 3, "releases": [{"date": "2026-10-20", "title": "Base"}],
                    "notes": notes("2026-10-20", ("Chat", ["Old.", "Older."]))}, path)
    git = lambda *args: subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)
    git("add", "-A")
    git("commit", "-qm", "base")
    git("checkout", "-qb", "side")
    changelog.main(["--file", str(path), "add", "--area", "World", "--date", "2026-10-22", "Side note"])
    git("commit", "-qam", "side")
    git("checkout", "-q", "main")
    changelog.main(["--file", str(path), "add", "--area", "Chat", "--date", "2026-10-21", "Main note"])
    git("commit", "-qam", "main")
    git("merge", "-q", "--no-edit", "side")
    data = changelog.load(path)
    assert changelog.main(["--file", str(path), "check"]) == 0
    assert [e["date"] for e in changelog.entries(data)] == ["2026-10-22", "2026-10-21", "2026-10-20"]


def test_the_ios_build_bundles_the_shared_changelog(tmp_path):
    stamp = load_stamp()
    out = tmp_path / "Changelog.json"
    assert stamp.main(["--version", "1.4.1", "--build", "303", "--out", str(out)]) == 0
    assert json.loads(out.read_text()) == changelog.export(changelog.load(), "ios")
    assert stamp.main(["--changelog", str(tmp_path / "missing.json"), "--version", "1.4.1", "--build", "303",
                       "--out", str(out)]) == 0
    assert json.loads(out.read_text()) == {"schema": 1, "entries": []}


def test_the_cli_exports_for_android(tmp_path):
    out = tmp_path / "changelog.json"
    assert changelog.main(["export", "--platform", "android", "--out", str(out)]) == 0
    assert json.loads(out.read_text()) == changelog.export(changelog.load(), "android")


def test_the_placeholders_stay_empty():
    for path in (ROOT / "native" / "ios" / "ForkMesh" / "Resources" / "Changelog.json",
                 ROOT / "native" / "android" / "app" / "src" / "changelog" / "changelog.json"):
        assert json.loads(path.read_text()) == {"schema": 1, "entries": []}


def test_the_adhoc_workflow_bundles_the_changelog():
    import yaml
    workflow = yaml.safe_load((ROOT / "codemagic.yaml").read_text())["workflows"]["ios-native-adhoc"]
    names = [step.get("name") for step in workflow["scripts"] if isinstance(step, dict)]
    assert names.index("Bundle the changelog") == names.index("Pick the ad hoc build number") + 1
    script = next(step["script"] for step in workflow["scripts"]
                  if isinstance(step, dict) and step.get("name") == "Bundle the changelog")
    assert "tools/changelog_stamp.py" in script and "ForkMesh/Resources/Changelog.json" in script
    assert "--draft" not in script


def test_the_android_release_bundles_the_changelog():
    text = (ROOT / "tools" / "forkmesh-release-host.py").read_text()
    assert "tools/changelog.py export --platform android" in text and "-Pforkmesh.changelog=" in text
    assert "forkmesh.changelog" in (ROOT / "native" / "android" / "app" / "build.gradle.kts").read_text()
