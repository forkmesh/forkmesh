package mirrornode

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func TestNativeCollaborationIntakeMaterializesAndAcknowledges(t *testing.T) {
	_, bare := makeBareRepo(t)
	identity, _ := testIdentity(t)
	rows := map[string]string{
		"issues":      `{"pending":[{"id":11,"number":42,"titleIfNew":"Offline issue","event":{"type":"open","id":"issue-open-42","title":"Offline issue","body":"accepted while source was offline","author":"alice","authorName":"Alice","ts":1700000000000,"sig":"issue-signature"},"meta":{"labels":["bug"],"assignees":[]}}]}`,
		"pulls":       `{"pending":[{"id":12,"number":43,"pull":{"number":43,"title":"Offline pull","description":"accepted while source was offline","base":"main","head":"feature","patch":"","commits":"","author":"bob","authorName":"Bob","ts":1700000001000,"sig":"pull-signature"}},{"id":14,"number":0,"branch":"feature/review","event":{"type":"thread-comment","id":"branch-thread-1","threadId":"branch-thread-1","path":"a.txt","side":"new","lineStart":1,"lineEnd":1,"body":"Branch note","author":"dave","ts":1700000003000,"sig":"branch-signature"}}]}`,
		"discussions": `{"pending":[{"id":13,"number":44,"titleIfNew":"Offline discussion","event":{"type":"open","id":"discussion-open-44","title":"Offline discussion","category":"Ideas","body":"hello mirrors","author":"carol","authorName":"Carol","ts":1700000002000,"sig":"discussion-signature"}}]}`,
	}
	var mu sync.Mutex
	acked := map[string]string{}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		parts := strings.Split(strings.Trim(r.URL.Path, "/"), "/")
		if len(parts) != 5 || strings.Join(parts[:4], "/") != "api/repo/forkmesh/forkmesh" {
			http.NotFound(w, r)
			return
		}
		kind := parts[4]
		if r.URL.Query().Get("mirror") != "1" ||
			r.URL.Query().Get("owner") != "mirror9" ||
			r.URL.Query().Get("ts") == "" || r.URL.Query().Get("sig") == "" {
			http.Error(w, "unsigned mirror request", http.StatusUnauthorized)
			return
		}
		switch r.Method {
		case http.MethodGet:
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write([]byte(rows[kind]))
		case http.MethodDelete:
			if r.URL.Query().Get("state") == "" ||
				r.URL.Query().Get("stateTs") == "" ||
				r.URL.Query().Get("stateSig") == "" {
				http.Error(w, "missing repository attestation", http.StatusBadRequest)
				return
			}
			mu.Lock()
			acked[kind] = r.URL.Query().Get("ids")
			mu.Unlock()
			_, _ = w.Write([]byte(`{"ok":true}`))
		default:
			http.Error(w, "wrong method", http.StatusMethodNotAllowed)
		}
	}))
	defer server.Close()

	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}
	daemon := &Daemon{
		config:   Config{CatalogURL: server.URL, Version: "test"},
		identity: identity,
	}
	daemon.gateway.Node.Name = "mirror9"
	if err := daemon.drainCollaboration(context.Background(), repo); err != nil {
		t.Fatal(err)
	}
	if acked["issues"] != "11" || acked["pulls"] != "12,14" ||
		acked["discussions"] != "13" {
		t.Fatalf("wrong acknowledgements: %+v", acked)
	}

	for ref, expected := range map[string]struct {
		path, contains string
	}{
		"main:issue": {
			".forkmesh/issues/open/42/issue-42.json", "Offline issue"},
		"forkmesh/pulls:pull": {
			".forkmesh/pulls/43/pull.md", "Offline pull"},
		"main:discussion": {
			".forkmesh/discussions/44/discussion.md", "Offline discussion"},
		"forkmesh/pulls:branch-review": {
			".forkmesh/branch-reviews/306395bddc1b0cac0297da67/0002-thread-comment.md", "Branch note"},
	} {
		branch := strings.SplitN(ref, ":", 2)[0]
		command := exec.Command("git", "--git-dir="+bare, "show", branch+":"+expected.path)
		output, err := command.CombinedOutput()
		if err != nil || !strings.Contains(string(output), expected.contains) {
			t.Fatalf("%s was not materialized: %v %s", ref, err, output)
		}
	}
	isolated := run(t, ".", "git", "--git-dir="+bare, "ls-tree", "-r",
		"--name-only", "refs/heads/forkmesh/pull/43")
	if strings.TrimSpace(isolated) != ".forkmesh/pulls/43/pull.md" {
		t.Fatalf("pull metadata branch is not isolated:\n%s", isolated)
	}
	if got, want := gitValue(context.Background(), bare, "rev-parse", "refs/pr/43/metadata"),
		gitValue(context.Background(), bare, "rev-parse", "refs/heads/forkmesh/pull/43"); got == "" || got != want {
		t.Fatalf("metadata compatibility ref %q does not point at isolated branch %q", got, want)
	}
}

