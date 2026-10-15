package stats

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

type fixture struct {
	t      *testing.T
	core   *daemon.Core
	module *Module
	phone  *coretest.Peer
	now    time.Time
}

type repoEntry struct {
	owner, name, localPath, cloneURL, relayURL string
}

func iniValue(value string) string {
	if strings.ContainsAny(value, ",\"=;") {
		return `"` + strings.ReplaceAll(value, `"`, `\"`) + `"`
	}
	return value
}

func writeSettings(t *testing.T, core *daemon.Core, groups map[string][]string) {
	t.Helper()
	path := core.Layout.SettingsFile()
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	var b strings.Builder
	for group, lines := range groups {
		b.WriteString("[" + group + "]\n")
		for _, line := range lines {
			b.WriteString(line + "\n")
		}
		b.WriteString("\n")
	}
	if err := os.WriteFile(path, []byte(b.String()), 0o600); err != nil {
		t.Fatal(err)
	}
	settingsGeneration++
	future := time.Now().Add(time.Duration(settingsGeneration) * time.Second)
	_ = os.Chtimes(path, future, future)
}

func repoSettings(account string, repos []repoEntry, extra map[string][]string) map[string][]string {
	groups := map[string][]string{
		"account": {"nodeName=" + account},
		"servers": {"active=0", `items="[{\"id\":\"x\",\"name\":\"app.forkmesh.com\",\"url\":\"wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws\"}]"`},
	}
	lines := []string{}
	for i, repo := range repos {
		prefix := "items\\" + itoa(i+1) + "\\"
		lines = append(lines,
			prefix+"owner="+repo.owner,
			prefix+"name="+repo.name,
			prefix+"localPath="+iniValue(repo.localPath),
			prefix+"cloneUrl="+iniValue(repo.cloneURL),
			prefix+"relayUrl="+iniValue(repo.relayURL),
			prefix+"agentAllowedDirectories=@Invalid()",
		)
	}
	lines = append(lines, "items\\size="+itoa(len(repos)))
	groups["repositories"] = lines
	for group, more := range extra {
		groups[group] = append(groups[group], more...)
	}
	return groups
}

func itoa(n int) string { return strconv.Itoa(n) }

var settingsGeneration int

func newFixture(t *testing.T) *fixture {
	t.Helper()
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("XDG_CONFIG_HOME", filepath.Join(home, ".config"))
	t.Setenv("GIT_CONFIG_NOSYSTEM", "1")
	t.Setenv("GIT_CONFIG_GLOBAL", filepath.Join(home, ".gitconfig"))
	f := &fixture{t: t, core: coretest.New(t), now: time.Date(2026, 10, 2, 12, 0, 0, 0, time.Local)}
	f.module = New()
	f.module.Now = func() time.Time { return f.now }
	coretest.Start(t, f.core, f.module)
	f.phone = coretest.NewPeer("phone", rpc.LAN)
	f.phone.Dev.User = "alice"
	f.phone.Dev.Name = "Pixel"
	f.core.Hub.Add(f.phone)
	return f
}

func (f *fixture) call(method string, params any) (map[string]any, string) {
	f.t.Helper()
	raw, err := coretest.Call(f.t, f.core, f.phone, method, params)
	if err != nil {
		return nil, rpc.Code(err)
	}
	var out map[string]any
	if err := json.Unmarshal(raw, &out); err != nil {
		f.t.Fatalf("%s: %v", method, err)
	}
	return out, ""
}

func (f *fixture) mustCall(method string, params any) map[string]any {
	f.t.Helper()
	out, code := f.call(method, params)
	if code != "" {
		f.t.Fatalf("%s: %s", method, code)
	}
	return out
}

func (f *fixture) expectError(method string, params any, want string) {
	f.t.Helper()
	if _, code := f.call(method, params); code != want {
		f.t.Fatalf("%s %v: got %q want %q", method, params, code, want)
	}
}

