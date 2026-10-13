package mirrornode

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"testing"
)

func commitTestIssue(t *testing.T, source, bare, branch, base string,
	number int, title string, createdAt int64) string {
	t.Helper()
	run(t, source, "git", "checkout", "-B", branch, base)
	dir := filepath.Join(source, ".forkmesh", "issues", "open",
		strconv.Itoa(number))
	if err := os.MkdirAll(filepath.Join(dir, "attachments"), 0700); err != nil {
		t.Fatal(err)
	}
	record := map[string]any{
		"schema": "forkmesh-issue-v1", "number": number,
		"title": title, "status": "open", "createdAt": createdAt,
		"updatedAt": createdAt,
		"events": []any{map[string]any{
			"type": "open", "id": "open-" + title, "title": title,
			"author": "forkbot", "authorName": "forkbot", "ts": createdAt,
			"sig": "",
		}},
	}
	raw, err := json.MarshalIndent(record, "", "    ")
	if err != nil {
		t.Fatal(err)
	}
	raw = append(raw, '\n')
	if err := os.WriteFile(filepath.Join(dir,
		"issue-"+strconv.Itoa(number)+".json"), raw, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "attachments", title+".txt"),
		[]byte(title+" attachment\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", ".forkmesh/issues")
	run(t, source, "git", "commit", "-m", "issue #"+strconv.Itoa(number)+": mirror intake")
	oid := run(t, source, "git", "rev-parse", "HEAD")
	run(t, source, "git", "push", bare, branch+":refs/heads/"+branch)
	return oid
}

func readMergedIssue(t *testing.T, bare, commit string, number int) map[string]any {
	t.Helper()
	path := ".forkmesh/issues/open/" + strconv.Itoa(number) +
		"/issue-" + strconv.Itoa(number) + ".json"
	raw := run(t, ".", "git", "--git-dir="+bare, "show", commit+":"+path)
	var record map[string]any
	if err := json.Unmarshal([]byte(raw), &record); err != nil {
		t.Fatalf("merged issue #%d is invalid: %v", number, err)
	}
	return record
}

func TestIssueOnlyDivergenceMergesAndRenumbersCollisionDeterministically(t *testing.T) {
	source, bare := makeBareRepo(t)
	base := run(t, source, "git", "rev-parse", "main")
	older := commitTestIssue(t, source, bare, "issue-older", base, 7,
		"older-report", 1700000000000)
	newer := commitTestIssue(t, source, bare, "issue-newer", base, 7,
		"newer-report", 1700000001000)

	merged, recognized, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, newer, older)
	if err != nil || !recognized {
		t.Fatalf("issue divergence was not merged: recognized=%v err=%v", recognized, err)
	}
	reversed, recognized, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, older, newer)
	if err != nil || !recognized || reversed != merged {
		t.Fatalf("merge is not deterministic: %s %s recognized=%v err=%v",
			merged, reversed, recognized, err)
	}
	parents := strings.Fields(run(t, ".", "git", "--git-dir="+bare,
		"show", "-s", "--format=%P", merged))
	wantParents := []string{older, newer}
	sort.Strings(wantParents)
	if strings.Join(parents, " ") != strings.Join(wantParents, " ") {
		t.Fatalf("merge parents are %v want %v", parents, wantParents)
	}
	issue7 := readMergedIssue(t, bare, merged, 7)
	issue8 := readMergedIssue(t, bare, merged, 8)
	if issue7["title"] != "older-report" || issue7["number"] != float64(7) ||
		issue8["title"] != "newer-report" || issue8["number"] != float64(8) {
		t.Fatalf("collision was not losslessly renumbered: #7=%v #8=%v", issue7, issue8)
	}
	attachment := run(t, ".", "git", "--git-dir="+bare, "show",
		merged+":.forkmesh/issues/open/8/attachments/newer-report.txt")
	if attachment != "newer-report attachment" {
		t.Fatalf("renumbered attachment was lost: %q", attachment)
	}

	// Exercise the production caller too: a staged peer head updates main to
	// the exact same deterministic merge object, never to a one-sided winner.
	run(t, ".", "git", "--git-dir="+bare, "update-ref", "refs/heads/main", newer)
	run(t, ".", "git", "--git-dir="+bare, "update-ref",
		"refs/forkmesh/upstream/heads/main", older)
	if err := reconcileUpstreamRefs(context.Background(), bare); err != nil {
		t.Fatal(err)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "main"); got != merged {
		t.Fatalf("gateway selected %s, want issue merge %s", got, merged)
	}
}

