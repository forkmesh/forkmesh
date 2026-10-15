package actions

import (
	"context"
	"fmt"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	verificationTimeoutMs = 2 * 60 * 1000
	spoolPollInterval     = 4 * time.Second
	ringDelay             = 750 * time.Millisecond
	explicitClaimMs       = 60 * 1000
	interruptedRetryDelay = 60 * time.Second
)

type actionQueue struct {
	m    *Module
	exec *queue

	armed    bool
	restored bool
	snap     RepoSnapshot
	native   map[int]bool

	ids       []int
	verifying map[int]int64
	waiting   map[int]bool
	explicit  map[string]int64
	reasons   map[int]string

	servedPushed     map[string]map[string]bool
	servedPending    bool
	servedInFlight   bool
	servedGeneration uint64
	externalLastScan int64

	mirrorGeneration     string
	mirrorState          string
	mirrorStateWrittenAt int64
	summaryAttemptedAt   int64
	summaryTimer         *time.Timer
	summaryDue           time.Time

	ringTimer  *time.Timer
	stopTick   chan struct{}
	pullChecks *pullCheckState
}

func newActionQueue(m *Module) *actionQueue {
	q := &actionQueue{
		m:            m,
		exec:         newQueue(),
		native:       map[int]bool{},
		verifying:    map[int]int64{},
		waiting:      map[int]bool{},
		explicit:     map[string]int64{},
		reasons:      map[int]string{},
		servedPushed: map[string]map[string]bool{},
		stopTick:     make(chan struct{}),
		pullChecks:   newPullCheckState(),
	}
	go q.exec.loop()
	return q
}

func (q *actionQueue) post(fn func()) { q.exec.post(fn) }

func await[T any](ctx context.Context, q *actionQueue, start func(done func(T))) (T, error) {
	result := make(chan T, 1)
	q.post(func() {
		start(func(value T) {
			select {
			case result <- value:
			default:
			}
		})
	})
	select {
	case value := <-result:
		return value, nil
	case <-ctx.Done():
		var zero T
		return zero, ctx.Err()
	}
}

func (q *actionQueue) startTicker() {
	go func() {
		ticker := time.NewTicker(spoolPollInterval)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				q.post(func() {
					if q.armed {
						q.scanSpool()
					}
				})
			case <-q.stopTick:
				return
			}
		}
	}()
}

func (q *actionQueue) stop() {
	select {
	case <-q.stopTick:
	default:
		close(q.stopTick)
	}
	q.exec.close()
}

func (q *actionQueue) concurrency() int {
	if q.m.Concurrency > 0 {
		return q.m.Concurrency
	}
	return clampInt(runtime.NumCPU(), 2, 4)
}

func (q *actionQueue) busyRunners() int {
	q.m.mu.Lock()
	busy := len(q.m.runs)
	q.m.mu.Unlock()
	for id := range q.native {
		if !q.m.owns(id) {
			busy++
		}
	}
	return busy
}

func (q *actionQueue) idleRunners() int {
	idle := q.concurrency() - q.busyRunners()
	if idle < 0 {
		return 0
	}
	return idle
}

func (q *actionQueue) runs() []Run { return q.m.st.LoadAllRuns() }

func findIn(runs []Run, id int) (Run, bool) {
	for _, run := range runs {
		if run.ID == id {
			return run, true
		}
	}
	return Run{}, false
}

func (q *actionQueue) find(id int) (Run, bool) { return findIn(q.runs(), id) }

func (q *actionQueue) queued(id int) bool {
	for _, queued := range q.ids {
		if queued == id {
			return true
		}
	}
	return false
}

func (q *actionQueue) enqueue(id int) {
	if !q.queued(id) {
		q.ids = append(q.ids, id)
	}
}

func (q *actionQueue) prepend(id int) {
	q.removeQueued(id)
	q.ids = append([]int{id}, q.ids...)
}

func (q *actionQueue) removeQueued(id int) {
	kept := q.ids[:0]
	for _, queued := range q.ids {
		if queued != id {
			kept = append(kept, queued)
		}
	}
	q.ids = kept
}