func TestDirectCollaborationMaterializesDurablyWithoutController(t *testing.T) {
	_, bare := makeBareRepo(t)
	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}
	issueNumber, err := materializeDirectCollaboration(
		context.Background(), repo, "issues", collaborationItem{
			TitleIfNew: "Direct issue",
			Event: map[string]any{
				"type": "open", "id": "direct-issue", "title": "Direct issue",
				"body": "controller offline", "author": "alice",
				"ts": 1700000000000, "sig": "issue-signature",
			},
			Meta: map[string]any{},
		}, "mirror9")
	if err != nil || issueNumber != 1 {
		t.Fatalf("direct issue failed: number=%d err=%v", issueNumber, err)
	}
	pullNumber, err := materializeDirectCollaboration(
		context.Background(), repo, "pulls", collaborationItem{
			Pull: map[string]any{
				"title": "Direct pull", "description": "controller offline",
				"base": "main", "head": "feature", "author": "bob",
				"ts": 1700000001000, "sig": "pull-signature",
			},
		}, "mirror9")
	if err != nil || pullNumber != 1 {
		t.Fatalf("direct pull failed: number=%d err=%v", pullNumber, err)
	}
	discussionNumber, err := materializeDirectCollaboration(
		context.Background(), repo, "discussions", collaborationItem{
			TitleIfNew: "Direct discussion",
			Event: map[string]any{
				"type": "open", "id": "direct-discussion",
				"title": "Direct discussion", "category": "Ideas",
				"body": "controller offline", "author": "carol",
				"ts": 1700000002000, "sig": "discussion-signature",
			},
		}, "mirror9")
	if err != nil || discussionNumber != 1 {
		t.Fatalf("direct discussion failed: number=%d err=%v",
			discussionNumber, err)
	}
	for ref, path := range map[string]string{
		"main":           ".forkmesh/issues/open/1/issue-1.json",
		"forkmesh/pulls": ".forkmesh/pulls/1/pull.md",
	} {
		if got := gitValue(context.Background(), bare, "show", ref+":"+path); got == "" {
			t.Fatalf("%s was not committed on %s", path, ref)
		}
	}
	if got := gitValue(context.Background(), bare, "show",
		"main:.forkmesh/discussions/1/discussion.md"); !strings.Contains(got, "Direct discussion") {
		t.Fatalf("direct discussion was not committed: %s", got)
	}
}

// The loopback request is what tools/mirror_gateway.py _repo_collaboration
// builds, key for key. The handler decodes it strictly, so an item key the
// struct does not declare refused every web issue comment a mirror took
// (issue #700) while the materializer test above never saw the wire format.
func TestDirectCollaborationRequestAcceptsGatewayItems(t *testing.T) {
	for kind, item := range map[string]string{
		"issues":      `{"number":700,"signedNumber":700,"titleIfNew":"","event":{"type":"comment","id":"comment-web-1","body":"issues look great","attachments":[],"author":"alice","authorName":"jett","ts":1789501027000,"sig":"issue-signature"},"meta":{},"attachmentData":[],"submittedAt":1789501027100}`,
		"pulls":       `{"number":7,"event":{"type":"comment","body":"looks good","author":"bob","authorName":"jett","ts":1789501027000,"sig":"pull-signature"},"submittedAt":1789501027100}`,
		"discussions": `{"number":0,"titleIfNew":"Idea","event":{"type":"open","title":"Idea","category":"Ideas","body":"hello","author":"carol","authorName":"jett","ts":1789501027000,"sig":"discussion-signature"},"submittedAt":1789501027100}`,
	} {
		body := `{"owner":"forkmesh","repo":"forkmesh","kind":"` + kind + `","item":` + item + `}`
		input, err := decodeDirectCollaborationRequest(strings.NewReader(body))
		if err != nil {
			t.Fatalf("%s gateway item was refused: %v", kind, err)
		}
		if input.Kind != kind || len(input.Item.Event) == 0 || input.Item.SubmittedAt != 1789501027100 {
			t.Fatalf("%s gateway item decoded wrongly: %+v", kind, input)
		}
	}
	for name, body := range map[string]string{
		"unknown item key": `{"owner":"forkmesh","repo":"forkmesh","kind":"issues","item":{"number":1,"surprise":true}}`,
		"trailing data":    `{"owner":"forkmesh","repo":"forkmesh","kind":"issues","item":{}}{}`,
	} {
		if _, err := decodeDirectCollaborationRequest(strings.NewReader(body)); err == nil {
			t.Fatalf("%s was accepted", name)
		}
	}
}

