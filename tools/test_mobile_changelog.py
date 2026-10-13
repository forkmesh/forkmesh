import json
import subprocess
from pathlib import Path

import pytest

import mobile_changelog
from mobile_changelog import Commit

ROOT = Path(__file__).resolve().parents[1]
stamp_tool = mobile_changelog.stamp_tool


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


def published(*builds):
    return {f"sha{number}": {"ipa": f"/data/shortcut/20261013120000/mac-adhoc/x/ipa/"
                                    f"ForkMesh-native-1.4.1-{number}-adhoc.ipa",
                             "commit": sha, "sentAt": sent, "complete": True}
            for number, sha, sent in builds}


def items(sections):
    return {section["title"]: section["items"] for section in sections}


def test_noise_is_dropped():
    assert mobile_changelog.is_noise(Commit("a", "12 files changed", ["x.py"]))
    assert mobile_changelog.is_noise(Commit("a", "Merge main into 1990-thing", ["x.py"]))
    assert mobile_changelog.is_noise(Commit("a", "Docs: the iOS Market redesign", ["docs/a.md"]))
    assert mobile_changelog.is_noise(Commit("a", "Market: cards look nicer", ["docs/a.md", "app/tests/test_x.py"]))
    assert mobile_changelog.is_noise(Commit("a", "chore(ios): bump a pin", ["native/ios/a.swift"]))
    assert mobile_changelog.is_noise(Commit("a", "Add tests for the market list", ["app/src/a.py"]))
    assert not mobile_changelog.is_noise(Commit("a", "iOS Market: one-tap install", ["native/ios/a.swift"]))


def test_areas_come_from_the_subject_then_the_paths():
    area = mobile_changelog.area_of
    assert area(Commit("a", "Android Market: sort and filter chips", [])) == "Market"
    assert area(Commit("a", "Desktop: a project with no instance lists nothing", ["desktop/src/a.cpp"])) == "Projects"
    assert area(Commit("a", "Calls: blur the background behind you", [])) == "Calls"
    assert area(Commit("a", "Real photo face swap with mesh warp", [])) == "Calls"
    assert area(Commit("a", "iOS: walking scene in the top bar", ["native/ios/a.swift"])) == "iPhone"
    assert area(Commit("a", "Faster startup on slow phones", ["native/android/a.kt"])) == "Android"
    assert area(Commit("a", "Something nice for everyone", ["native/ios/a.swift", "desktop/b.cpp"])) == "Other improvements"


def test_subjects_become_short_sentences():
    assert mobile_changelog.sentence("iOS Market: flat cards, sort bar") == "Flat cards, sort bar."
    assert mobile_changelog.sentence("Fix the crash when a call ends (#123)") == "Fixed the crash when a call ends."
    assert mobile_changelog.sentence("feat(desktop): open the palette with Cmd+P") == "Opens the palette with Cmd+P."
    long = mobile_changelog.sentence("Market: " + "word " * 60)
    assert len(long) <= mobile_changelog.MAX_ITEM_CHARS and long.endswith("…")


def test_code_heavy_subjects_are_left_out():
    readable = mobile_changelog.readable
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
    commits = mobile_changelog.commits_between(repo, "main", base)
    ios = items(mobile_changelog.sections_for(commits, "ios"))
    assert ios == {"Market": ["One-tap install from the list."],
                   "Projects": ["A project with no instance lists no network repositories."]}
    android = items(mobile_changelog.sections_for(commits, "android"))
    assert android["Market"] == ["Compact divided cards."]


def test_duplicates_and_long_sections_are_capped():
    commits = [Commit(str(i), f"Market: change number {i} to the list", ["app/a.py"]) for i in range(20)]
    commits.append(Commit("d", "Market: change number 1 to the list.", ["app/a.py"]))
    sections = mobile_changelog.sections_for(commits)
    assert len(sections[0]["items"]) == stamp_tool.MAX_ITEMS_PER_SECTION
    assert len(set(sections[0]["items"])) == len(sections[0]["items"])


