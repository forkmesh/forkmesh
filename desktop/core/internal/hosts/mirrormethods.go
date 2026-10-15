package hosts

import (
	"context"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

type mirrorRequest struct {
	Repo               mirrorRepo `json:"repo"`
	UserInitiated      bool       `json:"userInitiated"`
	ReleaseTag         string     `json:"releaseTag"`
	PruneRefs          bool       `json:"pruneRefs"`
	WakeIfUnchanged    *bool      `json:"wakeIfUnchanged"`
	ReconcileArtifacts *bool      `json:"reconcileArtifacts"`
	ArtifactHashes     []string   `json:"artifactHashes"`
	Token              string     `json:"token"`
}

func boolOr(value *bool, fallback bool) bool {
	if value == nil {
		return fallback
	}
	return *value
}

func (m *Module) registerMirrorMethods(desktop func(string, rpc.Handler)) {
	desktop("core.mirror.repos", m.handleMirrorRepos)
	desktop("core.mirror.push", m.handleMirrorPush)
	desktop("core.mirror.managed", m.handleMirrorManaged)
	desktop("core.mirror.release", m.handleMirrorRelease)
	desktop("core.mirror.unwire", m.handleMirrorUnwire)
	desktop("core.mirror.state", m.handleMirrorState)
}

func (m *Module) mirrorFinisher(token string) func(int, int) {
	if strings.TrimSpace(token) == "" {
		return nil
	}
	return func(succeeded, failed int) {
		m.broadcastUI("core.mirror.finished", map[string]any{"token": token, "succeeded": succeeded, "failed": failed})
	}
}

func (m *Module) bindMirrorRequest(req *rpc.Request) (mirrorRequest, mirrorRepo, error) {
	var params mirrorRequest
	if err := req.Bind(&params); err != nil {
		return params, mirrorRepo{}, err
	}
	repo, ok := m.mirror().noteRepo(params.Repo)
	if !ok {
		return params, repo, rpc.Fail("bad_repo")
	}
	return params, repo, nil
}

func (m *Module) handleMirrorRepos(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Repos []mirrorRepo `json:"repos"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	return map[string]any{"count": m.mirror().setRepos(params.Repos)}, nil
}

func (m *Module) handleMirrorPush(ctx context.Context, req *rpc.Request) (any, error) {
	params, repo, err := m.bindMirrorRequest(req)
	if err != nil {
		return nil, err
	}
	targets, idle := m.mirror().pushRemotes(repo, params.UserInitiated, params.ReleaseTag, params.PruneRefs)
	return map[string]any{"targets": targets, "idleReason": idle}, nil
}

func (m *Module) handleMirrorManaged(ctx context.Context, req *rpc.Request) (any, error) {
	params, repo, err := m.bindMirrorRequest(req)
	if err != nil {
		return nil, err
	}
	targets := m.mirror().pushManaged(repo, boolOr(params.WakeIfUnchanged, true), params.ReleaseTag, params.ArtifactHashes,
		m.mirrorFinisher(params.Token), boolOr(params.ReconcileArtifacts, true), params.PruneRefs)
	return map[string]any{"targets": targets}, nil
}

func (m *Module) pushRelease(repo mirrorRepo, tag string, hashes []string, onFinished func(int, int)) int {
	if strings.TrimSpace(tag) == "" || len(hashes) == 0 {
		m.logUI("%s", "Release: managed-host refs were held back because no authenticated artifact set was supplied.")
		finishLater(onFinished, 0, 1)
		return 0
	}
	return m.mirror().pushManaged(repo, true, tag, hashes, onFinished, true, false)
}

func (m *Module) handleMirrorRelease(ctx context.Context, req *rpc.Request) (any, error) {
	params, repo, err := m.bindMirrorRequest(req)
	if err != nil {
		return nil, err
	}
	return map[string]any{"targets": m.pushRelease(repo, params.ReleaseTag, params.ArtifactHashes, m.mirrorFinisher(params.Token))}, nil
}

func (m *Module) handleMirrorUnwire(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Node  string       `json:"node"`
		Repos []mirrorRepo `json:"repos"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	if params.Repos != nil {
		m.mirror().setRepos(params.Repos)
	}
	done := m.track()
	defer done()
	return map[string]any{"removed": m.mirror().unwire(ctx, params.Node)}, nil
}

func (m *Module) handleMirrorState(ctx context.Context, req *rpc.Request) (any, error) {
	engine := m.mirror()
	pushing, pending := engine.counts()
	return map[string]any{"pushing": pushing, "pending": pending, "gateways": engine.gatewayRecords(), "repos": len(engine.repoList())}, nil
}

func (m *Module) unwireNode(ctx context.Context, node string) int {
	callCtx, cancel := context.WithTimeout(ctx, unwireTimeout)
	defer cancel()
	return m.mirror().unwire(callCtx, node)
}

func (m *Module) mirrorPushFromPhone(ctx context.Context, req *rpc.Request, action, subject string) (any, error) {
	repo, ok := m.mirror().repo(subject)
	if !ok {
		return nil, rpc.Fail("unknown_repo")
	}
	key := action + ":" + subject
	remotes, idle := m.mirror().pushRemotes(repo, true, "", false)
	m.mu.Lock()
	m.fleetRunning[key] = peerID(req)
	m.mu.Unlock()
	var once sync.Once
	gate := make(chan struct{})
	managed := m.mirror().pushManaged(repo, true, "", nil, func(succeeded, failed int) {
		<-gate
		once.Do(func() {
			m.finishFleetAction(action, subject, failed == 0, "Managed hosts: "+itoa(succeeded)+" updated, "+itoa(failed)+" failed.")
		})
	}, true, false)
	if managed <= 0 {
		m.mu.Lock()
		delete(m.fleetRunning, key)
		m.mu.Unlock()
	}
	close(gate)
	if remotes <= 0 && managed <= 0 {
		if idle == "" {
			return nil, rpc.Fail("no_mirror_targets")
		}
		return nil, rpc.Fail(plainText(idle, 400))
	}
	m.logUI("Mirror push of %s requested by %s.", subject, deviceLabel(req.Peer))
	time.AfterFunc(broadcastAfterAnswer, func() { m.broadcastPhones("fleet.changed", nil) })
	return map[string]any{"started": true, "action": action, "subject": subject, "remotes": remotes, "managedHosts": max(0, managed),
		"message": "Pushing to " + itoa(max(0, remotes)) + " SSH remote(s) and " + itoa(max(0, managed)) + " managed host(s)."}, nil
}
