package update

import (
	"fmt"
	"strings"
	"testing"
)

func TestCleanLineStripsColourAndCuts(t *testing.T) {
	if got := cleanLine("\x1b[1;32mok\x1b[0m  \t"); got != "ok" {
		t.Fatalf("got %q", got)
	}
	long := strings.Repeat("x", 500)
	got := cleanLine(long)
	if utf16Len(got) != maxLineChars || !strings.HasSuffix(got, "…") {
		t.Fatalf("cut to %d", utf16Len(got))
	}
}

func TestFeedSplitsCarriesAndBatches(t *testing.T) {
	var feed Feed
	feed.Reset()
	feed.Append("one\r\ntw")
	feed.Append("o\rthree\n\n   \n")
	if got := feed.Backlog(10); strings.Join(got, ",") != "one,two,three" {
		t.Fatalf("backlog %v", got)
	}
	batch := feed.BatchSince(1)
	if batch.First != 2 || batch.Seq != 3 || batch.Skipped != 0 || len(batch.Lines) != 2 {
		t.Fatalf("batch %+v", batch)
	}
	for i := 0; i < 400; i++ {
		feed.Append(fmt.Sprintf("line %d\n", i))
	}
	if len(feed.Backlog(1000)) != maxBacklogLines {
		t.Fatalf("backlog kept %d", len(feed.Backlog(1000)))
	}
	batch = feed.BatchSince(0)
	if len(batch.Lines) != maxBatchLines || batch.Skipped != batch.First-1 || batch.Seq != feed.Seq() {
		t.Fatalf("batch %d lines, first %d skipped %d", len(batch.Lines), batch.First, batch.Skipped)
	}
	feed.Append(strings.Repeat("y", 390) + "\n")
	for i := 0; i < 30; i++ {
		feed.Append(strings.Repeat("z", 390) + "\n")
	}
	batch = feed.BatchSince(0)
	chars := 0
	for _, line := range batch.Lines {
		chars += utf16Len(line)
	}
	if chars > maxBatchChars {
		t.Fatalf("batch carried %d chars", chars)
	}
	if empty := feed.BatchSince(feed.Seq()); len(empty.Lines) != 0 || empty.Skipped != 0 {
		t.Fatalf("empty batch %+v", empty)
	}
}

func TestFeedStatusAndRun(t *testing.T) {
	var feed Feed
	feed.Reset()
	feed.NoteStatus("Update failed: boom", true)
	feed.NoteStatus("Relaunching...", false)
	if !feed.Relaunching() || feed.Error() != "Update failed: boom" || feed.Status() != "Relaunching..." {
		t.Fatalf("status %q error %q relaunching %v", feed.Status(), feed.Error(), feed.Relaunching())
	}
	run := feed.Run()
	feed.Reset()
	if feed.Run() != run+1 || feed.Error() != "" || feed.Relaunching() {
		t.Fatal("reset did not start a new run")
	}
	state := stateObject(&feed, RunState{Running: true, Kind: "rebuild", Percent: 140})
	if state["percent"] != 100 || state["running"] != true || state["kind"] != "rebuild" {
		t.Fatalf("state %v", state)
	}
}

func TestHelpersMatchTheDesktop(t *testing.T) {
	if got := formatByteSize(10); got != "10 B" {
		t.Fatal(got)
	}
	if got := formatByteSize(3 << 30); got != "3.0 GB" {
		t.Fatal(got)
	}
	if got := buildJobsForMemory(32<<30, 30<<30, 36); got != 12 {
		t.Fatalf("jobs %d", got)
	}
	if got := buildJobsForMemory(8<<30, 3<<30, 8); got != 1 {
		t.Fatalf("jobs %d", got)
	}
	if got := buildJobsForMemory(0, 0, 4); got != 4 {
		t.Fatalf("jobs %d", got)
	}
	if got := updateFailureSummary("a\nb: error: broke\nc"); got != "b: error: broke" {
		t.Fatal(got)
	}
	if got := updateFailureSummary("x\nwrite failed: No space left on device"); !strings.HasPrefix(got, "the disk filled up") {
		t.Fatal(got)
	}
	if got := updateFailureSummary("first\nlast line"); got != "last line" {
		t.Fatal(got)
	}
	if got := actionSummary(2, "1.2.3"); got != "Rebuild ForkMesh from this desktop's source checkout and restart it (now on 1.2.3). The desktop drops this connection while it restarts. 2 agents are running; they are queued again after the restart." {
		t.Fatal(got)
	}
}
