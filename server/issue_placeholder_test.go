package mirrornode

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

const placeholderClaimRow = `{"id":%d,"number":729,"titleIfNew":"","event":{"type":"claim","id":"claim-web-1","assignee":"kaif","claimState":"released","author":"kaif-key","authorName":"kaif","ts":1790617567277,"sig":"claim-signature"},"meta":{}}`

func issueListing(t *testing.T, bare string) string {
	t.Helper()
	return run(t, ".", "git", "--git-dir="+bare, "ls-tree", "-r", "--name-only",
		"main", ".forkmesh/issues")
}

func TestDirectEventForUnknownIssueIsRefusedNotFabricated(t *testing.T) {
	_, bare := makeBareRepo(t)
	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}
	before := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "main")
	item, err := decodeCollaborationItem(json.RawMessage(
		strings.Replace(placeholderClaimRow, "%d", "1", 1)))
	if err != nil {
		t.Fatal(err)
	}
	_, err = materializeDirectCollaboration(context.Background(), repo, "issues", item, "n2")
	if !errors.Is(err, errIssueUnknown) {
		t.Fatalf("event for an unknown issue should be refused as retryable, got %v", err)
	}
	if after := run(t, ".", "git", "--git-dir="+bare, "rev-parse", "main"); after != before {
		t.Fatalf("refused event still committed %s", after)
	}
	if listing := issueListing(t, bare); strings.Contains(listing, "729") {
		t.Fatalf("a placeholder issue was fabricated:\n%s", listing)
	}
}

func TestInboxLeavesEventForUnknownIssueUntilItsOpeningArrives(t *testing.T) {
	_, bare := makeBareRepo(t)
	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}
	claim := json.RawMessage(strings.Replace(placeholderClaimRow, "%d", "5", 1))
	ids, err := materializeIssueInbox(context.Background(), repo,
		[]json.RawMessage{claim}, "n2")
	if err != nil || len(ids) != 0 {
		t.Fatalf("unknown-issue row should stay pending: ids=%v err=%v", ids, err)
	}
	if listing := issueListing(t, bare); strings.Contains(listing, "729") {
		t.Fatalf("a placeholder issue was fabricated:\n%s", listing)
	}
	open := json.RawMessage(`{"id":4,"number":729,"titleIfNew":"Real title","event":{"type":"open","id":"open-real","title":"Real title","author":"kaif-key","authorName":"kaif","ts":1790617500000,"sig":"open-signature"},"meta":{"assignees":["kaif"]}}`)
	ids, err = materializeIssueInbox(context.Background(), repo,
		[]json.RawMessage{open, claim}, "n2")
	if err != nil || len(ids) != 2 {
		t.Fatalf("opening and its claim should both land: ids=%v err=%v", ids, err)
	}
	record := readMergedIssue(t, bare, "main", 729)
	events, _ := record["events"].([]any)
	if record["title"] != "Real title" || len(events) != 2 {
		t.Fatalf("issue #729 is wrong: %v", record)
	}
}

func TestSignedOpeningReplacesLegacyPlaceholderInPlace(t *testing.T) {
	source, bare := makeBareRepo(t)
	base := run(t, source, "git", "rev-parse", "main")
	commitIssueRecord(t, source, bare, "main", base, 729,
		legacyPlaceholderRecord(729), nil)
	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}
	open := json.RawMessage(`{"id":4,"number":729,"titleIfNew":"Real title","event":{"type":"open","id":"open-real","title":"Real title","author":"kaif-key","authorName":"kaif","ts":1790617500000,"sig":"open-signature"},"meta":{"assignees":["kaif"]}}`)
	if _, err := materializeIssueInbox(context.Background(), repo,
		[]json.RawMessage{open}, "n2"); err != nil {
		t.Fatal(err)
	}
	listing := issueListing(t, bare)
	if strings.Contains(listing, "730") {
		t.Fatalf("the signed opening was renumbered away from its placeholder:\n%s", listing)
	}
	record := readMergedIssue(t, bare, "main", 729)
	events, _ := record["events"].([]any)
	opening, _ := events[0].(map[string]any)
	if record["title"] != "Real title" || opening["sig"] != "open-signature" ||
		len(events) != 2 {
		t.Fatalf("placeholder was not replaced by the signed opening: %v", record)
	}
	if assignees, _ := record["assignees"].([]any); len(assignees) != 0 {
		t.Fatalf("the placeholder's later claim release was not replayed: %v", assignees)
	}
}

func legacyPlaceholderRecord(number int) map[string]any {
	return map[string]any{
		"schema": "forkmesh-issue-v1", "number": number, "title": "",
		"status": "open", "createdAt": 1790617567277, "updatedAt": 1790617567277,
		"assignees": []any{}, "author": "kaif-key", "authorName": "kaif",
		"events": []any{
			map[string]any{
				"type": "open", "id": "open-" + strconv.Itoa(number), "title": "",
				"author": "kaif-key", "authorName": "kaif", "ts": 1790617567277,
				"attachments": []any{}, "sig": "relay-placeholder",
			},
			map[string]any{
				"type": "claim", "id": "claim-web-1", "assignee": "kaif",
				"claimState": "released", "author": "kaif-key", "authorName": "kaif",
				"ts": 1790617567277, "sig": "claim-signature",
			},
		},
	}
}

