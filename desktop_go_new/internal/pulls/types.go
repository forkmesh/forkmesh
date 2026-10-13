package pulls

import (
	"encoding/base64"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"unicode/utf8"
)

type Event struct {
	Type            string `json:"type"`
	ID              string `json:"id"`
	Author          string `json:"author"`
	AuthorName      string `json:"authorName"`
	Ts              int64  `json:"ts"`
	Body            string `json:"body"`
	State           string `json:"state"`
	Path            string `json:"path"`
	Side            string `json:"side"`
	Line            int    `json:"line"`
	ThreadID        string `json:"threadId"`
	ParentID        string `json:"parentId"`
	LineStart       int    `json:"lineStart"`
	LineEnd         int    `json:"lineEnd"`
	SuggestionPatch string `json:"suggestionPatch"`
	TargetPath      string `json:"targetPath"`
	AppliedCommit   string `json:"appliedCommit"`
	Patchset        string `json:"patchset"`
	AnchorLines     string `json:"anchorLines"`
	Sig             string `json:"sig"`
}

type Pull struct {
	Number                 int
	Title                  string
	Description            string
	Base                   string
	Head                   string
	Status                 string
	Ts                     int64
	Author                 string
	AuthorName             string
	Sig                    string
	Patch                  string
	Commits                string
	BranchBacked           bool
	CreationBaseOid        string
	CreationHeadOid        string
	MergeBase              string
	MergeHead              string
	Client                 string
	ClientSig              string
	RewardEstimateLamports int64
	RewardRecipient        string
	RewardID               string
	RewardCondition        string
	RewardStatus           string
	RewardTxSignature      string
	FilesChanged           int
	Additions              int
	Deletions              int
	Events                 []Event
	PatchHeadOid           string
	ArchivedOnCodeBranch   bool
}

func (p *Pull) JSON() map[string]any {
	return map[string]any{
		"number":                 p.Number,
		"title":                  p.Title,
		"description":            p.Description,
		"base":                   p.Base,
		"head":                   p.Head,
		"status":                 p.Status,
		"ts":                     float64(p.Ts),
		"author":                 p.Author,
		"authorName":             p.AuthorName,
		"sig":                    p.Sig,
		"creationBaseOid":        p.CreationBaseOid,
		"creationHeadOid":        p.CreationHeadOid,
		"client":                 p.Client,
		"clientSig":              p.ClientSig,
		"rewardEstimateLamports": float64(p.RewardEstimateLamports),
		"rewardRecipient":        p.RewardRecipient,
		"rewardId":               p.RewardID,
		"rewardCondition":        p.RewardCondition,
		"rewardStatus":           p.RewardStatus,
		"rewardTxSignature":      p.RewardTxSignature,
	}
}

func latestIndependentReviewStates(p *Pull) map[string]string {
	latest := map[string]string{}
	for _, ev := range p.Events {
		if ev.Type != "review" || ev.Author == "" || ev.Author == p.Author {
			continue
		}
		if ev.State == "approved" || ev.State == "changes_requested" {
			latest[ev.Author] = ev.State
		} else {
			delete(latest, ev.Author)
		}
	}
	return latest
}

func (p *Pull) IndependentApprovalCount() int {
	latest := latestIndependentReviewStates(p)
	approvals := 0
	for _, state := range latest {
		if state == "changes_requested" {
			return 0
		}
		if state == "approved" {
			approvals++
		}
	}
	return approvals
}

func (p *Pull) IndependentChangesRequestedBy() []string {
	latest := latestIndependentReviewStates(p)
	names := map[string]string{}
	for _, ev := range p.Events {
		if ev.Type == "review" && ev.Author != "" && ev.AuthorName != "" {
			names[ev.Author] = ev.AuthorName
		}
	}
	blocking := []string{}
	for author, state := range latest {
		if state != "changes_requested" {
			continue
		}
		name := names[author]
		if name == "" {
			name = leftRunes(author, 10)
		}
		blocking = append(blocking, name)
	}
	sort.SliceStable(blocking, func(i, j int) bool {
		return strings.ToLower(blocking[i]) < strings.ToLower(blocking[j])
	})
	return blocking
}

func (p *Pull) HasIndependentChangesRequested() bool {
	return len(p.IndependentChangesRequestedBy()) > 0
}

func (p *Pull) IndependentReviewGateSatisfied() bool {
	return !p.HasIndependentChangesRequested() && p.IndependentApprovalCount() > 0
}

func AdvanceRewardForMerge(p *Pull) bool {
	if strings.TrimSpace(p.RewardStatus) != "pending_merge" {
		return false
	}
	condition := strings.TrimSpace(p.RewardCondition)
	if condition != "" && condition != "merged" {
		return false
	}
	p.RewardStatus = "pending_wallet"
	return true
}

func ComputeStats(p *Pull) {
	files, add, del := 0, 0, 0
	patch := p.Patch
	start := 0
	for start <= len(patch) {
		nl := strings.IndexByte(patch[start:], '\n')
		var line string
		if nl < 0 {
			line = patch[start:]
		} else {
			line = patch[start : start+nl]
		}
		switch {
		case strings.HasPrefix(line, "diff --git "):
			files++
		case strings.HasPrefix(line, "+++ ") || strings.HasPrefix(line, "--- "):
		case strings.HasPrefix(line, "+"):
			add++
		case strings.HasPrefix(line, "-"):
			del++
		}
		if nl < 0 {
			break
		}
		start += nl + 1
	}
	p.FilesChanged = files
	p.Additions = add
	p.Deletions = del
}

type frontMatter struct {
	values map[string]string
	body   string
	valid  bool
}

func (f frontMatter) get(key string) string { return f.values[key] }

func (f frontMatter) has(key string) bool {
	_, ok := f.values[key]
	return ok
}

