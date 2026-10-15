package cleanup

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"time"

	"forkmesh.com/desktop-core/internal/safedelete"
)

type Choice struct {
	Confirmed      bool `json:"confirmed"`
	DiscardChanges bool `json:"discardChanges"`
	RetireBranch   bool `json:"retireBranch"`
}

type Step struct {
	Item   Item   `json:"item"`
	Choice Choice `json:"choice"`
}

type Skip struct {
	Item Item
	Why  string
}

type Plan struct {
	Files     []Step
	Sandboxes []Step
	Worktrees []Step
	Skipped   []Skip
	Bytes     int64
}

func (p Plan) Count() int { return len(p.Files) + len(p.Sandboxes) + len(p.Worktrees) }

func PlanRefusal(item Item, choice Choice) string {
	switch {
	case item.Status == StatusProtected:
		return "protected"
	case item.Status == StatusInUse:
		return "in use"
	case item.Kind == KindReport:
		return "report only"
	case !item.Deletable():
		return "nothing here can be deleted"
	case item.NeedsConfirm() && !choice.Confirmed:
		return "not confirmed"
	case item.Kind == KindWorktree && item.DirtyFiles > 0 && !choice.DiscardChanges:
		return "has uncommitted changes; tick Discard to remove it"
	}
	return ""
}

func BuildPlan(selected []Item, choices map[string]Choice) Plan {
	var plan Plan
	for _, item := range selected {
		choice := choices[item.ID]
		if why := PlanRefusal(item, choice); why != "" {
			plan.Skipped = append(plan.Skipped, Skip{item, why})
			continue
		}
		step := Step{item, choice}
		switch item.Kind {
		case KindWorktree:
			plan.Worktrees = append(plan.Worktrees, step)
		case KindActionSandbox:
			plan.Sandboxes = append(plan.Sandboxes, step)
		default:
			plan.Files = append(plan.Files, step)
		}
		plan.Bytes += item.Bytes
	}
	return plan
}

func Describe(plan Plan) string {
	lines := []string{fmt.Sprintf("%d item%s, %s reclaimed", plan.Count(), plural(plan.Count(), "s"), SizeText(plan.Bytes))}
	for _, step := range plan.Worktrees {
		line := "worktree " + step.Item.Path
		if step.Choice.DiscardChanges && step.Item.DirtyFiles > 0 {
			line += fmt.Sprintf(" (discarding %d uncommitted files)", step.Item.DirtyFiles)
		}
		if step.Choice.RetireBranch && step.Item.Branch != "" {
			line += fmt.Sprintf(", branch %s moved to refs/forkmesh/trash/cleanup/", step.Item.Branch)
		}
		lines = append(lines, line)
	}
	for _, step := range plan.Sandboxes {
		lines = append(lines, "Actions sandbox "+step.Item.Path)
	}
	for _, step := range plan.Files {
		if len(step.Item.Members) == 1 && step.Item.Members[0].Path == step.Item.Path {
			lines = append(lines, step.Item.Path)
			continue
		}
		suffix := "ies"
		if len(step.Item.Members) == 1 {
			suffix = "y"
		}
		lines = append(lines, fmt.Sprintf("%d entr%s in %s", len(step.Item.Members), suffix, step.Item.Path))
	}
	for _, skip := range plan.Skipped {
		lines = append(lines, fmt.Sprintf("kept %s: %s", skip.Item.Title, skip.Why))
	}
	return strings.Join(lines, "\n")
}

type Recheck struct {
	Processes   ProcessSnapshot
	Live        map[string]bool
	NowMs       int64
	LiveMinutes int
	Protected   []string
}

func (r Recheck) liveMinutes() int {
	if r.LiveMinutes <= 0 {
		return 30
	}
	return r.LiveMinutes
}

type Outcome struct {
	Path    string `json:"path"`
	Bytes   int64  `json:"bytes"`
	Reason  string `json:"reason"`
	Removed bool   `json:"removed"`
	Error   string `json:"error"`
}

func symlinkBetween(member, parent string) bool {
	if isSymlink(parent) {
		return true
	}
	current := filepath.Dir(member)
	for len(current) > len(parent) && PathWithin(current, parent) {
		if isSymlink(current) {
			return true
		}
		up := filepath.Dir(current)
		if up == current {
			break
		}
		current = up
	}
	return false
}

