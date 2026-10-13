package files

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"testing"
)

func writeFile(t *testing.T, path string, data []byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
}

func symlink(t *testing.T, target, link string) {
	t.Helper()
	if err := os.Symlink(target, link); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
}

func jsonOf(t *testing.T, value any) string {
	t.Helper()
	raw, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

func TestNormalizeRelativePath(t *testing.T) {
	cases := []struct {
		in   string
		want string
		ok   bool
	}{
		{"", "", true},
		{".", "", true},
		{"a/./b//c", "a/b/c", true},
		{"a/b/", "a/b", true},
		{"../x", "", false},
		{"a/../b", "", false},
		{"/etc/passwd", "", false},
		{"a\\b", "", false},
		{"a\nb", "", false},
		{"a\x7fb", "", false},
		{"..hidden", "..hidden", true},
		{strings.Repeat("a", 255), strings.Repeat("a", 255), true},
		{strings.Repeat("a", 256), "", false},
		{strings.Repeat("a/", 512), strings.TrimSuffix(strings.Repeat("a/", 512), "/"), true},
		{strings.Repeat("a/", 512) + "b", "", false},
		{"é/ü", "é/ü", true},
	}
	for _, c := range cases {
		got, ok := normalizeRelativePath(c.in)
		if got != c.want || ok != c.ok {
			t.Errorf("normalize(%q) = %q,%v want %q,%v", c.in, got, ok, c.want, c.ok)
		}
	}
}

func TestSensitiveNames(t *testing.T) {
	cases := map[string]bool{
		".env": true, ".ENV": true, ".env.local": true, ".envrc": false, ".netrc": true,
		".npmrc": true, ".pypirc": true, ".git-credentials": true, "credentials.json": true,
		"id_rsa": true, "id_rsa.pub": true, "id_ed25519": true, "id_ecdsa_sk": true,
		"server.PEM": true, "tls.key": true, "a.p12": true, "b.pfx": true, "c.keystore": true,
		"d.jks": true, "README.md": false, "keys.txt": false, "monkey": false,
	}
	for name, want := range cases {
		if got := isSensitiveName(name); got != want {
			t.Errorf("isSensitiveName(%q) = %v want %v", name, got, want)
		}
	}
}

func TestValidRootID(t *testing.T) {
	cases := map[string]bool{
		"f/forkmesh":                      true,
		"f/forkmesh#feature-x":            true,
		"f/.dot":                          true,
		"f/../x":                          false,
		"/etc":                            false,
		"f":                               false,
		"-f/x":                            false,
		"f/x#":                            false,
		"f/x#a#b":                         false,
		"f/x y":                           false,
		"f/x/y":                           false,
		"f/a..b":                          false,
		strings.Repeat("o", 64) + "/x":    true,
		strings.Repeat("o", 65) + "/x":    false,
		"f/" + strings.Repeat("n", 100):   true,
		"f/" + strings.Repeat("n", 101):   false,
		"f/x#" + strings.Repeat("w", 128): true,
		"f/x#" + strings.Repeat("w", 129): false,
	}
	for id, want := range cases {
		if got := validRootID(id); got != want {
			t.Errorf("validRootID(%q) = %v want %v", id, got, want)
		}
	}
}

func sandbox(t *testing.T) (string, string) {
	t.Helper()
	outside := t.TempDir()
	dir := t.TempDir()
	root := filepath.Join(dir, "repo")
	writeFile(t, filepath.Join(root, "src", "main.cpp"), []byte("int main() {}\n"))
	writeFile(t, filepath.Join(root, ".git", "config"), []byte("[remote]\nurl = https://token@x\n"))
	writeFile(t, filepath.Join(root, ".env"), []byte("SECRET=1\n"))
	writeFile(t, filepath.Join(root, "bin.dat"), []byte{0, 1, 2})
	writeFile(t, filepath.Join(outside, "secret.txt"), []byte("outside\n"))
	symlink(t, outside, filepath.Join(root, "escape"))
	symlink(t, filepath.Join(root, "src"), filepath.Join(root, "srclink"))
	symlink(t, filepath.Join(root, ".git"), filepath.Join(root, "gitlink"))
	return dir, root
}

func TestResolveInside(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("symlink fixture")
	}
	dir, root := sandbox(t)
	symlink(t, filepath.Join(root, "missing-target"), filepath.Join(root, "dangling"))
	canonicalRoot := canonicalPath(root)
	cases := []struct {
		root, rel, want, err string
	}{
		{root, "src/main.cpp", canonicalRoot + "/src/main.cpp", ""},
		{root, "", canonicalRoot, ""},
		{root, "./src//", canonicalRoot + "/src", ""},
		{root, "escape/secret.txt", "", "outside_root"},
		{root, "escape", "", "outside_root"},
		{root, ".git/config", "", "not_allowed"},
		{root, ".GIT", "", "not_allowed"},
		{root, "gitlink/config", "", "not_allowed"},
		{root, "gitlink", "", "not_allowed"},
		{root, "srclink/main.cpp", canonicalRoot + "/src/main.cpp", ""},
		{root, "missing", "", "not_found"},
		{root, "dangling", "", "not_found"},
		{root, "../repo", "", "bad_path"},
		{root, "/etc", "", "bad_path"},
		{filepath.Join(dir, "nope"), "", "", "root_unavailable"},
		{filepath.Join(dir, "nope"), "../x", "", "root_unavailable"},
	}
	for _, c := range cases {
		got, err := resolveInside(c.root, c.rel)
		if got != c.want || err != c.err {
			t.Errorf("resolveInside(%q) = %q,%q want %q,%q", c.rel, got, err, c.want, c.err)
		}
	}
}

