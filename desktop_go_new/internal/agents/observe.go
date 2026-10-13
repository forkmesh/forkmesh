package agents

import "sync"

type ChangeKind int

const (
	ChangeRecord ChangeKind = iota + 1
	ChangeRemoved
	ChangeRestored
	ChangeBin
	ChangeLog
	ChangeEvent
)

type Change struct {
	Kind ChangeKind
	ID   int
}

type observerList struct {
	mu   sync.Mutex
	next int
	fns  map[int]func(Change)
}

func (l *observerList) add(fn func(Change)) func() {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.fns == nil {
		l.fns = map[int]func(Change){}
	}
	l.next++
	id := l.next
	l.fns[id] = fn
	return func() {
		l.mu.Lock()
		delete(l.fns, id)
		l.mu.Unlock()
	}
}

func (l *observerList) notify(kind ChangeKind, id int) {
	l.mu.Lock()
	if len(l.fns) == 0 {
		l.mu.Unlock()
		return
	}
	fns := make([]func(Change), 0, len(l.fns))
	for _, fn := range l.fns {
		fns = append(fns, fn)
	}
	l.mu.Unlock()
	for _, fn := range fns {
		fn(Change{Kind: kind, ID: id})
	}
}

func (w *Writer) Observe(fn func(Change)) func() {
	if w == nil || fn == nil {
		return func() {}
	}
	return w.observers.add(fn)
}
