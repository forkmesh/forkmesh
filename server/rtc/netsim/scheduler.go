package netsim

import (
	"container/heap"
	"sync"
	"time"
)

type event struct {
	at  time.Time
	seq uint64
	fn  func()
}

type eventHeap []event

func (h eventHeap) Len() int { return len(h) }

func (h eventHeap) Less(i, j int) bool {
	if h[i].at.Equal(h[j].at) {
		return h[i].seq < h[j].seq
	}
	return h[i].at.Before(h[j].at)
}

func (h eventHeap) Swap(i, j int) { h[i], h[j] = h[j], h[i] }

func (h *eventHeap) Push(x any) { *h = append(*h, x.(event)) }

func (h *eventHeap) Pop() any {
	old := *h
	last := old[len(old)-1]
	*h = old[:len(old)-1]
	return last
}

type scheduler struct {
	mu     sync.Mutex
	events eventHeap
	seq    uint64
	wake   chan struct{}
	done   chan struct{}
	once   sync.Once
	exited chan struct{}
}

func newScheduler() *scheduler {
	s := &scheduler{wake: make(chan struct{}, 1), done: make(chan struct{}), exited: make(chan struct{})}
	go s.run()
	return s
}

func (s *scheduler) at(when time.Time, fn func()) {
	s.mu.Lock()
	heap.Push(&s.events, event{at: when, seq: s.seq, fn: fn})
	first := s.events[0].seq == s.seq
	s.seq++
	s.mu.Unlock()
	if first {
		select {
		case s.wake <- struct{}{}:
		default:
		}
	}
}

func (s *scheduler) pending() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.events)
}

func (s *scheduler) stop() {
	s.once.Do(func() { close(s.done) })
	<-s.exited
}

func (s *scheduler) run() {
	defer close(s.exited)
	timer := time.NewTimer(time.Hour)
	defer timer.Stop()
	var due []func()
	for {
		s.mu.Lock()
		now := time.Now()
		due = due[:0]
		for len(s.events) > 0 && !s.events[0].at.After(now) {
			due = append(due, heap.Pop(&s.events).(event).fn)
		}
		wait := time.Hour
		if len(s.events) > 0 {
			wait = s.events[0].at.Sub(now)
		}
		s.mu.Unlock()
		for _, fn := range due {
			fn()
		}
		if len(due) > 0 {
			continue
		}
		timer.Reset(wait)
		select {
		case <-s.done:
			return
		case <-s.wake:
		case <-timer.C:
		}
	}
}