func TestListDirectory(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("symlink fixture")
	}
	_, root := sandbox(t)
	writeFile(t, filepath.Join(root, "Zeta.txt"), []byte("z"))
	writeFile(t, filepath.Join(root, "alpha.txt"), []byte("alpha"))
	writeFile(t, filepath.Join(root, "server.pem"), []byte("pem"))
	writeFile(t, filepath.Join(root, "Docs", "x"), []byte("x"))
	symlink(t, filepath.Join(root, "alpha.txt"), filepath.Join(root, "alias.txt"))
	symlink(t, filepath.Join(root, "nowhere"), filepath.Join(root, "broken"))
	symlink(t, filepath.Join(root, ".env"), filepath.Join(root, "id_rsa_link"))
	got := listDirectory(root, canonicalPath(root), 100)
	type row struct {
		name, kind string
		size       int64
		escapes    bool
		sensitive  bool
	}
	want := []row{
		{"Docs", "dir", 0, false, false},
		{"gitlink", "dirlink", 0, false, false},
		{"src", "dir", 0, false, false},
		{"srclink", "dirlink", 0, false, false},
		{".env", "file", 9, false, true},
		{"alias.txt", "symlink", 5, false, false},
		{"alpha.txt", "file", 5, false, false},
		{"bin.dat", "file", 3, false, false},
		{"broken", "symlink", 0, true, false},
		{"escape", "symlink", 0, true, false},
		{"id_rsa_link", "symlink", 9, false, true},
		{"server.pem", "file", 3, false, true},
		{"Zeta.txt", "file", 1, false, false},
	}
	if got.total != len(want) || got.truncated || len(got.entries) != len(want) {
		t.Fatalf("total %d truncated %v entries %d: %s", got.total, got.truncated, len(got.entries), jsonOf(t, got.entries))
	}
	for i, w := range want {
		e := got.entries[i]
		_, hasEscapes := e["escapes"]
		_, hasSensitive := e["sensitive"]
		if e["name"] != w.name || e["kind"] != w.kind || e["size"] != w.size || hasEscapes != w.escapes || hasSensitive != w.sensitive {
			t.Errorf("row %d = %s want %+v", i, jsonOf(t, e), w)
		}
		if _, ok := e["modifiedMs"].(int64); !ok {
			t.Errorf("row %d modifiedMs missing", i)
		}
		keys := []string{}
		for k := range e {
			keys = append(keys, k)
		}
		if len(keys) != 4+boolInt(w.escapes)+boolInt(w.sensitive) {
			t.Errorf("row %d keys %v", i, keys)
		}
	}
	if got.entries[1]["modifiedMs"] == int64(0) {
		t.Errorf("dirlink modifiedMs should follow the target")
	}
	if got.entries[8]["modifiedMs"] != int64(0) {
		t.Errorf("broken link modifiedMs = %v", got.entries[8]["modifiedMs"])
	}
	small := listDirectory(root, canonicalPath(root), 2)
	if !small.truncated || len(small.entries) != 2 || small.total != len(want) {
		t.Errorf("bounded listing: %+v", small)
	}
	empty := listDirectory(root, filepath.Join(root, "nothing"), 10)
	if jsonOf(t, empty.entries) != "[]" || empty.total != 0 {
		t.Errorf("missing directory listing: %+v", empty)
	}
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

