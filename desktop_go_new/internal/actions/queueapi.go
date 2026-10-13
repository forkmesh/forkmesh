package actions

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strings"
	"time"

	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/rpc"
)

var errDesktopUnavailable = rpc.Fail("desktop_unavailable")

const queueOpTimeout = 2 * time.Minute

func (m *Module) registerQueue(handle func(string, rpc.Handler)) {
	handle("core.actions.queue.attach", m.local(m.handleQueueAttach))
	handle("core.actions.queue.repos", m.local(m.handleQueueRepos))
	handle("core.actions.queue.kick", m.local(m.handleQueueKick))
	handle("core.actions.queue.state", m.local(m.handleQueueState))
	handle("core.actions.queue.push", m.local(m.handleQueuePush))
	handle("core.actions.queue.commit", m.local(m.handleQueueCommit))
	handle("core.actions.queue.manual", m.local(m.handleQueueManual))
	handle("core.actions.queue.approve", m.local(m.handleQueueApprove))
	handle("core.actions.queue.reject", m.local(m.handleQueueReject))
	handle("core.actions.queue.rerun", m.local(m.handleQueueRerun))
	handle("core.actions.queue.stop", m.local(m.handleQueueStop))
	handle("core.actions.queue.skip", m.local(m.handleQueueSkip))
	handle("core.actions.queue.autoApprove", m.local(m.handleQueueAutoApprove))
	handle("core.actions.queue.pullChecks", m.local(m.handleQueuePullChecks))
}

func (m *Module) queueArmed() bool {
	if m.q == nil {
		return false
	}
	armed, err := await(context.Background(), m.q, func(done func(bool)) { done(m.q.armed) })
	return err == nil && armed
}

func (m *Module) withQueue(ctx context.Context, fn func(q *actionQueue, done func(any, error))) (any, error) {
	if m.q == nil {
		return nil, rpc.Fail("core_stopping")
	}
	ctx, cancel := context.WithTimeout(ctx, queueOpTimeout)
	defer cancel()
	type reply struct {
		value any
		err   error
	}
	out, err := await(ctx, m.q, func(done func(reply)) {
		fn(m.q, func(value any, err error) { done(reply{value, err}) })
	})
	if err != nil {
		return nil, rpc.Fail("core_timeout")
	}
	return out.value, out.err
}

func (m *Module) requireArmed(ctx context.Context, fn func(q *actionQueue, done func(any, error))) (any, error) {
	return m.withQueue(ctx, func(q *actionQueue, done func(any, error)) {
		if !q.armed {
			done(nil, rpc.Fail("queue_not_attached"))
			return
		}
		fn(q, done)
	})
}

func (m *Module) handleQueueAttach(ctx context.Context, req *rpc.Request) (any, error) {
	var snap RepoSnapshot
	if err := req.Bind(&snap); err != nil {
		return nil, err
	}
	return m.withQueue(ctx, func(q *actionQueue, done func(any, error)) {
		q.attach(snap)
		runs := q.runs()
		records := make([]map[string]any, 0, len(runs))
		highest := 0
		for _, run := range runs {
			records = append(records, run.Record())
			if run.ID > highest {
				highest = run.ID
			}
		}
		state := q.state()
		state["runs"] = records
		state["maxRunId"] = highest
		state["pid"] = os.Getpid()
		done(state, nil)
	})
}

func (m *Module) handleQueueRepos(ctx context.Context, req *rpc.Request) (any, error) {
	var snap RepoSnapshot
	if err := req.Bind(&snap); err != nil {
		return nil, err
	}
	return m.withQueue(ctx, func(q *actionQueue, done func(any, error)) {
		q.applySnapshot(snap)
		if q.armed {
			q.processQueue()
		}
		done(map[string]any{"armed": q.armed}, nil)
	})
}

func (m *Module) handleQueueKick(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Native *[]int `json:"native"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	return m.withQueue(ctx, func(q *actionQueue, done func(any, error)) {
		if params.Native != nil {
			q.native = map[int]bool{}
			for _, id := range *params.Native {
				q.native[id] = true
			}
			q.snap.Native = append([]int{}, (*params.Native)...)
		}
		if q.armed {
			q.scanSpool()
		}
		done(q.state(), nil)
	})
}

func (m *Module) handleQueueState(ctx context.Context, req *rpc.Request) (any, error) {
	return m.withQueue(ctx, func(q *actionQueue, done func(any, error)) { done(q.state(), nil) })
}

func (m *Module) handleQueuePush(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Owner  string `json:"owner"`
		Name   string `json:"name"`
		Commit string `json:"commit"`
		Ref    string `json:"ref"`
		Before string `json:"before"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	return m.requireArmed(ctx, func(q *actionQueue, done func(any, error)) {
		q.enqueuePushEvent(params.Owner, params.Name, params.Commit, params.Ref, params.Before)
		done(map[string]any{"accepted": true}, nil)
	})
}

