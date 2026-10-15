package hosts

import (
	"context"
	"sort"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	transcriptCap   = 1 << 20
	maxFinishedJobs = 50
	installChannel  = "install"
	deployPrefix    = "deploy:"
)

type transcript struct {
	text  []byte
	start int64
	epoch int64
}

func (t *transcript) end() int64 { return t.start + int64(len(t.text)) }

type Job struct {
	ID       string         `json:"id"`
	Kind     string         `json:"kind"`
	Host     string         `json:"host"`
	Subject  string         `json:"subject,omitempty"`
	Origin   string         `json:"origin"`
	Channel  string         `json:"channel"`
	Started  int64          `json:"startedAt"`
	Finished int64          `json:"finishedAt,omitempty"`
	Running  bool           `json:"running"`
	OK       bool           `json:"ok"`
	Message  string         `json:"message,omitempty"`
	Result   map[string]any `json:"result,omitempty"`

	ctx    context.Context
	cancel context.CancelFunc
	done   chan struct{}
}

type jobTable struct {
	mu          sync.Mutex
	jobs        map[string]*Job
	finished    []string
	channels    map[string]*transcript
	statuses    map[string]string
	installBusy int
}

func newJobTable() *jobTable {
	return &jobTable{jobs: map[string]*Job{}, channels: map[string]*transcript{}, statuses: map[string]string{}}
}

func (m *Module) table() *jobTable {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.jobs == nil {
		m.jobs = newJobTable()
	}
	return m.jobs
}

func (m *Module) appendOutput(channel, text string) {
	if text == "" {
		return
	}
	t := m.table()
	t.mu.Lock()
	tr := t.channels[channel]
	if tr == nil {
		tr = &transcript{}
		t.channels[channel] = tr
	}
	offset := tr.end()
	tr.text = append(tr.text, text...)
	if len(tr.text) > transcriptCap {
		drop := len(tr.text) - transcriptCap
		tr.text = append([]byte(nil), tr.text[drop:]...)
		tr.start += int64(drop)
	}
	epoch := tr.epoch
	t.mu.Unlock()
	m.broadcastUI("core.hosts.output", map[string]any{"channel": channel, "offset": offset, "epoch": epoch, "text": text})
	if channel == installChannel {
		m.installLogChanged()
	}
}

func (m *Module) clearOutput(channel string) {
	t := m.table()
	t.mu.Lock()
	tr := t.channels[channel]
	if tr == nil {
		tr = &transcript{}
		t.channels[channel] = tr
	}
	tr.start = tr.end()
	tr.text = nil
	tr.epoch++
	offset, epoch := tr.start, tr.epoch
	t.mu.Unlock()
	m.broadcastUI("core.hosts.output", map[string]any{"channel": channel, "offset": offset, "epoch": epoch, "text": "", "clear": true})
}

func (m *Module) transcriptText(channel string) string {
	t := m.table()
	t.mu.Lock()
	defer t.mu.Unlock()
	if tr := t.channels[channel]; tr != nil {
		return string(tr.text)
	}
	return ""
}

func (m *Module) setStatus(target, text string) {
	t := m.table()
	t.mu.Lock()
	t.statuses[target] = text
	t.mu.Unlock()
	m.broadcastUI("core.hosts.status", map[string]any{"target": target, "text": text})
}

func (m *Module) status(target string) string {
	t := m.table()
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.statuses[target]
}

func (m *Module) startJob(kind, host, subject, origin, channel string) *Job {
	ctx, cancel := context.WithCancel(m.ctx)
	job := &Job{ID: newStreamID(), Kind: kind, Host: host, Subject: subject, Origin: origin, Channel: channel,
		Started: m.nowMs(), Running: true, ctx: ctx, cancel: cancel, done: make(chan struct{})}
	t := m.table()
	t.mu.Lock()
	t.jobs[job.ID] = job
	snapshot := job.snapshot()
	t.mu.Unlock()
	m.mu.Lock()
	m.active++
	m.mu.Unlock()
	m.broadcastUI("core.hosts.job", snapshot)
	return job
}

func (j *Job) snapshot() Job {
	out := *j
	out.ctx = nil
	out.cancel = nil
	out.done = nil
	if j.Result != nil {
		out.Result = map[string]any{}
		for k, v := range j.Result {
			out.Result[k] = v
		}
	}
	return out
}

func (j *Job) Context() context.Context { return j.ctx }