func (q *actionQueue) log(format string, args ...any) { q.m.logUI(format, args...) }

func (q *actionQueue) save(run Run) {
	_ = q.m.st.SaveRun(run)
	q.emitRun(run, nil)
}

func (q *actionQueue) emitRun(run Run, extra map[string]any) {
	data := map[string]any{"run": run.ID, "record": run.Record()}
	for key, value := range extra {
		data[key] = value
	}
	q.m.emit("actions.run", data)
}

func (q *actionQueue) ring() {
	if q.m.core == nil || q.ringTimer != nil {
		return
	}
	q.ringTimer = time.AfterFunc(ringDelay, func() {
		q.post(func() {
			q.ringTimer = nil
			q.m.ringPhones()
		})
	})
}

func (q *actionQueue) attach(snap RepoSnapshot) {
	q.applySnapshot(snap)
	first := !q.armed
	q.armed = true
	q.m.forgetValueOverlay()
	go q.m.flushQueuedApprovals()
	go q.m.flushAutoFix()
	if first {
		go q.m.recoverMirrorConfigAtAttach()
	}
	q.restore(first)
}

func (q *actionQueue) applySnapshot(snap RepoSnapshot) {
	q.snap = snap
	q.native = map[int]bool{}
	for _, id := range snap.Native {
		q.native[id] = true
	}
	if snap.MirrorGeneration != q.mirrorGeneration {
		q.mirrorGeneration = snap.MirrorGeneration
		q.mirrorState = ""
		q.mirrorStateWrittenAt = 0
		q.summaryAttemptedAt = 0
		q.scheduleSummary(0)
	}
}

func (q *actionQueue) restore(first bool) {
	for _, run := range q.runs() {
		switch {
		case run.Status == StatusRunning && !q.m.owns(run.ID) && !q.native[run.ID]:
			if !run.RequeueAfterDesktopInterruption() {
				continue
			}
			q.m.st.AppendLog(run, "\n==> ⚠ RESTARTING: ForkMesh exited while this run was active. The same approved commit will run again from the beginning.\n")
			q.save(run)
			q.enqueue(run.ID)
			q.log("Actions: re-queued run #%d \"%s\" for %s/%s @ %s after ForkMesh exited; it will restart from its approved snapshot. Run log: %s",
				run.ID, run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8), q.m.st.LogPath(run))
		case run.Status == StatusQueued && !q.m.owns(run.ID):
			if _, checking := q.verifying[run.ID]; !checking {
				q.enqueue(run.ID)
			}
		}
	}
	q.restored = true
	if first {
		for _, repo := range q.snap.Repos {
			q.autoApproveAwaiting(repo, nil)
		}
	}
	q.scanSpool()
}

func (q *actionQueue) needsState(run Run, history []Run) (NeedsState, string) {
	wf := Parse(run.WorkflowPath, run.WorkflowContent)
	if len(wf.Needs) == 0 {
		return NeedsReady, ""
	}
	return ResolveNeeds(run, wf.Needs, history)
}

func (q *actionQueue) noteWaiting(run Run, detail string) {
	if q.waiting[run.ID] {
		return
	}
	q.waiting[run.ID] = true
	q.log("Actions: “%s” for %s/%s @ %s is waiting — %s.", run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8), detail)
}

func (q *actionQueue) requeueOrphans(runs []Run) {
	if !q.restored {
		return
	}
	now := nowMs()
	for id, since := range q.verifying {
		if now-since < verificationTimeoutMs {
			continue
		}
		if run, ok := findIn(runs, id); ok {
			q.log("Actions: the snapshot check for “%s” for %s/%s @ %s has not reported back after %ds — checking again.",
				run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8), (now-since)/1000)
		}
		delete(q.verifying, id)
	}
	for _, run := range runs {
		if run.Status != StatusQueued || q.queued(run.ID) || q.m.owns(run.ID) {
			continue
		}
		if _, checking := q.verifying[run.ID]; checking {
			continue
		}
		q.ids = append(q.ids, run.ID)
		q.log("Actions: re-queued “%s” for %s/%s @ %s — it was Queued but had dropped out of the run queue.",
			run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8))
	}
}