func TestIssueDivergenceMergesOntoOrdinarySourceChanges(t *testing.T) {
	source, bare := makeBareRepo(t)
	base := run(t, source, "git", "rev-parse", "main")
	issue := commitTestIssue(t, source, bare, "issue-only", base, 7,
		"report", 1700000000000)
	run(t, source, "git", "checkout", "-B", "source-change", base)
	if err := os.WriteFile(filepath.Join(source, "code.txt"), []byte("change\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "code.txt")
	run(t, source, "git", "commit", "-m", "source change")
	code := run(t, source, "git", "rev-parse", "HEAD")
	run(t, source, "git", "push", bare, "source-change:refs/heads/source-change")

	merged, recognized, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, issue, code)
	if !recognized || err != nil || merged == "" {
		t.Fatalf("issue was not merged onto source update: merged=%q recognized=%v err=%v",
			merged, recognized, err)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "show", merged+":code.txt"); got != "change" {
		t.Fatalf("source update was not preserved: %q", got)
	}
	if got := readMergedIssue(t, bare, merged, 7)["title"]; got != "report" {
		t.Fatalf("issue update was not preserved: %v", got)
	}
	reversed, recognized, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, code, issue)
	if err != nil || !recognized || reversed != merged {
		t.Fatalf("source/issue merge is not deterministic: %s %s recognized=%v err=%v",
			merged, reversed, recognized, err)
	}
}

