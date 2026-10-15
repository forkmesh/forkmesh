package stats

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"forkmesh.com/desktop-core/internal/qvariant"
)

func writeExecutable(t *testing.T, path, text string) {
	t.Helper()
	writeFile(t, path, text)
	if err := os.Chmod(path, 0o755); err != nil {
		t.Fatal(err)
	}
}

func shortcutsFixture(t *testing.T) (*fixture, string) {
	t.Helper()
	f := newFixture(t)
	checkout := t.TempDir()
	shortcuts := filepath.Join(checkout, ".forkmesh", "shortcuts")
	writeFile(t, filepath.Join(shortcuts, "build.sh"), "#!/bin/bash\n# name: Build it\n# description: Builds the app\nmake\n")
	writeExecutable(t, filepath.Join(shortcuts, "deploy"), "#!/bin/sh\n# Ships it\n")
	writeFile(t, filepath.Join(shortcuts, "notes.txt"), "# agent: Codex\n# model: gpt-5\nSummarise the notes.\r\n")
	writeFile(t, filepath.Join(shortcuts, "empty.md"), "# name: Nothing\n# description: no prompt\n")
	writeFile(t, filepath.Join(shortcuts, "data.bin"), "binary")
	writeFile(t, filepath.Join(shortcuts, ".hidden.sh"), "echo")
	writeFile(t, filepath.Join(shortcuts, "bad..name.sh"), "echo")
	if err := os.MkdirAll(filepath.Join(shortcuts, "folder.sh"), 0o700); err != nil {
		t.Fatal(err)
	}
	skills := filepath.Join(checkout, ".forkmesh", "skills")
	writeFile(t, filepath.Join(skills, "Fix.MD"), "Fix the failing tests.\n")
	writeFile(t, filepath.Join(skills, "readme.rst"), "not a skill")
	writeFile(t, filepath.Join(skills, "review.skill"), "# name: Careful review\nReview the diff.\n")
	writeFile(t, filepath.Join(f.core.Layout.Data, "skills", "triage.prompt"), "# description: "+strings.Repeat("é", 310)+"\nTriage.\n")
	writeFile(t, filepath.Join(f.core.Layout.Data, "organization-skills", "Acme-Co", "org.txt"), "Org wide.\n")
	usage := qvariant.EncodeSettingsRaw(map[string]any{
		"shortcut:Acme Co/app:build.sh": int32(4),
		"app:triage.prompt":             int32(2),
		"repository:Acme Co/app:Fix.MD": int32(1),
	})
	runStats := qvariant.EncodeSettingsRaw(map[string]any{
		"shortcut:Acme Co/app:build.sh": map[string]any{
			"lastRunAtMs": int64(1790982478196), "finished": int32(3), "succeeded": int32(2), "timed": int32(3),
			"totalMs": int64(900), "lastMs": int64(300), "lastOk": true, "lastOutput": "built, 3 targets",
		},
	})
	writeSettings(t, f.core, repoSettings("alice", []repoEntry{
		{owner: "Acme Co", name: "app", localPath: checkout},
		{owner: "acme", name: "remote-only", cloneURL: "https://app.forkmesh.com/acme/remote-only.git"},
	}, map[string][]string{"shortcuts": {"usageCounts=" + usage, "runStats=" + runStats}}))
	return f, checkout
}

func entryByFile(t *testing.T, out map[string]any, scope, file string) map[string]any {
	t.Helper()
	for _, raw := range out["entries"].([]any) {
		entry := raw.(map[string]any)
		if entry["scope"] == scope && entry["file"] == file {
			return entry
		}
	}
	t.Fatalf("no %s entry %s in %v", scope, file, out["entries"])
	return nil
}

func TestShortcutsListMatchesTheDesktop(t *testing.T) {
	f, _ := shortcutsFixture(t)
	calls := 0
	f.module.ShortcutState = func(ctx context.Context) DesktopShortcutState {
		calls++
		return DesktopShortcutState{}
	}
	out := f.mustCall("insights.shortcuts", map[string]any{"repo": " acme co/APP "})
	if out["repo"] != "acme co/APP" {
		t.Fatalf("repo %v", out["repo"])
	}
	assertJSON(t, out["repos"], []any{"Acme Co/app"})
	order := []string{}
	for _, raw := range out["entries"].([]any) {
		entry := raw.(map[string]any)
		order = append(order, entry["scope"].(string)+":"+entry["file"].(string))
	}
	assertJSON(t, order, []string{"shortcut:build.sh", "shortcut:data.bin", "shortcut:deploy", "shortcut:empty.md",
		"shortcut:notes.txt", "repository:Fix.MD", "repository:review.skill", "app:triage.prompt", "organization:org.txt"})
	assertJSON(t, entryByFile(t, out, "shortcut", "build.sh"), map[string]any{
		"file": "build.sh", "scope": "shortcut", "name": "Build it", "description": "Builds the app", "kind": "script",
		"agent": "", "model": "", "uses": float64(4), "runnable": true, "running": false, "lastOutput": "built, 3 targets",
		"lastOk": true, "finished": float64(3), "lastRunAtMs": float64(1790982478196)})
	deploy := entryByFile(t, out, "shortcut", "deploy")
	if deploy["kind"] != "script" || deploy["description"] != "Ships it" || deploy["runnable"] != true {
		t.Fatalf("deploy %v", deploy)
	}
	notes := entryByFile(t, out, "shortcut", "notes.txt")
	if notes["kind"] != "prompt" || notes["agent"] != "codex" || notes["model"] != "gpt-5" || notes["runnable"] != true {
		t.Fatalf("notes %v", notes)
	}
	if e := entryByFile(t, out, "shortcut", "empty.md"); e["runnable"] != false || e["name"] != "Nothing" {
		t.Fatalf("empty %v", e)
	}
	if e := entryByFile(t, out, "shortcut", "data.bin"); e["kind"] != "file" || e["runnable"] != false {
		t.Fatalf("data %v", e)
	}
	fix := entryByFile(t, out, "repository", "Fix.MD")
	if fix["kind"] != "skill" || fix["name"] != "Fix" || fix["uses"] != float64(1) || fix["runnable"] != true {
		t.Fatalf("fix %v", fix)
	}
	if e := entryByFile(t, out, "repository", "review.skill"); e["name"] != "Careful review" {
		t.Fatalf("review %v", e)
	}
	triage := entryByFile(t, out, "app", "triage.prompt")
	if triage["uses"] != float64(2) || triage["name"] != "triage" || len([]rune(triage["description"].(string))) != 300 {
		t.Fatalf("triage %v", triage)
	}
	if calls != 1 {
		t.Fatalf("desktop asked %d times", calls)
	}
}