func TestLooksBinary(t *testing.T) {
	cases := []struct {
		name string
		data []byte
		want bool
	}{
		{"empty", nil, false},
		{"text", []byte("hello\n"), false},
		{"nul", []byte("a\x00b"), true},
		{"utf8", []byte("héllo ✓"), false},
		{"four invalid", []byte("a\xff\xfe\xfd\xfcz"), false},
		{"five invalid", []byte("a\xff\xfe\xfd\xfc\xfbz"), true},
		{"truncated tail", []byte("abc\xe2\x82"), false},
		{"genuine replacement counts once invalid", []byte("\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xff"), true},
		{"genuine replacement alone", []byte("\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"), false},
		{"surrogate bytes", []byte("\xed\xa0\x80\xed\xa0\x80"), true},
		{"overlong", []byte("\xe0\x80\x80x"), false},
	}
	for _, c := range cases {
		if got := looksBinary(c.data); got != c.want {
			t.Errorf("%s: looksBinary = %v want %v", c.name, got, c.want)
		}
	}
}

func TestReadFileSlice(t *testing.T) {
	dir := t.TempDir()
	text := filepath.Join(dir, "main.cpp")
	writeFile(t, text, []byte("int main() {}\n"))
	bin := filepath.Join(dir, "bin.dat")
	writeFile(t, bin, []byte{0, 1, 2})
	utf := filepath.Join(dir, "utf.txt")
	writeFile(t, utf, []byte("a\xc3\xa9z"))
	tail := filepath.Join(dir, "tail.txt")
	writeFile(t, tail, []byte("ab\xc3\xa9\xe2\x9c\x93!"))
	cases := []struct {
		name   string
		path   string
		offset int64
		length int64
		want   string
		err    string
	}{
		{"text", text, 0, 1 << 20, `{"binary":false,"eof":true,"length":14,"offset":0,"size":14,"text":"int main() {}\n"}`, ""},
		{"binary", bin, 0, 10, `{"base64":"AAEC","binary":true,"eof":true,"length":3,"offset":0,"size":3}`, ""},
		{"slice", text, 4, 3, `{"binary":false,"eof":false,"length":3,"offset":4,"size":14,"text":"mai"}`, ""},
		{"never splits", utf, 0, 2, `{"binary":false,"eof":false,"length":1,"offset":0,"size":4,"text":"a"}`, ""},
		{"trims complete trailing character", tail, 0, 4, `{"binary":false,"eof":false,"length":2,"offset":0,"size":8,"text":"ab"}`, ""},
		{"trims three byte", tail, 0, 7, `{"binary":false,"eof":false,"length":4,"offset":0,"size":8,"text":"abé"}`, ""},
		{"eof keeps tail", tail, 0, 8, `{"binary":false,"eof":true,"length":8,"offset":0,"size":8,"text":"abé✓!"}`, ""},
		{"lone continuation start", utf, 2, 1, `{"binary":false,"eof":false,"length":1,"offset":2,"size":4,"text":"�"}`, ""},
		{"offset past end", text, 99, 10, `{"binary":false,"eof":true,"length":0,"offset":14,"size":14,"text":""}`, ""},
		{"negative offset", text, -5, 3, `{"binary":false,"eof":false,"length":3,"offset":0,"size":14,"text":"int"}`, ""},
		{"negative length", text, 0, -1, `{"binary":false,"eof":false,"length":0,"offset":0,"size":14,"text":""}`, ""},
		{"missing", filepath.Join(dir, "missing"), 0, 1, "", "unreadable"},
	}
	for _, c := range cases {
		got, err := readFileSlice(c.path, c.offset, c.length)
		if err != c.err {
			t.Errorf("%s: err %q want %q", c.name, err, c.err)
			continue
		}
		if err != "" {
			continue
		}
		var want map[string]any
		if e := json.Unmarshal([]byte(c.want), &want); e != nil {
			t.Fatal(e)
		}
		var have map[string]any
		_ = json.Unmarshal([]byte(jsonOf(t, got)), &have)
		if !reflect.DeepEqual(have, want) {
			t.Errorf("%s: got %s want %s", c.name, jsonOf(t, got), c.want)
		}
	}
	big := filepath.Join(dir, "big.txt")
	writeFile(t, big, []byte(strings.Repeat("x", maxReadBytes+10)))
	got, _ := readFileSlice(big, 0, 1<<30)
	if got["length"] != int64(maxReadBytes) || got["eof"] != false {
		t.Errorf("read cap: length %v eof %v", got["length"], got["eof"])
	}
}