func (m *Module) handleQueueCommit(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Owner    string      `json:"owner"`
		Name     string      `json:"name"`
		Commit   string      `json:"commit"`
		Ref      string      `json:"ref"`
		Trigger  Trigger     `json:"trigger"`
		Pull     PullContext `json:"pull"`
		Before   string      `json:"before"`
		Explicit bool        `json:"explicit"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	switch params.Trigger {
	case TriggerPush, TriggerPullRequest, TriggerPullRequestManual, TriggerRelease:
	default:
		return nil, rpc.Fail("bad_request")
	}
	return m.requireArmed(ctx, func(q *actionQueue, done func(any, error)) {
		if params.Explicit {
			q.claimExplicit(params.Owner, params.Name, params.Commit)
		}
		q.queueWorkflowsForCommit(params.Owner, params.Name, params.Commit, params.Ref, params.Trigger, params.Pull, params.Before)
		done(map[string]any{"accepted": true}, nil)
	})
}

func opReply(result opResult) (any, error) {
	if !result.ok {
		if result.error == "" {
			return nil, rpc.Fail("not_started")
		}
		return nil, rpc.Fail(result.error)
	}
	return map[string]any{"run": result.run}, nil
}

func (m *Module) handleQueueManual(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Owner  string `json:"owner"`
		Name   string `json:"name"`
		Path   string `json:"path"`
		Branch string `json:"branch"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	return m.requireArmed(ctx, func(q *actionQueue, done func(any, error)) {
		q.startManual(params.Owner, params.Name, params.Path, params.Branch, func(r opResult) { done(opReply(r)) })
	})
}

func runParam(req *rpc.Request) int { return numberParam(req.Object(), "run") }

func (m *Module) handleQueueApprove(ctx context.Context, req *rpc.Request) (any, error) {
	id := runParam(req)
	return m.requireArmed(ctx, func(q *actionQueue, done func(any, error)) {
		q.approveRun(id, func(r opResult) { done(opReply(r)) })
	})
}

func (m *Module) handleQueueReject(ctx context.Context, req *rpc.Request) (any, error) {
	id := runParam(req)
	return m.requireArmed(ctx, func(q *actionQueue, done func(any, error)) {
		if !q.rejectRun(id) {
			done(nil, rpc.Fail("wrong_state"))
			return
		}
		done(map[string]any{"run": id}, nil)
	})
}

func (m *Module) handleQueueRerun(ctx context.Context, req *rpc.Request) (any, error) {
	id := runParam(req)
	return m.requireArmed(ctx, func(q *actionQueue, done func(any, error)) {
		q.rerunRun(id, func(r opResult) { done(opReply(r)) })
	})
}

func (m *Module) handleQueueStop(ctx context.Context, req *rpc.Request) (any, error) {
	id := runParam(req)
	return m.requireArmed(ctx, func(q *actionQueue, done func(any, error)) {
		q.stopRun(id)
		done(map[string]any{"run": id}, nil)
	})
}

func (m *Module) handleQueueSkip(ctx context.Context, req *rpc.Request) (any, error) {
	id := runParam(req)
	return m.requireArmed(ctx, func(q *actionQueue, done func(any, error)) {
		q.skipRun(id)
		done(map[string]any{"run": id}, nil)
	})
}