func TestShortcutsShowTheDesktopsRunningScript(t *testing.T) {
	f, _ := shortcutsFixture(t)
	f.module.ShortcutState = func(ctx context.Context) DesktopShortcutState {
		return DesktopShortcutState{Active: true, Running: true, Repo: "acme co/app", File: "build.sh", LastOutput: "step 2/3"}
	}
	out := f.mustCall("insights.shortcuts", map[string]any{"repo": "Acme Co/app"})
	build := entryByFile(t, out, "shortcut", "build.sh")
	if build["running"] != true || build["lastOutput"] != "step 2/3" {
		t.Fatalf("build %v", build)
	}
	if deploy := entryByFile(t, out, "shortcut", "deploy"); deploy["running"] != false {
		t.Fatalf("deploy %v", deploy)
	}
	f.module.ShortcutState = func(ctx context.Context) DesktopShortcutState {
		return DesktopShortcutState{Active: false, Running: true, Repo: "Acme Co/app", File: "build.sh"}
	}
	out = f.mustCall("insights.shortcuts", map[string]any{"repo": "Acme Co/app"})
	if build := entryByFile(t, out, "shortcut", "build.sh"); build["running"] != false {
		t.Fatalf("finished process still running: %v", build)
	}
}

func TestShortcutsWithoutARepoOrDesktop(t *testing.T) {
	f, _ := shortcutsFixture(t)
	out := f.mustCall("insights.shortcuts", map[string]any{})
	order := []string{}
	for _, raw := range out["entries"].([]any) {
		entry := raw.(map[string]any)
		order = append(order, entry["scope"].(string)+":"+entry["file"].(string))
	}
	assertJSON(t, order, []string{"app:triage.prompt"})
	out = f.mustCall("insights.shortcuts", map[string]any{"repo": "acme/remote-only"})
	if n := len(out["entries"].([]any)); n != 1 {
		t.Fatalf("remote-only repo listed %d entries", n)
	}
	if !f.core.Registry.Native("insights.shortcuts") || f.core.Registry.Native("insights.shortcutRun") {
		t.Fatal("routes")
	}
}

func TestShortcutsCapAndGate(t *testing.T) {
	f, checkout := shortcutsFixture(t)
	for i := 0; i < 230; i++ {
		writeFile(t, filepath.Join(checkout, ".forkmesh", "shortcuts", "s"+itoa(1000+i)+".sh"), "echo\n")
	}
	f.module.ShortcutState = func(ctx context.Context) DesktopShortcutState { return DesktopShortcutState{} }
	out := f.mustCall("insights.shortcuts", map[string]any{"repo": "Acme Co/app"})
	if n := len(out["entries"].([]any)); n != maxShortcutEntries {
		t.Fatalf("entries %d", n)
	}
	stranger := f.phone
	stranger.Dev.User = "mallory"
	f.expectError("insights.shortcuts", map[string]any{"repo": "Acme Co/app"}, "not_owner")
}

func TestShortcutMetaHelpers(t *testing.T) {
	meta := parseShortcutMeta("x.sh", "#!/bin/sh\n  # name:  Spaced  \n#plain comment\n# agent: CLAUDE\n")
	if meta.Name != "Spaced" || meta.Description != "plain comment" || meta.Agent != "claude" {
		t.Fatalf("meta %+v", meta)
	}
	if got := shortcutPrompt("# name: a\nline one\n  # model: m\nline two\n"); got != "line one\nline two" {
		t.Fatalf("prompt %q", got)
	}
	for name, want := range map[string]string{"a.SH": "script", "a.bash": "script", "a.skill": "skill", "a.Prompt": "prompt", "a.py": "file"} {
		if got := shortcutKind(name, false); got != want {
			t.Errorf("%s: %s", name, got)
		}
	}
	if shortcutKind("run", true) != "script" || shortcutKind("run", false) != "file" {
		t.Fatal("suffixless")
	}
	if completeBaseName("a.b.md") != "a.b" || completeBaseName("plain") != "plain" {
		t.Fatal("base name")
	}
}
