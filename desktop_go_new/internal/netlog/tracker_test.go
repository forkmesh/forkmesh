package netlog

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

var testZone = time.FixedZone("UTC+2", 2*3600)

func useTestZone(t *testing.T) {
	t.Helper()
	saved := time.Local
	time.Local = testZone
	t.Cleanup(func() { time.Local = saved })
}

func at(text string) time.Time {
	parsed, err := time.ParseInLocation(storedFormat, text, testZone)
	if err != nil {
		panic(err)
	}
	return parsed
}

func ms(text string) int64 { return at(text).UnixMilli() }

func writeFile(t *testing.T, path, text string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0o600); err != nil {
		t.Fatal(err)
	}
}

func appendFile(t *testing.T, path, text string) {
	t.Helper()
	file, err := os.OpenFile(path, os.O_APPEND|os.O_WRONLY|os.O_CREATE, 0o600)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	if _, err := file.WriteString(text); err != nil {
		t.Fatal(err)
	}
}

func lines(items ...string) string {
	return strings.Join(items, "\n") + "\n"
}

func digestText(rows ...string) string {
	return strings.Join(rows, "\n") + "\n"
}

func row(stamp, badge string, count int) string {
	return fmt.Sprintf("%d\t%s\t%d", BucketOf(ms(stamp)), badge, count)
}

func counts(d Digest, stamp, badge string) int {
	return d[BucketOf(ms(stamp))][badge]
}

func newTestTracker(t *testing.T, now string) (*Tracker, string, *time.Time) {
	t.Helper()
	useTestZone(t)
	dir := t.TempDir()
	live := LivePath(dir)
	clock := at(now)
	tracker := NewTracker(live)
	tracker.Now = func() time.Time { return clock }
	return tracker, live, &clock
}

func TestParseDigestMatchesLoadDigest(t *testing.T) {
	digest := ParseDigest("100\tERROR\t2\n100\tERROR\t3\n200\tINFO\t0\n300\tINFO\t-1\nbad\tINFO\t1\n400\tINFO\n500\tA\tB\tC\n600\tWARN\t 4 \n\n")
	if digest[100]["ERROR"] != 5 || len(digest[200]) != 0 || len(digest[300]) != 0 || digest[600]["WARN"] != 4 || len(digest) != 2 {
		t.Fatalf("digest = %v", digest)
	}
}

func TestStoredLineTimestamp(t *testing.T) {
	useTestZone(t)
	if got := StoredLineTimestampMs("2026-10-03 12:00:05  x"); got != ms("2026-10-03 12:00:05") {
		t.Fatalf("stamp = %d", got)
	}
	for _, line := range []string{"2026-10-03 12:00", "2026-02-30 12:00:00  x", "  2026-10-03 12:00:00", "continuation line of a dump"} {
		if got := StoredLineTimestampMs(line); got != -1 {
			t.Errorf("%q -> %d", line, got)
		}
	}
	if first, last, ok := SegmentSpan("/x/netlog-20261003120000-20261003130000-2.txt"); !ok || first != ms("2026-10-03 12:00:00") || last != ms("2026-10-03 13:00:00") {
		t.Fatalf("span = %d %d %v", first, last, ok)
	}
	if _, _, ok := SegmentSpan("/x/netlog-2026-x.txt"); ok {
		t.Fatal("bad span parsed")
	}
}

