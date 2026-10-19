package mirrornode

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"slices"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	pullHeadCommitterName   = "ForkMesh"
	pullHeadCommitterEmail  = "pulls@users.noreply.forkmesh.com"
	pullHeadBaseSearchLimit = 200
	pullHeadSweepReplays    = 4
)

var pullPatchIndexLine = regexp.MustCompile(`^index ([0-9a-fA-F]{7,64})\.\.`)

type pullHead struct {
	base string
	head string
}

func pullHeadRef(number int) string {
	return "refs/pr/" + strconv.Itoa(number) + "/head"
}

func pullHeadEnvironment() []string {
	return pullGitEnvironment([]string{
		"GIT_COMMITTER_NAME=" + pullHeadCommitterName,
		"GIT_COMMITTER_EMAIL=" + pullHeadCommitterEmail,
	})
}

func syntheticPullMbox(values map[string]string, patch []byte) []byte {
	name := strings.TrimSpace(values["authorName"])
	if name == "" {
		name = "ForkMesh contributor"
	}
	author := strings.TrimSpace(values["author"])
	if author == "" {
		author = "contributor"
	}
	ts, err := strconv.ParseInt(strings.TrimSpace(values["ts"]), 10, 64)
	if err != nil || ts < 0 {
		ts = 0
	}
	title := strings.TrimSpace(values["title"])
	if title == "" {
		title = "Pull request"
	}
	var out bytes.Buffer
	out.WriteString("From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001\n")
	fmt.Fprintf(&out, "From: %s <%s@forkmesh>\n", name, author)
	fmt.Fprintf(&out, "Date: %s\n", time.UnixMilli(ts).UTC().Format(time.RFC1123Z))
	fmt.Fprintf(&out, "Subject: [PATCH] %s\n\n---\n", title)
	out.Write(patch)
	if !bytes.HasSuffix(patch, []byte("\n")) {
		out.WriteByte('\n')
	}
	return out.Bytes()
}

func patchPreimageBlobs(patch []byte) map[string]string {
	blobs := map[string]string{}
	blob, path := "", ""
	flush := func() {
		if blob != "" && path != "" {
			blobs[path] = blob
		}
		blob, path = "", ""
	}
	for _, line := range strings.Split(string(patch), "\n") {
		line = strings.TrimSuffix(line, "\r")
		switch {
		case strings.HasPrefix(line, "diff --git "):
			flush()
		case blob == "" && strings.HasPrefix(line, "index "):
			if match := pullPatchIndexLine.FindStringSubmatch(line); match != nil &&
				strings.Trim(match[1], "0") != "" {
				blob = strings.ToLower(match[1])
			}
		case path == "" && strings.HasPrefix(line, "--- a/"):
			if candidate := strings.TrimSpace(line[6:]); candidate != "" &&
				!strings.HasPrefix(candidate, "\"") {
				path = candidate
			}
		}
	}
	flush()
	return blobs
}

func patchPreimageBase(ctx context.Context, gitDir string, patch []byte, baseTip string) string {
	wanted := patchPreimageBlobs(patch)
	if len(wanted) == 0 {
		return ""
	}
	paths := make([]string, 0, len(wanted))
	for path := range wanted {
		paths = append(paths, path)
	}
	sort.Strings(paths)
	args := append([]string{"rev-list", "--max-count=" + strconv.Itoa(pullHeadBaseSearchLimit),
		"--end-of-options", baseTip, "--"}, paths...)
	var candidates []string
	var query bytes.Buffer
	for _, rev := range gitLines(ctx, gitDir, args...) {
		if !gitObjectIDPattern.MatchString(rev) {
			continue
		}
		candidates = append(candidates, rev)
		for _, path := range paths {
			query.WriteString(rev + ":" + path + "\n")
		}
	}
	if len(candidates) == 0 {
		return ""
	}
	command := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "cat-file", "--batch-check=%(objectname)")
	command.Stdin = &query
	output, err := command.Output()
	if err != nil {
		return ""
	}
	answers := strings.Split(string(output), "\n")
	for i, candidate := range candidates {
		fits := true
		for j, path := range paths {
			at := i*len(paths) + j
			answer := ""
			if at < len(answers) {
				answer = strings.ToLower(strings.TrimSpace(answers[at]))
			}
			if answer == "" || strings.Contains(answer, " ") || !strings.HasPrefix(answer, wanted[path]) {
				fits = false
				break
			}
		}
		if fits {
			return candidate
		}
	}
	return ""
}