type verification struct {
	runID     int
	verified  Run
	repo      RepoEntry
	mirror    string
	pinned    bool
	content   string
	contentOK bool
	failure   string
}

func (q *actionQueue) processQueue() {
	if !q.armed {
		return
	}
	runs := q.runs()
	q.requeueOrphans(runs)
	if len(q.waiting) > 0 {
		for id := range q.waiting {
			if !q.queued(id) {
				delete(q.waiting, id)
			}
		}
	}
	for len(q.ids) > 0 {
		if q.idleRunners() <= len(q.verifying) {
			break
		}
		index := -1
		state := NeedsReady
		detail := ""
		for i, id := range q.ids {
			run, ok := findIn(runs, id)
			if !ok {
				index, state = i, NeedsReady
				break
			}
			s, d := q.needsState(run, runs)
			if s == NeedsWaiting {
				q.noteWaiting(run, d)
				continue
			}
			index, state, detail = i, s, d
			break
		}
		if index < 0 {
			break
		}
		id := q.ids[index]
		q.ids = append(q.ids[:index], q.ids[index+1:]...)
		delete(q.waiting, id)
		run, ok := findIn(runs, id)
		if _, checking := q.verifying[id]; !ok || run.Status != StatusQueued || checking || q.m.owns(id) {
			continue
		}
		if state == NeedsBlocked {
			run.Status = StatusSkipped
			run.FinishedAtMs = nowMs()
			q.save(run)
			q.scheduleSummary(0)
			q.log("Actions: skipped “%s” for %s/%s @ %s — it needs %s.", run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8), detail)
			runs = q.runs()
			continue
		}
		repo, found := q.snap.lookup(run.Owner, run.Name)
		if !found {
			run.Status = StatusFailed
			q.save(run)
			q.scheduleSummary(0)
			runs = q.runs()
			continue
		}
		q.verifying[id] = nowMs()
		snapshot := run
		go func() {
			result := verification{runID: id, verified: snapshot, repo: repo, mirror: repo.MirrorPath}
			if repo.Pinned {
				if path, ok := q.m.pinMirror(snapshot); ok {
					result.mirror, result.pinned = path, true
				}
			}
			result.content, result.contentOK = WorkflowContentAt(result.mirror, snapshot.Commit, snapshot.WorkflowPath)
			result.verified.RepositoryTree, result.verified.ExecutionDigest, result.failure = RepositoryStateDigest(result.mirror, snapshot.Commit)
			q.post(func() { q.finishVerification(result) })
		}()
	}
	q.emitReasons(runs)
}

