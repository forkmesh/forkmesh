package actions

import (
	"fmt"
	"strings"
	"time"
)

type opResult struct {
	run   int
	ok    bool
	error string
}

func (q *actionQueue) startManual(owner, name, path, branch string, done func(opResult)) {
	repo, ok := q.snap.lookup(owner, name)
	if !ok {
		done(opResult{error: "This repository is not on this desktop."})
		return
	}
	if path == "" || repo.MirrorPath == "" {
		done(opResult{error: "This repository has no mirror to run Actions from."})
		return
	}
	mirror := repo.MirrorPath
	type manualRead struct {
		commit   string
		showOK   bool
		content  string
		workflow Workflow
		run      Run
		bound    bool
		failure  string
	}
	go func() {
		var read manualRead
		revParse := func(rev string) string {
			out, code := gitStdoutAny(mirror, 8*time.Second, "rev-parse", "--verify", rev+"^{commit}")
			if code != 0 {
				return ""
			}
			return qtTrim(qtFromUtf8(out))
		}
		read.commit = revParse(branch)
		if read.commit == "" {
			read.commit = revParse("refs/heads/" + branch)
		}
		if read.commit != "" {
			out, code := gitStdoutAny(mirror, 10*time.Second, "show", read.commit+":"+path)
			if code == 0 {
				read.showOK = true
				read.content = qtFromUtf8(out)
				read.workflow = Parse(path, read.content)
				if read.workflow.Valid {
					read.run = NewRun()
					read.run.Owner, read.run.Name = owner, name
					read.run.WorkflowPath, read.run.WorkflowName, read.run.WorkflowContent = path, read.workflow.Name, read.content
					read.run.Commit, read.run.Ref = read.commit, "refs/heads/"+branch
					read.run.RepositoryTree, read.run.ExecutionDigest, read.failure = RepositoryStateDigest(mirror, read.commit)
					read.bound = read.failure == ""
				}
			}
		}
		q.post(func() {
			repo, ok := q.snap.lookup(owner, name)
			switch {
			case !ok:
				done(opResult{error: "The repository changed while the run was prepared."})
			case read.commit == "":
				done(opResult{error: "Branch not found in this repo's mirror: " + branch})
			case !read.showOK:
				done(opResult{error: fmt.Sprintf("“%s” doesn't exist on %s.", path, branch)})
			case !read.workflow.Valid:
				done(opResult{error: "Workflow is invalid: " + read.workflow.Error})
			case !WorkflowRunsOnThisNode(repo.WorkflowNodes, read.workflow, q.snap.NodeLabels):
				done(opResult{error: fmt.Sprintf("“%s” runs on %s — start it from that node.", read.workflow.Name,
					strings.Join(WorkflowRunsOnLabels(repo.WorkflowNodes, read.workflow), ", "))})
			case !read.bound:
				done(opResult{error: "Could not bind this run to the repository state: " + read.failure})
			default:
				run := read.run
				approved := q.resolveApproval(repo, run)
				run.Status = StatusAwaitingApproval
				if approved {
					run.Status = StatusQueued
				}
				created, ok := q.createRun(run)
				if !ok {
					done(opResult{error: "not_started"})
					return
				}
				q.emitRun(created, map[string]any{"created": true})
				q.scheduleSummary(0)
				q.cancelSuperseded(created)
				if approved {
					q.ids = append(q.ids, created.ID)
				}
				what := "run awaiting approval of"
				if approved {
					what = "run of"
				}
				q.log("Actions: manual %s \"%s\" for %s/%s on %s @ %s", what, read.workflow.Name, repo.Owner, repo.Name, branch, qtLeft(read.commit, 8))
				q.processQueue()
				q.ring()
				done(opResult{run: created.ID, ok: true})
			}
		})
	}()
}

type stateCheck struct {
	contentOK bool
	content   string
	tree      string
	digest    string
	failure   string
}

func checkState(mirror string, run Run) stateCheck {
	var check stateCheck
	check.content, check.contentOK = WorkflowContentAt(mirror, run.Commit, run.WorkflowPath)
	check.tree, check.digest, check.failure = RepositoryStateDigest(mirror, run.Commit)
	return check
}

func (c stateCheck) matches(run Run) bool {
	return c.contentOK && c.content == run.WorkflowContent && c.failure == "" &&
		c.tree == run.RepositoryTree && c.digest == run.ExecutionDigest
}