func TestDiscoverWorktrees(t *testing.T) {
	dir := t.TempDir()
	repo := filepath.Join(dir, "repo")
	wt := filepath.Join(dir, "wt", "feature-x")
	writeFile(t, filepath.Join(wt, ".git"), []byte("gitdir: x\n"))
	writeFile(t, filepath.Join(repo, ".git", "worktrees", "feature-x", "gitdir"), []byte(wt+"/.git\n"))
	writeFile(t, filepath.Join(repo, ".git", "worktrees", "feature-x", "HEAD"), []byte("ref: refs/heads/agent/feature-x\n"))
	writeFile(t, filepath.Join(repo, ".git", "worktrees", "gone", "gitdir"), []byte(filepath.Join(dir, "missing", ".git")))
	detached := filepath.Join(dir, "wt", "detached")
	writeFile(t, filepath.Join(detached, ".git"), []byte("gitdir: y\n"))
	writeFile(t, filepath.Join(repo, ".git", "worktrees", "detached", "gitdir"), []byte(detached+"/.git"))
	writeFile(t, filepath.Join(repo, ".git", "worktrees", "detached", "HEAD"), []byte("0123456789abcdef\n"))
	writeFile(t, filepath.Join(repo, ".git", "worktrees", "nohead", "gitdir"), []byte(detached+"/.git"))
	writeFile(t, filepath.Join(repo, ".git", "worktrees", ".hidden", "gitdir"), []byte(detached+"/.git"))
	writeFile(t, filepath.Join(repo, ".git", "worktrees", "bad name", "gitdir"), []byte(detached+"/.git"))
	writeFile(t, filepath.Join(repo, ".git", "worktrees", "empty", "gitdir"), []byte("  \n"))
	roots := discoverWorktrees("f/forkmesh", repo, 10)
	got := []string{}
	for _, r := range roots {
		got = append(got, jsonOf(t, r.row())+" "+filepath.ToSlash(r.path))
	}
	want := []string{
		`{"branch":"0123456","id":"f/forkmesh#detached","kind":"worktree","label":"0123456","repo":"f/forkmesh"} ` + filepath.ToSlash(detached),
		`{"branch":"agent/feature-x","id":"f/forkmesh#feature-x","kind":"worktree","label":"agent/feature-x","repo":"f/forkmesh"} ` + filepath.ToSlash(wt),
		`{"branch":"","id":"f/forkmesh#nohead","kind":"worktree","label":"nohead","repo":"f/forkmesh"} ` + filepath.ToSlash(detached),
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("worktrees\n got %v\nwant %v", got, want)
	}
	if limited := discoverWorktrees("f/forkmesh", repo, 1); len(limited) != 1 {
		t.Errorf("max worktrees not honoured: %d", len(limited))
	}
	if none := discoverWorktrees("f/forkmesh", wt, 10); len(none) != 0 {
		t.Errorf("a .git file is not a repository: %v", none)
	}
}