func (q *actionQueue) finishVerification(result verification) {
	delete(q.verifying, result.runID)
	release := func() {
		if result.pinned {
			q.m.unpinMirror(result.runID)
		}
	}
	run, ok := q.find(result.runID)
	if !ok || run.Status != StatusQueued {
		release()
		q.processQueue()
		return
	}
	repo, found := q.snap.lookup(run.Owner, run.Name)
	if !found {
		release()
		q.prepend(result.runID)
		q.processQueue()
		return
	}
	stale := repo.Owner != result.verified.Owner || repo.Name != result.verified.Name || repo.MirrorPath != result.mirror ||
		run.Commit != result.verified.Commit || run.WorkflowPath != result.verified.WorkflowPath ||
		run.WorkflowContent != result.verified.WorkflowContent
	if stale {
		release()
		q.prepend(result.runID)
		q.processQueue()
		return
	}
	approved := result.contentOK && result.content == result.verified.WorkflowContent && result.failure == "" &&
		result.verified.RepositoryTree != "" && result.verified.ExecutionDigest != "" &&
		result.verified.RepositoryTree == run.RepositoryTree && result.verified.ExecutionDigest == run.ExecutionDigest &&
		q.m.isApproved(result.verified.RepoKey(), result.verified.WorkflowPath, result.verified.WorkflowContent,
			result.verified.RepositoryTree, result.verified.ExecutionDigest)
	if !approved {
		release()
		run.Status = StatusAwaitingApproval
		q.save(run)
		q.scheduleSummary(0)
		suffix := ""
		if result.failure != "" {
			suffix = ": " + result.failure
		}
		q.log("Actions: blocked \"%s\" for %s/%s @ %s because its approved repository state could not be reverified%s.",
			run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8), suffix)
		q.processQueue()
		return
	}
	wf := Parse(run.WorkflowPath, run.WorkflowContent)
	if !wf.Valid {
		release()
		run.Status = StatusFailed
		q.save(run)
		q.scheduleSummary(0)
		q.processQueue()
		return
	}
	if !WorkflowRunsOnThisNode(repo.WorkflowNodes, wf, q.snap.NodeLabels) {
		release()
		run.Status = StatusSkipped
		q.save(run)
		q.scheduleSummary(0)
		q.log("Actions: skipped “%s” for %s/%s — it is dedicated to %s.", run.WorkflowName, run.Owner, run.Name,
			strings.Join(WorkflowRunsOnLabels(repo.WorkflowNodes, wf), ", "))
		q.processQueue()
		return
	}
	if q.idleRunners() <= 0 {
		release()
		q.prepend(result.runID)
		q.emitReasons(q.runs())
		return
	}
	request := StartRequest{Run: run, Workflow: wf, Mirror: result.mirror, RepoWorkTree: repo.LocalPath,
		Variables: Variables(q.m.settings()), Slice: q.snap.Slice, Limits: DefaultLimits()}
	if !q.m.startQueued(request) {
		release()
	}
	q.processQueue()
}

func (q *actionQueue) queueReason(run Run, history []Run) string {
	if run.Status != StatusQueued {
		return ""
	}
	state, detail := q.needsState(run, history)
	switch state {
	case NeedsWaiting:
		if detail == "" {
			return "needs another workflow to pass first"
		}
		return "needs another workflow to pass first: " + detail
	case NeedsBlocked:
		if detail == "" {
			return "will not start: a workflow it needs did not pass"
		}
		return "will not start: a workflow it needs did not pass (" + detail + ")"
	}
	if since, checking := q.verifying[run.ID]; checking {
		seconds := (nowMs() - since) / 1000
		if seconds < 0 {
			seconds = 0
		}
		if seconds < 5 {
			return "starting: checking its approved repository snapshot"
		}
		return fmt.Sprintf("checking its approved repository snapshot (%ds)", seconds)
	}
	total := q.concurrency()
	idle := q.idleRunners()
	reserved := len(q.verifying)
	if idle > reserved {
		return "starting"
	}
	if idle > 0 {
		return fmt.Sprintf("waiting for a runner: %d of %d set aside for runs checking their snapshot", reserved, total)
	}
	var busyWith []string
	for _, active := range q.m.ActiveRuns() {
		record := RunFromObject(active["run"].(map[string]any))
		label := "\"" + record.WorkflowName + "\""
		if !containsString(busyWith, label) {
			busyWith = append(busyWith, label)
		}
	}
	if len(busyWith) == 0 {
		return fmt.Sprintf("waiting for a runner: all %d are busy", total)
	}
	return fmt.Sprintf("waiting for a runner: all %d are busy with %s", total, strings.Join(busyWith, ", "))
}

func (q *actionQueue) reasonMap(runs []Run) map[int]string {
	out := map[int]string{}
	for _, run := range runs {
		if run.Status != StatusQueued {
			continue
		}
		if reason := q.queueReason(run, runs); reason != "" {
			out[run.ID] = reason
		}
	}
	return out
}

func (q *actionQueue) emitReasons(runs []Run) {
	reasons := q.reasonMap(runs)
	same := len(reasons) == len(q.reasons)
	if same {
		for id, reason := range reasons {
			if q.reasons[id] != reason {
				same = false
				break
			}
		}
	}
	if same {
		return
	}
	q.reasons = reasons
	q.m.emit("actions.queue", q.state())
}

