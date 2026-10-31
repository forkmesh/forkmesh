package quic

import (
	"context"
	"sync"
	"time"
)

type StandbyOptions struct {
	KeepAlive     time.Duration
	DeadAfter     time.Duration
	MinDeadAfter  time.Duration
	DeadPTOs      int
	Poll          time.Duration
	RedialBackoff time.Duration
	MaxBackoff    time.Duration
}

type DialFunc func(ctx context.Context) (*Conn, error)

type DeadReason uint8

const (
	DeadNone DeadReason = iota
	DeadClosed
	DeadPTO
	DeadSilent
)

func (r DeadReason) String() string {
	switch r {
	case DeadClosed:
		return "closed"
	case DeadPTO:
		return "probe timeouts"
	case DeadSilent:
		return "no acknowledgement"
	}
	return "alive"
}

type Standby struct {
	opts StandbyOptions
	dial DialFunc

	mu         sync.Mutex
	primary    *Conn
	standby    *Conn
	dead       chan struct{}
	deadAt     time.Time
	deadReason DeadReason
	ready      chan struct{}
	redial     chan struct{}
	dialErr    error

	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

func (o StandbyOptions) withDefaults() StandbyOptions {
	if o.KeepAlive <= 0 {
		o.KeepAlive = time.Second
	}
	if o.DeadAfter <= 0 {
		o.DeadAfter = 250 * time.Millisecond
	}
	if o.MinDeadAfter <= 0 {
		o.MinDeadAfter = 60 * time.Millisecond
	}
	if o.MinDeadAfter > o.DeadAfter {
		o.MinDeadAfter = o.DeadAfter
	}
	if o.DeadPTOs <= 0 {
		o.DeadPTOs = 2
	}
	if o.Poll <= 0 {
		o.Poll = 5 * time.Millisecond
	}
	if o.RedialBackoff <= 0 {
		o.RedialBackoff = 250 * time.Millisecond
	}
	if o.MaxBackoff < o.RedialBackoff {
		o.MaxBackoff = 5 * time.Second
	}
	return o
}

func NewStandby(primary *Conn, dial DialFunc, opts StandbyOptions) *Standby {
	ctx, cancel := context.WithCancel(context.Background())
	s := &Standby{
		opts:    opts.withDefaults(),
		dial:    dial,
		primary: primary,
		dead:    make(chan struct{}),
		ready:   make(chan struct{}),
		redial:  make(chan struct{}, 1),
		ctx:     ctx,
		cancel:  cancel,
	}
	s.wg.Add(2)
	go s.monitor()
	go s.maintain()
	return s
}

func (s *Standby) Primary() *Conn {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.primary
}

func (s *Standby) PrimaryDead() <-chan struct{} {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.dead
}

func (s *Standby) DeadSince() (time.Time, DeadReason) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.deadAt, s.deadReason
}

func (s *Standby) Ready() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.standby != nil
}

func (s *Standby) Conn(ctx context.Context) (*Conn, error) {
	for {
		s.mu.Lock()
		c, ready := s.standby, s.ready
		s.mu.Unlock()
		if c != nil {
			return c, nil
		}
		select {
		case <-ready:
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-s.ctx.Done():
			return nil, ErrClosed
		}
	}
}

func (s *Standby) Promote(ctx context.Context) (*Conn, error) {
	c, err := s.Conn(ctx)
	if err != nil {
		return nil, err
	}
	s.mu.Lock()
	if s.standby != c {
		s.mu.Unlock()
		return s.Promote(ctx)
	}
	s.primary = c
	s.standby = nil
	s.ready = make(chan struct{})
	s.dead = make(chan struct{})
	s.deadAt, s.deadReason = time.Time{}, DeadNone
	s.mu.Unlock()
	s.kick()
	return c, nil
}

func (s *Standby) Close() error {
	s.cancel()
	s.wg.Wait()
	s.mu.Lock()
	c := s.standby
	s.standby = nil
	s.mu.Unlock()
	if c != nil {
		c.CloseWithError(0, "standby closed")
	}
	return nil
}

func (s *Standby) kick() {
	select {
	case s.redial <- struct{}{}:
	default:
	}
}

func (s *Standby) monitor() {
	defer s.wg.Done()
	t := time.NewTicker(s.opts.Poll)
	defer t.Stop()
	for {
		select {
		case <-s.ctx.Done():
			return
		case <-t.C:
		}
		s.mu.Lock()
		primary, already := s.primary, !s.deadAt.IsZero()
		s.mu.Unlock()
		if primary == nil || already {
			continue
		}
		now := time.Now()
		reason := s.judge(primary.Health(), now)
		if reason == DeadNone {
			continue
		}
		s.mu.Lock()
		if s.primary == primary && s.deadAt.IsZero() {
			s.deadAt, s.deadReason = now, reason
			close(s.dead)
		}
		s.mu.Unlock()
	}
}

func (s *Standby) judge(h Health, now time.Time) DeadReason {
	if h.Closed {
		return DeadClosed
	}
	if h.UsablePaths == 0 {
		return DeadClosed
	}
	if h.PTOCount >= s.opts.DeadPTOs {
		return DeadPTO
	}
	if h.OldestUnacked.IsZero() {
		return DeadNone
	}
	limit := 2 * (h.SmoothedRTT + 4*h.RTTVariance + h.MaxAckDelay)
	if limit < s.opts.MinDeadAfter {
		limit = s.opts.MinDeadAfter
	}
	if limit > s.opts.DeadAfter {
		limit = s.opts.DeadAfter
	}
	if now.Sub(h.OldestUnacked) > limit && now.Sub(h.LastAcked) > limit/2 {
		return DeadSilent
	}
	return DeadNone
}

func (s *Standby) maintain() {
	defer s.wg.Done()
	backoff := s.opts.RedialBackoff
	for {
		if s.ctx.Err() != nil {
			return
		}
		s.mu.Lock()
		c := s.standby
		s.mu.Unlock()
		if c == nil {
			nc, err := s.dial(s.ctx)
			if err != nil {
				s.mu.Lock()
				s.dialErr = err
				s.mu.Unlock()
				if !s.sleep(backoff) {
					return
				}
				backoff *= 2
				if backoff > s.opts.MaxBackoff {
					backoff = s.opts.MaxBackoff
				}
				continue
			}
			backoff = s.opts.RedialBackoff
			s.mu.Lock()
			s.standby = nc
			s.dialErr = nil
			close(s.ready)
			s.mu.Unlock()
			c = nc
		}
		t := time.NewTimer(s.opts.KeepAlive)
		select {
		case <-s.ctx.Done():
			t.Stop()
			return
		case <-c.Done():
			t.Stop()
			s.mu.Lock()
			if s.standby == c {
				s.standby = nil
				s.ready = make(chan struct{})
			}
			s.mu.Unlock()
		case <-s.redial:
			t.Stop()
		case <-t.C:
			c.ping()
		}
	}
}

func (s *Standby) sleep(d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-s.ctx.Done():
		return false
	case <-t.C:
		return true
	case <-s.redial:
		return true
	}
}

func (s *Standby) LastDialError() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.dialErr
}