func TestHeadBranch(t *testing.T) {
	dir := t.TempDir()
	cases := []struct{ head, want string }{
		{"ref: refs/heads/main\n", "main"},
		{"ref: refs/heads/agent/x", "agent/x"},
		{"0123456789abcdef\n", "0123456"},
		{"abc", "abc"},
		{"ref: refs/remotes/origin/main", "ref: re"},
	}
	for i, c := range cases {
		repo := filepath.Join(dir, string(rune('a'+i)))
		writeFile(t, filepath.Join(repo, ".git", "HEAD"), []byte(c.head))
		if got := headBranch(repo); got != c.want {
			t.Errorf("headBranch(%q) = %q want %q", c.head, got, c.want)
		}
	}
	if got := headBranch(filepath.Join(dir, "none")); got != "" {
		t.Errorf("missing HEAD = %q", got)
	}
}

func TestParsePorcelainStatus(t *testing.T) {
	status := strings.Join([]string{
		"## main...origin/main [ahead 2, behind 1]",
		" M src/a.cpp",
		"M  src/b.cpp",
		"R  new.cpp", "old.cpp",
		"?? notes.txt",
		"UU conflict.cpp",
	}, "\x00") + "\x00"
	got := jsonOf(t, parsePorcelainStatus([]byte(status), 3))
	want := `{"ahead":2,"behind":1,"branch":"main","conflicted":1,"files":[` +
		`{"index":" ","kind":"modified","path":"src/a.cpp","worktree":"M"},` +
		`{"index":"M","kind":"modified","path":"src/b.cpp","worktree":" "},` +
		`{"from":"old.cpp","index":"R","kind":"renamed","path":"new.cpp","worktree":" "}],` +
		`"staged":2,"total":5,"truncated":true,"unstaged":1,"untracked":1,"upstream":"origin/main"}`
	if got != want {
		t.Errorf("status\n got %s\nwant %s", got, want)
	}
	cases := []struct {
		name string
		in   string
		want string
	}{
		{"no upstream", "## topic\x00", `{"ahead":0,"behind":0,"branch":"topic","conflicted":0,"files":[],"staged":0,"total":0,"truncated":false,"unstaged":0,"untracked":0,"upstream":""}`},
		{"no commits", "## No commits yet on main\x00A  x\x00", `{"ahead":0,"behind":0,"branch":"main","conflicted":0,"files":[{"index":"A","kind":"added","path":"x","worktree":" "}],"staged":1,"total":1,"truncated":false,"unstaged":0,"untracked":0,"upstream":""}`},
		{"gone", "## main...origin/main [gone]\x00", `{"ahead":0,"behind":0,"branch":"main","conflicted":0,"files":[],"staged":0,"total":0,"truncated":false,"unstaged":0,"untracked":0,"upstream":"origin/main"}`},
		{"kinds", " D gone.txt\x00AA both\x00DD del\x00C  c2\x00c1\x00AM am\x00MM mm\x00", `{"ahead":0,"behind":0,"branch":"","conflicted":2,"files":[{"index":" ","kind":"deleted","path":"gone.txt","worktree":"D"},{"index":"A","kind":"conflicted","path":"both","worktree":"A"},{"index":"D","kind":"conflicted","path":"del","worktree":"D"},{"from":"c1","index":"C","kind":"copied","path":"c2","worktree":" "},{"index":"A","kind":"added","path":"am","worktree":"M"},{"index":"M","kind":"modified","path":"mm","worktree":"M"}],"staged":3,"total":6,"truncated":false,"unstaged":3,"untracked":0,"upstream":""}`},
		{"short entries skipped", "M \x00?? \x00?? a\x00", `{"ahead":0,"behind":0,"branch":"","conflicted":0,"files":[{"index":"?","kind":"untracked","path":"a","worktree":"?"}],"staged":0,"total":1,"truncated":false,"unstaged":0,"untracked":1,"upstream":""}`},
		{"empty", "", `{"ahead":0,"behind":0,"branch":"","conflicted":0,"files":[],"staged":0,"total":0,"truncated":false,"unstaged":0,"untracked":0,"upstream":""}`},
	}
	for _, c := range cases {
		if got := jsonOf(t, parsePorcelainStatus([]byte(c.in), 500)); got != c.want {
			t.Errorf("%s\n got %s\nwant %s", c.name, got, c.want)
		}
	}
}

