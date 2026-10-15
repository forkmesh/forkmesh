package pulls

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"strconv"
	"strings"
	"unicode/utf16"
)

func readEventFilesIn(dir string) []Event {
	events := []Event{}
	for _, name := range listDir(dir, false) {
		if !eventFilePattern.MatchString(name) {
			continue
		}
		raw, err := os.ReadFile(dir + "/" + name)
		if err != nil {
			continue
		}
		events = append(events, eventFromFrontMatter(parseFrontMatter(raw)))
	}
	return events
}

func nextEventIndexIn(dir string) int {
	max := 1
	for _, name := range listDir(dir, false) {
		if !eventFilePattern.MatchString(name) {
			continue
		}
		if n, ok := qtToInt(name[:4]); ok && n > max {
			max = n
		}
	}
	return max + 1
}

func writeEventFileIn(dir string, index int, ev *Event) error {
	lines := []string{
		"---",
		"type: " + ev.Type,
		"id: " + ev.ID,
		"author: " + ev.Author,
		"authorName: " + ev.AuthorName,
		"ts: " + strconv.FormatInt(ev.Ts, 10),
	}
	if ev.Type == "review" {
		lines = append(lines, "state: "+ev.State)
	}
	if ev.Type == "line-comment" {
		lines = append(lines, "path: "+ev.Path, "side: "+ev.Side, "line: "+strconv.Itoa(ev.Line))
	}
	if ev.ThreadID != "" {
		lines = append(lines, "threadId: "+ev.ThreadID)
	}
	if ev.ParentID != "" {
		lines = append(lines, "parentId: "+ev.ParentID)
	}
	if ev.Type == "thread-comment" {
		lines = append(lines, "path: "+ev.Path, "side: "+ev.Side, "lineStart: "+strconv.Itoa(ev.LineStart),
			"lineEnd: "+strconv.Itoa(ev.LineEnd))
	}
	if ev.SuggestionPatch != "" {
		lines = append(lines, "suggestionPatchB64: "+encodeFrontMatterBlob(ev.SuggestionPatch))
	}
	if ev.TargetPath != "" {
		lines = append(lines, "targetPath: "+ev.TargetPath)
	}
	if ev.AppliedCommit != "" {
		lines = append(lines, "appliedCommit: "+ev.AppliedCommit)
	}
	if ev.Patchset != "" {
		lines = append(lines, "patchset: "+ev.Patchset)
	}
	if ev.AnchorLines != "" {
		lines = append(lines, "anchorLinesB64: "+encodeFrontMatterBlob(ev.AnchorLines))
	}
	if (ev.Type == "thread-state" || ev.Type == "suggestion-state") && ev.State != "" {
		lines = append(lines, "state: "+ev.State)
	}
	lines = append(lines, "sig: "+ev.Sig, "---", "")
	name := fmt.Sprintf("%04d-%s.md", index, ev.Type)
	return writeTextFile(dir+"/"+name, strings.Join(lines, "\n")+"\n"+ev.Body+"\n")
}

func (s *Store) appendEvent(number int, ev *Event, commitMsg string) error {
	if !exists(s.pullDir(number) + "/pull.md") {
		return errorString(MissingPullMessage(number))
	}
	defer s.lockLedger()()
	if err := writeEventFileIn(s.pullDir(number), nextEventIndexIn(s.pullDir(number)), ev); err != nil {
		return err
	}
	return s.commit(commitMsg)
}

type EventInput struct {
	Type            string
	Body            string
	State           string
	Path            string
	Side            string
	Line            int
	LineStart       int
	LineEnd         int
	SuggestionPatch string
	ThreadID        string
	ParentID        string
	AppliedCommit   string
	Patchset        string
	AnchorLines     string
}

