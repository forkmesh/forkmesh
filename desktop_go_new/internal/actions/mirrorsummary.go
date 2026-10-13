package actions

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
	"time"
)

const (
	MaximumSummaryRuns         = 20
	MaximumSummaryLogTailBytes = 16 * 1024
	SummaryLeaseMilliseconds   = 10 * 60 * 1000
	SystemSummaryPath          = "/var/lib/forkmesh-mirror/gateway/actions-summary.json"
	SystemStatePath            = "/var/lib/forkmesh-mirror/gateway/actions-state.json"
	maximumSafeJSONInteger     = (int64(1) << 53) - 1
	maximumSummaryBytes        = 256 * 1024
	maximumStateBytes          = 4 * 1024
	maximumSummaryClockSkewMs  = 60 * 1000
	summaryType                = "forkmesh.mirror-actions-summary"
	summaryFileName            = "actions-summary.json"
	stateType                  = "forkmesh.mirror-actions-state"
	stateFileName              = "actions-state.json"
	mirrorRuntimeRewriteMs     = 3 * 60 * 1000
	mirrorSummaryMaxDelayMs    = 3000
	mirrorRenewUnit            = "forkmesh-mirror-renew.service"
	mirrorRenewSystemctlBinary = "/usr/bin/systemctl"
)

var (
	summaryNodePattern    = regexp.MustCompile(`^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$`)
	summarySegmentPattern = regexp.MustCompile(`^[A-Za-z0-9._-]{1,100}$`)
	summaryRefBody        = regexp.MustCompile(`^[A-Za-z0-9._/-]{1,160}$`)
)

func validSummaryRef(ref string) bool {
	var body string
	switch {
	case strings.HasPrefix(ref, "refs/heads/"):
		body = ref[len("refs/heads/"):]
	case strings.HasPrefix(ref, "refs/tags/"):
		body = ref[len("refs/tags/"):]
	default:
		return false
	}
	return summaryRefBody.MatchString(body) && !strings.HasPrefix(body, "/") && !strings.HasSuffix(body, "/") &&
		!strings.Contains(body, "..") && !strings.Contains(body, "//")
}

func boundedSingleLine(value string, maximum int) string {
	var result []uint16
	for _, unit := range utf16Units(value) {
		switch {
		case unit == '\r' || unit == '\n' || unit == '\t':
			if len(result) > 0 && result[len(result)-1] != ' ' {
				result = append(result, ' ')
			}
		case unit >= 0x20 && !(unit >= 0x7f && unit <= 0x9f):
			result = append(result, unit)
		}
		if len(result) >= maximum {
			break
		}
	}
	return qtSimplified(fromUnits(result))
}

func boundedLogTail(value string, maximumBytes int) string {
	var sanitized []uint16
	for _, unit := range utf16Units(value) {
		switch {
		case unit == '\r' || unit == '\n':
			sanitized = append(sanitized, '\n')
		case unit == '\t' || (unit >= 0x20 && !(unit >= 0x7f && unit <= 0x9f)):
			sanitized = append(sanitized, unit)
		}
	}
	encoded := []byte(fromUnits(sanitized))
	if len(encoded) > maximumBytes {
		encoded = encoded[len(encoded)-maximumBytes:]
		for len(encoded) > 0 && encoded[0]&0xc0 == 0x80 {
			encoded = encoded[1:]
		}
	}
	tail := qtFromUtf8(encoded)
	for len([]byte(tail)) > maximumBytes {
		units := utf16Units(tail)
		tail = fromUnits(units[1:])
	}
	return tail
}

func validTimestamp(value int64, allowZero bool) bool {
	low := int64(1)
	if allowZero {
		low = 0
	}
	return value >= low && value <= maximumSafeJSONInteger
}

var summaryStatuses = map[string]bool{
	StatusAwaitingApproval: true, StatusQueued: true, StatusRunning: true, StatusSuccess: true,
	StatusFailed: true, StatusRejected: true, StatusCancelled: true, StatusSkipped: true,
}