func (q *actionQueue) state() map[string]any {
	reasons := map[string]any{}
	for id, reason := range q.reasons {
		reasons[strconv.Itoa(id)] = reason
	}
	verifying := make([]int, 0, len(q.verifying))
	for id := range q.verifying {
		verifying = append(verifying, id)
	}
	sort.Ints(verifying)
	return map[string]any{"armed": q.armed, "queue": append([]int{}, q.ids...), "verifying": verifying,
		"reasons": reasons, "runners": q.concurrency()}
}

func (q *actionQueue) runFinished(id int) {
	if !q.armed {
		return
	}
	run, ok := q.find(id)
	if !ok || (run.Status != StatusSuccess && run.Status != StatusFailed && run.Status != StatusCancelled) {
		status := "missing"
		if ok {
			status = actionStatusText(run.Status)
		}
		q.log("Actions: ignored stale completion for run #%d; the persisted run is still %s.", id, status)
		return
	}
	q.scheduleSummary(0)
	q.processQueue()
	q.updateMirrorRuntimeState()
	if run.Status == StatusCancelled {
		q.deleteCancelledAutomatically([]Run{run})
	}
	q.ring()
}

func (q *actionQueue) runInterrupted(id int) {
	if !q.armed || q.m.shuttingDown() {
		return
	}
	run, ok := q.find(id)
	if !ok {
		return
	}
	if run.RequeueAfterDesktopInterruption() {
		q.save(run)
		q.enqueue(run.ID)
	}
	q.log("Actions: run #%d \"%s\" for %s/%s @ %s was interrupted by a stop signal to ForkMesh; it is queued again and restarts from its approved snapshot. Run log: %s",
		run.ID, run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8), q.m.st.LogPath(run))
	q.scheduleSummary(0)
	time.AfterFunc(interruptedRetryDelay, func() { q.post(q.processQueue) })
	q.updateMirrorRuntimeState()
}

func actionStatusText(status string) string {
	switch status {
	case StatusAwaitingApproval:
		return "Awaiting approval"
	case StatusQueued:
		return "Queued"
	case StatusRunning:
		return "Running"
	case StatusSuccess:
		return "Success"
	case StatusFailed:
		return "Failed"
	case StatusRejected:
		return "Rejected"
	case StatusCancelled:
		return "Cancelled"
	case StatusSkipped:
		return "Skipped"
	}
	return status
}

func (q *actionQueue) autoDeleteCancelled() bool {
	return q.m.settings().Bool(autoDeleteCancelledSetting, true)
}

func (q *actionQueue) deleteCancelledAutomatically(runs []Run) bool {
	if !q.autoDeleteCancelled() {
		return false
	}
	seen := map[int]bool{}
	var doomed []Run
	for _, run := range runs {
		if run.ID <= 0 || run.Status != StatusCancelled || seen[run.ID] {
			continue
		}
		seen[run.ID] = true
		doomed = append(doomed, run)
	}
	if len(doomed) == 0 {
		return false
	}
	records := make([]map[string]any, 0, len(doomed))
	for _, run := range doomed {
		q.m.st.DeleteRun(run)
		q.removeQueued(run.ID)
		delete(q.waiting, run.ID)
		records = append(records, run.Record())
	}
	q.scheduleSummary(0)
	q.m.emit("actions.deleted", map[string]any{"runs": records})
	return true
}

