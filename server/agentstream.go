package mirrornode

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"strconv"
	"sync"
	"time"
	"unicode/utf8"
)

const (
	agentStreamRunnerPath   = "/api/runners/stream"
	agentStreamChunk        = 8192
	agentStreamTick         = time.Second
	agentStreamHeartbeat    = 5 * time.Second
	agentStreamBacklog      = 32 * agentStreamChunk
	agentStreamEventLimit   = 32 << 10
	agentStreamFieldLimit   = 4 << 10
	agentStreamPostsPerTick = 8
	agentStreamFinalWait    = 5 * time.Second
)

type agentStreamPoster interface {
	postStream(ctx context.Context, work agentWork, body []byte) (int, []byte, error)
}

type agentStreamTapKey struct{}

func withAgentStreamTap(ctx context.Context, tap func([]byte)) context.Context {
	if tap == nil {
		return ctx
	}
	return context.WithValue(ctx, agentStreamTapKey{}, tap)
}

func agentStreamTapFrom(ctx context.Context) func([]byte) {
	tap, _ := ctx.Value(agentStreamTapKey{}).(func([]byte))
	return tap
}

type agentStream struct {
	post      func(ctx context.Context, fields map[string]any) (int, []byte, error)
	tick      time.Duration
	heartbeat time.Duration
	now       func() time.Time

	mu       sync.Mutex
	pending  []byte
	midLine  bool
	broken   bool
	unsent   []byte
	seq      int64
	watched  bool
	disabled bool
	lastPost time.Time
	wake     chan struct{}
}

func newAgentStream(post func(context.Context, map[string]any) (int, []byte, error)) *agentStream {
	return &agentStream{post: post, tick: agentStreamTick, heartbeat: agentStreamHeartbeat,
		now: time.Now, wake: make(chan struct{}, 1)}
}

func (s *agentStream) line(raw []byte) {
	event := scrubTokens(compactStreamEvent(bytes.TrimSpace(raw)))
	if len(event) == 0 {
		return
	}
	s.mu.Lock()
	if s.disabled {
		s.mu.Unlock()
		return
	}
	s.pending = append(s.pending, event...)
	s.pending = append(s.pending, '\n')
	s.trimLocked()
	ready := s.watched && len(s.pending) >= agentStreamChunk
	s.mu.Unlock()
	if ready {
		select {
		case s.wake <- struct{}{}:
		default:
		}
	}
}

func (s *agentStream) trimLocked() {
	dropped := false
	for len(s.pending) > agentStreamBacklog {
		cut := bytes.IndexByte(s.pending, '\n')
		if cut < 0 {
			s.pending = s.pending[:0]
		} else {
			s.pending = s.pending[cut+1:]
		}
		dropped = true
	}
	if dropped && s.midLine {
		s.midLine, s.broken = false, true
	}
}

func (s *agentStream) flush(ctx context.Context, final bool) {
	for posts := 0; posts < agentStreamPostsPerTick && ctx.Err() == nil; posts++ {
		s.mu.Lock()
		if s.disabled {
			s.mu.Unlock()
			return
		}
		fields := map[string]any{"seq": int64(0)}
		if s.watched {
			if s.unsent == nil && len(s.pending) > 0 {
				if s.broken {
					s.pending = append([]byte{'\n'}, s.pending...)
					s.broken = false
				}
				size := min(len(s.pending), agentStreamChunk)
				s.unsent = append([]byte(nil), s.pending[:size]...)
				s.pending = s.pending[size:]
				s.midLine = s.unsent[size-1] != '\n'
				s.seq++
			}
			if s.unsent != nil {
				fields["seq"] = s.seq
				fields["data"] = base64.StdEncoding.EncodeToString(s.unsent)
			}
		}
		sending := fields["data"] != nil
		due := sending || (!final && s.now().Sub(s.lastPost) >= s.heartbeat)
		s.mu.Unlock()
		if !due {
			return
		}
		status, reply, err := s.post(ctx, fields)
		s.mu.Lock()
		s.lastPost = s.now()
		if err != nil || status >= 500 || status == 429 {
			s.mu.Unlock()
			return
		}
		if status < 200 || status >= 300 {
			s.disabled, s.pending, s.unsent = true, nil, nil
			s.mu.Unlock()
			return
		}
		var parsed struct {
			Watched bool `json:"watched"`
		}
		_ = json.Unmarshal(reply, &parsed)
		if sending {
			s.unsent = nil
		}
		s.watched = parsed.Watched
		more := s.watched && (s.unsent != nil || len(s.pending) > 0) &&
			(final || !sending || len(s.pending) >= agentStreamChunk)
		s.mu.Unlock()
		if !more {
			return
		}
	}
}