func TestPullIntakeDoesNotReuseIndependentMetadataNumber(t *testing.T) {
	source, bare := makeBareRepo(t)
	run(t, source, "git", "checkout", "-q", "-b", "forkmesh/pull/70")
	writeFile(t, source+"/.forkmesh/pulls/70/pull.md", "---\nnumber: 70\ntitle: reserved\n---\n")
	run(t, source, "git", "add", ".forkmesh/pulls/70/pull.md")
	run(t, source, "git", "commit", "-q", "-m", "reserved independent pull")
	run(t, source, "git", "push", "-q", bare,
		"HEAD:refs/heads/forkmesh/pull/70")
	run(t, source, "git", "checkout", "-q", "main")

	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}
	rows := []json.RawMessage{json.RawMessage(
		`{"id":1,"number":70,"pull":{"title":"collision","description":"body","base":"main","head":"feature","author":"alice","authorName":"Alice","ts":1700000000000,"sig":"new-signature"}}`,
	)}
	ids, err := materializePullInbox(context.Background(), repo, rows, "mirror9")
	if err != nil || len(ids) != 1 {
		t.Fatalf("pull intake failed: ids=%v err=%v", ids, err)
	}
	if got := gitValue(context.Background(), bare, "show",
		"refs/heads/forkmesh/pulls:.forkmesh/pulls/71/pull.md"); !strings.Contains(got, "title: collision") {
		t.Fatalf("collision did not advance beyond reserved PR #70:\n%s", got)
	}
}

func TestIssueIntakeAllocatesCollisionAndDeduplicatesSignature(t *testing.T) {
	_, bare := makeBareRepo(t)
	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}
	rows := []json.RawMessage{
		json.RawMessage(`{"id":1,"number":7,"titleIfNew":"first","event":{"type":"open","id":"open-a","title":"first","author":"alice","ts":1700000000000,"sig":"same-signature"},"meta":{}}`),
		json.RawMessage(`{"id":2,"number":7,"titleIfNew":"collision","event":{"type":"open","id":"open-b","title":"collision","author":"bob","ts":1700000001000,"sig":"different-signature"},"meta":{}}`),
		json.RawMessage(`{"id":3,"number":99,"titleIfNew":"duplicate","event":{"type":"open","id":"open-c","title":"duplicate","author":"alice","ts":1700000000000,"sig":"same-signature"},"meta":{}}`),
	}
	ids, err := materializeIssueInbox(context.Background(), repo, rows, "mirror9")
	if err != nil {
		t.Fatal(err)
	}
	if got := len(ids); got != 3 {
		t.Fatalf("all durable rows should be acknowledged, got %d", got)
	}
	listing := run(t, ".", "git", "--git-dir="+bare, "ls-tree", "-r", "--name-only", "main", ".forkmesh/issues")
	if !strings.Contains(listing, "issue-7.json") ||
		!strings.Contains(listing, "issue-8.json") ||
		strings.Contains(listing, "issue-99.json") {
		t.Fatalf("collision/dedup result is wrong:\n%s", listing)
	}
}

