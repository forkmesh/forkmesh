package mirrornode

// What a failed provider CLI run reports as the session's lastError.
//
// A Claude Code run that dies at once (2026-09-24: a host CLI too old for the
// requested model) exits non-zero with an empty stderr, and its stdout opens
// with the stream-json init event. The daemon used to quote the first bytes of
// stdout, so the website and phone showed `{"type":"system","subtype":"init"…`
// while the real "API Error: 400 … version 2.1.280 or newer is required" sat
// in an assistant message. The error now comes from the stream's own verdict:
// the result event, then an API-error assistant message, then stderr's last
// lines, and only then a bare exit status. The init event is never quoted.

import (
	"context"
	"errors"
	"fmt"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

// agentErrorLimit caps one lastError: long enough for an API error with its
// request id, short enough for a phone's session header.
const agentErrorLimit = 600

// agentCLIOutdated is a run the provider refused because the host's CLI is
// older than the requested model needs. The mirror cannot update a root-owned
// CLI itself (it runs as forkmesh-node), so the message names the fix.
type agentCLIOutdated struct {
	label    string // "Claude Code" or "Codex"
	node     string
	model    string
	current  string // the host CLI's version, when known
	required string // the minimum the provider named, when it named one
}

func (e *agentCLIOutdated) Error() string {
	model := e.model
	if model == "" {
		model = "the requested model"
	}
	have := e.label + " on " + e.node
	if e.current != "" {
		have += " is " + e.current
	} else {
		have += " is out of date"
	}
	need := "a newer version"
	if e.required != "" {
		need = e.required + " or newer"
	}
	return have + "; " + model + " needs " + need + ". Update the host (Hosts → Update)."
}

var (
	claudeOutdatedPattern = regexp.MustCompile(
		`(?i)Claude Code\s+v?([0-9][0-9A-Za-z.+-]*)\s+does not support this model;\s*version\s+v?([0-9][0-9A-Za-z.+-]*)\s+or newer is required`)
	codexOutdatedPattern = regexp.MustCompile(`(?i)requires a newer version of Codex`)
	cliVersionPattern    = regexp.MustCompile(`[0-9]+(?:\.[0-9]+)+`)
)

// claudeOutdated recognizes the API's "this CLI is too old for the model"
// refusal anywhere in detail.
func claudeOutdated(detail, node, model string) *agentCLIOutdated {
	match := claudeOutdatedPattern.FindStringSubmatch(detail)
	if match == nil {
		return nil
	}
	return &agentCLIOutdated{label: "Claude Code", node: node, model: model,
		current: strings.TrimRight(match[1], "."), required: strings.TrimRight(match[2], ".")}
}

// claudeFailureDetail is the most specific account of a failed Claude run the
// stream holds, in order of authority. It is empty when the CLI said nothing.
func claudeFailureDetail(stream *claudeStream, stderr []byte) string {
	if stream.parsed {
		if text := strings.TrimSpace(firstNonEmpty(strings.Join(stream.resultErrors, "; "),
			stream.result.Result)); text != "" {
			return text
		}
	}
	if stream.apiError != "" {
		return stream.apiError
	}
	return lastOutputLines(stderr, 3)
}

// claudeRunError turns a finished Claude run into the error it reports, or nil
// when the run succeeded.
func claudeRunError(stream *claudeStream, stderr []byte, runErr error, node, model string) error {
	detail := claudeFailureDetail(stream, stderr)
	failed := runErr != nil || (stream.parsed && stream.result.IsError) ||
		(!stream.parsed && stream.apiError != "")
	if !failed {
		if !stream.parsed {
			return errors.New("Claude Code returned no result")
		}
		return nil
	}
	if outdated := claudeOutdated(detail, node, model); outdated != nil {
		return outdated
	}
	if outdated := claudeOutdated(stream.apiError, node, model); outdated != nil {
		return outdated
	}
	return agentExitError("Claude Code", runErr, detail)
}

// codexRunError is claudeRunError for Codex, whose exec mode prints its
// errors to stderr and its narration to stdout.
func codexRunError(stdout, stderr []byte, runErr error, node, model string, version func() string) error {
	if runErr == nil {
		return nil
	}
	detail := firstNonEmpty(lastOutputLines(stderr, 3), lastOutputLines(stdout, 3))
	if codexOutdatedPattern.MatchString(string(stderr)) || codexOutdatedPattern.MatchString(string(stdout)) {
		current := ""
		if version != nil {
			current = version()
		}
		return &agentCLIOutdated{label: "Codex", node: node, model: model, current: current}
	}
	return agentExitError("Codex", runErr, detail)
}

// agentExitError names how the CLI ended, then what it said.
func agentExitError(label string, runErr error, detail string) error {
	how := label + " failed"
	var exitErr *exec.ExitError
	switch {
	case errors.As(runErr, &exitErr) && exitErr.ExitCode() >= 0:
		how = fmt.Sprintf("%s exited with status %d", label, exitErr.ExitCode())
	case errors.Is(runErr, context.DeadlineExceeded):
		how = label + " ran past the session time limit"
	case runErr != nil:
		how = label + " stopped: " + runErr.Error()
	}
	if detail == "" {
		return errors.New(how)
	}
	return errors.New(boundedRunes(how+": "+detail, agentErrorLimit))
}

// lastOutputLines is the last n non-empty lines of output, joined, capped:
// a CLI's closing lines say why it stopped; its opening lines are banners.
func lastOutputLines(output []byte, n int) string {
	lines := strings.Split(strings.ReplaceAll(string(output), "\r\n", "\n"), "\n")
	var kept []string
	for i := len(lines) - 1; i >= 0 && len(kept) < n; i-- {
		if line := strings.TrimSpace(lines[i]); line != "" {
			kept = append([]string{line}, kept...)
		}
	}
	text := strings.Join(kept, " | ")
	return tailRunes(text, agentErrorLimit)
}

// cliVersion runs `<binary> --version` and keeps the dotted version in it.
func cliVersion(binary string) string {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	output, err := exec.CommandContext(ctx, binary, "--version").Output()
	if err != nil {
		return ""
	}
	return cliVersionPattern.FindString(string(output))
}

// versionLess compares dotted numeric versions ("2.1.248" < "2.1.280").
func versionLess(a, b string) bool {
	as, bs := strings.Split(a, "."), strings.Split(b, ".")
	for i := 0; i < len(as) || i < len(bs); i++ {
		var x, y int
		if i < len(as) {
			x, _ = strconv.Atoi(as[i])
		}
		if i < len(bs) {
			y, _ = strconv.Atoi(bs[i])
		}
		if x != y {
			return x < y
		}
	}
	return false
}

// agentCLIFloor remembers, per provider and model, the CLI version a run was
// refused on. Until the host's CLI changes, the next run for that model fails
// at once with the same clear message instead of cloning, running, and
// failing again; the check costs a `--version` only after such a refusal.
type agentCLIFloor struct {
	mu      sync.Mutex
	refused map[string]agentCLIOutdated
}

func agentCLIFloorKey(provider, model string) string { return provider + "\x00" + model }

func (f *agentCLIFloor) learn(provider string, outdated *agentCLIOutdated) {
	if outdated == nil || (outdated.current == "" && outdated.required == "") {
		return
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.refused == nil {
		f.refused = map[string]agentCLIOutdated{}
	}
	f.refused[agentCLIFloorKey(provider, outdated.model)] = *outdated
}

// check reports the refusal a run for model would meet with the CLI at
// version, or nil. A CLI that moved past the refused version clears it.
func (f *agentCLIFloor) check(provider, model string, version func() string) *agentCLIOutdated {
	f.mu.Lock()
	known, ok := f.refused[agentCLIFloorKey(provider, model)]
	f.mu.Unlock()
	if !ok {
		return nil
	}
	current := version()
	if current == "" {
		return nil // cannot tell; let the run say what is wrong
	}
	stale := current == known.current ||
		(known.required != "" && versionLess(current, known.required))
	if !stale {
		f.mu.Lock()
		delete(f.refused, agentCLIFloorKey(provider, model))
		f.mu.Unlock()
		return nil
	}
	known.current = current
	return &known
}
