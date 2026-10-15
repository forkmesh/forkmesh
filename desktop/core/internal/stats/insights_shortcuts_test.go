package stats

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
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
	usage := `@Variant(\0\0\0\b\0\0\0\x3\0\0\0\"\0\x61\0p\0p\0:\0t\0r\0i\0\x61\0g\0\x65\0.\0p\0r\0o\0m\0p\0t\0\0\0\x2\0\0\0\x2\0\0\0:\0r\0\x65\0p\0o\0s\0i\0t\0o\0r\0y\0:\0\x41\0\x63\0m\0\x65\0 \0\x43\0o\0/\0\x61\0p\0p\0:\0\x46\0i\0x\0.\0M\0\x44\0\0\0\x2\0\0\0\x1\0\0\0:\0s\0h\0o\0r\0t\0\x63\0u\0t\0:\0\x41\0\x63\0m\0\x65\0 \0\x43\0o\0/\0\x61\0p\0p\0:\0\x62\0u\0i\0l\0\x64\0.\0s\0h\0\0\0\x2\0\0\0\x4)`
	runStats := `"@Variant(\0\0\0\b\0\0\0\x1\0\0\0:\0s\0h\0o\0r\0t\0\x63\0u\0t\0:\0\x41\0\x63\0m\0\x65\0 \0\x43\0o\0/\0\x61\0p\0p\0:\0\x62\0u\0i\0l\0\x64\0.\0s\0h\0\0\0\b\0\0\0\b\0\0\0\x10\0\x66\0i\0n\0i\0s\0h\0\x65\0\x64\0\0\0\x2\0\0\0\x3\0\0\0\f\0l\0\x61\0s\0t\0M\0s\0\0\0\x4\0\0\0\0\0\0\x1,\0\0\0\f\0l\0\x61\0s\0t\0O\0k\0\0\0\x1\x1\0\0\0\x14\0l\0\x61\0s\0t\0O\0u\0t\0p\0u\0t\0\0\0\n\0\0\0 \0\x62\0u\0i\0l\0t\0,\0 \0\x33\0 \0t\0\x61\0r\0g\0\x65\0t\0s\0\0\0\x16\0l\0\x61\0s\0t\0R\0u\0n\0\x41\0t\0M\0s\0\0\0\x4\0\0\x1\xa0\xfe\xdf\xd9t\0\0\0\x12\0s\0u\0\x63\0\x63\0\x65\0\x65\0\x64\0\x65\0\x64\0\0\0\x2\0\0\0\x2\0\0\0\n\0t\0i\0m\0\x65\0\x64\0\0\0\x2\0\0\0\x3\0\0\0\xe\0t\0o\0t\0\x61\0l\0M\0s\0\0\0\x4\0\0\0\0\0\0\x3\x84)"`
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