// A Discord bug report reaches the mirror with the relay's reward estimate in
// its meta. The issue record has to keep it, or the report is filed with no
// reward and never reaches the ledger. A reward that does not credit the
// reporter it names is dropped rather than copied.
func TestIssueIntakeKeepsTheDiscordReportReward(t *testing.T) {
	_, bare := makeBareRepo(t)
	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}
	rows := []json.RawMessage{
		json.RawMessage(`{"id":1,"number":7,"titleIfNew":"crash","event":{"type":"open","id":"open-a","title":"crash","author":"relay","authorName":"forkbot","ts":1700000000000,"sig":"sig-a"},"meta":{"reporter":"spidertest","rewardEstimateLamports":3000000,"rewardRecipient":"spidertest","rewardId":"reward-discord-0123456789abcdef01234567","rewardCondition":"accepted_report","rewardStatus":"pending_wallet"}}`),
		json.RawMessage(`{"id":2,"number":8,"titleIfNew":"other","event":{"type":"open","id":"open-b","title":"other","author":"relay","authorName":"forkbot","ts":1700000001000,"sig":"sig-b"},"meta":{"reporter":"spidertest","rewardEstimateLamports":3000000,"rewardRecipient":"someone-else","rewardId":"reward-discord-fedcba9876543210fedcba98","rewardCondition":"accepted_report","rewardStatus":"pending_wallet"}}`),
	}
	if _, err := materializeIssueInbox(context.Background(), repo, rows, "mirror9"); err != nil {
		t.Fatal(err)
	}
	var kept map[string]any
	if err := json.Unmarshal([]byte(run(t, ".", "git", "--git-dir="+bare, "show",
		"main:.forkmesh/issues/open/7/issue-7.json")), &kept); err != nil {
		t.Fatal(err)
	}
	if kept["rewardId"] != "reward-discord-0123456789abcdef01234567" ||
		kept["rewardRecipient"] != "spidertest" ||
		kept["rewardEstimateLamports"] != float64(3000000) ||
		kept["rewardCondition"] != "accepted_report" ||
		kept["rewardStatus"] != "pending_wallet" || kept["authorName"] != "spidertest" {
		t.Fatalf("reward estimate was not carried onto the issue: %v", kept)
	}
	var dropped map[string]any
	if err := json.Unmarshal([]byte(run(t, ".", "git", "--git-dir="+bare, "show",
		"main:.forkmesh/issues/open/8/issue-8.json")), &dropped); err != nil {
		t.Fatal(err)
	}
	if _, ok := dropped["rewardId"]; ok {
		t.Fatalf("a reward crediting someone other than the reporter was copied: %v", dropped)
	}
}

// A public contributor may self-assign from the website (issue #570). The
// mirror projects that "claim" as a delta on the assignee list, so it adds or
// drops only the account it names and never replaces the maintainer's list.
func TestIssueClaimAddsAndRemovesOnlyTheClaimingAccount(t *testing.T) {
	base := []any{"maintainer"}
	assign := map[string]any{"type": "claim", "assignee": "Alice", "claimState": "assigned"}
	release := map[string]any{"type": "claim", "assignee": "alice", "claimState": "released"}

	claimed := applyIssueClaim(base, assign)
	if len(claimed) != 2 || stringValue(claimed[0]) != "maintainer" || stringValue(claimed[1]) != "Alice" {
		t.Fatalf("a claim should append only the claiming account, got %v", claimed)
	}
	if again := applyIssueClaim(claimed, assign); len(again) != 2 {
		t.Fatalf("re-claiming should not duplicate the assignee, got %v", again)
	}
	// The relay lower-cases the account session it checks a claim against, so
	// the projection has to match names case-insensitively or a release would
	// silently do nothing.
	released := applyIssueClaim(claimed, release)
	if len(released) != 1 || stringValue(released[0]) != "maintainer" {
		t.Fatalf("a release should drop only the claiming account, got %v", released)
	}
	if unknown := applyIssueClaim(base, release); len(unknown) != 1 {
		t.Fatalf("releasing an unheld claim should leave the list alone, got %v", unknown)
	}
	if empty := applyIssueClaim(base, map[string]any{"type": "claim", "claimState": "assigned"}); len(empty) != 1 {
		t.Fatalf("a nameless claim should change nothing, got %v", empty)
	}
}

