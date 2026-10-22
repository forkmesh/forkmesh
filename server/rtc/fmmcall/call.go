package fmmcall

import (
	"context"
	"errors"
	"net"
	"net/netip"
	"sync"
	"sync/atomic"
	"time"

	"forkmesh.com/mirror-node/rtc/fmm"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
	"forkmesh.com/mirror-node/rtc/quic"
)

const (
	tickEvery        = 5 * time.Millisecond
	webTransportRoom = 4
	controlBacklog   = 1 << 20
)

var (
	ErrNoStandby = errors.New("fmmcall: no standby core")
	errPromoted  = errors.New("fmmcall: standby already promoted")
)

type Endpoint struct {
	URL    string   `json:"url"`
	Addr   string   `json:"addr"`
	ALPN   string   `json:"alpn"`
	Hashes []string `json:"hashes"`
	Ticket string   `json:"ticket"`
}

type Access struct {
	Endpoint
	Standby *Endpoint `json:"standby,omitempty"`
}

type Options struct {
	Client    *fmm.Config
	QUIC      *quic.Config
	Standby   quic.StandbyOptions
	Resolve   func(addr string) (netip.AddrPort, error)
	OnSession func(s *Session, moved bool)
	Now       func() time.Time
}

type Session struct {
	Conn   *quic.Conn
	Stream *quic.Stream
	Client *fmm.Client

	epoch  time.Time
	ctx    context.Context
	cancel context.CancelFunc
	out    *writer
	audio  [256]atomic.Bool
	wg     sync.WaitGroup

	pmu       sync.Mutex
	checked   time.Time
	redundant bool
}

func NewSession(conn *quic.Conn, st *quic.Stream, cfg *fmm.Config, epoch time.Time) *Session {
	ctx, cancel := context.WithCancel(context.Background())
	s := &Session{Conn: conn, Stream: st, epoch: epoch, ctx: ctx, cancel: cancel}
	s.out = newWriter(st, conn)
	s.Client = fmm.NewClient(s, cfg)
	s.wg.Add(3)
	go s.readControl()
	go s.readDatagrams()
	go s.tick()
	return s
}

func (s *Session) Now() int64 { return time.Since(s.epoch).Microseconds() }

func (s *Session) Done() <-chan struct{} { return s.ctx.Done() }

func (s *Session) SendDatagram(p []byte) error {
	if len(p) > 1 && p[0]&0x0F == wire.KindMedia && s.audio[p[1]].Load() && s.multipath() {
		return s.Conn.SendDatagramWith(p, quic.Redundant)
	}
	return s.Conn.SendDatagram(p)
}

func (s *Session) multipath() bool {
	if !s.Conn.MultipathEnabled() {
		return false
	}
	s.pmu.Lock()
	defer s.pmu.Unlock()
	if now := time.Now(); now.Sub(s.checked) >= 200*time.Millisecond {
		s.checked = now
		s.redundant = s.Conn.Health().UsablePaths > 1
	}
	return s.redundant
}

func (s *Session) MaxDatagramSize() int {
	return max(s.Conn.MaxDatagramSize()-webTransportRoom, 0)
}

func (s *Session) SendControl(p []byte) error {
	if len(p) > 0 && p[0] == wire.MsgPublish {
		if m, _, err := wire.Decode(p); err == nil {
			if v, ok := m.(wire.Publish); ok {
				s.audio[v.Stream].Store(wire.AudioLike(v.Kind))
			}
		}
	}
	return s.out.send(p)
}

func (s *Session) readControl() {
	defer s.wg.Done()
	defer s.cancel()
	buf := make([]byte, 4096)
	for {
		n, err := s.Stream.Read(buf)
		if n > 0 {
			s.Client.HandleControl(buf[:n], s.Now())
		}
		if err != nil {
			return
		}
	}
}

func (s *Session) readDatagrams() {
	defer s.wg.Done()
	for {
		b, err := s.Conn.ReceiveDatagram(s.ctx)
		if err != nil {
			return
		}
		s.Client.HandleDatagram(b, s.Now())
	}
}