func git(t *testing.T, dir string, env []string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-c", "user.name=Test", "-c", "user.email=test@example.com",
		"-c", "commit.gpgsign=false", "-c", "init.defaultBranch=main"}, args...)...)
	cmd.Dir = dir
	cmd.Env = append(os.Environ(), env...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

func writeFile(t *testing.T, path, text string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

func commitAt(t *testing.T, dir, when, message string) {
	t.Helper()
	env := []string{"GIT_AUTHOR_DATE=" + when, "GIT_COMMITTER_DATE=" + when}
	git(t, dir, env, "add", "-A")
	git(t, dir, env, "commit", "-q", "-m", message)
}

func makeRepo(t *testing.T) string {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "checkout")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	git(t, dir, nil, "init", "-q")
	writeFile(t, filepath.Join(dir, "a.txt"), "1\n2\n")
	commitAt(t, dir, "2026-09-22T12:00:00+0000", "first")
	writeFile(t, filepath.Join(dir, "b.txt"), "x\n")
	writeFile(t, filepath.Join(dir, "bin.dat"), "a\x00b")
	writeFile(t, filepath.Join(dir, ".forkmesh/stats/ignored.txt"), "skip me\n")
	commitAt(t, dir, "2026-10-02T09:00:00+0000", "second")
	writeFile(t, filepath.Join(dir, "b.txt"), "x\ny")
	return dir
}

func TestOwnerGate(t *testing.T) {
	f := newFixture(t)
	writeSettings(t, f.core, repoSettings("alice", nil, nil))
	stranger := coretest.NewPeer("stranger", rpc.Relay)
	stranger.Dev.User = "mallory"
	for _, method := range []string{"stats.repo", "stats.ratchet.prepare", "stats.ratchet.run", "stats.codemagic", "insights.shortcut"} {
		if _, err := coretest.Call(t, f.core, stranger, method, map[string]any{}); rpc.Code(err) != "not_owner" {
			t.Fatalf("%s: got %v", method, err)
		}
	}
	nokey := coretest.NewPeer("nokey", rpc.LAN)
	nokey.Dev.Key = ""
	nokey.Dev.User = "alice"
	if _, err := coretest.Call(t, f.core, nokey, "stats.repo", map[string]any{}); rpc.Code(err) != "not_owner" {
		t.Fatalf("keyless device: %v", err)
	}
	f.core.Set("control.owners", []string{"Mallory"})
	if _, err := coretest.Call(t, f.core, stranger, "stats.repo", map[string]any{"repo": "x"}); rpc.Code(err) != "bad_request" {
		t.Fatalf("linked owner: %v", err)
	}
}

func TestRepoErrors(t *testing.T) {
	f := newFixture(t)
	noGit := t.TempDir()
	writeSettings(t, f.core, repoSettings("alice", []repoEntry{
		{owner: "alice", name: "plain", localPath: noGit, cloneURL: "https://app.forkmesh.com/alice/plain"},
		{owner: "alice", name: "elsewhere", localPath: makeRepo(t), relayURL: "https://other.example"},
	}, nil))
	f.expectError("stats.repo", map[string]any{"repo": "bad slug"}, "bad_request")
	f.expectError("stats.repo", map[string]any{"repo": "../x"}, "bad_request")
	f.expectError("stats.repo", map[string]any{"repo": 7}, "bad_request")
	f.expectError("stats.repo", map[string]any{"repo": "alice/missing"}, "unknown_repo")
	f.expectError("stats.repo", map[string]any{"repo": "Alice/plain"}, "unknown_repo")
	f.expectError("stats.repo", map[string]any{"repo": "alice/plain"}, "no_checkout")
	f.expectError("stats.repo", map[string]any{"repo": "alice/elsewhere"}, "unknown_repo")
}

func TestRepoStatsAndBackfill(t *testing.T) {
	f := newFixture(t)
	dir := makeRepo(t)
	writeSettings(t, f.core, repoSettings("alice", []repoEntry{
		{owner: "alice", name: "tool", localPath: dir, cloneURL: "https://app.forkmesh.com/alice/tool"},
	}, nil))
	got := f.mustCall("stats.repo", map[string]any{"repo": " alice/tool "})
	want := map[string]any{"commits": 2.0, "files": 3.0, "lines": 4.0, "bytes": 10.0, "day": "2026-10-02",
		"ratchet": map[string]any{"enabled": false, "note": ""}}
	assertJSON(t, got, want)
	if _, err := os.Stat(statsPath(dir)); !os.IsNotExist(err) {
		t.Fatalf("stats file written without opt-in: %v", err)
	}

	git(t, dir, nil, "config", "--local", repositoryJSONKey, "true")
	f.mustCall("stats.repo", map[string]any{"repo": "alice/tool"})
	raw, err := os.ReadFile(statsPath(dir))
	if err != nil {
		t.Fatal(err)
	}
	var b strings.Builder
	b.WriteString(`{"days":[`)
	for day := 22; day <= 30; day++ {
		b.WriteString(`{"bytes":4,"day":"2026-09-` + itoa(day) + `","files":1,"lines":3},`)
	}
	b.WriteString(`{"bytes":4,"day":"2026-10-01","files":1,"lines":3},{"bytes":10,"day":"2026-10-02","files":3,"lines":4}]}`)
	if string(raw) != b.String() {
		t.Fatalf("repository.json\n got %s\nwant %s", raw, b.String())
	}

	writeFile(t, filepath.Join(dir, "b.txt"), "changed but cached day\n")
	got = f.mustCall("stats.repo", map[string]any{"repo": "alice/tool"})
	if got["bytes"] != 10.0 {
		t.Fatalf("same-day reading should come from repository.json: %v", got)
	}

	f.now = f.now.Add(24 * time.Hour)
	got = f.mustCall("stats.repo", map[string]any{"repo": "alice/tool"})
	if got["day"] != "2026-10-03" || got["bytes"] != 30.0 || got["lines"] != 3.0 {
		t.Fatalf("next day: %v", got)
	}
	var doc map[string]any
	raw, _ = os.ReadFile(statsPath(dir))
	_ = json.Unmarshal(raw, &doc)
	if days := doc["days"].([]any); len(days) != 12 {
		t.Fatalf("expected appended day, got %d", len(days))
	}
}

func TestMeasureCacheTracksEdits(t *testing.T) {
	store := newRepoStatsStore("git", func() time.Time { return time.Date(2026, 10, 2, 12, 0, 0, 0, time.Local) })
	t.Setenv("GIT_CONFIG_NOSYSTEM", "1")
	t.Setenv("HOME", t.TempDir())
	dir := makeRepo(t)
	first, err := store.measure(dir)
	if err != nil || first.Bytes != 10 || first.Files != 3 || first.Lines != 4 {
		t.Fatalf("first measure %+v %v", first, err)
	}
	path := filepath.Join(dir, "a.txt")
	writeFile(t, path, "1\n2\n3\n")
	later := time.Now().Add(2 * time.Second)
	_ = os.Chtimes(path, later, later)
	second, _ := store.measure(dir)
	if second.Bytes != 12 || second.Lines != 5 {
		t.Fatalf("edit not picked up: %+v", second)
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	third, _ := store.measure(dir)
	if third.Files != 2 || third.Bytes != 6 {
		t.Fatalf("deleted file still counted: %+v", third)
	}
	if _, err := store.measure(t.TempDir()); err == nil || err.Error() != "git ls-files failed" {
		t.Fatalf("non-repo: %v", err)
	}
}

func TestRatchetPrepareAndRun(t *testing.T) {
	f := newFixture(t)
	dir := makeRepo(t)
	noGit := t.TempDir()
	writeSettings(t, f.core, repoSettings("alice", []repoEntry{
		{owner: "alice", name: "tool", localPath: dir, cloneURL: "https://app.forkmesh.com/alice/tool"},
		{owner: "alice", name: "plain", localPath: noGit, cloneURL: "https://app.forkmesh.com/alice/plain"},
	}, nil))
	f.expectError("stats.ratchet.prepare", map[string]any{"repo": "alice/tool"}, "bad_request")
	f.expectError("stats.ratchet.prepare", map[string]any{"repo": "alice/tool", "enabled": "yes"}, "bad_request")
	f.expectError("stats.ratchet.prepare", map[string]any{"repo": "alice/nope", "enabled": true}, "unknown_repo")
	f.expectError("stats.ratchet.prepare", map[string]any{"repo": "nope", "enabled": true}, "bad_request")
	f.expectError("stats.ratchet.prepare", map[string]any{"repo": "alice/plain", "enabled": true}, "no_checkout")

	prepared := f.mustCall("stats.ratchet.prepare", map[string]any{"repo": "alice/tool", "enabled": true})
	token, _ := prepared["token"].(string)
	if !regexp.MustCompile(`^[0-9a-f]{48}$`).MatchString(token) {
		t.Fatalf("token %q", token)
	}
	assertJSON(t, prepared, map[string]any{
		"token":       token,
		"action":      "ratchet.on",
		"summary":     "Turn on Ratchet Mode for alice/tool on the desktop: commits may not grow the repository past the size it is now.",
		"expiresAt":   float64(f.now.UnixMilli() + 60000),
		"expiresInMs": 60000.0,
	})

	other := coretest.NewPeer("tablet", rpc.LAN)
	other.Dev.User = "alice"
	if _, err := coretest.Call(t, f.core, other, "stats.ratchet.run", map[string]any{"token": token}); rpc.Code(err) != "confirmation_mismatch" {
		t.Fatalf("other device: %v", err)
	}
	f.expectError("stats.ratchet.run", map[string]any{"token": token}, "confirmation_required")
	f.expectError("stats.ratchet.run", map[string]any{}, "confirmation_required")

	prepared = f.mustCall("stats.ratchet.prepare", map[string]any{"repo": "alice/tool", "enabled": true})
	f.now = f.now.Add(61 * time.Second)
	f.expectError("stats.ratchet.run", map[string]any{"token": prepared["token"]}, "confirmation_expired")
	f.expectError("stats.ratchet.run", map[string]any{"token": prepared["token"]}, "confirmation_required")
	f.now = time.Date(2026, 10, 2, 12, 0, 0, 0, time.Local)

	prepared = f.mustCall("stats.ratchet.prepare", map[string]any{"repo": "alice/tool", "enabled": true})
	result := f.mustCall("stats.ratchet.run", map[string]any{"token": prepared["token"]})
	assertJSON(t, result, map[string]any{"commits": 2.0, "files": 3.0, "lines": 4.0, "bytes": 10.0, "day": "2026-10-02",
		"ratchet": map[string]any{"enabled": true, "note": "Commits may not grow the repository past 10 B (set 2026-10-02).",
			"ceilingBytes": 10.0, "ceilingDay": "2026-10-02"}})
	if got := git(t, dir, nil, "config", "--local", "--get", ratchetEnabledKey); got != "true" {
		t.Fatalf("ratchet config %q", got)
	}
	event := f.phone.Next(t, "stats.changed", 2*time.Second)
	assertJSON(t, decode(t, event), map[string]any{"what": "ratchet", "repo": "alice/tool"})

	prepared = f.mustCall("stats.ratchet.prepare", map[string]any{"repo": "alice/tool", "enabled": false})
	if prepared["action"] != "ratchet.off" || !strings.HasPrefix(prepared["summary"].(string), "Turn off Ratchet Mode for alice/tool") {
		t.Fatalf("off prepare %v", prepared)
	}
	result = f.mustCall("stats.ratchet.run", map[string]any{"token": prepared["token"]})
	assertJSON(t, result["ratchet"], map[string]any{"enabled": false, "note": ""})
	if _, err := exec.Command("git", "-C", dir, "config", "--local", "--get", ratchetCeilingKey).Output(); err == nil {
		t.Fatalf("ceiling should be unset")
	}
}

func TestRatchetNoteAndBytes(t *testing.T) {
	cases := map[int64]string{0: "0 B", 1023: "1023 B", 1024: "1.0 KB", 1536: "1.5 KB", 5 << 20: "5.0 MB", 3 << 30: "3.0 GB", -5: "0 B"}
	for in, want := range cases {
		if got := byteText(in); got != want {
			t.Fatalf("byteText(%d)=%q want %q", in, got, want)
		}
	}
	if got := ratchetNote(repoFacts{RatchetEnabled: true}); got != "Commits may not grow the repository." {
		t.Fatal(got)
	}
	if got := ratchetNote(repoFacts{RatchetEnabled: true, CeilingBytes: 2048, CeilingDay: "2026-09-30T00:00"}); got != "Commits may not grow the repository past 2.0 KB (set 2026-09-30)." {
		t.Fatal(got)
	}
}

func TestRelayScope(t *testing.T) {
	f := newFixture(t)
	writeSettings(t, f.core, repoSettings("alice", nil, map[string][]string{
		"servers": {},
	}))
	s := f.module.settings()
	scope := loadRelayScope(s)
	if scope.active != "https://app.forkmesh.com" || len(scope.configured) != 1 {
		t.Fatalf("scope %+v", scope)
	}
	cases := map[string]string{
		"wss://App.ForkMesh.com:443/api/x?y#z": "https://app.forkmesh.com",
		"ws://127.0.0.1:8787/api":              "http://127.0.0.1:8787",
		"http://example.com:80/":               "http://example.com",
		"app.forkmesh.com":                     "",
		"":                                     "",
	}
	for in, want := range cases {
		if got := normalizedOrigin(in); got != want {
			t.Fatalf("normalizedOrigin(%q)=%q want %q", in, got, want)
		}
	}
	if got := canonicalServerURL("localhost:8787/x"); got != "ws://localhost:8787"+roomPath {
		t.Fatal(got)
	}
	scope.lists = map[string]map[string]bool{"https://other.example": {"bob/legacy": true}}
	scope.configured = append(scope.configured, "https://other.example")
	if got := scope.resolveOrigin("", "", []string{"bob/legacy"}); got != "https://other.example" {
		t.Fatalf("listed legacy: %q", got)
	}
	scope.lists["https://app.forkmesh.com"] = map[string]bool{"bob/legacy": true}
	if got := scope.resolveOrigin("", "", []string{"bob/legacy"}); got != "https://app.forkmesh.com" {
		t.Fatalf("doubly listed falls back to default: %q", got)
	}
	if got := scope.resolveOrigin("https://other.example/", "", nil); got != "https://other.example" {
		t.Fatalf("stamp is final: %q", got)
	}
	if got := accountNameFromInput("  9Bob_Smith--- ", "owner"); got != "bobsmith" {
		t.Fatalf("account name %q", got)
	}
	if got := accountNameFromInput("___", "owner"); got != "owner" {
		t.Fatal(got)
	}

	writeSettings(t, f.core, map[string][]string{
		"servers":      {"active=1", `items="[{\"url\":\"wss://app.forkmesh.com/x\"},{\"url\":\"wss://Relay.Example:8443/x\"}]"`},
		"repositories": {`relayLists="{\"https://relay.example:8443\":[\"Bob/Legacy\"],\"https://gone.example\":[\"bob/legacy\"]}"`},
	})
	scope = loadRelayScope(f.module.settings())
	if scope.active != "https://relay.example:8443" {
		t.Fatalf("active %q", scope.active)
	}
	if _, ok := scope.lists["https://gone.example"]; ok {
		t.Fatalf("unconfigured relay list kept")
	}
	if !scope.lists["https://relay.example:8443"]["bob/legacy"] {
		t.Fatalf("relay list %+v", scope.lists)
	}
}

func assertJSON(t *testing.T, got, want any) {
	t.Helper()
	g, _ := json.Marshal(got)
	w, _ := json.Marshal(want)
	if string(g) != string(w) {
		t.Fatalf("JSON mismatch\n got %s\nwant %s", g, w)
	}
}

func decode(t *testing.T, raw json.RawMessage) map[string]any {
	t.Helper()
	var out map[string]any
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatal(err)
	}
	return out
}
