package mergequeue

import (
	"regexp"
	"strconv"
	"strings"
)

const (
	maxRepos      = 100
	maxEntries    = 100
	maxTitleChars = 200
)

var knownActions = []string{"enqueue", "remove", "move", "pause", "resume", "enable", "disable", "clear", "process"}

var repoSegmentPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$`)

func isKnownAction(action string) bool {
	for _, known := range knownActions {
		if known == action {
			return true
		}
	}
	return false
}

func needsNumber(action string) bool {
	return action == "enqueue" || action == "remove" || action == "move"
}

func validRepoSegment(segment string) bool {
	return repoSegmentPattern.MatchString(segment) && !strings.Contains(segment, "..")
}

func validRequest(owner, name, action string, number, delta int) bool {
	if !validRepoSegment(owner) || !validRepoSegment(name) || !isKnownAction(action) {
		return false
	}
	if needsNumber(action) && (number <= 0 || number > 10000000) {
		return false
	}
	if action == "move" && (delta == 0 || delta < -maxEntries || delta > maxEntries) {
		return false
	}
	return true
}

func confirmationTarget(owner, name, action string, number, delta int) string {
	target := strings.ToLower(owner) + "/" + strings.ToLower(name)
	if needsNumber(action) {
		target += "#" + strconv.Itoa(number)
	}
	if action == "move" {
		target += ":" + strconv.Itoa(delta)
	}
	return target
}

func abs(n int) int {
	if n < 0 {
		return -n
	}
	return n
}

func actionSummary(owner, name, action string, number, delta int) string {
	repo := owner + "/" + name
	n := strconv.Itoa(number)
	switch action {
	case "enqueue":
		return "Add pull request #" + n + " to the merge queue of " + repo + ". The desktop merges it when its turn comes and the review and checks allow it."
	case "remove":
		return "Take pull request #" + n + " out of the merge queue of " + repo + ". It stays open."
	case "move":
		direction := "down"
		if delta < 0 {
			direction = "up"
		}
		plural := "s"
		if abs(delta) == 1 {
			plural = ""
		}
		return "Move pull request #" + n + " " + direction + " " + strconv.Itoa(abs(delta)) + " place" + plural + " in the merge queue of " + repo + "."
	case "pause":
		return "Pause the merge queue of " + repo + ". Nothing merges until it is resumed."
	case "resume":
		return "Resume the merge queue of " + repo + "."
	case "enable":
		return "Turn on the merge queue for " + repo + "."
	case "disable":
		return "Turn off the merge queue for " + repo + ". Queued pull requests stay listed but nothing merges."
	case "clear":
		return "Remove every pull request from the merge queue of " + repo + ". They all stay open."
	case "process":
		return "Merge the next ready pull request in the queue of " + repo + " now, on this desktop."
	}
	return ""
}

type repoState struct {
	owner    string
	name     string
	enabled  bool
	paused   bool
	open     bool
	writable bool
	queue    Queue
}

func precheck(repo repoState, action string, number int, pullStatus string) string {
	if !repo.writable {
		return "read_only"
	}
	if action == "enable" || action == "disable" {
		return ""
	}
	if !repo.enabled {
		return "queue_off"
	}
	if action == "enqueue" {
		if pullStatus != "" && pullStatus != "open" {
			return "not_open_pull"
		}
		if repo.queue.Contains(number) {
			return "already_queued"
		}
		if repo.queue.Size() >= maxEntries {
			return "queue_full"
		}
	}
	if (action == "remove" || action == "move") && !repo.queue.Contains(number) {
		return "not_queued"
	}
	if action == "move" {
		state := repo.queue.At(number).State
		if state == StateUpdating || state == StateMerging {
			return "busy"
		}
	}
	if action == "process" {
		if repo.paused {
			return "paused"
		}
		if !repo.open {
			return "repo_not_open"
		}
	}
	return ""
}

func applyToQueue(queue *Queue, action string, number, delta int, nowMs int64) string {
	switch action {
	case "enqueue":
		if queue.Enqueue(number, nowMs) {
			return ""
		}
		return "already_queued"
	case "remove":
		if queue.Remove(number) {
			return ""
		}
		return "not_queued"
	case "move":
		if !queue.Contains(number) {
			return "not_queued"
		}
		queue.Move(number, delta)
		return ""
	case "clear":
		queue.Clear()
		return ""
	}
	return "unsupported_action"
}

func resultMessage(action string, number int, queue Queue) string {
	place := strconv.Itoa(queue.IndexOf(number) + 1)
	n := strconv.Itoa(number)
	switch action {
	case "enqueue":
		return "Pull request #" + n + " joined the merge queue at place " + place + "."
	case "remove":
		return "Pull request #" + n + " left the merge queue."
	case "move":
		return "Pull request #" + n + " is now at place " + place + "."
	case "pause":
		return "Merge queue paused."
	case "resume":
		return "Merge queue resumed."
	case "enable":
		return "Merge queue turned on."
	case "disable":
		return "Merge queue turned off."
	case "clear":
		return "Merge queue cleared."
	case "process":
		return "The desktop is working through the queue."
	}
	return ""
}

func repoRow(repo repoState, titles map[int]string) map[string]any {
	entries := []any{}
	for _, e := range repo.queue.Entries() {
		if len(entries) >= maxEntries {
			break
		}
		entries = append(entries, map[string]any{
			"number":     e.Number,
			"title":      leftUTF16(titles[e.Number], maxTitleChars),
			"state":      e.State,
			"detail":     SanitizeDetail(e.Detail),
			"queuedAtMs": e.QueuedAtMs,
			"stateAtMs":  e.StateAtMs,
		})
	}
	summary := "Off"
	if repo.enabled {
		summary = repo.queue.Summary()
	}
	return map[string]any{
		"owner":    repo.owner,
		"name":     repo.name,
		"enabled":  repo.enabled,
		"paused":   repo.paused,
		"open":     repo.open,
		"writable": repo.writable,
		"summary":  summary,
		"entries":  entries,
	}
}