func (f frontMatter) num(key string) int64 { return qtToLongLong(f.values[key]) }

func parseFrontMatter(raw []byte) frontMatter {
	fm := frontMatter{values: map[string]string{}}
	text := qtFromUtf8(raw)
	lines := strings.Split(text, "\n")
	if len(lines) == 0 || strings.TrimSpace(lines[0]) != "---" {
		fm.body = text
		return fm
	}
	i := 1
	for ; i < len(lines) && lines[i] != "---"; i++ {
		line := lines[i]
		if sep := strings.Index(line, ": "); sep >= 0 {
			fm.values[line[:sep]] = line[sep+2:]
		} else if strings.HasSuffix(line, ":") {
			fm.values[line[:len(line)-1]] = ""
		}
	}
	if i >= len(lines) {
		return fm
	}
	body := strings.Join(lines[i+1:], "\n")
	body = strings.TrimLeft(body, "\n")
	body = strings.TrimRight(body, "\n")
	fm.body = body
	fm.valid = true
	return fm
}

var eventFilePattern = regexp.MustCompile(`^\d{4}-[a-z-]+\.md$`)

func decodeFrontMatterBlob(s string) string {
	if s == "" {
		return ""
	}
	cleaned := strings.Map(func(r rune) rune {
		if (r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '-' || r == '_' {
			return r
		}
		return -1
	}, strings.TrimRight(s, "="))
	raw, err := base64.RawURLEncoding.DecodeString(cleaned)
	if err != nil {
		for n := len(cleaned) - 1; n >= 0 && err != nil; n-- {
			raw, err = base64.RawURLEncoding.DecodeString(cleaned[:n])
		}
	}
	return qtFromUtf8(raw)
}

func eventFromFrontMatter(fm frontMatter) Event {
	return Event{
		Type:            fm.get("type"),
		ID:              fm.get("id"),
		Author:          fm.get("author"),
		AuthorName:      fm.get("authorName"),
		Ts:              fm.num("ts"),
		State:           fm.get("state"),
		Path:            fm.get("path"),
		Side:            fm.get("side"),
		Line:            int(int32(fm.num("line"))),
		ThreadID:        fm.get("threadId"),
		ParentID:        fm.get("parentId"),
		LineStart:       int(int32(fm.num("lineStart"))),
		LineEnd:         int(int32(fm.num("lineEnd"))),
		SuggestionPatch: decodeFrontMatterBlob(fm.get("suggestionPatchB64")),
		TargetPath:      fm.get("targetPath"),
		AppliedCommit:   fm.get("appliedCommit"),
		Patchset:        fm.get("patchset"),
		AnchorLines:     decodeFrontMatterBlob(fm.get("anchorLinesB64")),
		Sig:             fm.get("sig"),
		Body:            fm.body,
	}
}

func qtFromUtf8(raw []byte) string {
	if utf8.Valid(raw) {
		return string(raw)
	}
	return strings.ToValidUTF8(string(raw), "�")
}

func qtToLongLong(text string) int64 {
	value, err := strconv.ParseInt(strings.TrimSpace(text), 10, 64)
	if err != nil {
		return 0
	}
	return value
}

func qtToInt(text string) (int, bool) {
	value, err := strconv.ParseInt(strings.TrimSpace(text), 10, 32)
	if err != nil {
		return 0, false
	}
	return int(value), true
}

func leftRunes(text string, n int) string {
	if n <= 0 {
		return ""
	}
	count := 0
	for i := range text {
		if count == n {
			return text[:i]
		}
		count++
	}
	return text
}

func simplified(text string) string {
	return strings.Join(strings.Fields(text), " ")
}

func validCommitOid(oid string) bool {
	if len(oid) != 40 && len(oid) != 64 {
		return false
	}
	for i := 0; i < len(oid); i++ {
		c := oid[i]
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
			return false
		}
	}
	return true
}

func DecidedPullMessage(status string) string {
	return "This pull request is already " + status + "."
}

func LocalChangesBlockMessage(number int, checkout, gitError string) string {
	files := []string{}
	for _, line := range strings.Split(gitError, "\n") {
		if strings.HasPrefix(line, "\t") && strings.TrimSpace(line) != "" {
			files = append(files, strings.TrimSpace(line))
		}
	}
	message := "Pull request #" + strconv.Itoa(number) + " changes files that have uncommitted changes in " +
		checkout + ". Commit or stash them, then merge again."
	if len(files) > 0 {
		if len(files) > 5 {
			files = files[:5]
		}
		message += " (" + strings.Join(files, ", ") + ")"
	}
	return message
}

func OverLocalChanges(gitOutput string) bool {
	lower := strings.ToLower(gitOutput)
	return strings.Contains(lower, strings.ToLower("Your local changes to the following files would be overwritten")) ||
		strings.Contains(lower, "untracked working tree files would be overwritten")
}

var applyConflictPatterns = []*regexp.Regexp{
	regexp.MustCompile(`^error: patch failed: (.+):\d+$`),
	regexp.MustCompile(`^error: (.+): (?:patch does not apply|does not (?:exist|match) in index)$`),
	regexp.MustCompile(`^Applied patch to '(.+)' with conflicts\.$`),
}

func ParseApplyConflicts(text string) []string {
	files := []string{}
	for _, line := range strings.Split(text, "\n") {
		trimmed := strings.TrimSpace(line)
		for _, re := range applyConflictPatterns {
			if m := re.FindStringSubmatch(trimmed); m != nil {
				if !containsString(files, m[1]) {
					files = append(files, m[1])
				}
				break
			}
		}
	}
	return files
}

func containsString(list []string, value string) bool {
	for _, item := range list {
		if item == value {
			return true
		}
	}
	return false
}