func TestIssueIntakeRemovesIssueOnOwnerDelete(t *testing.T) {
	_, bare := makeBareRepo(t)
	repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}
	open := []json.RawMessage{
		json.RawMessage(`{"id":1,"number":7,"titleIfNew":"keep","event":{"type":"open","id":"open-keep","title":"keep","author":"alice","ts":1700000000000,"sig":"keep-signature"},"meta":{}}`),
		json.RawMessage(`{"id":2,"number":8,"titleIfNew":"drop","event":{"type":"open","id":"open-drop","title":"drop","author":"alice","ts":1700000001000,"sig":"drop-signature"},"meta":{}}`),
	}
	if _, err := materializeIssueInbox(context.Background(), repo, open, "mirror9"); err != nil {
		t.Fatal(err)
	}
	// A relay-authorized delete/self is the owner's removal, not a tombstone:
	// the folder leaves the served tree even though "bob" never opened it.
	deletes := []json.RawMessage{
		json.RawMessage(`{"id":3,"number":8,"event":{"type":"delete","id":"delete-web-8","target":"self","author":"bob","authorName":"Bob","ts":1700000002000,"sig":"delete-signature"}}`),
		json.RawMessage(`{"id":4,"number":8,"event":{"type":"delete","id":"delete-web-8-again","target":"self","author":"bob","authorName":"Bob","ts":1700000003000,"sig":"delete-again-signature"}}`),
	}
	ids, err := materializeIssueInbox(context.Background(), repo, deletes, "mirror9")
	if err != nil {
		t.Fatal(err)
	}
	if len(ids) != 2 {
		t.Fatalf("both the delete and its redelivery should be acknowledged, got %v", ids)
	}
	listing := run(t, ".", "git", "--git-dir="+bare, "ls-tree", "-r", "--name-only", "main", ".forkmesh/issues")
	if !strings.Contains(listing, "issue-7.json") || strings.Contains(listing, "issue-8.json") {
		t.Fatalf("owner delete did not remove the issue:\n%s", listing)
	}
	history := run(t, ".", "git", "--git-dir="+bare, "log", "--oneline", "--", ".forkmesh/issues/open/8")
	if !strings.Contains(history, "issue #8: delete (mirror intake)") {
		t.Fatalf("delete was not committed with a readable message:\n%s", history)
	}

	// Deleting the last remaining issue must still commit cleanly even though
	// the pathspec is left with nothing in the working tree.
	last := []json.RawMessage{
		json.RawMessage(`{"id":5,"number":7,"event":{"type":"delete","id":"delete-web-7","target":"self","author":"bob","ts":1700000004000,"sig":"last-delete-signature"}}`),
	}
	if _, err := materializeIssueInbox(context.Background(), repo, last, "mirror9"); err != nil {
		t.Fatal(err)
	}
	listing = run(t, ".", "git", "--git-dir="+bare, "ls-tree", "-r", "--name-only", "main", ".forkmesh/issues")
	if strings.Contains(listing, "issue-7.json") {
		t.Fatalf("the last issue survived its delete:\n%s", listing)
	}
}

// A relay hands back a bounded page, not the whole queue, so a backlog has to
// be followed across reads. It also must not be followed forever: a page whose
// rows the mirror cannot acknowledge (owner-retained or undecodable) comes
// back byte-identical on the next read.
func TestCollaborationDrainFollowsPagesUntilAcknowledgementsStop(t *testing.T) {
	_, bare := makeBareRepo(t)
	identity, _ := testIdentity(t)
	pages := []string{
		`{"pending":[{"id":11,"number":42,"titleIfNew":"First","event":{"type":"open","id":"issue-open-42","title":"First","body":"page one","author":"alice","authorName":"Alice","ts":1700000000000,"sig":"sig-a"},"meta":{}}],"more":true}`,
		`{"pending":[{"id":12,"number":43,"titleIfNew":"Second","event":{"type":"open","id":"issue-open-43","title":"Second","body":"page two","author":"alice","authorName":"Alice","ts":1700000001000,"sig":"sig-b"},"meta":{}}],"more":true}`,
		// Nothing acknowledgeable: the identical page would repeat forever.
		`{"pending":[{"id":0,"number":44,"event":{"type":"open"}}],"more":true}`,
		`{"pending":[{"id":13,"number":45,"titleIfNew":"Unreached","event":{"type":"open","id":"issue-open-45","title":"Unreached","author":"alice","ts":1700000002000,"sig":"sig-c"},"meta":{}}],"more":true}`,
	}
	var mu sync.Mutex
	reads := 0
	var acked []string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch r.Method {
		case http.MethodGet:
			mu.Lock()
			page := ""
			if reads < len(pages) {
				page = pages[reads]
			} else {
				page = `{"pending":[]}`
			}
			reads++
			mu.Unlock()
			_, _ = w.Write([]byte(page))
		case http.MethodDelete:
			mu.Lock()
			acked = append(acked, r.URL.Query().Get("ids"))
			mu.Unlock()
			_, _ = w.Write([]byte(`{"ok":true}`))
		}
	}))
	defer server.Close()

	daemon := &Daemon{
		config:   Config{CatalogURL: server.URL, Version: "test"},
		identity: identity,
	}
	daemon.gateway.Node.Name = "mirror9"
	err := daemon.drainCollaborationKind(context.Background(),
		Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}, "issues")
	if err != nil {
		t.Fatal(err)
	}
	mu.Lock()
	defer mu.Unlock()
	if reads != 3 {
		t.Fatalf("expected the drain to stop on the unacknowledgeable page, got %d reads", reads)
	}
	if strings.Join(acked, ",") != "11,12" {
		t.Fatalf("wrong acknowledgements across pages: %v", acked)
	}
}