func pullHeadBases(ctx context.Context, gitDir string, values map[string]string, baseTip string, patch []byte) []string {
	var bases []string
	add := func(candidate string) {
		if !gitObjectIDPattern.MatchString(candidate) {
			return
		}
		oid := gitValue(ctx, gitDir, "rev-parse", "--verify", "--quiet", "--end-of-options", candidate+"^{commit}")
		if !gitObjectIDPattern.MatchString(oid) || slices.Contains(bases, oid) ||
			!gitIsAncestor(ctx, gitDir, oid, baseTip) {
			return
		}
		bases = append(bases, oid)
	}
	for _, key := range []string{"headBaseOid", "creationBaseOid"} {
		add(strings.ToLower(strings.TrimSpace(values[key])))
	}
	add(patchPreimageBase(ctx, gitDir, patch, baseTip))
	add(baseTip)
	return bases
}

func replayPullSeries(ctx context.Context, gitDir, scratch, hooks, onto string, series []byte) (string, error) {
	if scratch == "" {
		scratch = os.TempDir()
	}
	work, err := os.MkdirTemp(scratch, ".pull-head-*")
	if err != nil {
		return "", err
	}
	_ = os.Remove(work)
	payload := work + ".mbox"
	defer func() {
		_ = exec.Command("git", "--git-dir="+gitDir, "worktree", "remove", "--force", work).Run()
		_ = exec.Command("git", "--git-dir="+gitDir, "worktree", "prune").Run()
		_ = removeOwnWorktreeUnder(scratch, work)
		_ = os.Remove(payload)
	}()
	env := pullHeadEnvironment()
	add := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "-c", "core.hooksPath="+hooks,
		"worktree", "add", "--detach", "--force", work, onto)
	add.Env = env
	if output, err := add.CombinedOutput(); err != nil {
		if ctx.Err() != nil {
			return "", ctx.Err()
		}
		return "", fmt.Errorf("pull head worktree: %s", boundedText(output, 240))
	}
	if err := os.WriteFile(payload, series, 0600); err != nil {
		return "", err
	}
	inTree := func(args ...string) *exec.Cmd {
		command := exec.CommandContext(ctx, "git", append([]string{"-C", work, "-c", "core.hooksPath=" + hooks}, args...)...)
		command.Env = env
		command.Stdout, command.Stderr = io.Discard, io.Discard
		return command
	}
	if err := inTree("am", "--quiet", "--3way", "--no-gpg-sign", "--no-message-id", "--no-scissors",
		"--no-keep-cr", "--committer-date-is-author-date", payload).Run(); err != nil {
		var exit *exec.ExitError
		if ctx.Err() != nil || !errors.As(err, &exit) {
			return "", fmt.Errorf("git am: %w", err)
		}
		_ = inTree("am", "--abort").Run()
		return "", nil
	}
	resolve := exec.CommandContext(ctx, "git", "-C", work, "rev-parse", "--verify", "HEAD^{commit}")
	resolve.Env = env
	output, err := resolve.Output()
	head := strings.TrimSpace(string(output))
	if err != nil || !gitObjectIDPattern.MatchString(head) {
		return "", errors.New("replayed pull head is invalid")
	}
	return head, nil
}

func materializePullHead(ctx context.Context, gitDir, scratch, hooks string,
	values map[string]string, baseTip string, mbox, patch []byte) (pullHead, error) {
	series := mbox
	if len(bytes.TrimSpace(series)) == 0 {
		if len(bytes.TrimSpace(patch)) == 0 {
			return pullHead{}, nil
		}
		series = syntheticPullMbox(values, patch)
	}
	for _, base := range pullHeadBases(ctx, gitDir, values, baseTip, patch) {
		head, err := replayPullSeries(ctx, gitDir, scratch, hooks, base, series)
		if err != nil {
			return pullHead{}, err
		}
		if head != "" && head != base {
			return pullHead{base: base, head: head}, nil
		}
	}
	return pullHead{}, nil
}

