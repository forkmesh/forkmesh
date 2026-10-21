package fmmserver

import (
	"context"
	"errors"
	"sync"
	"sync/atomic"
	"time"

	"forkmesh.com/mirror-node/rtc/fmm/wire"
	"forkmesh.com/mirror-node/rtc/quic"
	"forkmesh.com/mirror-node/rtc/webtransport"
)

var errControlFull = errors.New("fmmserver: control queue full")

type link interface {
	acceptStream(ctx context.Context) (*quic.Stream, error)
	receive(ctx context.Context) ([]byte, error)
	send(b []byte, audio bool) error
	maxDatagram() int
	close(code uint32, reason string)
	waitClose(d time.Duration)
	done() <-chan struct{}
}

type quicLink struct {
	conn *quic.Conn

	mu        sync.Mutex
	checked   time.Time
	redundant bool
}

func newQUICLink(c *quic.Conn) *quicLink { return &quicLink{conn: c} }

func (l *quicLink) acceptStream(ctx context.Context) (*quic.Stream, error) {
	return l.conn.AcceptStream(ctx)
}

func (l *quicLink) receive(ctx context.Context) ([]byte, error) { return l.conn.ReceiveDatagram(ctx) }

func (l *quicLink) multipath() bool {
	if !l.conn.MultipathEnabled() {
		return false
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	if now := time.Now(); now.Sub(l.checked) >= pathCheckEvery {
		l.checked = now
		l.redundant = l.conn.Health().UsablePaths > 1
	}
	return l.redundant
}

func (l *quicLink) send(b []byte, audio bool) error {
	if audio && l.multipath() {
		return l.conn.SendDatagramWith(b, quic.Redundant)
	}
	return l.conn.SendDatagram(b)
}

func (l *quicLink) maxDatagram() int { return l.conn.MaxDatagramSize() }

func (l *quicLink) close(code uint32, reason string) {
	l.conn.CloseWithError(uint64(code), reason)
}

func (l *quicLink) waitClose(d time.Duration) {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-l.conn.Done():
	case <-t.C:
	}
}

func (l *quicLink) done() <-chan struct{} { return l.conn.Done() }

type wtLink struct {
	sess *webtransport.Session
}

func (l *wtLink) acceptStream(ctx context.Context) (*quic.Stream, error) {
	return l.sess.AcceptStream(ctx)
}

func (l *wtLink) receive(ctx context.Context) ([]byte, error) { return l.sess.ReceiveDatagram(ctx) }

func (l *wtLink) send(b []byte, audio bool) error { return l.sess.SendDatagram(b) }

func (l *wtLink) maxDatagram() int { return l.sess.MaxDatagramSize() }

func (l *wtLink) close(code uint32, reason string) {
	_ = l.sess.CloseWithError(code, reason)
	_ = l.sess.Conn().CloseWithError(0, reason)
}

func (l *wtLink) waitClose(d time.Duration) {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-l.sess.Done():
	case <-t.C:
	}
}

func (l *wtLink) done() <-chan struct{} { return l.sess.Done() }

type controlWriter struct {
	st   *quic.Stream
	link link

	mu      sync.Mutex
	queue   [][]byte
	bytes   int
	closed  bool
	broken  bool
	wake    chan struct{}
	idle    chan struct{}
	stopped chan struct{}
}

func newControlWriter(st *quic.Stream, l link) *controlWriter {
	w := &controlWriter{st: st, link: l, wake: make(chan struct{}, 1), stopped: make(chan struct{})}
	go w.run()
	return w
}

func (w *controlWriter) SendControl(p []byte) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.closed || w.broken {
		return ErrClosed
	}
	if w.bytes+len(p) > controlBytes {
		w.broken = true
		go w.link.close(0x12, "control backlog")
		w.signal()
		return errControlFull
	}
	w.queue = append(w.queue, append([]byte(nil), p...))
	w.bytes += len(p)
	w.signal()
	return nil
}

func (w *controlWriter) signal() {
	select {
	case w.wake <- struct{}{}:
	default:
	}
}

func (w *controlWriter) run() {
	defer close(w.stopped)
	for {
		w.mu.Lock()
		batch := w.queue
		w.queue, w.bytes = nil, 0
		done := w.closed || w.broken
		idle := w.idle
		if len(batch) == 0 && idle != nil {
			close(idle)
			w.idle = nil
		}
		w.mu.Unlock()
		for _, b := range batch {
			if _, err := w.st.Write(b); err != nil {
				w.mu.Lock()
				w.broken = true
				w.mu.Unlock()
				return
			}
		}
		if len(batch) > 0 {
			continue
		}
		if done {
			return
		}
		select {
		case <-w.wake:
		case <-w.link.done():
			return
		}
	}
}

func (w *controlWriter) flush(d time.Duration) {
	w.mu.Lock()
	if w.broken || w.closed {
		w.mu.Unlock()
		return
	}
	if w.idle == nil {
		w.idle = make(chan struct{})
	}
	idle := w.idle
	w.signal()
	w.mu.Unlock()
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-idle:
	case <-w.stopped:
	case <-t.C:
	}
}

func (w *controlWriter) close() {
	w.mu.Lock()
	w.closed = true
	w.signal()
	w.mu.Unlock()
}

type transport struct {
	link  link
	ctl   *controlWriter
	audio [256]atomic.Bool
}

func (t *transport) SendDatagram(p []byte) error {
	audio := false
	if len(p) > 1 {
		kind := p[0] & 0x0F
		audio = (kind == wire.KindMedia || kind == wire.KindRepair) && t.audio[p[1]].Load()
	}
	return t.link.send(p, audio)
}

func (t *transport) MaxDatagramSize() int { return t.link.maxDatagram() }

func (t *transport) SendControl(p []byte) error {
	if len(p) > 0 && (p[0] == wire.MsgTrackAdded || p[0] == wire.MsgTrackRemoved) {
		if m, _, err := wire.Decode(p); err == nil {
			switch v := m.(type) {
			case wire.TrackAdded:
				t.audio[v.Stream].Store(v.Kind == wire.TrackAudio)
			case wire.TrackRemoved:
				t.audio[v.Stream].Store(false)
			}
		}
	}
	return t.ctl.SendControl(p)
}
