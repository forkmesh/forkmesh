package mirrornode

import (
	"context"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

const pullSynchronizeEvent = "synchronize"

type pullSyncTarget struct {
	head    string
	base    string
	after   string
	derived bool
}

func isPullSynchronize(item collaborationItem) bool {
	return len(item.Event) != 0 && stringValue(item.Event["type"]) == pullSynchronizeEvent
}

func pullSynchronizeTarget(ctx context.Context, gitDir, dir string, number int, event map[string]any) (pullSyncTarget, bool) {
	if number <= 0 || stringValue(event["type"]) != pullSynchronizeEvent {
		return pullSyncTarget{}, false
	}
	raw, err := os.ReadFile(filepath.Join(dir, ".forkmesh", "pulls", strconv.Itoa(number), "pull.md"))
	if err != nil {
		return pullSyncTarget{}, false
	}
	matter, ok := parseFrontMatter(raw)
	if !ok || matter.values["status"] != "open" {
		return pullSyncTarget{}, false
	}
	head := validPullBranch(matter.values["head"])
	base := validPullBranch(matter.values["base"])
	if head == "" || base == "" || head == base || strings.HasPrefix(head, "forkmesh/") ||
		strings.Contains(head, ":") || stringValue(event["ref"]) != "refs/heads/"+head {
		return pullSyncTarget{}, false
	}
	after := strings.ToLower(strings.TrimSpace(stringValue(event["after"])))
	if !gitObjectIDPattern.MatchString(after) ||
		gitValue(ctx, gitDir, "rev-parse", "--verify", "--quiet", "--end-of-options", after+"^{commit}") != after {
		return pullSyncTarget{}, false
	}
	tip := gitRefValue(ctx, gitDir, "refs/heads/"+head)
	if tip == "" || (tip != after && !gitIsAncestor(ctx, gitDir, after, tip)) {
		return pullSyncTarget{}, false
	}
	target := pullSyncTarget{head: head, base: base, after: after, derived: matter.values["derive"] == "branch"}
	if !target.derived && strings.EqualFold(strings.TrimSpace(matter.values["syncedHeadOid"]), after) {
		return pullSyncTarget{}, false
	}
	return target, true
}

func applyPullSynchronize(ctx context.Context, gitDir, dir string, number int, target pullSyncTarget) (string, error) {
	if target.derived || target.after == "" {
		return "", nil
	}
	recordPath := filepath.Join(dir, ".forkmesh", "pulls", strconv.Itoa(number), "pull.md")
	raw, err := os.ReadFile(recordPath)
	if err != nil {
		return "", err
	}
	matter, ok := parseFrontMatter(raw)
	if !ok {
		return "", nil
	}
	baseTip := gitRefValue(ctx, gitDir, "refs/heads/"+target.base)
	if !gitObjectIDPattern.MatchString(baseTip) {
		return "", nil
	}
	onto := gitValue(ctx, gitDir, "merge-base", "--end-of-options", target.after, baseTip)
	if !gitObjectIDPattern.MatchString(onto) {
		return "", nil
	}
	updated := matter.encode(map[string]string{
		"headBaseOid":   onto,
		"headOid":       target.after,
		"syncedHeadOid": target.after,
	}, []string{"headBaseOid", "headOid", "syncedHeadOid"})
	if err := os.WriteFile(recordPath, updated, 0600); err != nil {
		return "", err
	}
	return target.after, nil
}

func syncedPullHead(ctx context.Context, gitDir string, values map[string]string, baseOid string) (pullHead, bool) {
	synced := strings.ToLower(strings.TrimSpace(values["syncedHeadOid"]))
	if synced == "" {
		return pullHead{}, false
	}
	if !gitObjectIDPattern.MatchString(synced) || synced != strings.ToLower(strings.TrimSpace(values["headOid"])) ||
		gitValue(ctx, gitDir, "rev-parse", "--verify", "--quiet", "--end-of-options", synced+"^{commit}") != synced {
		return pullHead{}, true
	}
	onto := gitValue(ctx, gitDir, "merge-base", "--end-of-options", synced, baseOid)
	if !gitObjectIDPattern.MatchString(onto) {
		return pullHead{}, true
	}
	return pullHead{base: onto, head: synced}, true
}