func TestIssueDivergenceRenumbersAgainstUpdatedSourceTree(t *testing.T) {
	source, bare := makeBareRepo(t)
	base := run(t, source, "git", "rev-parse", "main")
	local := commitTestIssue(t, source, bare, "local-issue", base, 7,
		"local-report", 1700000001000)
	canonical := commitTestIssue(t, source, bare, "canonical", base, 7,
		"canonical-report", 1700000000000)
	if err := os.WriteFile(filepath.Join(source, "code.txt"), []byte("change\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "code.txt")
	run(t, source, "git", "commit", "-m", "source change")
	canonical = run(t, source, "git", "rev-parse", "HEAD")
	run(t, source, "git", "push", bare, "canonical:refs/heads/canonical")

	merged, recognized, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, local, canonical)
	if err != nil || !recognized || merged == "" {
		t.Fatalf("collision against source tree was not merged: %q %v %v",
			merged, recognized, err)
	}
	if got := readMergedIssue(t, bare, merged, 7)["title"]; got != "canonical-report" {
		t.Fatalf("canonical issue moved unexpectedly: %v", got)
	}
	if got := readMergedIssue(t, bare, merged, 8)["title"]; got != "local-report" {
		t.Fatalf("local issue was not losslessly renumbered: %v", got)
	}
}

func TestIssueDivergenceWithDisjointSourceChangesOnBothSidesMerges(t *testing.T) {
	source, bare := makeBareRepo(t)
	base := run(t, source, "git", "rev-parse", "main")
	left := commitTestIssue(t, source, bare, "left", base, 7,
		"left-report", 1700000000000)
	if err := os.WriteFile(filepath.Join(source, "left.txt"), []byte("left\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "left.txt")
	run(t, source, "git", "commit", "-m", "left source change")
	left = run(t, source, "git", "rev-parse", "HEAD")
	run(t, source, "git", "push", bare, "left:refs/heads/left")

	right := commitTestIssue(t, source, bare, "right", base, 8,
		"right-report", 1700000001000)
	if err := os.WriteFile(filepath.Join(source, "right.txt"), []byte("right\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "right.txt")
	run(t, source, "git", "commit", "-m", "right source change")
	right = run(t, source, "git", "rev-parse", "HEAD")
	run(t, source, "git", "push", bare, "right:refs/heads/right")

	merged, recognized, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, left, right)
	if !recognized || err != nil || merged == "" {
		t.Fatalf("two-sided source divergence did not merge: merged=%q recognized=%v err=%v",
			merged, recognized, err)
	}
	for _, name := range []string{"left", "right"} {
		if got := run(t, ".", "git", "--git-dir="+bare, "show", merged+":"+name+".txt"); got != name {
			t.Fatalf("%s source change was lost: %q", name, got)
		}
	}
	if readMergedIssue(t, bare, merged, 7)["title"] != "left-report" ||
		readMergedIssue(t, bare, merged, 8)["title"] != "right-report" {
		t.Fatalf("issues were not both kept")
	}
}

func makeCrissCrossIssueHistory(t *testing.T) (string, string, string, string) {
	t.Helper()
	source, bare := makeBareRepo(t)
	base := run(t, source, "git", "rev-parse", "main")
	laneA := commitTestIssue(t, source, bare, "lane-a", base, 7,
		"lane-a-report", 1700000000000)
	laneB := commitTestIssue(t, source, bare, "lane-b", base, 8,
		"lane-b-report", 1700000000500)
	run(t, source, "git", "checkout", "-B", "healed-left", laneA)
	run(t, source, "git", "merge", "--no-ff", "-m", "heal left", laneB)
	healedLeft := run(t, source, "git", "rev-parse", "HEAD")
	run(t, source, "git", "checkout", "-B", "healed-right", laneB)
	run(t, source, "git", "merge", "--no-ff", "-m", "heal right", laneA)
	healedRight := run(t, source, "git", "rev-parse", "HEAD")
	return source, bare, healedLeft, healedRight
}

func TestIssueDivergenceWithCrissCrossMergeBasesMerges(t *testing.T) {
	source, bare, healedLeft, healedRight := makeCrissCrossIssueHistory(t)
	left := commitTestIssue(t, source, bare, "left", healedLeft, 9,
		"left-report", 1700000002000)
	right := commitTestIssue(t, source, bare, "right", healedRight, 9,
		"right-report", 1700000001000)
	bases := strings.Fields(run(t, ".", "git", "--git-dir="+bare,
		"merge-base", "--all", left, right))
	if len(bases) != 2 {
		t.Fatalf("history is not criss-cross: bases=%v", bases)
	}

	merged, recognized, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, left, right)
	if err != nil || !recognized || merged == "" {
		t.Fatalf("criss-cross issue divergence was not merged: %q %v %v",
			merged, recognized, err)
	}
	reversed, recognized, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, right, left)
	if err != nil || !recognized || reversed != merged {
		t.Fatalf("criss-cross merge is not deterministic: %s %s %v %v",
			merged, reversed, recognized, err)
	}
	want := map[int]string{
		7: "lane-a-report", 8: "lane-b-report",
		9: "right-report", 10: "left-report",
	}
	for number, title := range want {
		if got := readMergedIssue(t, bare, merged, number)["title"]; got != title {
			t.Fatalf("issue #%d is %v, want %s", number, got, title)
		}
	}
	listing := run(t, ".", "git", "--git-dir="+bare, "ls-tree", "-d",
		"--name-only", merged+":.forkmesh/issues/open")
	if got := strings.Fields(listing); len(got) != 4 {
		t.Fatalf("merged issue directories are %v, want exactly four", got)
	}

	run(t, ".", "git", "--git-dir="+bare, "update-ref", "refs/heads/main", left)
	run(t, ".", "git", "--git-dir="+bare, "update-ref",
		"refs/forkmesh/upstream/heads/main", right)
	if err := reconcileUpstreamRefs(context.Background(), bare); err != nil {
		t.Fatal(err)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "main"); got != merged {
		t.Fatalf("gateway selected %s, want criss-cross issue merge %s", got, merged)
	}
}

func TestIssueDivergenceWithCrissCrossMergeBasesKeepsSourceSide(t *testing.T) {
	source, bare, healedLeft, healedRight := makeCrissCrossIssueHistory(t)
	left := commitTestIssue(t, source, bare, "left", healedLeft, 9,
		"left-report", 1700000002000)
	run(t, source, "git", "checkout", "-B", "right", healedRight)
	if err := os.WriteFile(filepath.Join(source, "code.txt"), []byte("change\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "code.txt")
	run(t, source, "git", "commit", "-m", "source change")
	right := run(t, source, "git", "rev-parse", "HEAD")
	run(t, source, "git", "push", bare, "right:refs/heads/right")

	merged, recognized, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, left, right)
	if err != nil || !recognized || merged == "" {
		t.Fatalf("criss-cross issue side was not layered onto source: %q %v %v",
			merged, recognized, err)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "show", merged+":code.txt"); got != "change" {
		t.Fatalf("source update was not preserved: %q", got)
	}
	for number, title := range map[int]string{
		7: "lane-a-report", 8: "lane-b-report", 9: "left-report",
	} {
		if got := readMergedIssue(t, bare, merged, number)["title"]; got != title {
			t.Fatalf("issue #%d is %v, want %s", number, got, title)
		}
	}
}

func TestIssueDivergenceWithCrissCrossMergeBasesAndSourceOnBothSidesMerges(t *testing.T) {
	source, bare, healedLeft, healedRight := makeCrissCrossIssueHistory(t)
	sides := map[string]string{"left": healedLeft, "right": healedRight}
	heads := map[string]string{}
	for index, name := range []string{"left", "right"} {
		heads[name] = commitTestIssue(t, source, bare, name, sides[name], 9+index,
			name+"-report", int64(1700000001000+index))
		if err := os.WriteFile(filepath.Join(source, name+".txt"), []byte(name+"\n"), 0600); err != nil {
			t.Fatal(err)
		}
		run(t, source, "git", "add", name+".txt")
		run(t, source, "git", "commit", "-m", name+" source change")
		heads[name] = run(t, source, "git", "rev-parse", "HEAD")
		run(t, source, "git", "push", bare, name+":refs/heads/"+name)
	}
	merged, recognized, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, heads["left"], heads["right"])
	if !recognized || err != nil || merged == "" {
		t.Fatalf("criss-cross two-sided source divergence did not merge: %q %v %v",
			merged, recognized, err)
	}
	for number, title := range map[int]string{
		7: "lane-a-report", 8: "lane-b-report", 9: "left-report", 10: "right-report",
	} {
		if got := readMergedIssue(t, bare, merged, number)["title"]; got != title {
			t.Fatalf("issue #%d is %v, want %s", number, got, title)
		}
	}
	for _, name := range []string{"left", "right"} {
		if got := run(t, ".", "git", "--git-dir="+bare, "show", merged+":"+name+".txt"); got != name {
			t.Fatalf("%s source change was lost: %q", name, got)
		}
	}
}