func (s *Store) AddEvent(number int, in EventInput) (Event, error) {
	if !s.CanWrite() {
		return Event{}, errorString(readOnlyMessage)
	}
	n := strconv.Itoa(number)
	ev := Event{Type: in.Type, Body: stripEdgeNewlines(in.Body)}
	var message string
	switch in.Type {
	case "comment":
		message = "pull #" + n + ": comment"
	case "review":
		ev.State = in.State
		message = "pull #" + n + ": review (" + in.State + ")"
	case "line-comment":
		ev.Path, ev.Side, ev.Line = in.Path, in.Side, in.Line
		ev.Patchset, ev.AnchorLines = in.Patchset, in.AnchorLines
		message = "pull #" + n + ": comment on " + in.Path + ":" + strconv.Itoa(in.Line)
	case "thread-comment":
		ev.Path, ev.Side, ev.LineStart = in.Path, in.Side, in.LineStart
		ev.LineEnd = in.LineEnd
		if ev.LineEnd <= 0 {
			ev.LineEnd = in.LineStart
		}
		ev.SuggestionPatch = stripEdgeNewlines(in.SuggestionPatch)
		ev.Patchset, ev.AnchorLines = in.Patchset, in.AnchorLines
		message = "pull #" + n + ": thread on " + in.Path + ":" + strconv.Itoa(in.LineStart)
	case "thread-reply":
		ev.ThreadID, ev.ParentID = in.ThreadID, in.ParentID
		message = "pull #" + n + ": thread reply"
	case "thread-state":
		ev.ThreadID, ev.State = in.ThreadID, in.State
		message = "pull #" + n + ": thread " + in.State
	case "suggestion-state":
		ev.ThreadID, ev.State, ev.AppliedCommit = in.ThreadID, in.State, in.AppliedCommit
		message = "pull #" + n + ": suggestion " + in.State
	default:
		return Event{}, errorString("bad_request")
	}
	ev = s.Signer.SignEvent(number, ev)
	return ev, s.appendEvent(number, &ev, message)
}

func (s *Store) ApplyRemoteEvent(number int, ev Event) error {
	if !s.CanWrite() {
		return errNoMessage
	}
	kind := "comment"
	if ev.Type == "review" {
		kind = "review"
	}
	return s.appendEvent(number, &ev, "pull #"+strconv.Itoa(number)+": "+kind+" (from "+shortAuthor(ev.AuthorName, ev.Author)+")")
}

func BranchReviewKey(branch string) string {
	sum := sha256.Sum256([]byte(branch))
	return hex.EncodeToString(sum[:])[:24]
}

func (s *Store) BranchReviewDir(branch string) string {
	meta := s.MetaWorkTree()
	if meta == "" || branch == "" {
		return ""
	}
	return meta + "/.forkmesh/branch-reviews/" + BranchReviewKey(branch)
}

func (s *Store) existingPullMetaCheckout() string {
	if s.WorkTree == "" {
		return ""
	}
	if headBranchFromFile(s.WorkTree) == "forkmesh/pulls" {
		return s.WorkTree
	}
	if s.DataRoot == "" {
		return ""
	}
	dir := MetaWorkTreePath(s.DataRoot, s.WorkTree)
	if exists(dir+"/.git") && headBranchFromFile(dir) == "forkmesh/pulls" {
		return dir
	}
	return ""
}

func (s *Store) ReadBranchReviewEvents(branch string) []Event {
	if branch == "" {
		return []Event{}
	}
	if s.CanWrite() {
		meta := s.existingPullMetaCheckout()
		if meta == "" {
			return []Event{}
		}
		return readEventFilesIn(meta + "/.forkmesh/branch-reviews/" + BranchReviewKey(branch))
	}
	if s.Mirror == "" {
		return []Event{}
	}
	ref := s.mirrorRef(s.Mirror)
	if ref == "" {
		return []Event{}
	}
	prefix := ".forkmesh/branch-reviews/" + BranchReviewKey(branch) + "/"
	listing := s.runGit(s.Mirror, "ls-tree", "--name-only", ref, prefix)
	if !listing.ok {
		return []Event{}
	}
	names := []string{}
	for _, raw := range strings.Split(listing.out(), "\n") {
		path := strings.TrimSpace(qtFromUtf8([]byte(raw)))
		if !strings.HasPrefix(path, prefix) {
			continue
		}
		name := path[len(prefix):]
		if eventFilePattern.MatchString(name) {
			names = append(names, name)
		}
	}
	sortStrings(names)
	events := []Event{}
	for _, name := range names {
		shown := s.runGit(s.Mirror, "show", ref+":"+prefix+name)
		if shown.ok {
			events = append(events, eventFromFrontMatter(parseFrontMatter(shown.output)))
		}
	}
	return events
}