func (q *actionQueue) approveRun(id int, done func(opResult)) {
	run, ok := q.find(id)
	if !ok || run.Status != StatusAwaitingApproval {
		done(opResult{error: "This run is not waiting for approval."})
		return
	}
	repo, found := q.snap.lookup(run.Owner, run.Name)
	if !found {
		done(opResult{error: "This run's repository is not on this node."})
		return
	}
	pending := run
	go func() {
		check := checkState(repo.MirrorPath, pending)
		q.post(func() {
			if !check.matches(pending) {
				done(opResult{error: "The repository state changed or could not be verified. Queue a fresh run before approving it."})
				return
			}
			q.m.approve(pending.RepoKey(), pending.WorkflowPath, pending.WorkflowContent, pending.RepositoryTree, pending.ExecutionDigest)
			if !q.m.isApproved(pending.RepoKey(), pending.WorkflowPath, pending.WorkflowContent, pending.RepositoryTree, pending.ExecutionDigest) {
				done(opResult{error: "The bound approval could not be saved."})
				return
			}
			live, ok := q.find(id)
			if !ok || live.Status != StatusAwaitingApproval {
				done(opResult{error: "This run is no longer waiting for approval."})
				return
			}
			live.Status = StatusQueued
			q.save(live)
			q.scheduleSummary(0)
			q.enqueue(live.ID)
			pullNote := ""
			if pending.PullNumber > 0 {
				pullNote = fmt.Sprintf(" (pull request #%d)", pending.PullNumber)
			}
			q.log("Actions: approved \"%s\" for %s/%s%s.", pending.WorkflowName, pending.Owner, pending.Name, pullNote)
			q.processQueue()
			q.ring()
			done(opResult{run: id, ok: true})
		})
	}()
}

func (q *actionQueue) rejectRun(id int) bool {
	run, ok := q.find(id)
	if !ok || run.Status != StatusAwaitingApproval {
		return false
	}
	run.Status = StatusRejected
	run.FinishedAtMs = nowMs()
	q.save(run)
	q.scheduleSummary(0)
	q.log("Actions: rejected \"%s\" for %s/%s.", run.WorkflowName, run.Owner, run.Name)
	q.ring()
	return true
}

func (q *actionQueue) rerunRun(id int, done func(opResult)) {
	prev, ok := q.find(id)
	if !ok {
		done(opResult{error: "not_started"})
		return
	}
	run := NewRun()
	run.Owner, run.Name = prev.Owner, prev.Name
	run.WorkflowPath, run.WorkflowName, run.WorkflowContent = prev.WorkflowPath, prev.WorkflowName, prev.WorkflowContent
	run.Commit, run.Ref = prev.Commit, prev.Ref
	repo, found := q.snap.lookup(run.Owner, run.Name)
	if !found {
		done(opResult{error: "not_started"})
		return
	}
	wf := Parse(run.WorkflowPath, run.WorkflowContent)
	if !WorkflowRunsOnThisNode(repo.WorkflowNodes, wf, q.snap.NodeLabels) {
		done(opResult{error: fmt.Sprintf("“%s” runs on %s — rerun it from that node.", run.WorkflowName,
			strings.Join(WorkflowRunsOnLabels(repo.WorkflowNodes, wf), ", "))})
		return
	}
	go func() {
		content, contentOK := WorkflowContentAt(repo.MirrorPath, run.Commit, run.WorkflowPath)
		bound := run
		var failure string
		if contentOK && content == run.WorkflowContent {
			bound.RepositoryTree, bound.ExecutionDigest, failure = RepositoryStateDigest(repo.MirrorPath, run.Commit)
		}
		q.post(func() {
			if !contentOK || content != run.WorkflowContent || failure != "" {
				done(opResult{error: "Could not verify the repository state for this rerun: " + failure})
				return
			}
			current, ok := q.snap.lookup(run.Owner, run.Name)
			if !ok {
				done(opResult{error: "not_started"})
				return
			}
			approved := q.resolveApproval(current, bound)
			bound.Status = StatusAwaitingApproval
			if approved {
				bound.Status = StatusQueued
			}
			created, ok := q.createRun(bound)
			if !ok {
				done(opResult{error: "not_started"})
				return
			}
			q.emitRun(created, map[string]any{"created": true, "rerunOf": id})
			q.scheduleSummary(0)
			q.cancelSuperseded(created)
			if approved {
				q.ids = append(q.ids, created.ID)
			}
			what := "awaiting approval of"
			if approved {
				what = "of"
			}
			q.log("Actions: rerun %s \"%s\" for %s/%s @ %s", what, bound.WorkflowName, bound.Owner, bound.Name, qtLeft(bound.Commit, 8))
			q.processQueue()
			q.ring()
			done(opResult{run: created.ID, ok: true})
		})
	}()
}

