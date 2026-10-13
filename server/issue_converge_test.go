package mirrornode

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"testing"
)

type issueFleet struct {
	t      *testing.T
	source string
	bare   string
	repo   Repository
	heads  map[string]string
}

func newIssueFleet(t *testing.T) *issueFleet {
	t.Helper()
	source, bare := makeBareRepo(t)
	return &issueFleet{
		t: t, source: source, bare: bare,
		repo:  Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare},
		heads: map[string]string{},
	}
}

func (f *issueFleet) git(args ...string) string {
	f.t.Helper()
	return run(f.t, ".", append([]string{"git", "--git-dir=" + f.bare}, args...)...)
}

func (f *issueFleet) fork(core, from string) {
	f.t.Helper()
	f.heads[core] = f.heads[from]
	if from == "main" && f.heads[core] == "" {
		f.heads[core] = f.git("rev-parse", "main")
	}
}

func (f *issueFleet) intake(core string, number int, event map[string]any,
	attachments map[string]string) int {
	f.t.Helper()
	head := f.heads[core]
	if head == "" {
		head = f.git("rev-parse", "main")
	}
	f.git("update-ref", "refs/heads/"+core, head)
	f.git("symbolic-ref", "HEAD", "refs/heads/"+core)
	item := collaborationItem{ID: 1, Number: number, Event: event, Meta: map[string]any{}}
	for name, content := range attachments {
		item.AttachmentData = append(item.AttachmentData, map[string]any{
			"name": name, "data": base64.StdEncoding.EncodeToString([]byte(content)),
		})
	}
	raw, err := json.Marshal(item)
	if err != nil {
		f.t.Fatal(err)
	}
	decoded, err := decodeCollaborationItem(raw)
	if err != nil {
		f.t.Fatal(err)
	}
	written, err := materializeDirectCollaboration(context.Background(), f.repo,
		"issues", decoded, core)
	f.git("symbolic-ref", "HEAD", "refs/heads/main")
	if err != nil {
		f.t.Fatalf("%s intake of %v failed: %v", core, event, err)
	}
	f.heads[core] = f.git("rev-parse", "refs/heads/"+core)
	return written
}

func (f *issueFleet) commitRecord(core string, number int, record map[string]any) {
	f.t.Helper()
	head := f.heads[core]
	if head == "" {
		head = f.git("rev-parse", "main")
	}
	f.fetchSource()
	f.heads[core] = commitIssueRecord(f.t, f.source, f.bare, core, head, number, record, nil)
}

func (f *issueFleet) fetchSource() {
	f.t.Helper()
	run(f.t, f.source, "git", "fetch", "-q", f.bare, "+refs/heads/*:refs/remotes/fleet/*")
}

func (f *issueFleet) converge(a, b string) string {
	f.t.Helper()
	if a == b {
		return a
	}
	if gitIsAncestor(context.Background(), f.bare, a, b) {
		return b
	}
	if gitIsAncestor(context.Background(), f.bare, b, a) {
		return a
	}
	merged, recognized, err := reconcileIssueOnlyDivergence(context.Background(), f.bare, a, b)
	if err == nil && !recognized && f.tree(a) == f.tree(b) {
		aAt, bAt := gitCommitTime(context.Background(), f.bare, a), gitCommitTime(context.Background(), f.bare, b)
		if aAt > bAt || (aAt == bAt && a > b) {
			return a
		}
		return b
	}
	if err != nil || !recognized || merged == "" {
		f.t.Fatalf("merge of %s and %s did not converge: merged=%q recognized=%v err=%v",
			a, b, merged, recognized, err)
	}
	return merged
}

func (f *issueFleet) tree(commit string) string {
	f.t.Helper()
	return f.git("rev-parse", commit+"^{tree}")
}

