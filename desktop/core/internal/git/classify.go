package git

import (
	"strings"
	"time"
)

const (
	ReadTimeout      = 8 * time.Second
	WriteTimeout     = 60 * time.Second
	MergeTimeout     = 120 * time.Second
	QueueWait        = 300 * time.Second
	LockStale        = 60 * time.Second
	HeldLockWait     = 1500 * time.Millisecond
	LockBackoffTries = 6
	StopGrace        = 2 * time.Second
)

var valuedGlobals = map[string]bool{
	"-c": true, "-C": true, "--git-dir": true, "--work-tree": true, "--namespace": true, "--config-env": true,
}

var indexWriters = map[string]bool{
	"merge": true, "rebase": true, "am": true, "cherry-pick": true, "revert": true, "pull": true,
	"checkout": true, "switch": true, "restore": true, "reset": true, "commit": true, "add": true,
	"rm": true, "mv": true, "read-tree": true, "update-index": true, "stash": true, "apply": true,
}

var mergeLike = map[string]bool{
	"merge": true, "rebase": true, "am": true, "cherry-pick": true, "revert": true, "pull": true,
}

var waitWriters = map[string]bool{
	"add": true, "am": true, "apply": true, "checkout": true, "cherry-pick": true, "clean": true,
	"commit": true, "merge": true, "mv": true, "pull": true, "read-tree": true, "rebase": true,
	"reset": true, "restore": true, "revert": true, "rm": true, "stash": true, "switch": true,
	"update-index": true, "worktree": true,
}

func subcommandIndex(args []string) int {
	for i := 0; i < len(args); i++ {
		arg := args[i]
		if valuedGlobals[arg] {
			i++
			continue
		}
		if strings.HasPrefix(arg, "-") {
			continue
		}
		return i
	}
	return -1
}

func firstOperand(args []string, from int) string {
	for i := from; i < len(args); i++ {
		if !strings.HasPrefix(args[i], "-") {
			return args[i]
		}
	}
	return ""
}

func Subcommand(args []string) string {
	at := subcommandIndex(args)
	if at < 0 {
		return ""
	}
	return args[at]
}

func WritesIndex(args []string) bool {
	at := subcommandIndex(args)
	if at < 0 {
		return false
	}
	sub := args[at]
	if !indexWriters[sub] {
		return false
	}
	switch sub {
	case "stash":
		action := firstOperand(args, at+1)
		return action != "list" && action != "show"
	case "apply":
		for _, arg := range args[at+1:] {
			if arg == "--index" || arg == "--cached" || arg == "--3way" || arg == "-3" {
				return true
			}
		}
		return false
	}
	return true
}

func MergeLike(args []string) bool {
	return mergeLike[Subcommand(args)]
}

func waitTimeout(args []string) time.Duration {
	for i := 0; i < len(args); i++ {
		arg := args[i]
		if arg == "-C" || arg == "-c" || arg == "--git-dir" || arg == "--work-tree" || arg == "--namespace" {
			i++
			continue
		}
		if strings.HasPrefix(arg, "-") {
			continue
		}
		if waitWriters[arg] {
			return WriteTimeout
		}
		return ReadTimeout
	}
	return ReadTimeout
}

func TimeoutFor(args []string, fallback time.Duration) time.Duration {
	if fallback <= 0 {
		fallback = waitTimeout(args)
	}
	if MergeLike(args) && fallback < MergeTimeout {
		return MergeTimeout
	}
	return fallback
}