func BuildSummary(node string, runs []Run, store *Store, nowMs int64) map[string]any {
	if !summaryNodePattern.MatchString(node) || !validTimestamp(nowMs, false) || nowMs > maximumSafeJSONInteger-SummaryLeaseMilliseconds {
		return nil
	}
	ordered := append([]Run(nil), runs...)
	sort.SliceStable(ordered, func(i, j int) bool {
		if ordered[i].CreatedAtMs != ordered[j].CreatedAtMs {
			return ordered[i].CreatedAtMs > ordered[j].CreatedAtMs
		}
		return ordered[i].ID > ordered[j].ID
	})
	serialized := []any{}
	for _, run := range ordered {
		if len(serialized) >= MaximumSummaryRuns {
			break
		}
		workflow := boundedSingleLine(run.WorkflowName, 160)
		ref := qtTrim(run.Ref)
		commit := strings.ToLower(qtTrim(run.Commit))
		if run.ID <= 0 || !summarySegmentPattern.MatchString(run.Owner) || !summarySegmentPattern.MatchString(run.Name) ||
			workflow == "" || !externalCommitPattern.MatchString(commit) || !validSummaryRef(ref) || !summaryStatuses[run.Status] ||
			!validTimestamp(run.CreatedAtMs, false) || !validTimestamp(run.StartedAtMs, true) || !validTimestamp(run.FinishedAtMs, true) ||
			run.CreatedAtMs > nowMs+maximumSummaryClockSkewMs || run.StartedAtMs > nowMs+maximumSummaryClockSkewMs ||
			run.FinishedAtMs > nowMs+maximumSummaryClockSkewMs {
			continue
		}
		serialized = append(serialized, map[string]any{
			"id":         run.ID,
			"owner":      run.Owner,
			"repository": run.Name,
			"workflow":   workflow,
			"commit":     commit,
			"ref":        ref,
			"status":     run.Status,
			"createdAt":  run.CreatedAtMs,
			"startedAt":  run.StartedAtMs,
			"finishedAt": run.FinishedAtMs,
			"logTail":    boundedLogTail(store.ReadLog(run), MaximumSummaryLogTailBytes),
		})
	}
	result := map[string]any{
		"schemaVersion": 1,
		"type":          summaryType,
		"node":          node,
		"updatedAt":     nowMs,
		"expiresAt":     nowMs + SummaryLeaseMilliseconds,
		"runs":          serialized,
	}
	encoded := qtJSONCompact(result)
	for index := len(serialized) - 1; len(encoded)+1 > maximumSummaryBytes && index >= 0; index-- {
		item := serialized[index].(map[string]any)
		current, _ := item["logTail"].(string)
		excess := len(encoded) + 1 - maximumSummaryBytes
		keep := len([]byte(current)) - excess - 64
		if keep < 0 {
			keep = 0
		}
		item["logTail"] = boundedLogTail(current, keep)
		result["runs"] = serialized
		encoded = qtJSONCompact(result)
	}
	if len(qtJSONCompact(result))+1 > maximumSummaryBytes {
		return nil
	}
	return result
}

func WriteSummaryFile(path, node string, runs []Run, store *Store, nowMs int64) bool {
	summary := BuildSummary(node, runs, store, nowMs)
	if summary == nil {
		return false
	}
	return writeProtectedJSONFile(path, qtJSONCompact(summary), maximumSummaryBytes)
}

func WriteStateFile(path, node, state string, nowMs int64) bool {
	if !summaryNodePattern.MatchString(node) || (state != "disabled" && state != "enabled" && state != "running") ||
		!validTimestamp(nowMs, false) || nowMs > maximumSafeJSONInteger-SummaryLeaseMilliseconds {
		return false
	}
	lease := map[string]any{
		"schemaVersion": 1,
		"type":          stateType,
		"node":          node,
		"state":         state,
		"updatedAt":     nowMs,
		"expiresAt":     nowMs + SummaryLeaseMilliseconds,
	}
	return writeProtectedJSONFile(path, qtJSONCompact(lease), maximumStateBytes)
}

type summaryWritePolicy int

const (
	summaryReject summaryWritePolicy = iota
	summarySameAccount
	summaryRootGatewayHandoff
)

func ClassifySummaryWritePolicy(path string, effectiveUID, parentUID, parentGID uint64, parentMode uint32,
	parentIsProtectedRealDirectory, targetExists, targetIsRegular, targetIsSymbolicLink bool,
	targetUID, targetGID uint64, targetMode uint32, targetLinkCount uint64) summaryWritePolicy {
	if !parentIsProtectedRealDirectory || parentMode != 0o700 || targetIsSymbolicLink ||
		(targetExists && (!targetIsRegular || targetLinkCount != 1)) {
		return summaryReject
	}
	if parentUID == effectiveUID {
		if targetExists && (targetUID != effectiveUID || targetMode != 0o600) {
			return summaryReject
		}
		return summarySameAccount
	}
	fixedPath := path == SystemSummaryPath || path == SystemStatePath
	if !(effectiveUID == 0 && parentUID != 0 && parentGID != 0 && fixedPath) {
		return summaryReject
	}
	if targetExists && (targetUID != 0 || targetGID != parentGID || targetMode != 0o640) {
		return summaryReject
	}
	return summaryRootGatewayHandoff
}

type summaryInspection struct {
	policy      summaryWritePolicy
	parentGID   uint64
	parentIdent string
}