func MemberRefusal(item Item, member Member, recheck Recheck) string {
	parent := cleanPath(item.Parent)
	path := cleanPath(member.Path)
	if parent == "" {
		return "no expected parent folder"
	}
	if path == parent || !PathWithin(path, parent) {
		return fmt.Sprintf("%s is not inside %s", path, parent)
	}
	for _, root := range recheck.Protected {
		if root = cleanPath(root); root != "" && (PathWithin(path, root) || PathWithin(root, path)) {
			return fmt.Sprintf("%s is or is inside protected ForkMesh folder %s", path, root)
		}
	}
	if symlinkBetween(path, parent) {
		return fmt.Sprintf("a link sits between %s and %s", parent, path)
	}
	link := isSymlink(path)
	if !exists(path) && !link {
		return "already gone"
	}
	if users := recheck.Processes.UsersOf(path); len(users) > 0 {
		return "now open in " + firstUsers(users)
	}
	for uuid := range UUIDsIn(fileName(path)) {
		if recheck.Live[uuid] {
			return "its session is live"
		}
	}
	if !link {
		usage := Measure(path, nil)
		if usage.NewestMs > 0 && recheck.NowMs-usage.NewestMs < int64(recheck.liveMinutes())*minuteMs {
			return fmt.Sprintf("changed in the last %d minutes", recheck.liveMinutes())
		}
	}
	return ""
}

func WorktreeRefusal(item Item, recheck Recheck) string {
	if item.Kind != KindWorktree {
		return "not a worktree"
	}
	if !isDir(item.Path) {
		return "already gone"
	}
	if users := recheck.Processes.UsersOf(item.Path); len(users) > 0 {
		return "now open in " + firstUsers(users)
	}
	return ""
}

func removeTree(path, parent string) string {
	if why := safedelete.Refusal(parent, path); why != "" {
		return why
	}
	info, statErr := os.Lstat(path)
	if err := safedelete.RemoveTreeUnder(parent, path); err != nil {
		if statErr == nil && info.IsDir() {
			return "could not remove everything in " + path
		}
		return "could not remove it"
	}
	return ""
}

func DeleteFiles(steps []Step, recheck Recheck, cancel *atomic.Bool) []Outcome {
	outcomes := []Outcome{}
	for _, step := range steps {
		for _, member := range step.Item.Members {
			if cancelled(cancel) {
				return outcomes
			}
			outcome := Outcome{Path: cleanPath(member.Path), Bytes: member.Bytes, Reason: step.Item.ReasonText()}
			outcome.Error = MemberRefusal(step.Item, member, recheck)
			if outcome.Error == "" {
				outcome.Error = removeTree(outcome.Path, cleanPath(step.Item.Parent))
				outcome.Removed = outcome.Error == ""
			}
			outcomes = append(outcomes, outcome)
		}
	}
	return outcomes
}

type SandboxRemover func(base, path string) string

func DeleteSandboxes(steps []Step, recheck Recheck, remove SandboxRemover) []Outcome {
	outcomes := []Outcome{}
	for _, step := range steps {
		outcome := Outcome{Path: cleanPath(step.Item.Path), Bytes: step.Item.Bytes, Reason: step.Item.ReasonText()}
		if users := recheck.Processes.UsersOf(outcome.Path); len(users) > 0 {
			outcome.Error = "now open in " + firstUsers(users)
		} else if remove == nil {
			outcome.Error = "the desktop is not available to remove it"
		} else {
			outcome.Error = remove(cleanPath(step.Item.Parent), outcome.Path)
		}
		outcome.Removed = outcome.Error == ""
		outcomes = append(outcomes, outcome)
	}
	return outcomes
}

func FreedBytes(outcomes []Outcome) int64 {
	total := int64(0)
	for _, outcome := range outcomes {
		if outcome.Removed {
			total += outcome.Bytes
		}
	}
	return total
}

func LogLine(outcome Outcome, nowMs int64) string {
	state := "kept"
	if outcome.Removed {
		state = "removed"
	}
	fields := []string{
		time.UnixMilli(nowMs).Local().Format("2006-01-02T15:04:05"),
		state,
		fmt.Sprintf("%d", outcome.Bytes),
		outcome.Path,
		outcome.Reason,
	}
	if outcome.Error != "" {
		fields = append(fields, outcome.Error)
	}
	for index, field := range fields {
		fields[index] = strings.NewReplacer("\t", " ", "\n", " ").Replace(field)
	}
	return strings.Join(fields, "\t")
}

func AppendLog(file string, outcomes []Outcome, nowMs int64) bool {
	if len(outcomes) == 0 {
		return true
	}
	_ = os.MkdirAll(filepath.Dir(cleanPath(file)), 0o755)
	log, err := os.OpenFile(file, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return false
	}
	defer log.Close()
	_ = log.Chmod(0o600)
	var text strings.Builder
	for _, outcome := range outcomes {
		text.WriteString(LogLine(outcome, nowMs))
		text.WriteByte('\n')
	}
	_, err = log.WriteString(text.String())
	return err == nil
}