func (q *actionQueue) cancelSuperseded(newRun Run) {
	var superseded []int
	for _, run := range q.runs() {
		if run.ID == newRun.ID || run.Owner != newRun.Owner || run.Name != newRun.Name || run.WorkflowPath != newRun.WorkflowPath {
			continue
		}
		if run.Status == StatusRunning || run.Status == StatusQueued || run.Status == StatusAwaitingApproval {
			superseded = append(superseded, run.ID)
		}
	}
	var cancelled []Run
	for _, id := range superseded {
		run, ok := q.find(id)
		if !ok {
			continue
		}
		switch run.Status {
		case StatusRunning:
			if r := q.m.activeRunner(run.ID); r != nil {
				q.log("Actions: aborting \"%s\" for %s/%s @ %s — superseded by a newer run of the same workflow.",
					run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8))
				r.post(r.stop)
			} else if q.native[run.ID] {
				q.log("Actions: aborting \"%s\" for %s/%s @ %s — superseded by a newer run of the same workflow.",
					run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8))
				q.m.emit("actions.stop", map[string]any{"run": run.ID})
			}
		case StatusQueued, StatusAwaitingApproval:
			wasPending := run.Status == StatusAwaitingApproval
			q.removeQueued(run.ID)
			run.Status = StatusCancelled
			run.FinishedAtMs = nowMs()
			_ = q.m.st.SaveRun(run)
			q.scheduleSummary(0)
			cancelled = append(cancelled, run)
			kind := "queued"
			if wasPending {
				kind = "pending"
			}
			q.log("Actions: cancelled %s \"%s\" for %s/%s @ %s — superseded by a newer run of the same workflow.",
				kind, run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8))
		}
	}
	if !q.deleteCancelledAutomatically(cancelled) {
		for _, run := range cancelled {
			q.emitRun(run, nil)
		}
	}
}

func (q *actionQueue) resolveApproval(repo RepoEntry, run Run) bool {
	already := q.m.isApproved(run.RepoKey(), run.WorkflowPath, run.WorkflowContent, run.RepositoryTree, run.ExecutionDigest)
	if already || !repo.AutoApprove || run.FirstTimePullAuthor {
		return already
	}
	q.m.approve(run.RepoKey(), run.WorkflowPath, run.WorkflowContent, run.RepositoryTree, run.ExecutionDigest)
	return q.m.isApproved(run.RepoKey(), run.WorkflowPath, run.WorkflowContent, run.RepositoryTree, run.ExecutionDigest)
}

func (q *actionQueue) createRun(run Run) (Run, bool) {
	created, err := q.m.st.CreateRun(run)
	if err != nil {
		q.log("Actions: could not record a run of \"%s\" for %s/%s: %v", run.WorkflowName, run.Owner, run.Name, err)
		return Run{}, false
	}
	return created, true
}

func (m *Module) owns(id int) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	_, ok := m.runs[id]
	return ok
}

func (m *Module) startQueued(request StartRequest) bool {
	if m.stopping.Load() {
		return false
	}
	m.mu.Lock()
	if _, exists := m.runs[request.Run.ID]; exists {
		m.mu.Unlock()
		return false
	}
	r := newRunner(m, request)
	m.runs[request.Run.ID] = &activeRun{runner: r, request: request, queued: true}
	m.mu.Unlock()
	m.emit("actions.started", map[string]any{
		"run":      request.Run.Record(),
		"workflow": map[string]any{"path": request.Workflow.Path, "content": request.Workflow.Content},
		"mirror":   request.Mirror,
		"worktree": request.RepoWorkTree,
	})
	r.post(func() { r.start(request) })
	m.logf("run #%d %q for %s/%s @ %s started from the core queue", request.Run.ID, request.Run.WorkflowName,
		request.Run.Owner, request.Run.Name, qtLeft(request.Run.Commit, 8))
	return true
}

func (m *Module) pinMirror(run Run) (string, bool) {
	if m.PinMirror != nil {
		return m.PinMirror(run)
	}
	if m.core == nil {
		return "", false
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	raw, err := m.core.CallUI(ctx, "ui.actions.pin", map[string]any{"run": run.ID, "owner": run.Owner, "name": run.Name})
	if err != nil {
		return "", false
	}
	object, ok := decodeJSONObject(raw)
	path := jsonString(object, "path")
	if !ok || path == "" {
		return "", false
	}
	return path, true
}

func (m *Module) unpinMirror(id int) {
	if m.UnpinMirror != nil {
		m.UnpinMirror(id)
		return
	}
	if m.core != nil {
		m.core.NotifyUI("ui.actions.unpin", map[string]any{"run": id})
	}
}

func dirIsReal(path string) bool {
	info, err := os.Lstat(path)
	return err == nil && info.IsDir()
}
