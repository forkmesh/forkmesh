package mirrornode

import (
	"context"
	"encoding/json"
	"strconv"
	"sync"
	"time"
)

var (
	agentLiveReportInterval  = 2500 * time.Millisecond
	agentLiveWatchedInterval = 250 * time.Millisecond
	agentLiveReportTimeout   = 10 * time.Second
)

type agentLiveReporter struct {
	mu      sync.Mutex
	text    string
	sent    string
	wake    chan struct{}
	done    chan struct{}
	stopped chan struct{}
}

func newAgentLiveReporter(ctx context.Context, send func(context.Context, string) bool) *agentLiveReporter {
	l := &agentLiveReporter{
		wake:    make(chan struct{}, 1),
		done:    make(chan struct{}),
		stopped: make(chan struct{}),
	}
	go l.loop(ctx, send)
	return l
}

func (l *agentLiveReporter) update(text string) {
	l.mu.Lock()
	l.text = text
	l.mu.Unlock()
	select {
	case l.wake <- struct{}{}:
	default:
	}
}

func (l *agentLiveReporter) stop() {
	close(l.done)
	<-l.stopped
}

func (l *agentLiveReporter) loop(ctx context.Context, send func(context.Context, string) bool) {
	defer close(l.stopped)
	var last time.Time
	interval := agentLiveReportInterval
	for {
		select {
		case <-l.done:
			return
		case <-ctx.Done():
			return
		case <-l.wake:
		}
		if wait := interval - time.Since(last); !last.IsZero() && wait > 0 {
			timer := time.NewTimer(wait)
			select {
			case <-l.done:
				timer.Stop()
				return
			case <-ctx.Done():
				timer.Stop()
				return
			case <-timer.C:
			}
		}
		l.mu.Lock()
		text := l.text
		l.mu.Unlock()
		if text == "" || text == l.sent {
			continue
		}
		l.sent = text
		last = time.Now()
		sendCtx, cancel := context.WithTimeout(ctx, agentLiveReportTimeout)
		watched := send(sendCtx, text)
		cancel()
		interval = agentLiveReportInterval
		if watched {
			interval = agentLiveWatchedInterval
		}
	}
}

func (r *agentJobRunner) reportLive(ctx context.Context, job agentJob, transcript string) bool {
	r.mu.Lock()
	record := r.state.Jobs[strconv.FormatInt(job.JobID, 10)]
	if record == nil || record.Status != "running" || record.LeaseID == "" {
		r.mu.Unlock()
		return false
	}
	copied := *record
	r.mu.Unlock()
	body, err := json.Marshal(map[string]any{
		"leaseId":         copied.LeaseID,
		"securityVerdict": "approved",
		"status":          "running",
		"live":            true,
		"transcript":      tailRunes(transcript, agentResultLimit),
	})
	if err != nil {
		return false
	}
	status, reply, err := r.be().postReport(ctx, copied, body)
	if err != nil || status < 200 || status >= 300 {
		return false
	}
	var answer struct {
		Watched bool `json:"watched"`
	}
	return json.Unmarshal(reply, &answer) == nil && answer.Watched
}