func TestPullEventMarkdownCarriesPatchsetAnchor(t *testing.T) {
	// A line-anchored review event names the patchset it was written against
	// and the commented text (both signed). The mirror must persist them the
	// way the desktop's PullStore::writeEventFile does, or a thread relayed
	// through a mirror loses the proof that keeps it on the right lines.
	markdown := pullEventMarkdown(map[string]any{
		"type": "thread-comment", "id": "ev-1", "author": "alice", "ts": 1700000000000,
		"path": "src/x.cpp", "side": "new", "lineStart": 42, "lineEnd": 44,
		"threadId": "ev-1", "body": "Use guard",
		"patchset":    "0123456789abcdef0123456789abcdef01234567",
		"anchorLines": "int x = 1;\nreturn x;\n}",
		"sig":         "sig-1",
	})
	if !strings.Contains(markdown, "\npatchset: 0123456789abcdef0123456789abcdef01234567\n") {
		t.Fatalf("patchset missing from event front matter:\n%s", markdown)
	}
	encoded := base64.RawURLEncoding.EncodeToString([]byte("int x = 1;\nreturn x;\n}"))
	if !strings.Contains(markdown, "\nanchorLinesB64: "+encoded+"\n") {
		t.Fatalf("anchorLinesB64 missing from event front matter:\n%s", markdown)
	}
	if strings.Contains(markdown, "anchorLines: ") {
		t.Fatalf("multi-line anchor text must not be written raw:\n%s", markdown)
	}
	// Without an anchor the file is byte-identical to what it was before.
	plain := pullEventMarkdown(map[string]any{
		"type": "comment", "id": "ev-2", "author": "alice", "ts": 1, "body": "hi", "sig": "s",
	})
	if strings.Contains(plain, "patchset") || strings.Contains(plain, "anchorLines") {
		t.Fatalf("unanchored event grew anchor keys:\n%s", plain)
	}
}

func TestPullRecordsKeepTheClaimedName(t *testing.T) {
	record := pullMarkdown(7, map[string]any{"title": "Fix", "base": "main", "head": "fix",
		"author": "key", "authorName": "", "authorClaimedName": "bob", "sig": "s", "ts": 1})
	if !strings.Contains(record, "\nauthorName: \n") || !strings.Contains(record, "\nauthorClaimedName: bob\n") {
		t.Fatalf("claimed name missing from pull.md:\n%s", record)
	}
	event := pullEventMarkdown(map[string]any{
		"type": "comment", "id": "ev-3", "author": "key", "authorClaimedName": "bob", "ts": 1, "body": "hi", "sig": "s",
	})
	if !strings.Contains(event, "\nauthorClaimedName: bob\n") {
		t.Fatalf("claimed name missing from the event:\n%s", event)
	}
	verified := pullMarkdown(8, map[string]any{"title": "Fix", "base": "main", "head": "fix",
		"author": "key", "authorName": "bob", "sig": "s", "ts": 1})
	if strings.Contains(verified, "authorClaimedName") {
		t.Fatalf("verified pull grew a claimed name:\n%s", verified)
	}
}

