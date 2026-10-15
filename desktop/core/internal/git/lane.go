package git

import (
	"context"
	"sync"
	"time"
)

type lane struct {
	mu      sync.Mutex
	busy    bool
	waiters []chan struct{}
	refs    int
}

type heldLanes struct {
	parent *heldLanes
	key    string
}

type heldKey struct{}

func holding(ctx context.Context, key string) bool {
	held, _ := ctx.Value(heldKey{}).(*heldLanes)
	for ; held != nil; held = held.parent {
		if held.key == key {
			return true
		}
	}
	return false
}

func withHeld(ctx context.Context, key string) context.Context {
	parent, _ := ctx.Value(heldKey{}).(*heldLanes)
	return context.WithValue(ctx, heldKey{}, &heldLanes{parent: parent, key: key})
}

func (r *Runner) laneFor(key string) *lane {
	r.lanesMu.Lock()
	defer r.lanesMu.Unlock()
	if r.lanes == nil {
		r.lanes = map[string]*lane{}
	}
	l := r.lanes[key]
	if l == nil {
		l = &lane{}
		r.lanes[key] = l
	}
	l.refs++
	return l
}

func (r *Runner) dropLane(key string, l *lane) {
	r.lanesMu.Lock()
	defer r.lanesMu.Unlock()
	l.refs--
	if l.refs <= 0 && r.lanes[key] == l {
		delete(r.lanes, key)
	}
}

func (l *lane) acquire(ctx context.Context, wait time.Duration) (bool, error) {
	l.mu.Lock()
	if !l.busy && len(l.waiters) == 0 {
		l.busy = true
		l.mu.Unlock()
		return true, nil
	}
	ticket := make(chan struct{})
	l.waiters = append(l.waiters, ticket)
	l.mu.Unlock()
	timer := time.NewTimer(wait)
	defer timer.Stop()
	var cause error
	select {
	case <-ticket:
		return true, nil
	case <-timer.C:
	case <-ctx.Done():
		cause = ctx.Err()
	}
	l.mu.Lock()
	for i, waiting := range l.waiters {
		if waiting == ticket {
			l.waiters = append(l.waiters[:i], l.waiters[i+1:]...)
			l.mu.Unlock()
			return false, cause
		}
	}
	l.mu.Unlock()
	if cause != nil {
		l.release()
		return false, cause
	}
	return true, nil
}

func (l *lane) release() {
	l.mu.Lock()
	defer l.mu.Unlock()
	if len(l.waiters) > 0 {
		next := l.waiters[0]
		l.waiters = l.waiters[1:]
		close(next)
		return
	}
	l.busy = false
}

type Lease struct {
	runner   *Runner
	key      string
	lane     *lane
	holds    bool
	nested   bool
	bypassed bool
	once     sync.Once
}

func (l *Lease) Holds() bool    { return l != nil && (l.holds || l.nested) }
func (l *Lease) Bypassed() bool { return l != nil && l.bypassed }
func (l *Lease) Key() string {
	if l == nil {
		return ""
	}
	return l.key
}

func (l *Lease) Release() {
	if l == nil {
		return
	}
	l.once.Do(func() {
		if l.lane == nil {
			return
		}
		if l.holds {
			l.lane.release()
		}
		l.runner.dropLane(l.key, l.lane)
	})
}

func (r *Runner) Hold(ctx context.Context, dir string) (context.Context, *Lease, error) {
	return r.hold(ctx, dir, "")
}

func (r *Runner) hold(ctx context.Context, dir, what string) (context.Context, *Lease, error) {
	key := RepositoryKey(dir)
	lease := &Lease{runner: r, key: key}
	if holding(ctx, key) {
		lease.nested = true
		return ctx, lease, nil
	}
	lease.lane = r.laneFor(key)
	wait := r.QueueWait
	if wait <= 0 {
		wait = QueueWait
	}
	started := time.Now()
	acquired, err := lease.lane.acquire(ctx, wait)
	if err != nil {
		lease.Release()
		return ctx, nil, err
	}
	if !acquired {
		lease.bypassed = true
		if what == "" {
			what = "git"
		}
		r.logf("git: index queue for %s still busy after %d ms; running %s without waiting further", key, time.Since(started).Milliseconds(), what)
		return ctx, lease, nil
	}
	lease.holds = true
	return withHeld(ctx, key), lease, nil
}