func (s *Session) tick() {
	defer s.wg.Done()
	t := time.NewTicker(tickEvery)
	defer t.Stop()
	for {
		select {
		case <-s.ctx.Done():
			return
		case <-s.Conn.Done():
			s.cancel()
			return
		case <-t.C:
			s.Client.Tick(s.Now())
		}
	}
}

func (s *Session) Close() {
	s.cancel()
	s.out.close()
	s.Conn.CloseWithError(0, "session closed")
	s.wg.Wait()
}

type writer struct {
	st   *quic.Stream
	conn *quic.Conn

	mu     sync.Mutex
	queue  [][]byte
	bytes  int
	closed bool
	wake   chan struct{}
}

func newWriter(st *quic.Stream, conn *quic.Conn) *writer {
	w := &writer{st: st, conn: conn, wake: make(chan struct{}, 1)}
	go w.run()
	return w
}

func (w *writer) send(p []byte) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.closed {
		return fmm.ErrClosed
	}
	if w.bytes+len(p) > controlBacklog {
		return fmm.ErrProtocol
	}
	w.queue = append(w.queue, append([]byte(nil), p...))
	w.bytes += len(p)
	select {
	case w.wake <- struct{}{}:
	default:
	}
	return nil
}

func (w *writer) close() {
	w.mu.Lock()
	w.closed = true
	w.mu.Unlock()
	select {
	case w.wake <- struct{}{}:
	default:
	}
}

func (w *writer) run() {
	for {
		w.mu.Lock()
		batch, closed := w.queue, w.closed
		w.queue, w.bytes = nil, 0
		w.mu.Unlock()
		for _, b := range batch {
			if _, err := w.st.Write(b); err != nil {
				return
			}
		}
		if len(batch) > 0 {
			continue
		}
		if closed {
			return
		}
		select {
		case <-w.wake:
		case <-w.conn.Done():
			return
		}
	}
}

type Call struct {
	ep     *quic.Endpoint
	access Access
	opts   Options
	epoch  time.Time
	ctx    context.Context
	cancel context.CancelFunc

	mu       sync.Mutex
	cur      *Session
	sb       *quic.Standby
	streams  map[*quic.Conn]*quic.Stream
	deadAt   time.Time
	movedAt  time.Time
	reason   quic.DeadReason
	moved    chan struct{}
	promoted atomic.Bool
	wg       sync.WaitGroup
}

func resolve(addr string) (netip.AddrPort, error) {
	if ap, err := netip.ParseAddrPort(addr); err == nil {
		return ap, nil
	}
	ua, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return netip.AddrPort{}, err
	}
	return ua.AddrPort(), nil
}

func (c *Call) config(e *Endpoint) (*quic.Config, netip.AddrPort, error) {
	lookup := c.opts.Resolve
	if lookup == nil {
		lookup = resolve
	}
	addr, err := lookup(e.Addr)
	if err != nil {
		return nil, addr, err
	}
	host, _, _ := net.SplitHostPort(e.Addr)
	tlsConf, err := PinnedTLS(host, e.Hashes, c.opts.Now)
	if err != nil {
		return nil, addr, err
	}
	conf := &quic.Config{}
	if c.opts.QUIC != nil {
		n := *c.opts.QUIC
		conf = &n
	}
	conf.TLSConfig = tlsConf
	if conf.MaxDatagramFrameSize == 0 {
		conf.MaxDatagramFrameSize = 1500
	}
	if conf.KeepAlivePeriod == 0 {
		conf.KeepAlivePeriod = 2 * time.Second
	}
	conf.Multipath = true
	conf.DatagramDedupKey = wire.DedupKey
	conf.DatagramLossExempt = true
	return conf, addr, nil
}

