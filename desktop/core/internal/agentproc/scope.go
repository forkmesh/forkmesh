package agentproc

import (
	"fmt"
	"regexp"
	"strings"
)

const (
	instanceVariable = "FORKMESH_DESKTOP_INSTANCE"
	defaultSlice     = "forkmesh.slice"
)

type Scope struct {
	SystemdRun  string `json:"systemdRun"`
	Contain     bool   `json:"contain"`
	Slice       string `json:"slice"`
	MemoryHigh  int64  `json:"memoryHigh"`
	MemoryMax   int64  `json:"memoryMax"`
	OOMPolicy   bool   `json:"oomPolicy"`
	Description string `json:"description"`
}

func (s Scope) usable() bool { return strings.TrimSpace(s.SystemdRun) != "" }

var unitLabelCleaner = regexp.MustCompile(`[^a-z0-9]+`)

func unitName(label string, pid int, serial int64) string {
	clean := unitLabelCleaner.ReplaceAllString(strings.ToLower(label), "-")
	if len(clean) > 32 {
		clean = clean[:32]
	}
	clean = strings.Trim(clean, "-")
	if clean == "" {
		clean = "agent"
	}
	return fmt.Sprintf("forkmesh-agent-core-%s-%d-%d.scope", clean, pid, serial)
}

func scopeArgs(scope Scope, unit, label string) []string {
	description := scope.Description
	if description == "" {
		description = "ForkMesh agent " + label
	}
	args := []string{"--user", "--scope", "--quiet", "--collect"}
	if scope.Contain {
		slice := scope.Slice
		if slice == "" {
			slice = defaultSlice
		}
		args = append(args, "--slice="+slice)
	}
	args = append(args, "--unit="+unit, "--description="+description)
	if scope.Contain {
		if scope.MemoryHigh > 0 {
			args = append(args, fmt.Sprintf("--property=MemoryHigh=%d", scope.MemoryHigh))
		}
		if scope.MemoryMax > 0 {
			args = append(args, fmt.Sprintf("--property=MemoryMax=%d", scope.MemoryMax))
		}
		if scope.OOMPolicy {
			args = append(args, "--property=OOMPolicy=continue")
		}
	}
	return args
}

func scopeCommand(scope Scope, unit, label, program string, args []string) (string, []string) {
	out := scopeArgs(scope, unit, label)
	out = append(out, "--", program)
	out = append(out, args...)
	return scope.SystemdRun, out
}

func releasedEnv(env []string) []string {
	out := make([]string, 0, len(env))
	prefix := instanceVariable + "="
	for _, entry := range env {
		if strings.HasPrefix(entry, prefix) || entry == instanceVariable {
			continue
		}
		out = append(out, entry)
	}
	return out
}

func envValue(env []string, name string) (string, bool) {
	prefix := name + "="
	for i := len(env) - 1; i >= 0; i-- {
		if strings.HasPrefix(env[i], prefix) {
			return env[i][len(prefix):], true
		}
	}
	return "", false
}
