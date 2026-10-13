package control

import (
	"net"
	"sync"
	"sync/atomic"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

type phonePeer struct {
	m         *Module
	conn      *connection
	id        string
	transport rpc.Transport
	mu        sync.Mutex
	device    rpc.Device
	paired    atomic.Bool
	done      chan struct{}
	once      sync.Once
}

func (p *phonePeer) ID() string               { return p.id }
func (p *phonePeer) Transport() rpc.Transport { return p.transport }
func (p *phonePeer) Done() <-chan struct{}    { return p.done }

func (p *phonePeer) Device() rpc.Device {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.device
}

func (p *phonePeer) Connection() int { return p.conn.id }

func (p *phonePeer) setDevice(device Device) {
	p.mu.Lock()
	p.device = rpc.Device{Key: device.Key, Name: device.Name, User: device.User, Platform: device.Platform}
	p.mu.Unlock()
}

func (p *phonePeer) close() {
	p.paired.Store(false)
	p.once.Do(func() { close(p.done) })
}

func (p *phonePeer) Send(event string, data any) bool {
	if event == "" || !p.paired.Load() {
		return false
	}
	select {
	case <-p.done:
		return false
	default:
	}
	if data == nil {
		data = map[string]any{}
	}
	raw := encodeJSON(data)
	if raw == nil || string(raw) == "null" {
		raw = []byte("{}")
	}
	plain := compactEventBytes(event, raw)
	conn := p.conn
	p.m.post(func() {
		if !conn.closed && conn.state == statePaired {
			p.m.writeJSON(conn, plain)
		}
	})
	return true
}

type lanWriter struct {
	conn    net.Conn
	mu      sync.Mutex
	cond    *sync.Cond
	queue   [][]byte
	pending int64
	closing bool
	dead    bool
	drained func()
}

func newLANWriter(conn net.Conn, drained func()) *lanWriter {
	writer := &lanWriter{conn: conn, drained: drained}
	writer.cond = sync.NewCond(&writer.mu)
	return writer
}

func (w *lanWriter) open() bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	return !w.closing && !w.dead
}

func (w *lanWriter) buffered() int64 {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.pending
}

func (w *lanWriter) write(frame []byte) bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.closing || w.dead {
		return false
	}
	w.queue = append(w.queue, frame)
	w.pending += int64(len(frame))
	w.cond.Signal()
	return true
}

func (w *lanWriter) finish() {
	w.mu.Lock()
	w.closing = true
	w.cond.Signal()
	w.mu.Unlock()
	_ = w.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
}

func (w *lanWriter) run() {
	for {
		w.mu.Lock()
		for len(w.queue) == 0 && !w.closing {
			w.cond.Wait()
		}
		if len(w.queue) == 0 {
			w.mu.Unlock()
			w.conn.Close()
			return
		}
		batch := net.Buffers(w.queue)
		w.queue = nil
		closing := w.closing
		w.mu.Unlock()
		total := int64(0)
		for _, frame := range batch {
			total += int64(len(frame))
		}
		_, err := batch.WriteTo(w.conn)
		w.mu.Lock()
		w.pending -= total
		if err != nil {
			w.dead = true
			w.queue = nil
			w.pending = 0
		}
		w.mu.Unlock()
		if err != nil {
			w.conn.Close()
			return
		}
		if w.drained != nil && !closing {
			w.drained()
		}
	}
}