func (m *Module) finishJob(job *Job, ok bool, message string, result map[string]any) {
	t := m.table()
	t.mu.Lock()
	if !job.Running {
		t.mu.Unlock()
		return
	}
	job.Running = false
	job.OK = ok
	job.Message = message
	if result != nil {
		job.Result = result
	}
	job.Finished = m.nowMs()
	cancel := job.cancel
	job.cancel = nil
	close(job.done)
	t.finished = append(t.finished, job.ID)
	for len(t.finished) > maxFinishedJobs {
		delete(t.jobs, t.finished[0])
		t.finished = t.finished[1:]
	}
	snapshot := job.snapshot()
	t.mu.Unlock()
	if cancel != nil {
		cancel()
	}
	m.mu.Lock()
	m.active--
	m.mu.Unlock()
	m.broadcastUI("core.hosts.job", snapshot)
}

func (m *Module) updateJob(job *Job, mutate func(*Job)) {
	t := m.table()
	t.mu.Lock()
	mutate(job)
	snapshot := job.snapshot()
	t.mu.Unlock()
	m.broadcastUI("core.hosts.job", snapshot)
}

func (m *Module) runningJobs(kinds ...string) []*Job {
	t := m.table()
	t.mu.Lock()
	defer t.mu.Unlock()
	out := []*Job{}
	for _, job := range t.jobs {
		if !job.Running {
			continue
		}
		if len(kinds) == 0 {
			out = append(out, job)
			continue
		}
		for _, kind := range kinds {
			if job.Kind == kind {
				out = append(out, job)
				break
			}
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Started < out[j].Started })
	return out
}

func (m *Module) jobRunning(kinds ...string) bool { return len(m.runningJobs(kinds...)) > 0 }

func (m *Module) jobList() []Job {
	t := m.table()
	t.mu.Lock()
	defer t.mu.Unlock()
	out := make([]Job, 0, len(t.jobs))
	for _, job := range t.jobs {
		out = append(out, job.snapshot())
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Started != out[j].Started {
			return out[i].Started < out[j].Started
		}
		return out[i].ID < out[j].ID
	})
	return out
}

func (m *Module) handleJobs(ctx context.Context, req *rpc.Request) (any, error) {
	t := m.table()
	t.mu.Lock()
	channels := map[string]any{}
	for name, tr := range t.channels {
		channels[name] = map[string]any{"start": tr.start, "end": tr.end(), "epoch": tr.epoch}
	}
	statuses := map[string]string{}
	for k, v := range t.statuses {
		statuses[k] = v
	}
	t.mu.Unlock()
	return map[string]any{"jobs": m.jobList(), "channels": channels, "statuses": statuses, "busy": m.installBusy(), "provision": m.provisionSnapshot(), "deploy": m.deploySnapshot()}, nil
}

func (m *Module) handleTranscript(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Channel string `json:"channel"`
		From    int64  `json:"from"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	t := m.table()
	t.mu.Lock()
	defer t.mu.Unlock()
	tr := t.channels[params.Channel]
	if tr == nil {
		return map[string]any{"channel": params.Channel, "offset": 0, "end": 0, "epoch": 0, "text": ""}, nil
	}
	from := params.From
	if from < tr.start {
		from = tr.start
	}
	if from > tr.end() {
		from = tr.end()
	}
	return map[string]any{"channel": params.Channel, "offset": from, "end": tr.end(), "epoch": tr.epoch,
		"text": string(tr.text[from-tr.start:])}, nil
}

func (m *Module) handleJobCancel(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		ID string `json:"id"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	t := m.table()
	t.mu.Lock()
	job := t.jobs[params.ID]
	var cancel context.CancelFunc
	if job != nil && job.Running {
		cancel = job.cancel
	}
	t.mu.Unlock()
	if job == nil {
		return nil, rpc.Fail("unknown_job")
	}
	if cancel != nil {
		cancel()
	}
	return map[string]any{"cancelled": cancel != nil}, nil
}

func (m *Module) waitJob(job *Job, timeout time.Duration) bool {
	select {
	case <-job.done:
		return true
	case <-time.After(timeout):
		return false
	}
}

func isoTimestamp(t time.Time) string { return t.Format("2006-01-02T15:04:05") }

func (m *Module) timestamped(message string) string {
	return "[" + isoTimestamp(m.now()) + "] " + message
}

func trimmedLower(text string) string { return strings.ToLower(strings.TrimSpace(text)) }

func (m *Module) registerJobMethods(desktop func(string, rpc.Handler)) {
	m.registerInstallMethods(desktop)
	m.registerProvisionMethods(desktop)
	m.registerAgentMethods(desktop)
	m.registerFeedMethods(desktop)
}

func (m *Module) startExtensions(ctx context.Context) {
	m.startProvision(ctx)
	m.startFeeds(ctx)
}