func signedIssueRecord(number int, extra ...map[string]any) map[string]any {
	events := []any{map[string]any{
		"type": "open", "id": "open-real", "title": "Real title",
		"author": "kaif-key", "authorName": "kaif", "ts": 1790617500000,
		"sig": "open-signature",
	}}
	for _, event := range extra {
		events = append(events, event)
	}
	return map[string]any{
		"schema": "forkmesh-issue-v1", "number": number, "title": "Real title",
		"status": "open", "createdAt": 1790617500000, "updatedAt": 1790617500000,
		"assignees": []any{"kaif"}, "author": "kaif-key", "authorName": "kaif",
		"events": events,
	}
}

func commitIssueRecord(t *testing.T, source, bare, branch, base string,
	number int, record map[string]any, files map[string]string) string {
	t.Helper()
	run(t, source, "git", "checkout", "-B", branch, base)
	dir := filepath.Join(source, ".forkmesh", "issues", "open", strconv.Itoa(number))
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	raw, err := json.MarshalIndent(record, "", "    ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir,
		"issue-"+strconv.Itoa(number)+".json"), append(raw, '\n'), 0600); err != nil {
		t.Fatal(err)
	}
	for name, content := range files {
		path := filepath.Join(dir, name)
		if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	run(t, source, "git", "add", ".forkmesh/issues")
	run(t, source, "git", "commit", "-m", "issue #"+strconv.Itoa(number)+": intake")
	oid := run(t, source, "git", "rev-parse", "HEAD")
	run(t, source, "git", "push", "-f", bare, branch+":refs/heads/"+branch)
	return oid
}

func TestIssueDivergenceFoldsPlaceholderIntoSourceIssueWithoutRenumbering(t *testing.T) {
	source, bare := makeBareRepo(t)
	base := run(t, source, "git", "rev-parse", "main")
	placeholder := commitIssueRecord(t, source, bare, "n2", base, 729,
		legacyPlaceholderRecord(729), map[string]string{"attachments/log.txt": "log\n"})
	real := commitIssueRecord(t, source, bare, "n1", base, 729,
		signedIssueRecord(729), nil)
	if err := os.WriteFile(filepath.Join(source, "code.txt"), []byte("change\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "code.txt")
	run(t, source, "git", "commit", "-m", "source change")
	real = run(t, source, "git", "rev-parse", "HEAD")
	run(t, source, "git", "push", "-f", bare, "n1:refs/heads/n1")

	merged, recognized, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, placeholder, real)
	if err != nil || !recognized {
		t.Fatalf("placeholder divergence was not merged: %v %v", recognized, err)
	}
	reversed, _, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, real, placeholder)
	if err != nil || reversed != merged {
		t.Fatalf("merge is not deterministic: %s %s %v", merged, reversed, err)
	}
	listing := run(t, ".", "git", "--git-dir="+bare, "ls-tree", "-r", "--name-only",
		merged, ".forkmesh/issues")
	if strings.Contains(listing, "730") {
		t.Fatalf("merge renumbered the placeholder into a phantom issue:\n%s", listing)
	}
	record := readMergedIssue(t, bare, merged, 729)
	events, _ := record["events"].([]any)
	opening, _ := events[0].(map[string]any)
	if record["title"] != "Real title" || opening["sig"] != "open-signature" || len(events) != 2 {
		t.Fatalf("merged #729 lost the signed opening or the claim: %v", record)
	}
	if assignees, _ := record["assignees"].([]any); len(assignees) != 0 {
		t.Fatalf("folded claim release was not projected: %v", assignees)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "show",
		merged+":.forkmesh/issues/open/729/attachments/log.txt"); got != "log" {
		t.Fatalf("placeholder attachment was lost: %q", got)
	}
	if got := run(t, ".", "git", "--git-dir="+bare, "show", merged+":code.txt"); got != "change" {
		t.Fatalf("source change was lost: %q", got)
	}
}

func TestIssueDivergenceFoldsSameIssueWithDifferentEvents(t *testing.T) {
	source, bare := makeBareRepo(t)
	base := run(t, source, "git", "rev-parse", "main")
	left := commitIssueRecord(t, source, bare, "left", base, 729,
		signedIssueRecord(729, map[string]any{
			"type": "title", "id": "title-1", "title": "Renamed",
			"author": "kaif-key", "ts": 1790617600000, "sig": "title-signature",
		}), nil)
	right := commitIssueRecord(t, source, bare, "right", base, 729,
		signedIssueRecord(729, map[string]any{
			"type": "claim", "id": "claim-web-1", "assignee": "kaif",
			"claimState": "released", "author": "kaif-key",
			"ts": 1790617700000, "sig": "claim-signature",
		}), nil)
	merged, recognized, err := reconcileIssueOnlyDivergence(
		context.Background(), bare, left, right)
	if err != nil || !recognized {
		t.Fatalf("same-issue divergence was not merged: %v %v", recognized, err)
	}
	listing := run(t, ".", "git", "--git-dir="+bare, "ls-tree", "-r", "--name-only",
		merged, ".forkmesh/issues")
	if strings.Contains(listing, "730") {
		t.Fatalf("merge renumbered the same issue:\n%s", listing)
	}
	record := readMergedIssue(t, bare, merged, 729)
	events, _ := record["events"].([]any)
	if len(events) != 3 || record["title"] != "Renamed" {
		t.Fatalf("events were not folded into one #729: %v", record)
	}
}