func inspectSummaryPath(path string) summaryInspection {
	inspection := summaryInspection{policy: summaryReject}
	if len(utf16Units(path)) < 2 || len(utf16Units(path)) > 4096 || strings.ContainsAny(path, "\x00\r\n") {
		return inspection
	}
	if !filepath.IsAbs(path) || filepath.Clean(path) != path {
		return inspection
	}
	base := filepath.Base(path)
	if base != summaryFileName && base != stateFileName {
		return inspection
	}
	if info, err := os.Lstat(path); err == nil && info.Mode()&os.ModeSymlink != 0 {
		return inspection
	}
	parent := filepath.Dir(path)
	parentInfo, err := os.Lstat(parent)
	if err != nil || !parentInfo.IsDir() {
		return inspection
	}
	if resolved, err := filepath.EvalSymlinks(parent); err != nil || resolved != parent {
		return inspection
	}
	return inspectOwnership(path, parent, inspection)
}

func writeProtectedJSONFile(path string, encoded []byte, maximum int) bool {
	before := inspectSummaryPath(path)
	if before.policy == summaryReject {
		return false
	}
	encoded = append(append([]byte(nil), encoded...), '\n')
	if len(encoded) > maximum {
		return false
	}
	dir := filepath.Dir(path)
	temp, err := os.CreateTemp(dir, "."+filepath.Base(path)+".*")
	if err != nil {
		return false
	}
	tempName := temp.Name()
	mode := os.FileMode(0o600)
	if before.policy == summaryRootGatewayHandoff {
		mode = 0o640
	}
	fail := func() bool {
		_ = temp.Close()
		_ = os.Remove(tempName)
		return false
	}
	if before.policy == summaryRootGatewayHandoff && !chownRootGroup(temp, before.parentGID) {
		return fail()
	}
	if temp.Chmod(mode) != nil {
		return fail()
	}
	if _, err := temp.Write(encoded); err != nil {
		return fail()
	}
	if temp.Sync() != nil {
		return fail()
	}
	if temp.Close() != nil {
		_ = os.Remove(tempName)
		return false
	}
	if os.Rename(tempName, path) != nil {
		_ = os.Remove(tempName)
		return false
	}
	after := inspectSummaryPath(path)
	return after.policy == before.policy && after.parentIdent == before.parentIdent
}

func (q *actionQueue) scheduleSummary(delayMs int) {
	if q.mirrorGeneration == "" {
		return
	}
	if delayMs < 0 {
		delayMs = 0
	}
	if delayMs > mirrorSummaryMaxDelayMs {
		delayMs = mirrorSummaryMaxDelayMs
	}
	due := time.Now().Add(time.Duration(delayMs) * time.Millisecond)
	if q.summaryTimer != nil {
		if !q.summaryDue.After(due) {
			return
		}
		q.summaryTimer.Stop()
	}
	q.summaryDue = due
	q.summaryTimer = time.AfterFunc(time.Duration(delayMs)*time.Millisecond, func() {
		q.post(func() {
			q.summaryTimer = nil
			q.writeSummary()
		})
	})
}

func (q *actionQueue) writeSummary() {
	if q.mirrorGeneration == "" {
		return
	}
	settings := q.m.settings()
	path := qtTrim(settings.String(mirrorSummaryPathSetting, ""))
	node := qtTrim(settings.String(mirrorNodeSetting, ""))
	now := nowMs()
	q.summaryAttemptedAt = now
	WriteSummaryFile(path, node, q.runs(), q.m.st, now)
}

func (q *actionQueue) updateMirrorRuntimeState() {
	if q.mirrorGeneration == "" {
		return
	}
	settings := q.m.settings()
	enabled := settings.Bool(mirrorEnabledSetting, false)
	running := q.busyRunners() > 0
	state := "enabled"
	if !enabled {
		state = "disabled"
	} else if running {
		state = "running"
	}
	now := nowMs()
	if now-q.summaryAttemptedAt >= mirrorRuntimeRewriteMs {
		q.scheduleSummary(0)
	}
	changed := state != q.mirrorState
	if !changed && now-q.mirrorStateWrittenAt < mirrorRuntimeRewriteMs {
		return
	}
	path := qtTrim(settings.String(mirrorStatePathSetting, ""))
	node := qtTrim(settings.String(mirrorNodeSetting, ""))
	if !WriteStateFile(path, node, state, now) {
		return
	}
	q.mirrorState = state
	q.mirrorStateWrittenAt = now
	if changed && runtime.GOOS != "windows" {
		q.m.renewMirror()
	}
}

func (m *Module) renewMirror() {
	if m.RenewMirror != nil {
		m.RenewMirror()
		return
	}
	cmd := exec.Command(mirrorRenewSystemctlBinary, "--no-block", "start", mirrorRenewUnit)
	hideWindow(cmd)
	if cmd.Start() == nil {
		_ = cmd.Process.Release()
	}
}