func TestSnapshotUsesSavedDigestBelowWatermarkAndFilesAbove(t *testing.T) {
	tracker, live, _ := newTestTracker(t, "2026-10-03 12:20:00")
	writeFile(t, DigestPath(live), digestText(
		row("2026-10-02 09:00:00", "ERROR", 7),
		row("2026-10-02 09:00:00", "INFO", 3),
		row("2026-10-03 12:00:00", "INFO", 1),
	))
	writeFile(t, live, lines(
		"2026-10-02 09:01:00  old line already in the digest",
		"2026-10-03 12:00:01  Session started",
		"2026-10-03 12:05:00  Could not reach the relay",
		"  continuation without a stamp",
		"2026-10-03 12:16:00  UI stalled for 900 ms",
	))
	d := tracker.Snapshot()
	if counts(d, "2026-10-02 09:00:00", "ERROR") != 7 || counts(d, "2026-10-02 09:00:00", "INFO") != 3 {
		t.Fatalf("base = %v", d)
	}
	if counts(d, "2026-10-03 12:00:00", "SESSION") != 1 || counts(d, "2026-10-03 12:00:00", "ERROR") != 1 || counts(d, "2026-10-03 12:00:00", "INFO") != 0 {
		t.Fatalf("watermark bucket = %v", d[BucketOf(ms("2026-10-03 12:00:00"))])
	}
	if counts(d, "2026-10-03 12:15:00", "STALL") != 1 {
		t.Fatalf("new bucket = %v", d)
	}
	again := tracker.Snapshot()
	if counts(again, "2026-10-03 12:00:00", "ERROR") != 1 || counts(again, "2026-10-03 12:15:00", "STALL") != 1 {
		t.Fatalf("second snapshot counted twice: %v", again)
	}
	appendFile(t, live, lines("2026-10-03 12:17:00  Could not reach the relay"))
	appendFile(t, live, "2026-10-03 12:17:01  half a li")
	d = tracker.Snapshot()
	if counts(d, "2026-10-03 12:15:00", "ERROR") != 1 || counts(d, "2026-10-03 12:15:00", "STALL") != 1 {
		t.Fatalf("incremental = %v", d[BucketOf(ms("2026-10-03 12:15:00"))])
	}
	appendFile(t, live, "ne that failed\n")
	d = tracker.Snapshot()
	if counts(d, "2026-10-03 12:15:00", "ERROR") != 2 {
		t.Fatalf("completed partial line = %v", d[BucketOf(ms("2026-10-03 12:15:00"))])
	}
}

func TestSnapshotFollowsRotationRewriteAndDigestSaves(t *testing.T) {
	tracker, live, clock := newTestTracker(t, "2026-10-03 12:20:00")
	writeFile(t, DigestPath(live), digestText(row("2026-10-03 10:00:00", "INFO", 4), row("2026-10-03 11:00:00", "INFO", 1)))
	writeFile(t, live, lines(
		"2026-10-03 12:01:00  Could not reach the relay",
		"2026-10-03 12:02:00  Session started",
	))
	if d := tracker.Snapshot(); counts(d, "2026-10-03 12:00:00", "ERROR") != 1 {
		t.Fatalf("before rotation = %v", d)
	}
	segment := filepath.Join(ArchiveDir(live), "netlog-20261003120100-20261003120200.txt")
	if err := os.Rename(live, segment); err != nil {
		t.Fatal(err)
	}
	writeFile(t, live, lines("2026-10-03 12:03:00  Unable to open the mirror"))
	d := tracker.Snapshot()
	if counts(d, "2026-10-03 12:00:00", "ERROR") != 2 || counts(d, "2026-10-03 12:00:00", "SESSION") != 1 || counts(d, "2026-10-03 10:00:00", "INFO") != 4 || counts(d, "2026-10-03 11:00:00", "INFO") != 0 {
		t.Fatalf("after rotation = %v", d)
	}
	writeFile(t, live+".tmp", lines("2026-10-03 12:03:00  Unable to open the mirror"))
	writeFile(t, segment+".tmp", lines("2026-10-03 12:02:00  Session started"))
	if err := os.Rename(segment+".tmp", segment); err != nil {
		t.Fatal(err)
	}
	d = tracker.Snapshot()
	if counts(d, "2026-10-03 12:00:00", "ERROR") != 1 || counts(d, "2026-10-03 12:00:00", "SESSION") != 1 {
		t.Fatalf("after a deleted line = %v", d[BucketOf(ms("2026-10-03 12:00:00"))])
	}
	writeFile(t, live, "")
	d = tracker.Snapshot()
	if counts(d, "2026-10-03 12:00:00", "ERROR") != 0 {
		t.Fatalf("after the live file was truncated = %v", d)
	}
	*clock = at("2026-10-03 12:31:00")
	writeFile(t, live, lines("2026-10-03 12:30:30  Could not reach the relay"))
	writeFile(t, DigestPath(live), digestText(
		row("2026-10-03 11:00:00", "INFO", 4),
		row("2026-10-03 12:00:00", "ERROR", 9),
		row("2026-10-03 12:30:00", "ERROR", 1),
	))
	d = tracker.Snapshot()
	if counts(d, "2026-10-03 12:00:00", "ERROR") != 9 || counts(d, "2026-10-03 12:30:00", "ERROR") != 1 {
		t.Fatalf("after the desktop saved its digest = %v", d)
	}
}