func (s *Store) appendBranchEvent(branch string, ev *Event, commitMsg string) error {
	if !s.CanWrite() {
		return errorString(readOnlyMessage)
	}
	dir := s.BranchReviewDir(branch)
	defer s.lockLedger()()
	if dir == "" || os.MkdirAll(dir, 0o755) != nil {
		return errorString("Could not open the review ledger for " + branch + ".")
	}
	marker := dir + "/branch.md"
	if !exists(marker) {
		if err := writeTextFile(marker, "---\nbranch: "+branch+"\n---\n"); err != nil {
			return err
		}
	}
	if err := writeEventFileIn(dir, nextEventIndexIn(dir), ev); err != nil {
		return err
	}
	meta := s.MetaWorkTree()
	relative := ".forkmesh/branch-reviews/" + BranchReviewKey(branch)
	if r := s.runGit(meta, "add", "--", relative); !r.ok {
		return errorString("git add failed: " + r.err)
	}
	if r := s.runGit(meta, "commit", "-m", commitMsg, "--", relative); !r.ok {
		if !(strings.Contains(r.err, "nothing to commit") || r.err == "") {
			return errorString("git commit failed: " + r.err)
		}
	}
	return nil
}

type BranchEventInput struct {
	Type        string
	Path        string
	Side        string
	LineStart   int
	LineEnd     int
	Body        string
	ThreadID    string
	ParentID    string
	State       string
	Patchset    string
	AnchorLines string
}

func (s *Store) AddBranchEvent(branch string, in BranchEventInput) (Event, error) {
	ev := Event{Type: in.Type}
	var message string
	switch in.Type {
	case "thread-comment":
		ev.Path, ev.Side, ev.LineStart = in.Path, in.Side, in.LineStart
		ev.LineEnd = in.LineEnd
		if ev.LineEnd <= 0 {
			ev.LineEnd = in.LineStart
		}
		ev.Body = stripEdgeNewlines(in.Body)
		ev.Patchset, ev.AnchorLines = in.Patchset, in.AnchorLines
		message = "branch " + branch + ": thread on " + in.Path + ":" + strconv.Itoa(in.LineStart)
	case "thread-reply":
		ev.ThreadID, ev.ParentID = in.ThreadID, in.ParentID
		ev.Body = stripEdgeNewlines(in.Body)
		message = "branch " + branch + ": reply on thread"
	case "thread-state":
		ev.ThreadID, ev.State = in.ThreadID, in.State
		message = "branch " + branch + ": thread " + in.State
	default:
		return Event{}, errorString("bad_request")
	}
	ev = s.Signer.SignBranchEvent(branch, ev)
	return ev, s.appendBranchEvent(branch, &ev, message)
}

func ValidReviewBranchName(branch string) bool {
	if branch == "" || len(utf16.Encode([]rune(branch))) > 255 || strings.HasPrefix(branch, "-") ||
		strings.HasPrefix(branch, "/") || strings.HasSuffix(branch, "/") || strings.HasSuffix(branch, ".lock") ||
		strings.Contains(branch, "..") || strings.Contains(branch, "//") || strings.Contains(branch, "@{") {
		return false
	}
	for _, r := range branch {
		if r < 0x20 || r == 0x7f || strings.ContainsRune(" ~^:?*[\\", r) {
			return false
		}
	}
	return true
}

func (s *Store) ApplyRemoteBranchEvent(branch string, ev Event) error {
	if !ValidReviewBranchName(branch) || (ev.Type != "thread-comment" && ev.Type != "thread-reply" && ev.Type != "thread-state") {
		return errorString("Not a branch review event.")
	}
	if ev.ID != "" && s.CanWrite() {
		for _, existing := range readEventFilesIn(s.BranchReviewDir(branch)) {
			if existing.ID == ev.ID {
				return nil
			}
		}
	}
	return s.appendBranchEvent(branch, &ev, "branch "+branch+": "+ev.Type+" (from "+shortAuthor(ev.AuthorName, ev.Author)+")")
}