func (m *Module) handleQueueAutoApprove(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Owner string `json:"owner"`
		Name  string `json:"name"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	return m.requireArmed(ctx, func(q *actionQueue, done func(any, error)) {
		for index := range q.snap.Repos {
			if q.snap.Repos[index].Owner == params.Owner && q.snap.Repos[index].Name == params.Name {
				q.snap.Repos[index].AutoApprove = true
			}
		}
		repo, ok := q.snap.lookup(params.Owner, params.Name)
		if !ok {
			done(nil, rpc.Fail("unknown_repo"))
			return
		}
		q.autoApproveAwaiting(repo, func(count int) {
			if count > 0 {
				plural := "s"
				if count == 1 {
					plural = ""
				}
				q.log("Actions: queued %d automatically approved pending run%s for %s/%s.", count, plural, repo.Owner, repo.Name)
			}
			done(map[string]any{"queued": count}, nil)
		})
	})
}

func (m *Module) ringPhones() {
	if m.core != nil {
		phonefamily.BroadcastPhones(m.core, "repos.changed", map[string]any{})
	}
}

func (m *Module) gate() *phonefamily.Gate { return &phonefamily.Gate{Core: m.core} }

func (m *Module) factsAuto(ctx context.Context, peer rpc.Peer, method, repo string) (RepoFacts, error) {
	if !m.queueArmed() {
		return m.factsFromUI(ctx, peer, method, repo)
	}
	return m.factsNative(ctx, peer, method, repo)
}

func (m *Module) factsNative(ctx context.Context, peer rpc.Peer, method, slug string) (RepoFacts, error) {
	if peer != nil && rpc.Remote(peer) && !m.gate().IsOwner(peer.Device()) {
		return RepoFacts{}, rpc.Fail("not_owner")
	}
	owner, name, ok := ParseRepoSlug(slug)
	if !ok {
		return RepoFacts{}, rpc.Fail("bad_request")
	}
	type found struct {
		facts RepoFacts
		ok    bool
	}
	result, err := await(ctx, m.q, func(done func(found)) {
		repo, ok := m.q.snap.lookup(owner, name)
		done(found{repo.facts(m.q.snap.NodeLabels), ok})
	})
	if err != nil {
		return RepoFacts{}, rpc.Fail("core_timeout")
	}
	if !result.ok {
		return RepoFacts{}, rpc.Fail("unknown_repo")
	}
	return result.facts, nil
}

func (m *Module) performAuto(ctx context.Context, intent Intent, who string, peer rpc.Peer) (map[string]any, error) {
	if !m.queueArmed() {
		return m.performWithUI(ctx, intent, who, peer)
	}
	return m.performNative(ctx, intent, who, peer)
}

func (m *Module) performNative(ctx context.Context, intent Intent, who string, peer rpc.Peer) (map[string]any, error) {
	if peer != nil && rpc.Remote(peer) && !m.gate().IsOwner(peer.Device()) {
		return nil, rpc.Fail("not_owner")
	}
	owner, name, _ := ParseRepoSlug(intent.Repo)
	started := func(message string) (map[string]any, error) {
		return map[string]any{"started": true, "action": intent.Action, "subject": intent.Repo, "message": message}, nil
	}
	value, err := m.requireArmed(ctx, func(q *actionQueue, done func(any, error)) {
		if _, ok := q.snap.lookup(owner, name); !ok {
			done(nil, rpc.Fail("unknown_repo"))
			return
		}
		q.log("Actions: %s for %s requested by %s.", strings.ToLower(ActionLabel(intent.Action)), IntentSubject(intent), who)
		reply := func(message string) {
			q.ring()
			out, err := started(message)
			done(out, err)
		}
		if intent.Action == actionRunWorkflow {
			q.startManual(owner, name, intent.Path, intent.Branch, func(r opResult) {
				if !r.ok {
					code := r.error
					if code == "" {
						code = "not_started"
					}
					done(nil, rpc.Fail(code))
					return
				}
				reply(fmt.Sprintf("Started run #%d.", r.run))
			})
			return
		}
		if actionSwitchesWorkflow(intent.Action) {
			disable := intent.Action == actionDisableWorkflow
			go func() {
				err := m.setWorkflowDisabled(owner, name, intent.Path, disable)
				q.post(func() {
					if err != nil {
						done(nil, err)
						return
					}
					if disable {
						reply(fmt.Sprintf("Turned off %s.", intent.Path))
					} else {
						reply(fmt.Sprintf("Turned on %s.", intent.Path))
					}
				})
			}()
			return
		}
		run, ok := q.find(intent.Run)
		if !ok || strings.ToLower(run.Owner+"/"+run.Name) != strings.ToLower(owner+"/"+name) {
			done(nil, rpc.Fail("unknown_run"))
			return
		}
		if !RunStatusAllows(intent.Action, run.Status) {
			done(nil, rpc.Fail("wrong_state"))
			return
		}
		switch intent.Action {
		case actionApprove:
			q.approveRun(intent.Run, func(r opResult) {
				if !r.ok {
					code := r.error
					if code == "" {
						code = "not_started"
					}
					done(nil, rpc.Fail(code))
					return
				}
				reply(fmt.Sprintf("Approved run #%d.", intent.Run))
			})
		case actionReject:
			if !q.rejectRun(intent.Run) {
				done(nil, rpc.Fail("wrong_state"))
				return
			}
			reply(fmt.Sprintf("Rejected run #%d.", intent.Run))
		case actionRerun:
			q.rerunRun(intent.Run, func(r opResult) {
				if !r.ok {
					done(nil, rpc.Fail("not_started"))
					return
				}
				reply(fmt.Sprintf("Started run #%d.", r.run))
			})
		case actionStop:
			q.stopRun(intent.Run)
			reply(fmt.Sprintf("Stopping run #%d.", intent.Run))
		case actionSkip:
			q.skipRun(intent.Run)
			reply(fmt.Sprintf("Skipped run #%d.", intent.Run))
		default:
			done(nil, rpc.Fail("unsupported_action"))
		}
	})
	if err != nil {
		return nil, err
	}
	out, _ := value.(map[string]any)
	return out, nil
}

func (m *Module) setWorkflowDisabled(owner, name, path string, disabled bool) error {
	if m.SetWorkflowDisabled != nil {
		return m.SetWorkflowDisabled(owner, name, path, disabled)
	}
	if m.core == nil {
		return errDesktopUnavailable
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	_, err := m.core.CallUI(ctx, "ui.actions.workflowDisabled", map[string]any{"owner": owner, "name": name, "path": path, "disabled": disabled})
	if err != nil {
		if uiUnavailable(err) || errors.Is(err, context.DeadlineExceeded) {
			return errDesktopUnavailable
		}
		return err
	}
	return nil
}
