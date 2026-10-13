package diag

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestSeverityOf(t *testing.T) {
	cases := map[string]string{
		"2026-10-02 10:00:00  Push failed: remote hung up": "error",
		"Fatal: nothing":                       "error",
		"connection refused by peer":           "error",
		"Retrying the fetch":                   "warning",
		"request timed out after 5 s":          "warning",
		"UI stalled for 2 s":                   "warning",
		"errorless run":                        "info",
		"all good":                             "info",
		"warn: careful, also an error here":    "error",
		"the mirror is degraded but serving":   "warning",
		"terrorist is not error-prone? errors": "error",
	}
	for line, want := range cases {
		if got := severityOf(line); got != want {
			t.Errorf("severityOf(%q) = %q, want %q", line, got, want)
		}
	}
}

func TestTimestampOf(t *testing.T) {
	want := time.Date(2026, 10, 2, 19, 56, 4, 0, time.Local).UnixMilli()
	if got := timestampOf("2026-10-02 19:56:04  Node n1 core query"); got != want {
		t.Fatalf("timestampOf = %d, want %d", got, want)
	}
	if timestampOf("short") != 0 || timestampOf("not a timestamp at all, sorry") != 0 {
		t.Fatal("non-stamped lines must give 0")
	}
}

func TestRedactSecrets(t *testing.T) {
	cases := map[string]string{
		"token=abcdef123456 next":                 "token=[redacted] next",
		`"api_key": "zzzzzzzz"`:                   `"api_key": "[redacted]"`,
		"Authorization: Bearer abcdefghij":        "Authorization: [redacted] abcdefghij",
		"key sk-abcdefghijklmnop done":            "key [redacted] done",
		"push ghp_abcdefghijklmnopqrstuvwxyz ok":  "push [redacted] ok",
		"-----BEGIN RSA PRIVATE KEY----- MIIE...": "[redacted private key]",  // forkmesh-secret-scan:ignore-line
		"password=short":                          "password[redacted]",
		"password=abc":                            "password=abc",
	}
	for in, want := range cases {
		if got := redactSecrets(in); got != want {
			t.Errorf("redactSecrets(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestLeftCountsUTF16(t *testing.T) {
	if got := left("héllo", 2); got != "hé" {
		t.Fatalf("left = %q", got)
	}
	if got := left("a\U0001F600b", 2); got != "a" {
		t.Fatalf("left must not split a surrogate pair, got %q", got)
	}
	if got := left("a\U0001F600b", 3); got != "a\U0001F600" {
		t.Fatalf("left = %q", got)
	}
	if utf16Len("a\U0001F600") != 3 {
		t.Fatal("utf16Len")
	}
	if fromUTF8([]byte{'a', 0xff, 'b'}) != "a�b" {
		t.Fatal("fromUTF8 must replace invalid bytes")
	}
}

func writeLog(t *testing.T, lines ...string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "network_log.txt")
	if err := os.WriteFile(path, []byte(strings.Join(lines, "\n")+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestLogPageTail(t *testing.T) {
	path := writeLog(t,
		"2026-10-02 10:00:00  first",
		"",
		"2026-10-02 10:00:01  push failed\r",
		"2026-10-02 10:00:02  retrying with token=abcdefghijk",
		"2026-10-02 10:00:03  done")
	page := logPage(path, -1, 2, "", "")
	if page.Scanned != 4 || len(page.Lines) != 2 || !page.More {
		t.Fatalf("page = %+v", page)
	}
	if page.Lines[0].Text != "2026-10-02 10:00:02  retrying with token=[redacted]" || page.Lines[0].Severity != "warning" {
		t.Fatalf("line = %+v", page.Lines[0])
	}
	if page.Lines[1].Text != "2026-10-02 10:00:03  done" || page.Lines[1].Severity != "info" {
		t.Fatalf("line = %+v", page.Lines[1])
	}
	info, _ := os.Stat(path)
	if page.End != info.Size() || page.Start != page.Lines[0].Offset {
		t.Fatalf("bounds %d..%d", page.Start, page.End)
	}
	older := logPage(path, page.Start, 10, "", "")
	if len(older.Lines) != 2 || older.More || older.Start != 0 || older.End != page.Start {
		t.Fatalf("older = %+v", older)
	}
	if older.Lines[1].Text != "2026-10-02 10:00:01  push failed" || older.Lines[1].Severity != "error" {
		t.Fatalf("CR must be chopped: %+v", older.Lines[1])
	}
	if older.Lines[1].Offset != int64(len("2026-10-02 10:00:00  first\n\n")) {
		t.Fatalf("offset = %d", older.Lines[1].Offset)
	}
}

func TestLogPageFilters(t *testing.T) {
	path := writeLog(t, "alpha ok", "beta failed", "gamma slow", "ALPHA warn")
	if page := logPage(path, -1, 50, "  alpha ", ""); len(page.Lines) != 2 || page.Scanned != 4 {
		t.Fatalf("query page = %+v", page)
	}
	if page := logPage(path, -1, 50, "", "warning"); len(page.Lines) != 3 {
		t.Fatalf("warning page = %+v", page)
	}
	if page := logPage(path, -1, 50, "", "error"); len(page.Lines) != 1 || page.Lines[0].Text != "beta failed" {
		t.Fatalf("error page = %+v", page)
	}
	if page := logPage(path, -1, 50, "", "all"); len(page.Lines) != 4 {
		t.Fatalf("all page = %+v", page)
	}
	if page := logPage(path, -1, 50, "", "bogus"); len(page.Lines) != 0 || page.Start != 0 || page.More {
		t.Fatalf("bogus page = %+v", page)
	}
	if page := logPage(path, -1, 0, "", ""); len(page.Lines) != 1 {
		t.Fatalf("lines clamp to at least one: %+v", page)
	}
	if page := logPage(filepath.Join(t.TempDir(), "missing"), -1, 5, "", ""); page.Lines == nil || len(page.Lines) != 0 || page.End != 0 {
		t.Fatalf("missing = %+v", page)
	}
}

func TestLogPageScanWindow(t *testing.T) {
	long := strings.Repeat("x", 1000)
	var lines []string
	for i := 0; i < 700; i++ {
		lines = append(lines, long)
	}
	lines = append(lines, "last line")
	path := writeLog(t, lines...)
	page := logPage(path, -1, 400, "", "")
	if len(page.Lines) != 400 || page.Lines[399].Text != "last line" || !page.More {
		t.Fatalf("lines %d more %v", len(page.Lines), page.More)
	}
	if page.Scanned > 600 {
		t.Fatalf("scan window exceeded: %d", page.Scanned)
	}
	for _, line := range page.Lines[:399] {
		if line.Offset%1001 != 0 {
			t.Fatalf("partial first line kept at %d", line.Offset)
		}
	}
	single := filepath.Join(t.TempDir(), "one")
	if err := os.WriteFile(single, []byte(strings.Repeat("y", maxLogScanSize+10)), 0o600); err != nil {
		t.Fatal(err)
	}
	page = logPage(single, -1, 10, "", "")
	if len(page.Lines) != 0 || !page.More || page.Start != page.End || page.End != maxLogScanSize+10 {
		t.Fatalf("no-newline window = %+v", page)
	}
}