def test_draft_covers_main_since_the_last_build_and_earlier_builds(repo):
    first = commit(repo, "Initial import of the tree", "README", when=1_790_000_000)
    commit(repo, "Chat: send voice notes in a channel", "app/chat.py", when=1_790_000_100)
    second = commit(repo, "Merge 12-thing into main", when=1_790_000_200)
    commit(repo, "Calls: blur the background behind you", "app/calls.py", when=1_790_000_300)
    ledger = published((10, first, 1_790_000_050), (11, second, 1_790_000_250))
    notes = {"ios": {"11": {"sections": [{"title": "Chat", "items": ["Voice notes are here."]}]}}}
    draft = mobile_changelog.draft(repo, "main", ledger, notes, "ios", today="2026-10-13")
    current, *older = draft["entries"]
    assert current["pending"] and current["after"] == "11"
    assert items(current["sections"]) == {"Calls": ["Blurs the background behind you."]}
    assert [entry["build"] for entry in older] == ["11"]
    assert items(older[0]["sections"]) == {"Chat": ["Voice notes are here.", "Sends voice notes in a channel."]}
    assert not any("sha" in json.dumps(entry) for entry in older)


def test_a_rewritten_previous_commit_falls_back_to_the_build_time(repo):
    commit(repo, "Initial import of the tree", "README", when=1_790_000_000)
    commit(repo, "Market: older change before the build", "app/m.py", when=1_790_000_100)
    commit(repo, "Market: newer change after the build", "app/m.py", when=1_790_000_900)
    ledger = published((5, "f" * 40, 1_790_000_500))
    ledger["sha5"]["ipa"] = "/x/ForkMesh-native-1.4.1-5-adhoc.ipa"
    draft = mobile_changelog.draft(repo, "main", ledger, {}, "ios")
    assert items(draft["entries"][0]["sections"]) == {"Market": ["Newer change after the build."]}


def test_stamp_applies_pending_notes_up_to_this_build():
    draft = {"notes": {"219": {"replace": True, "sections": [{"title": "Calls", "items": ["New faces."]}]},
                       "230": {"replace": True, "sections": [{"title": "Later", "items": ["Not yet."]}]},
                       "218": {"replace": True, "sections": [{"title": "Old", "items": ["Shipped."]}]}},
             "entries": [{"pending": True, "after": "218", "date": "2026-10-13",
                          "sections": [{"title": "Market", "items": ["Generated."]}]},
                         {"version": "1.4.1", "build": "218", "date": "2026-10-12",
                          "sections": [{"title": "Old", "items": ["Shipped."]}]}]}
    stamped = stamp_tool.stamp(draft, "1.4.1", "220")
    head = stamped["entries"][0]
    assert (head["version"], head["build"], head["date"]) == ("1.4.1", "220", "2026-10-13")
    assert items(head["sections"]) == {"Calls": ["New faces."]}
    assert [entry["build"] for entry in stamped["entries"]] == ["220", "218"]
    assert "notes" not in stamped and "pending" not in json.dumps(stamped)


def test_notes_that_extend_keep_the_generated_lines():
    merged = stamp_tool.apply_notes([{"title": "Market", "items": ["Generated."]}],
                                    [{"sections": [{"title": "Market", "items": ["Written."]},
                                                   {"title": "Calls", "items": ["Hand."]}]}])
    assert merged == [{"title": "Market", "items": ["Written.", "Generated."]},
                      {"title": "Calls", "items": ["Hand."]}]


def test_stamp_without_a_draft_still_names_the_build(tmp_path):
    out = tmp_path / "Changelog.json"
    assert stamp_tool.main(["--draft", str(tmp_path / "missing.json"), "--version", "1.4.1",
                            "--build", "219", "--date", "2026-10-13", "--out", str(out)]) == 0
    assert json.loads(out.read_text()) == {"schema": 1, "entries": [
        {"version": "1.4.1", "build": "219", "date": "2026-10-13", "sections": []}]}


def test_cli_writes_a_stamped_android_changelog(repo, tmp_path):
    base = commit(repo, "Initial import of the tree", "README")
    commit(repo, "Android: settings open faster on cold start", "native/android/a.kt")
    out = tmp_path / "changelog.json"
    assert mobile_changelog.main(["--repo", str(repo), "--platform", "android", "--since-ref", base,
                                  "--notes", str(tmp_path / "none.json"), "--version", "1.4.1",
                                  "--build", "10401", "--out", str(out)]) == 0
    entry = json.loads(out.read_text())["entries"][0]
    assert entry["build"] == "10401"
    assert items(entry["sections"]) == {"Android": ["Settings open faster on cold start."]}


def test_checked_in_notes_and_placeholders_are_valid():
    notes = json.loads((ROOT / "native" / "changelog" / "notes.json").read_text())
    for platform in ("ios", "android"):
        for key, note in notes[platform].items():
            assert key.isdigit()
            assert stamp_tool.clean_sections(note["sections"]) == note["sections"]
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
    assert "FORKMESH_CHANGELOG_DRAFT" in (ROOT / "native" / "ios" / "tools" / "mac_adhoc_build.sh").read_text()