func recordPullHead(ctx context.Context, gitDir, dir string, number int) (string, error) {
	pullDir := filepath.Join(dir, ".forkmesh", "pulls", strconv.Itoa(number))
	recordPath := filepath.Join(pullDir, "pull.md")
	raw, err := os.ReadFile(recordPath)
	if err != nil {
		return "", err
	}
	matter, ok := parseFrontMatter(raw)
	if !ok || matter.values["derive"] == "branch" || matter.values["status"] != "open" ||
		matter.values["headOid"] != "" {
		return "", nil
	}
	base := validPullBranch(matter.values["base"])
	if base == "" {
		return "", nil
	}
	baseTip := gitValue(ctx, gitDir, "rev-parse", "--verify", "--quiet", "--end-of-options", "refs/heads/"+base+"^{commit}")
	if !gitObjectIDPattern.MatchString(baseTip) {
		return "", nil
	}
	mbox, _ := os.ReadFile(filepath.Join(pullDir, "commits.mbox"))
	patch, _ := os.ReadFile(filepath.Join(pullDir, "changes.patch"))
	result, err := materializePullHead(ctx, gitDir, "", os.DevNull, matter.values, baseTip, mbox, patch)
	if err != nil || result.head == "" {
		return "", err
	}
	updated := matter.encode(map[string]string{"headBaseOid": result.base, "headOid": result.head},
		[]string{"headBaseOid", "headOid"})
	if err := os.WriteFile(recordPath, updated, 0600); err != nil {
		return "", err
	}
	return result.head, nil
}

func publishPullHeadRef(ctx context.Context, gitDir string, number int, head string) error {
	if number <= 0 || !gitObjectIDPattern.MatchString(head) {
		return errors.New("invalid pull head")
	}
	output, err := exec.CommandContext(ctx, "git", "--git-dir="+gitDir, "update-ref",
		pullHeadRef(number), head).CombinedOutput()
	if err != nil {
		return fmt.Errorf("git update-ref: %s", boundedText(output, 240))
	}
	return nil
}

var pullHeadSweepFailures sync.Map

func ensurePullHeadRefs(ctx context.Context, gitDir string) (int, error) {
	ledger := gitRefValue(ctx, gitDir, "refs/heads/"+pullMetadataBranch)
	if !gitObjectIDPattern.MatchString(ledger) {
		return 0, nil
	}
	var names []string
	var numbers []int
	for _, path := range gitLines(ctx, gitDir, "ls-tree", "-r", "--name-only", ledger, "--", ".forkmesh/pulls") {
		segments := strings.Split(strings.TrimPrefix(path, ".forkmesh/pulls/"), "/")
		if len(segments) != 2 || segments[1] != "pull.md" {
			continue
		}
		number, err := strconv.Atoi(segments[0])
		if err != nil || number <= 0 {
			continue
		}
		numbers = append(numbers, number)
		names = append(names, ledger+":"+path)
	}
	records, err := gitBatchBlobs(ctx, gitDir, names)
	if err != nil {
		return 0, err
	}
	current, _ := gitRefValues(ctx, gitDir, "refs/pr/")
	published, replays := 0, 0
	for i, number := range numbers {
		matter, ok := parseFrontMatter(records[i])
		if !ok || matter.values["status"] != "open" || matter.values["derive"] == "branch" {
			continue
		}
		head := strings.ToLower(strings.TrimSpace(matter.values["headOid"]))
		onto := strings.ToLower(strings.TrimSpace(matter.values["headBaseOid"]))
		if !gitObjectIDPattern.MatchString(head) || !gitObjectIDPattern.MatchString(onto) ||
			current[pullHeadRef(number)] == head {
			continue
		}
		if gitValue(ctx, gitDir, "rev-parse", "--verify", "--quiet", head+"^{commit}") != head {
			key := gitDir + "\x00" + head
			if _, failed := pullHeadSweepFailures.Load(key); failed || replays >= pullHeadSweepReplays {
				continue
			}
			if gitValue(ctx, gitDir, "rev-parse", "--verify", "--quiet", onto+"^{commit}") != onto {
				continue
			}
			replays++
			series := ledgerPullPayload(ctx, gitDir, ledger, number, "commits.mbox")
			if len(bytes.TrimSpace(series)) == 0 {
				patch := ledgerPullPayload(ctx, gitDir, ledger, number, "changes.patch")
				if len(bytes.TrimSpace(patch)) == 0 {
					pullHeadSweepFailures.Store(key, true)
					continue
				}
				series = syntheticPullMbox(matter.values, patch)
			}
			replayed, err := replayPullSeries(ctx, gitDir, "", os.DevNull, onto, series)
			if err != nil {
				return published, err
			}
			if replayed != head {
				pullHeadSweepFailures.Store(key, true)
				continue
			}
		}
		if err := publishPullHeadRef(ctx, gitDir, number, head); err != nil {
			return published, err
		}
		published++
	}
	return published, nil
}

func ledgerPullPayload(ctx context.Context, gitDir, ledger string, number int, name string) []byte {
	blobs, err := gitBatchBlobs(ctx, gitDir, []string{fmt.Sprintf("%s:.forkmesh/pulls/%d/%s", ledger, number, name)})
	if err != nil || len(blobs) != 1 {
		return nil
	}
	return blobs[0]
}