func TestSnapshotWithoutDigestRebuildsFromEveryFile(t *testing.T) {
	tracker, live, _ := newTestTracker(t, "2026-10-03 12:20:00")
	writeFile(t, filepath.Join(ArchiveDir(live), "netlog-20260920080000-20260920090000.txt"), lines(
		"2026-09-20 08:00:00  Could not reach the relay",
		"2026-09-20 09:00:00  UI stalled for 900 ms",
	))
	writeFile(t, filepath.Join(ArchiveDir(live), "netlog-20260801080000-20260801090000.txt"), lines(
		"2026-08-01 08:00:00  Could not reach the relay",
	))
	writeFile(t, live, lines("2026-10-03 12:00:00  Session started"))
	d := tracker.Snapshot()
	if counts(d, "2026-09-20 08:00:00", "ERROR") != 1 || counts(d, "2026-09-20 09:00:00", "STALL") != 1 || counts(d, "2026-10-03 12:00:00", "SESSION") != 1 {
		t.Fatalf("rebuild = %v", d)
	}
	if counts(d, "2026-08-01 08:00:00", "ERROR") != 0 {
		t.Fatal("lines past the retention window are dropped")
	}
}

func TestAppendCountsEachLineOnce(t *testing.T) {
	tracker, live, _ := newTestTracker(t, "2026-10-03 12:20:00")
	writeFile(t, DigestPath(live), digestText(row("2026-10-03 12:00:00", "INFO", 1)))
	writeFile(t, live, "")
	cloud := "2026-10-03 12:19:00  Worker tail: deploy failed upstream  [desktop/src/MainWindowLogs.cpp:3254]"
	plain := "2026-10-03 12:19:30  Could not reach the relay"
	tracker.Append([]AppendLine{{Line: cloud, Badge: "CLOUD"}, {Line: plain, Badge: "ERROR"}})
	d := tracker.Snapshot()
	if counts(d, "2026-10-03 12:15:00", "CLOUD") != 1 || counts(d, "2026-10-03 12:15:00", "ERROR") != 1 {
		t.Fatalf("pending = %v", d[BucketOf(ms("2026-10-03 12:15:00"))])
	}
	appendFile(t, live, lines(cloud, plain))
	d = tracker.Snapshot()
	if counts(d, "2026-10-03 12:15:00", "CLOUD") != 1 || counts(d, "2026-10-03 12:15:00", "ERROR") != 1 {
		t.Fatalf("after the write = %v", d[BucketOf(ms("2026-10-03 12:15:00"))])
	}
	late := "2026-10-03 12:19:40  Fleet feed line that failed"
	appendFile(t, live, lines(late))
	tracker.Snapshot()
	tracker.Append([]AppendLine{{Line: late, Badge: "CLOUD"}})
	d = tracker.Snapshot()
	if counts(d, "2026-10-03 12:15:00", "CLOUD") != 2 || counts(d, "2026-10-03 12:15:00", "ERROR") != 1 {
		t.Fatalf("notified after the read = %v", d[BucketOf(ms("2026-10-03 12:15:00"))])
	}
	multi := "2026-10-03 12:19:50  Dump follows\nline two\nline three"
	tracker.Append([]AppendLine{{Line: multi, Badge: "INFO"}})
	appendFile(t, live, multi+"\n")
	d = tracker.Snapshot()
	if counts(d, "2026-10-03 12:15:00", "INFO") != 1 {
		t.Fatalf("multi-line = %v", d[BucketOf(ms("2026-10-03 12:15:00"))])
	}
	if err := os.Rename(live, live+".gone"); err != nil {
		t.Fatal(err)
	}
	writeFile(t, live, lines(cloud, plain, late, "2026-10-03 12:19:50  Dump follows"))
	d = tracker.Snapshot()
	if counts(d, "2026-10-03 12:15:00", "CLOUD") != 2 || counts(d, "2026-10-03 12:15:00", "ERROR") != 1 {
		t.Fatalf("desktop badges survive a rewrite = %v", d[BucketOf(ms("2026-10-03 12:15:00"))])
	}
}