func (c *Call) dial(ctx context.Context, e *Endpoint) (*quic.Conn, *quic.Stream, error) {
	conf, addr, err := c.config(e)
	if err != nil {
		return nil, nil, err
	}
	conn, err := c.ep.Dial(ctx, addr, conf)
	if err != nil {
		return nil, nil, err
	}
	st, err := conn.OpenStreamSync(ctx)
	if err != nil {
		conn.CloseWithError(0, "no stream")
		return nil, nil, err
	}
	return conn, st, nil
}

func Start(ctx context.Context, ep *quic.Endpoint, access Access, opts Options) (*Call, error) {
	cctx, cancel := context.WithCancel(context.Background())
	c := &Call{
		ep: ep, access: access, opts: opts, epoch: time.Now(), ctx: cctx, cancel: cancel,
		streams: map[*quic.Conn]*quic.Stream{}, moved: make(chan struct{}),
	}
	conn, st, err := c.dial(ctx, &access.Endpoint)
	if err != nil {
		cancel()
		return nil, err
	}
	s := NewSession(conn, st, opts.Client, c.epoch)
	if err := s.Client.Join(access.Ticket, s.Now()); err != nil {
		s.Close()
		cancel()
		return nil, err
	}
	c.cur = s
	if opts.OnSession != nil {
		opts.OnSession(s, false)
	}
	if access.Standby != nil {
		c.sb = quic.NewStandby(conn, c.dialStandby, opts.Standby)
		c.wg.Add(1)
		go c.watch(c.sb)
	}
	return c, nil
}

func (c *Call) dialStandby(ctx context.Context) (*quic.Conn, error) {
	if c.promoted.Load() {
		return nil, errPromoted
	}
	conn, st, err := c.dial(ctx, c.access.Standby)
	if err != nil {
		return nil, err
	}
	raw, err := wire.Append(nil, wire.Join{Ticket: c.access.Standby.Ticket})
	if err == nil {
		_, err = st.Write(raw)
	}
	if err != nil {
		conn.CloseWithError(0, "standby join")
		return nil, err
	}
	c.mu.Lock()
	for old := range c.streams {
		select {
		case <-old.Done():
			delete(c.streams, old)
		default:
		}
	}
	c.streams[conn] = st
	c.mu.Unlock()
	return conn, nil
}

func (c *Call) StandbyReady(ctx context.Context) error {
	if c.sb == nil {
		return ErrNoStandby
	}
	_, err := c.sb.Conn(ctx)
	return err
}

func (c *Call) watch(sb *quic.Standby) {
	defer c.wg.Done()
	select {
	case <-c.ctx.Done():
		return
	case <-sb.PrimaryDead():
	}
	deadAt, reason := sb.DeadSince()
	c.promoted.Store(true)
	conn, err := sb.Promote(c.ctx)
	sb.Close()
	if err != nil {
		return
	}
	c.mu.Lock()
	st := c.streams[conn]
	old := c.cur
	c.mu.Unlock()
	if st == nil {
		conn.CloseWithError(0, "no standby stream")
		return
	}
	go old.Close()
	s := NewSession(conn, st, c.opts.Client, c.epoch)
	_ = s.Client.Join(c.access.Standby.Ticket, s.Now())
	c.mu.Lock()
	c.cur, c.deadAt, c.reason, c.movedAt = s, deadAt, reason, time.Now()
	c.mu.Unlock()
	if c.opts.OnSession != nil {
		c.opts.OnSession(s, true)
	}
	close(c.moved)
}

func (c *Call) Session() *Session {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.cur
}

func (c *Call) Moved() <-chan struct{} { return c.moved }

func (c *Call) Failover() (deadAt, movedAt time.Time, reason quic.DeadReason) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.deadAt, c.movedAt, c.reason
}

func (c *Call) Close() {
	c.cancel()
	if c.sb != nil {
		c.sb.Close()
	}
	c.wg.Wait()
	c.mu.Lock()
	s := c.cur
	c.mu.Unlock()
	if s != nil {
		_ = s.Client.Leave("bye", s.Now())
		s.Close()
	}
}