func (f *issueFleet) assertOrderIndependent(heads []string) string {
	f.t.Helper()
	orders := [][3]int{{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}
	want := ""
	check := func(label, commit string) {
		tree := f.tree(commit)
		if want == "" {
			want = tree
			return
		}
		if tree != want {
			f.t.Fatalf("%s produced tree %s, want %s\n%s", label, tree, want,
				f.git("diff", "--stat", want, tree))
		}
	}
	var final string
	for _, order := range orders {
		x, y, z := heads[order[0]], heads[order[1]], heads[order[2]]
		result := f.converge(f.converge(x, y), z)
		check(fmt.Sprintf("((%d,%d),%d)", order[0], order[1], order[2]), result)
		swapped := f.converge(z, f.converge(y, x))
		check(fmt.Sprintf("(%d,(%d,%d))", order[2], order[1], order[0]), swapped)
		gossip := f.converge(f.converge(x, y), f.converge(x, z))
		check(fmt.Sprintf("((%d,%d),(%d,%d))", order[0], order[1], order[0], order[2]), gossip)
		final = result
	}
	return final
}

func (f *issueFleet) record(commit, status string, number int) map[string]any {
	f.t.Helper()
	raw := f.git("show", fmt.Sprintf("%s:.forkmesh/issues/%s/%d/issue-%d.json",
		commit, status, number, number))
	var record map[string]any
	if err := json.Unmarshal([]byte(raw), &record); err != nil {
		f.t.Fatalf("issue #%d is invalid: %v", number, err)
	}
	return record
}

func issueEventIDs(record map[string]any) []string {
	events, _ := record["events"].([]any)
	var ids []string
	for _, value := range events {
		event, _ := value.(map[string]any)
		ids = append(ids, stringValue(event["id"]))
	}
	return ids
}

func signedOpenEvent(id string, ts int64) map[string]any {
	return map[string]any{
		"type": "open", "id": "open-" + id, "title": "Issue " + id,
		"author": id + "-key", "authorName": id, "ts": ts, "sig": "sig-" + id,
	}
}

func issueEvent(kind, id string, ts int64, extra map[string]any) map[string]any {
	event := map[string]any{
		"type": kind, "id": id, "author": "kaif-key", "authorName": "kaif",
		"ts": ts, "sig": "sig-" + id,
	}
	for key, value := range extra {
		event[key] = value
	}
	return event
}

func TestIssueDivergenceOfThreeCoresConvergesInEveryOrder(t *testing.T) {
	f := newIssueFleet(t)
	const at = int64(1790617500000)
	f.intake("main", 727, signedOpenEvent("E", at-2000), nil)
	f.intake("main", 728, signedOpenEvent("F", at-1000), nil)
	f.git("update-ref", "refs/heads/main", f.heads["main"])
	for _, core := range []string{"n1", "n2", "n3"} {
		f.fork(core, "main")
	}

	opening := signedOpenEvent("S", at)
	f.intake("n1", 729, opening, nil)
	f.intake("n1", 730, signedOpenEvent("X", at+200), nil)

	f.intake("n2", 729, opening, nil)
	f.intake("n2", 729, issueEvent("claim", "claim-n2", at+300,
		map[string]any{"assignee": "kaif", "claimState": "assigned"}), nil)
	f.intake("n2", 728, issueEvent("comment", "comment-728-n2", at+310,
		map[string]any{"body": "still broken"}), nil)
	yOpening := signedOpenEvent("Y", at+250)
	if got := f.intake("n2", 730, yOpening, map[string]string{"trace.txt": "trace\n"}); got != 730 {
		t.Fatalf("n2 materialized Y at #%d", got)
	}
	f.intake("n2", 730, issueEvent("claim", "claim-y-n2", at+400,
		map[string]any{"assignee": "mdkaif", "claimState": "assigned"}), nil)
	f.intake("n2", 730, issueEvent("comment", "comment-y-n2", at+410,
		map[string]any{"body": "log from n2"}), map[string]string{"log.txt": "n2 log\n"})

	f.commitRecord("n3", 729, legacyPlaceholderRecord(729))
	f.intake("n3", 730, yOpening, map[string]string{"trace.txt": "trace\n"})
	n1Early := f.heads["n1"]
	f.heads["n3"] = f.converge(f.heads["n3"], n1Early)
	if got := f.record(f.heads["n3"], "open", 731)["title"]; got != "Issue Y" {
		t.Fatalf("n3 did not renumber Y to #731 on its first merge: %v", got)
	}
	f.intake("n3", 731, issueEvent("comment", "comment-y-n3", at+500,
		map[string]any{"body": "log from n3"}), map[string]string{"log.txt": "n3 log\n"})

	f.intake("n1", 730, issueEvent("comment", "comment-x-n1", at+600,
		map[string]any{"body": "x detail"}), nil)
	f.intake("n1", 728, issueEvent("status", "close-728", at+700,
		map[string]any{"status": "closed"}), nil)

	final := f.assertOrderIndependent([]string{f.heads["n1"], f.heads["n2"], f.heads["n3"]})

	listing := f.git("ls-tree", "-r", "--name-only", final, ".forkmesh/issues")
	if strings.Contains(listing, "/732/") || strings.Contains(listing, "open/728/") {
		t.Fatalf("unexpected issue layout:\n%s", listing)
	}
	closed := f.record(final, "closed", 728)
	if ids := strings.Join(issueEventIDs(closed), ","); ids != "open-F,comment-728-n2,close-728" ||
		closed["status"] != "closed" {
		t.Fatalf("#728 lost its comment or its close: %v", closed)
	}
	s := f.record(final, "open", 729)
	if ids := strings.Join(issueEventIDs(s), ","); ids != "open-S,claim-n2,claim-web-1" ||
		s["title"] != "Issue S" {
		t.Fatalf("#729 did not fold the placeholder and the claim: %v", s)
	}
	x := f.record(final, "open", 730)
	if ids := strings.Join(issueEventIDs(x), ","); ids != "open-X,comment-x-n1" {
		t.Fatalf("#730 is not X with its comment: %v", x)
	}
	y := f.record(final, "open", 731)
	if ids := strings.Join(issueEventIDs(y), ","); ids != "open-Y,claim-y-n2,comment-y-n2,comment-y-n3" ||
		y["intakeNumber"] != float64(730) || y["number"] != float64(731) {
		t.Fatalf("#731 did not fold both materializations of Y: %v", y)
	}
	if assignees, _ := y["assignees"].([]any); len(assignees) != 1 || assignees[0] != "mdkaif" {
		t.Fatalf("Y's claim from its lower materialization was not projected: %v", y["assignees"])
	}
	logs := 0
	for _, path := range strings.Fields(listing) {
		if strings.HasPrefix(path, ".forkmesh/issues/open/731/log.txt") {
			logs++
		}
	}
	if logs != 2 || !strings.Contains(listing, ".forkmesh/issues/open/731/trace.txt") {
		t.Fatalf("Y's attachments were not all kept:\n%s", listing)
	}
	if got := f.git("show", final+":.forkmesh/issues/open/731/log.txt"); got != "n2 log" && got != "n3 log" {
		t.Fatalf("Y's log attachment is %q", got)
	}
}

func TestIssueIdentityAtTwoNumbersKeepsTheHigherNumber(t *testing.T) {
	f := newIssueFleet(t)
	const at = int64(1790617500000)
	f.fork("n1", "main")
	f.fork("n2", "main")
	f.intake("n1", 5, signedOpenEvent("X", at), nil)
	yOpening := signedOpenEvent("Y", at+10)
	f.intake("n2", 5, yOpening, nil)
	f.heads["n3"] = f.converge(f.heads["n1"], f.heads["n2"])
	f.intake("n2", 5, issueEvent("comment", "late-on-5", at+100, nil),
		map[string]string{"a.txt": "from five\n"})
	f.intake("n3", 6, issueEvent("comment", "late-on-6", at+200, nil),
		map[string]string{"b.txt": "from six\n"})

	for _, pair := range [][2]string{{"n2", "n3"}, {"n3", "n2"}} {
		merged := f.converge(f.heads[pair[0]], f.heads[pair[1]])
		listing := f.git("ls-tree", "-r", "--name-only", merged, ".forkmesh/issues")
		if strings.Contains(listing, "/7/") {
			t.Fatalf("Y moved again:\n%s", listing)
		}
		if f.record(merged, "open", 5)["title"] != "Issue X" {
			t.Fatalf("X lost #5:\n%s", listing)
		}
		y := f.record(merged, "open", 6)
		if ids := strings.Join(issueEventIDs(y), ","); ids != "open-Y,late-on-5,late-on-6" {
			t.Fatalf("Y's events were not folded into #6: %v", y)
		}
		for _, name := range []string{"a.txt", "b.txt"} {
			if !strings.Contains(listing, ".forkmesh/issues/open/6/"+name) ||
				strings.Contains(listing, ".forkmesh/issues/open/5/"+name) {
				t.Fatalf("attachment %s is not with Y at #6:\n%s", name, listing)
			}
		}
	}
}

func TestIssueCommentAndCloseOnBothSidesConverge(t *testing.T) {
	f := newIssueFleet(t)
	const at = int64(1790617500000)
	f.intake("main", 9, signedOpenEvent("A", at), map[string]string{"shot.png": "pixels"})
	f.git("update-ref", "refs/heads/main", f.heads["main"])
	f.fork("n1", "main")
	f.fork("n2", "main")
	f.intake("n1", 9, issueEvent("status", "close", at+100,
		map[string]any{"status": "closed"}), nil)
	f.intake("n2", 9, issueEvent("comment", "comment", at+50, nil), nil)
	f.intake("n2", 9, issueEvent("status", "reopen", at+20,
		map[string]any{"status": "open"}), nil)
	for _, pair := range [][2]string{{"n1", "n2"}, {"n2", "n1"}} {
		merged := f.converge(f.heads[pair[0]], f.heads[pair[1]])
		listing := f.git("ls-tree", "-r", "--name-only", merged, ".forkmesh/issues")
		if strings.Contains(listing, "open/9/") ||
			!strings.Contains(listing, "closed/9/shot.png") {
			t.Fatalf("#9 should be closed with its attachment:\n%s", listing)
		}
		record := f.record(merged, "closed", 9)
		if ids := strings.Join(issueEventIDs(record), ","); ids != "open-A,reopen,comment,close" ||
			record["status"] != "closed" {
			t.Fatalf("#9 events or status are wrong: %v", record)
		}
	}
}

func TestIssueDeletedOnOneCoreStaysDeleted(t *testing.T) {
	f := newIssueFleet(t)
	const at = int64(1790617500000)
	f.intake("main", 3, signedOpenEvent("A", at), nil)
	f.git("update-ref", "refs/heads/main", f.heads["main"])
	f.fork("n1", "main")
	f.fork("n2", "main")
	f.intake("n1", 3, map[string]any{"type": "delete", "target": "self",
		"id": "delete-3", "ts": at + 10, "sig": "sig-delete"}, nil)
	f.intake("n2", 3, issueEvent("comment", "comment", at+20, nil), nil)
	f.intake("n2", 4, signedOpenEvent("B", at+30), nil)
	merged := f.converge(f.heads["n1"], f.heads["n2"])
	if reversed := f.converge(f.heads["n2"], f.heads["n1"]); reversed != merged {
		t.Fatalf("delete merge is not deterministic: %s %s", merged, reversed)
	}
	listing := f.git("ls-tree", "-r", "--name-only", merged, ".forkmesh/issues")
	if strings.Contains(listing, "/3/") || !strings.Contains(listing, "open/4/issue-4.json") {
		t.Fatalf("owner deletion did not win or #4 was lost:\n%s", listing)
	}
}

func TestIssueDivergenceWithConflictingSourceKeepsNewerSourceAndBothIssues(t *testing.T) {
	f := newIssueFleet(t)
	const at = int64(1790617500000)
	for index, core := range []string{"n1", "n2"} {
		f.fork(core, "main")
		f.intake(core, 7, signedOpenEvent(core, at+int64(index)), nil)
		f.fetchSource()
		run(t, f.source, "git", "checkout", "-B", core, f.heads[core])
		if err := os.WriteFile(filepath.Join(f.source, "README.md"),
			[]byte(core+"\n"), 0600); err != nil {
			t.Fatal(err)
		}
		run(t, f.source, "git", "commit", "-am", core+" source")
		run(t, f.source, "git", "push", "-f", f.bare, core+":refs/heads/"+core)
		f.heads[core] = f.git("rev-parse", "refs/heads/"+core)
	}
	merged := f.converge(f.heads["n1"], f.heads["n2"])
	if reversed := f.converge(f.heads["n2"], f.heads["n1"]); reversed != merged {
		t.Fatalf("conflicting source merge is not deterministic: %s %s", merged, reversed)
	}
	newer := f.heads["n1"]
	if gitCommitTime(context.Background(), f.bare, f.heads["n2"]) > gitCommitTime(context.Background(), f.bare, newer) ||
		(gitCommitTime(context.Background(), f.bare, f.heads["n2"]) == gitCommitTime(context.Background(), f.bare, newer) &&
			f.heads["n2"] > newer) {
		newer = f.heads["n2"]
	}
	if got, want := f.git("show", merged+":README.md"), f.git("show", newer+":README.md"); got != want {
		t.Fatalf("conflicting source took %q, want the newer side %q", got, want)
	}
	if f.record(merged, "open", 7)["title"] != "Issue n1" || f.record(merged, "open", 8)["title"] != "Issue n2" {
		t.Fatalf("issues were not both kept")
	}
}

func TestIssueDivergenceRandomHistoriesConvergeInEveryOrder(t *testing.T) {
	for _, seed := range []int64{1, 2, 3, 4, 5, 6, 26} {
		t.Run(strconv.FormatInt(seed, 10), func(t *testing.T) {
			random := rand.New(rand.NewSource(seed))
			f := newIssueFleet(t)
			clock := int64(1790617500000)
			f.intake("main", 1, signedOpenEvent("base", clock), nil)
			f.git("update-ref", "refs/heads/main", f.heads["main"])
			cores := []string{"n1", "n2", "n3"}
			for _, core := range cores {
				f.fork(core, "main")
			}
			written := map[string]bool{"open-base": true}
			openings := 0
			for step := 0; step < 14; step++ {
				core := cores[random.Intn(len(cores))]
				clock += 100
				switch choice := random.Intn(10); {
				case choice < 4:
					openings++
					id := fmt.Sprintf("s%d-%d", seed, openings)
					f.intake(core, 0, signedOpenEvent(id, clock-int64(random.Intn(400))), nil)
					written["open-"+id] = true
				case choice < 8:
					numbers := f.issueNumbers(f.heads[core])
					number := numbers[random.Intn(len(numbers))]
					id := fmt.Sprintf("e%d-%d", seed, step)
					kinds := []string{"comment", "claim", "status", "title"}
					kind := kinds[random.Intn(len(kinds))]
					extra := map[string]any{"body": id, "assignee": "kaif",
						"claimState": []string{"assigned", "released"}[random.Intn(2)],
						"status":     []string{"open", "closed"}[random.Intn(2)],
						"title":      "title " + id}
					f.intake(core, number, issueEvent(kind, id, clock, extra), nil)
					written[id] = true
				default:
					other := cores[random.Intn(len(cores))]
					f.heads[core] = f.converge(f.heads[core], f.heads[other])
				}
			}
			final := f.assertOrderIndependent([]string{f.heads["n1"], f.heads["n2"], f.heads["n3"]})
			seen := map[string]int{}
			identities := map[string]bool{}
			for _, path := range strings.Fields(f.git("ls-tree", "-r", "--name-only", final, ".forkmesh/issues")) {
				if !strings.HasSuffix(path, ".json") {
					continue
				}
				var record map[string]any
				if err := json.Unmarshal([]byte(f.git("show", final+":"+path)), &record); err != nil {
					t.Fatal(err)
				}
				ids := issueEventIDs(record)
				if identities[ids[0]] {
					t.Fatalf("identity %s is materialized twice", ids[0])
				}
				identities[ids[0]] = true
				for _, id := range ids {
					seen[id]++
				}
			}
			for id := range written {
				if seen[id] != 1 {
					t.Fatalf("event %s appears %d times in the converged tree", id, seen[id])
				}
			}
		})
	}
}

func (f *issueFleet) issueNumbers(commit string) []int {
	f.t.Helper()
	set := map[int]bool{}
	for _, path := range strings.Fields(f.git("ls-tree", "-r", "--name-only", commit, ".forkmesh/issues")) {
		parts := strings.Split(path, "/")
		if len(parts) == 5 {
			if number, err := strconv.Atoi(parts[3]); err == nil {
				set[number] = true
			}
		}
	}
	numbers := make([]int, 0, len(set))
	for number := range set {
		numbers = append(numbers, number)
	}
	sort.Ints(numbers)
	return numbers
}

func TestIssueCloseKeepsAttachments(t *testing.T) {
	f := newIssueFleet(t)
	const at = int64(1790617500000)
	f.intake("n1", 4, signedOpenEvent("A", at), map[string]string{"shot.png": "pixels"})
	f.intake("n1", 4, issueEvent("status", "close", at+10,
		map[string]any{"status": "closed"}), nil)
	listing := f.git("ls-tree", "-r", "--name-only", f.heads["n1"], ".forkmesh/issues")
	if listing != ".forkmesh/issues/closed/4/issue-4.json\n.forkmesh/issues/closed/4/shot.png" {
		t.Fatalf("closing moved the record without its attachment:\n%s", listing)
	}
}