func (s *agentStream) run(ctx context.Context, stop <-chan struct{}) {
	ticker := time.NewTicker(s.tick)
	defer ticker.Stop()
	s.flush(ctx, false)
	for {
		select {
		case <-ctx.Done():
			return
		case <-stop:
			finalCtx, cancel := context.WithTimeout(context.Background(), agentStreamFinalWait)
			s.flush(finalCtx, true)
			cancel()
			return
		case <-ticker.C:
		case <-s.wake:
		}
		s.flush(ctx, false)
	}
}

func compactStreamEvent(raw []byte) []byte {
	if len(raw) == 0 || raw[0] != '{' {
		return nil
	}
	if len(raw) <= agentStreamEventLimit {
		if !json.Valid(raw) {
			return nil
		}
		return raw
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	var value any
	if decoder.Decode(&value) != nil {
		return nil
	}
	var buffer bytes.Buffer
	encoder := json.NewEncoder(&buffer)
	encoder.SetEscapeHTML(false)
	if encoder.Encode(shortenStreamStrings(value)) != nil {
		return nil
	}
	out := bytes.TrimSpace(buffer.Bytes())
	if len(out) > agentStreamEventLimit {
		return nil
	}
	return out
}

func shortenStreamStrings(value any) any {
	switch typed := value.(type) {
	case string:
		if len(typed) <= agentStreamFieldLimit {
			return typed
		}
		cut := agentStreamFieldLimit
		for cut > 0 && !utf8.RuneStart(typed[cut]) {
			cut--
		}
		return typed[:cut] + "…"
	case map[string]any:
		for key, item := range typed {
			typed[key] = shortenStreamStrings(item)
		}
	case []any:
		for index, item := range typed {
			typed[index] = shortenStreamStrings(item)
		}
	}
	return value
}

func (r *agentJobRunner) jobLease(job agentJob) string {
	r.mu.Lock()
	defer r.mu.Unlock()
	if record := r.state.Jobs[strconv.FormatInt(job.JobID, 10)]; record != nil && record.LeaseID != "" {
		return record.LeaseID
	}
	return job.LeaseID
}

func (r *agentJobRunner) startStream(ctx context.Context, work agentWork) (func([]byte), func()) {
	poster, ok := r.be().(agentStreamPoster)
	if !ok || work.job.JobID <= 0 {
		return nil, func() {}
	}
	stream := newAgentStream(func(ctx context.Context, fields map[string]any) (int, []byte, error) {
		fields["jobId"] = work.job.JobID
		fields["leaseId"] = r.jobLease(work.job)
		body, err := json.Marshal(fields)
		if err != nil {
			return 0, nil, err
		}
		return poster.postStream(ctx, work, body)
	})
	if r.streamTick > 0 {
		stream.tick = r.streamTick
	}
	stop, done := make(chan struct{}), make(chan struct{})
	go func() {
		defer close(done)
		stream.run(ctx, stop)
	}()
	return stream.line, func() {
		close(stop)
		<-done
	}
}

func (b *runnerBackend) postStream(ctx context.Context, _ agentWork, body []byte) (int, []byte, error) {
	return b.signedPost(ctx, agentStreamRunnerPath, body)
}

func (m mirrorAgentBackend) postStream(ctx context.Context, work agentWork, body []byte) (int, []byte, error) {
	return m.r.postMirrorJob(ctx, work.repo.Name, work.job.JobID, "stream", body)
}