func (q *actionQueue) stopRun(id int) {
	run, ok := q.find(id)
	if !ok {
		return
	}
	if run.Status == StatusRunning {
		if r := q.m.activeRunner(run.ID); r != nil {
			q.log("Actions: stopping \"%s\" for %s/%s.", run.WorkflowName, run.Owner, run.Name)
			r.post(r.stop)
		} else if q.native[run.ID] {
			q.log("Actions: stopping \"%s\" for %s/%s.", run.WorkflowName, run.Owner, run.Name)
			q.m.emit("actions.stop", map[string]any{"run": run.ID})
		}
		return
	}
	if run.Status == StatusQueued {
		q.removeQueued(run.ID)
		run.Status = StatusCancelled
		run.FinishedAtMs = nowMs()
		_ = q.m.st.SaveRun(run)
		q.scheduleSummary(0)
		q.log("Actions: cancelled queued \"%s\" for %s/%s.", run.WorkflowName, run.Owner, run.Name)
		if !q.deleteCancelledAutomatically([]Run{run}) {
			q.emitRun(run, nil)
		}
		q.ring()
	}
}

func (q *actionQueue) skipRun(id int) {
	run, ok := q.find(id)
	if !ok || (run.Status != StatusQueued && run.Status != StatusAwaitingApproval) {
		return
	}
	q.removeQueued(run.ID)
	run.Status = StatusSkipped
	run.FinishedAtMs = nowMs()
	q.save(run)
	q.scheduleSummary(0)
	q.log("Actions: skipped \"%s\" for %s/%s.", run.WorkflowName, run.Owner, run.Name)
	q.ring()
}

func (q *actionQueue) autoApproveAwaiting(repo RepoEntry, done func(int)) {
	finish := func(count int) {
		if done != nil {
			done(count)
		}
	}
	if !repo.Enabled || !repo.AutoApprove {
		finish(0)
		return
	}
	var candidates []Run
	for _, run := range q.runs() {
		if run.Status == StatusAwaitingApproval && run.Owner == repo.Owner && run.Name == repo.Name {
			candidates = append(candidates, run)
		}
	}
	if len(candidates) == 0 {
		finish(0)
		return
	}
	go func() {
		checks := make([]stateCheck, len(candidates))
		for index, run := range candidates {
			if !run.FirstTimePullAuthor {
				checks[index] = checkState(repo.MirrorPath, run)
			}
		}
		q.post(func() {
			queued := 0
			for index, run := range candidates {
				if run.FirstTimePullAuthor {
					author := run.PullAuthor
					if author == "" {
						author = "its author"
					}
					q.log("Actions: “%s” for %s/%s #%d stays held — %s has no merged work here yet, so their pull request needs your approval.",
						run.WorkflowName, run.Owner, run.Name, run.PullNumber, author)
					continue
				}
				check := checks[index]
				if !check.matches(run) {
					suffix := ""
					if check.failure != "" {
						suffix = ": " + check.failure
					}
					q.log("Actions: keeping \"%s\" for %s/%s @ %s awaiting review because its saved repository state could not be reverified%s.",
						run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8), suffix)
					continue
				}
				if !q.resolveApproval(repo, run) {
					q.log("Actions: keeping \"%s\" for %s/%s @ %s awaiting review because its automatic approval could not be saved.",
						run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8))
					continue
				}
				live, ok := q.find(run.ID)
				if !ok || live.Status != StatusAwaitingApproval {
					continue
				}
				live.Status = StatusQueued
				q.save(live)
				q.enqueue(live.ID)
				queued++
				q.log("Actions: automatically approved \"%s\" for %s/%s @ %s.", run.WorkflowName, run.Owner, run.Name, qtLeft(run.Commit, 8))
			}
			if queued > 0 {
				q.scheduleSummary(0)
				q.processQueue()
				q.ring()
			}
			finish(queued)
		})
	}()
}
