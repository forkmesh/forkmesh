package mirrornode

// Runner lease grants (docs/RUNNERS.md). The app signs one read grant and one
// push grant per runner job with the router key; the core's gateway checks
// them on /v1/runner/<owner>/<repo>.git and lets the push move exactly one
// ref. The canonical message is shared byte for byte with
// app/src/edge_routing.py and tools/mirror_gateway.py.

import (
	"encoding/base64"
	"regexp"
	"strconv"
	"strings"
	"unicode/utf8"
)

const (
	runnerGrantDomain = "forkmesh-runner-grant-v1"
	runnerGrantLines  = 10
	// A grant never outlives three hours, whatever the job asked for.
	runnerGrantMaxLifetimeMs = 10_800_000
	runnerAgentRefPrefix     = "refs/heads/agent/"
)

var runnerAgentRefRest = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._/-]{0,150}$`)

type runnerGrant struct {
	Node, Owner, Repository, Access, Ref, Runner, GrantID string
	IssuedAt, ExpiresAt                                   int64
}

// validRunnerAgentRef reports whether ref is a branch under refs/heads/agent/
// that git itself would accept.
func validRunnerAgentRef(ref string) bool {
	if !strings.HasPrefix(ref, runnerAgentRefPrefix) {
		return false
	}
	rest := ref[len(runnerAgentRefPrefix):]
	return runnerAgentRefRest.MatchString(rest) &&
		!strings.Contains(rest, "..") && !strings.Contains(rest, "//") &&
		!strings.Contains(rest, "@{") && !strings.HasSuffix(rest, "/") &&
		!strings.HasSuffix(rest, ".lock") && !strings.HasSuffix(rest, ".")
}

func runnerGrantMessage(g runnerGrant) string {
	if !nodePattern.MatchString(g.Node) || !nodePattern.MatchString(g.Owner) ||
		!repositoryPattern.MatchString(g.Repository) || !nodePattern.MatchString(g.Runner) ||
		!requestIDPattern.MatchString(g.GrantID) || g.IssuedAt <= 0 ||
		g.ExpiresAt <= g.IssuedAt || g.ExpiresAt-g.IssuedAt > runnerGrantMaxLifetimeMs {
		return ""
	}
	switch g.Access {
	case "read":
		if g.Ref != "" {
			return ""
		}
	case "push":
		if !validRunnerAgentRef(g.Ref) {
			return ""
		}
	default:
		return ""
	}
	return strings.Join([]string{runnerGrantDomain, g.Node, g.Owner, g.Repository,
		g.Access, g.Ref, g.Runner, g.GrantID,
		strconv.FormatInt(g.IssuedAt, 10), strconv.FormatInt(g.ExpiresAt, 10)}, "\n")
}

// parseRunnerGrantMessage accepts only the canonical spelling of a grant.
func parseRunnerGrantMessage(message string) (runnerGrant, bool) {
	lines := strings.Split(message, "\n")
	if len(lines) != runnerGrantLines || lines[0] != runnerGrantDomain ||
		!numberPattern.MatchString(lines[8]) || !numberPattern.MatchString(lines[9]) {
		return runnerGrant{}, false
	}
	issued, err1 := strconv.ParseInt(lines[8], 10, 64)
	expires, err2 := strconv.ParseInt(lines[9], 10, 64)
	if err1 != nil || err2 != nil {
		return runnerGrant{}, false
	}
	grant := runnerGrant{Node: lines[1], Owner: lines[2], Repository: lines[3],
		Access: lines[4], Ref: lines[5], Runner: lines[6], GrantID: lines[7],
		IssuedAt: issued, ExpiresAt: expires}
	if runnerGrantMessage(grant) != message {
		return runnerGrant{}, false
	}
	return grant, true
}

// parseRunnerGrantToken reads the grant a lease carries. The runner cannot
// verify the router signature (it holds no router key and does not need to);
// it reads the grant to check the lease is internally consistent before it
// spends compute on it. The gateway is what enforces it.
func parseRunnerGrantToken(token string) (runnerGrant, bool) {
	encoded, signature, found := strings.Cut(token, ".")
	if !found || len(token) > 4096 {
		return runnerGrant{}, false
	}
	raw, err := base64.RawURLEncoding.DecodeString(encoded)
	if err != nil || !utf8.Valid(raw) {
		return runnerGrant{}, false
	}
	if sig, err := base64.RawURLEncoding.DecodeString(signature); err != nil || len(sig) != 64 {
		return runnerGrant{}, false
	}
	return parseRunnerGrantMessage(string(raw))
}