func TestPendingLinesExpire(t *testing.T) {
	tracker, live, clock := newTestTracker(t, "2026-10-03 12:20:00")
	writeFile(t, live, "")
	tracker.Append([]AppendLine{{Line: "2026-10-03 12:19:59  Could not write", Badge: "ERROR"}})
	if d := tracker.Snapshot(); counts(d, "2026-10-03 12:15:00", "ERROR") != 1 {
		t.Fatalf("pending = %v", d)
	}
	*clock = clock.Add(31 * time.Second)
	if d := tracker.Snapshot(); counts(d, "2026-10-03 12:15:00", "ERROR") != 0 {
		t.Fatalf("expired = %v", d)
	}
}

func TestTimelineGolden(t *testing.T) {
	useTestZone(t)
	digest := Digest{}
	digest.add(BucketOf(ms("2026-10-03 10:00:00")), "ERROR", 3)
	digest.add(BucketOf(ms("2026-10-03 10:00:00")), "INFO", 2)
	digest.add(BucketOf(ms("2026-10-03 00:00:00")), "FATAL", 1)
	digest.add(BucketOf(ms("2026-10-02 23:45:00")), "STALL", 1)
	digest.add(BucketOf(ms("2026-10-02 23:45:00")), "BGBLOCK", 2)
	digest.add(BucketOf(ms("2026-10-02 23:45:00")), "warn", 1)
	digest.add(BucketOf(ms("2026-10-02 23:45:00")), "CLOUD", 5)
	digest.add(BucketOf(ms("2026-10-01 00:00:00")), "ERROR", 4)
	digest.add(BucketOf(ms("2026-09-30 23:45:00")), "ERROR", 100)
	digest.add(BucketOf(ms("2026-10-04 00:00:00")), "ERROR", 100)
	digest.add(BucketOf(ms("2026-10-02 08:00:00")), "INFO", -3)
	got, _ := json.Marshal(Timeline(digest, at("2026-10-03 15:30:00"), 3))
	want := `{"days":[` +
		`{"dayTs":1790805600000,"errors":4,"info":0,"warnings":0},` +
		`{"dayTs":1790892000000,"errors":0,"info":5,"warnings":4},` +
		`{"dayTs":1790978400000,"errors":4,"info":2,"warnings":0}]}`
	if string(got) != want {
		t.Fatalf("timeline\n got %s\nwant %s", got, want)
	}
	full := Timeline(Digest{}, at("2026-10-03 15:30:00"), 30)["days"].([]any)
	if len(full) != 30 || full[29].(map[string]any)["dayTs"].(float64) != 1790978400000 {
		t.Fatalf("30 days = %v", full)
	}
}

func TestClampDays(t *testing.T) {
	cases := map[string]int{"": 30, "null": 30, `"5"`: 30, "true": 30, "7": 7, "7.9": 7, "0": 1, "-4": 1, "0.5": 1, "31": 30, "1e300": 30, "1": 1}
	for raw, want := range cases {
		if got := ClampDays(json.RawMessage(raw)); got != want {
			t.Errorf("ClampDays(%s) = %d, want %d", raw, got, want)
		}
	}
}