func TestBranchReviewItemWritesLedgerOnce(t *testing.T) {
	root := t.TempDir()
	item := collaborationItem{
		ID:     9,
		Branch: "feature/review",
		Event: map[string]any{
			"type": "thread-comment", "id": "thread-comment-ios-1", "author": "alice",
			"ts": 1790000000000, "path": "src/app.txt", "side": "new",
			"lineStart": 3, "lineEnd": 5, "threadId": "thread-comment-ios-1",
			"body": "Rename these", "anchorLines": "alpha\nbeta\ngamma", "sig": "sig-1",
		},
	}
	if key := branchReviewKey("feature/review"); key != "306395bddc1b0cac0297da67" {
		t.Fatalf("branch review key = %s", key)
	}
	dir := filepath.Join(root, ".forkmesh", "branch-reviews", "306395bddc1b0cac0297da67")
	for attempt := 0; attempt < 2; attempt++ {
		wrote, err := writeBranchReviewItem(root, item)
		if err != nil || !wrote {
			t.Fatalf("attempt %d: wrote=%v err=%v", attempt, wrote, err)
		}
	}
	marker, err := os.ReadFile(filepath.Join(dir, "branch.md"))
	if err != nil || string(marker) != "---\nbranch: feature/review\n---\n" {
		t.Fatalf("branch.md = %q, %v", marker, err)
	}
	entries, _ := os.ReadDir(dir)
	if len(entries) != 2 {
		t.Fatalf("a replayed event must not be written twice: %d entries", len(entries))
	}
	event, err := os.ReadFile(filepath.Join(dir, "0002-thread-comment.md"))
	if err != nil || !strings.Contains(string(event), "\nlineStart: 3\n") ||
		!strings.Contains(string(event), "\nsig: sig-1\n") {
		t.Fatalf("event file = %q, %v", event, err)
	}
	for _, rejected := range []collaborationItem{
		{ID: 10, Branch: "feature/review", Event: map[string]any{"type": "comment", "id": "c"}},
		{ID: 11, Branch: "../escape", Event: item.Event},
		{ID: 12, Branch: "bad\nname", Event: item.Event},
	} {
		if wrote, err := writeBranchReviewItem(root, rejected); wrote || err != nil {
			t.Fatalf("item %d must be refused: wrote=%v err=%v", rejected.ID, wrote, err)
		}
	}
	if _, err := os.Stat(filepath.Join(root, ".forkmesh", "pulls")); err == nil {
		t.Fatalf("a branch review must not touch the pull directories")
	}
}

func TestMoveIssueDirectoryKeepsCollidingEntries(t *testing.T) {
	root := t.TempDir()
	issues := filepath.Join(root, ".forkmesh", "issues")
	from := filepath.Join(issues, "open", "7")
	to := filepath.Join(issues, "closed", "7")
	writeTestFile(t, filepath.Join(from, "issue-7.json"), "source record")
	writeTestFile(t, filepath.Join(from, "attachments", "a.png"), "source image")
	writeTestFile(t, filepath.Join(from, "only-source.txt"), "moved")
	writeTestFile(t, filepath.Join(to, "issue-7.json"), "destination record")
	writeTestFile(t, filepath.Join(to, "attachments", "b.png"), "destination image")
	if err := moveIssueDirectory(from, to); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Lstat(from); !os.IsNotExist(err) {
		t.Fatalf("source folder left behind: %v", err)
	}
	if got := readTestFile(t, filepath.Join(to, "issue-7.json")); got != "destination record" {
		t.Fatalf("destination record = %q", got)
	}
	if got := readTestFile(t, filepath.Join(to, "only-source.txt")); got != "moved" {
		t.Fatalf("moved entry = %q", got)
	}
	records, _ := filepath.Glob(filepath.Join(to, "issue-7.json.conflict-*"))
	if len(records) != 1 || readTestFile(t, records[0]) != "source record" {
		t.Fatalf("conflicting record = %v", records)
	}
	folders, _ := filepath.Glob(filepath.Join(to, "attachments.conflict-*"))
	if len(folders) != 1 || readTestFile(t, filepath.Join(folders[0], "a.png")) != "source image" {
		t.Fatalf("conflicting folder = %v", folders)
	}
	if got := readTestFile(t, filepath.Join(to, "attachments", "b.png")); got != "destination image" {
		t.Fatalf("destination attachment = %q", got)
	}
	if files, err := issueFiles(root); err != nil || len(files) != 1 {
		t.Fatalf("issue files = %v %v", files, err)
	}
}