func TestGitActions(t *testing.T) {
	if got := gitArguments("pull"); !reflect.DeepEqual(got, []string{"pull", "--ff-only", "--quiet"}) {
		t.Errorf("pull args %v", got)
	}
	if got := gitArguments("fetch"); !reflect.DeepEqual(got, []string{"fetch", "--quiet"}) {
		t.Errorf("fetch args %v", got)
	}
	for _, action := range []string{"push", "reset", "clean", ""} {
		if gitArguments(action) != nil || validGitAction(action) {
			t.Errorf("%q must not be offered", action)
		}
	}
	if got := gitActionSummary("fetch", "f/x"); got != `Fetch "f/x" from its remote. Nothing in the working tree changes.` {
		t.Errorf("fetch summary %q", got)
	}
	if got := gitActionSummary("pull", "main"); got != `Pull "main" from its upstream, fast-forward only. Nothing is merged or rewound; a diverged branch is left as it is.` {
		t.Errorf("pull summary %q", got)
	}
}

func TestConfirmations(t *testing.T) {
	store := newConfirmations()
	token := store.issue("dev", "pull", "F/X", 1000)
	if len(token) != 48 || strings.Trim(token, "0123456789abcdef") != "" {
		t.Fatalf("token shape %q", token)
	}
	cases := []struct {
		name    string
		token   func() string
		device  string
		action  string
		subject string
		now     int64
		want    verdict
	}{
		{"ok, subject case-insensitive", func() string { return store.issue("dev", "pull", "F/X", 1000) }, "dev", "pull", "f/x", 2000, verdictOK},
		{"empty", func() string { return "" }, "dev", "pull", "f/x", 2000, verdictMissing},
		{"unknown", func() string { return "nope" }, "dev", "pull", "f/x", 2000, verdictMissing},
		{"expired", func() string { return store.issue("dev", "pull", "f/x", 1000) }, "dev", "pull", "f/x", 61000, verdictExpired},
		{"other device", func() string { return store.issue("dev", "pull", "f/x", 1000) }, "other", "pull", "f/x", 2000, verdictMismatch},
		{"other action", func() string { return store.issue("dev", "fetch", "f/x", 1000) }, "dev", "pull", "f/x", 2000, verdictMismatch},
		{"other subject", func() string { return store.issue("dev", "pull", "f/x", 1000) }, "dev", "pull", "f/y", 2000, verdictMismatch},
	}
	for _, c := range cases {
		tok := c.token()
		if got := store.consume(tok, c.device, c.action, c.subject, c.now); got != c.want {
			t.Errorf("%s: %v want %v", c.name, got, c.want)
		}
		if tok != "" && tok != "nope" {
			if again := store.consume(tok, c.device, c.action, c.subject, c.now); again != verdictMissing {
				t.Errorf("%s: token reused: %v", c.name, again)
			}
		}
	}
	errors := map[verdict]string{verdictOK: "", verdictMissing: "confirmation_required",
		verdictExpired: "confirmation_expired", verdictMismatch: "confirmation_mismatch"}
	for v, want := range errors {
		if confirmationError(v) != want {
			t.Errorf("confirmationError(%v) = %q", v, confirmationError(v))
		}
	}
	capped := newConfirmations()
	first := capped.issue("d", "pull", "f/x", 0)
	for i := 1; i < 40; i++ {
		capped.issue("d", "pull", "f/x", int64(i))
	}
	if capped.outstanding() != maxOutstandingTokens {
		t.Errorf("outstanding %d", capped.outstanding())
	}
	if capped.consume(first, "d", "pull", "f/x", 50) != verdictMissing {
		t.Errorf("oldest token should be dropped at the cap")
	}
}
