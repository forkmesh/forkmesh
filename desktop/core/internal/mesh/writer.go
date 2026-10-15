package mesh

import (
	"net"
	"sync"
	"time"
)

type linkWriter struct {
	conn    net.Conn
	onError func(error)
	mu      sync.Mutex
	cond    *sync.Cond
	queue   [][]byte
	stop    bool
	keep    bool
	dead    bool
	done    chan struct{}
}

func newLinkWriter(conn net.Conn, onError func(error)) *linkWriter {
	w := &linkWriter{conn: conn, onError: onError, done: make(chan struct{})}
	w.cond = sync.NewCond(&w.mu)
	go w.run()
	return w
}

func (w *linkWriter) write(data []byte) bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.stop || w.dead {
		return false
	}
	w.queue = append(w.queue, data)
	w.cond.Signal()
	return true
}

func (w *linkWriter) run() {
	defer close(w.done)
	for {
		w.mu.Lock()
		for len(w.queue) == 0 && !w.stop {
			w.cond.Wait()
		}
		if len(w.queue) == 0 {
			keep := w.keep
			w.mu.Unlock()
			if !keep {
				w.conn.Close()
			}
			return
		}
		chunk := w.queue[0]
		w.queue = w.queue[1:]
		stopping := w.stop
		w.mu.Unlock()
		if _, err := w.conn.Write(chunk); err != nil {
			w.mu.Lock()
			w.dead = true
			keep := w.keep
			w.queue = nil
			w.mu.Unlock()
			if !stopping && w.onError != nil {
				w.onError(err)
			}
			if !keep {
				w.conn.Close()
			}
			return
		}
	}
}

func (w *linkWriter) finish(keep bool, flush time.Duration) {
	w.mu.Lock()
	if w.stop {
		w.mu.Unlock()
		return
	}
	w.stop = true
	w.keep = keep
	if flush <= 0 {
		w.queue = nil
	}
	w.cond.Signal()
	w.mu.Unlock()
	if flush > 0 {
		_ = w.conn.SetWriteDeadline(time.Now().Add(flush))
	} else if !keep {
		w.conn.Close()
	}
}

func (w *linkWriter) close(flush time.Duration) {
	w.finish(false, flush)
}

func (w *linkWriter) detach(flush time.Duration) {
	w.finish(true, flush)
	<-w.done
	_ = w.conn.SetWriteDeadline(time.Time{})
}