func TestCollaborationWorktreeChecksOutOnlyForkmeshAndKeepsTheRest(t *testing.T) {
	root := t.TempDir()
	source := filepath.Join(root, "source")
	if err := os.MkdirAll(filepath.Join(source, ".forkmesh", "issues"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(source, "big"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(source, "big", "blob"), []byte(strings.Repeat("x", 1<<16)), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(source, ".forkmesh", "issues", "1.json"), []byte(`{"number":1}`), 0o644); err != nil {
		t.Fatal(err)
	}
	runTestGit(t, root, "init", "-q", "-b", "main", source)
	runTestGit(t, source, "add", "-A")
	runTestGit(t, source, "-c", "user.name=t", "-c", "user.email=t@example.com", "commit", "-qm", "init")
	bare := filepath.Join(root, "bare.git")
	runTestGit(t, root, "clone", "-q", "--bare", source, bare)

	dir, cleanup, err := collaborationWorktree(context.Background(), Repository{GitDir: bare}, "main", false, "n1")
	if err != nil {
		t.Fatal(err)
	}
	defer cleanup()
	if _, err := os.Stat(filepath.Join(dir, "big", "blob")); !os.IsNotExist(err) {
		t.Fatalf("files outside .forkmesh were checked out: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, ".forkmesh", "issues", "1.json")); err != nil {
		t.Fatalf(".forkmesh was not checked out: %v", err)
	}
	if err := os.WriteFile(filepath.Join(dir, ".forkmesh", "issues", "2.json"), []byte(`{"number":2}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := commitCollaboration(context.Background(), dir, ".forkmesh/issues", "issue 2"); err != nil {
		t.Fatal(err)
	}
	tree := runTestGit(t, bare, "ls-tree", "-r", "--name-only", "main")
	for _, want := range []string{".forkmesh/issues/1.json", ".forkmesh/issues/2.json", "big/blob"} {
		if !strings.Contains(tree, want) {
			t.Fatalf("main lost %s after the intake commit:\n%s", want, tree)
		}
	}
	after := strings.ToLower(runTestGit(t, bare, "config", "--list", "--local"))
	for _, key := range []string{"sparsecheckout", "worktreeconfig"} {
		if strings.Contains(after, key) {
			t.Fatalf("intake put %s in the shared repository config:\n%s", key, after)
		}
	}
}

func TestCollaborationIntakeRefusesCommittedSymlinks(t *testing.T) {
	victim := filepath.Join(t.TempDir(), "post-receive")
	if err := os.WriteFile(victim, []byte("#!/bin/sh\nexit 0\n"), 0o700); err != nil {
		t.Fatal(err)
	}
	for name, link := range map[string]string{
		"file":   ".forkmesh/issues/open/1/issue-1.json",
		"folder": ".forkmesh/issues",
	} {
		t.Run(name, func(t *testing.T) {
			source, bare := makeBareRepo(t)
			target := victim
			if name == "folder" {
				target = filepath.Dir(victim)
			}
			if err := os.MkdirAll(filepath.Dir(filepath.Join(source, link)), 0o700); err != nil {
				t.Fatal(err)
			}
			if err := os.Symlink(target, filepath.Join(source, link)); err != nil {
				t.Fatal(err)
			}
			run(t, source, "git", "add", "-A")
			run(t, source, "git", "commit", "-m", "plant link")
			run(t, source, "git", "push", bare, "HEAD:main")
			repo := Repository{Owner: "forkmesh", Name: "forkmesh", GitDir: bare}
			_, err := materializeDirectCollaboration(context.Background(), repo, "issues",
				collaborationItem{
					Number: 1,
					Event: map[string]any{
						"type": "open", "id": "x", "title": "a\";touch /tmp/p;\"",
						"author": "mallory", "ts": 1700000000000, "sig": "s",
					},
					AttachmentData: []map[string]any{{"name": "post-receive",
						"data": base64.StdEncoding.EncodeToString([]byte("#!/bin/sh\nid\n"))}},
				}, "mirror9")
			if err == nil {
				t.Fatal("intake wrote through a committed symlink")
			}
			if got, _ := os.ReadFile(victim); string(got) != "#!/bin/sh\nexit 0\n" {
				t.Fatalf("file outside the worktree was rewritten: %q", got)
			}
			if entries, _ := os.ReadDir(filepath.Dir(victim)); len(entries) != 1 {
				t.Fatalf("intake created files outside the worktree: %v", entries)
			}
		})
	}
}

func TestPullEventTypeCannotNameAPathOutsideThePull(t *testing.T) {
	root := t.TempDir()
	pullDir := filepath.Join(root, ".forkmesh", "pulls", "1")
	if err := os.MkdirAll(pullDir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(pullDir, "pull.md"), []byte("# pull\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	escape := filepath.Join(root, "escaped")
	if err := os.MkdirAll(escape, 0o700); err != nil {
		t.Fatal(err)
	}
	wrote, _, err := writePullItem(root, collaborationItem{Number: 1, Event: map[string]any{
		"type": "x/../../../../escaped/p", "id": "e1", "body": "hi",
	}})
	if wrote || err != nil {
		t.Fatalf("traversing event type accepted: wrote=%v err=%v", wrote, err)
	}
	if entries, _ := os.ReadDir(escape); len(entries) != 0 {
		t.Fatalf("pull event escaped its folder: %v", entries)
	}
	if wrote, _, err := writePullItem(root, collaborationItem{Number: 1, Event: map[string]any{
		"type": "review", "id": "e2", "body": "ok",
	}}); !wrote || err != nil {
		t.Fatalf("plain review event refused: wrote=%v err=%v", wrote, err)
	}
}
