package fmmserver

import (
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"encoding/binary"
	"errors"
	"io"
	"sync"
	"sync/atomic"
	"time"

	"forkmesh.com/mirror-node/rtc/fmm/sfu"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
	"forkmesh.com/mirror-node/rtc/h3"
	"forkmesh.com/mirror-node/rtc/quic"
	"forkmesh.com/mirror-node/rtc/signal"
	"forkmesh.com/mirror-node/rtc/webtransport"
)

const (
	ALPN = "fmm/1"
	Path = "/calls/fmm"

	ErrCodeEngine = 409

	MaxConns            = 1000
	MaxWaiting          = 200
	MaxParked           = 400
	MaxParkedPerAccount = 8
	MaxPerAccount       = 4
	MaxServerPeople     = 400

	joinTimeout     = 10 * time.Second
	parkLifetime    = 12 * time.Hour
	tickEvery       = 5 * time.Millisecond
	idleTimeout     = 30 * time.Second
	errorGrace      = time.Second
	packetBudget    = 1500
	byteBudget      = 1_000_000
	controlBytes    = 1 << 20
	pathCheckEvery  = 200 * time.Millisecond
	defaultMaxBps   = 2_500_000
	defaultStartBps = 600_000
	defaultMinBps   = 50_000
)

var (
	ErrClosed = errors.New("fmmserver: closed")
)

type Config struct {
	Secret        []byte
	AllowOrigin   func(string) bool
	Engines       *signal.RoomEngines
	Certs         *CertStore
	MaxPerAccount int
	MaxPeers      int
	StartBitrate  int
	MinBitrate    int
	MaxBitrate    int
}

type Server struct {
	cfg   Config
	epoch time.Time
	wt    *webtransport.Server

	mu       sync.Mutex
	rooms    map[string]*room
	parked   int
	parkedBy map[string]int
	all      map[link]bool
	closed   bool

	conns   atomic.Int64
	waiting atomic.Int64
	joins   atomic.Int64
	resets  []byte
}

type room struct {
	id       string
	server   *Server
	core     *sfu.Room
	members  int
	accounts map[string]int
	stop     chan struct{}

	amu    sync.Mutex
	admits map[string]string
}

func New(cfg Config) (*Server, error) {
	if len(cfg.Secret) < 16 {
		return nil, errors.New("fmmserver: calls secret too short")
	}
	if cfg.AllowOrigin == nil {
		cfg.AllowOrigin = func(origin string) bool { return origin == "" }
	}
	if cfg.Engines == nil {
		cfg.Engines = signal.NewRoomEngines(MaxServerPeople)
	}
	if cfg.MaxPerAccount <= 0 {
		cfg.MaxPerAccount = MaxPerAccount
	}
	if cfg.StartBitrate <= 0 {
		cfg.StartBitrate = defaultStartBps
	}
	if cfg.MinBitrate <= 0 {
		cfg.MinBitrate = defaultMinBps
	}
	if cfg.MaxBitrate <= 0 {
		cfg.MaxBitrate = defaultMaxBps
	}
	mac := hmac.New(sha256.New, cfg.Secret)
	mac.Write([]byte("forkmesh-fmm-stateless-reset-v1"))
	s := &Server{
		cfg:      cfg,
		epoch:    time.Now(),
		rooms:    map[string]*room{},
		parkedBy: map[string]int{},
		all:      map[link]bool{},
		resets:   mac.Sum(nil),
	}
	s.wt = &webtransport.Server{
		MaxSessions: 1,
		Check:       s.check,
		Handler: func(sess *webtransport.Session) {
			s.serve(sess.Context(), &wtLink{sess: sess})
		},
	}
	return s, nil
}

func (s *Server) micros() int64 { return time.Since(s.epoch).Microseconds() }

func (s *Server) QUICConfig() *quic.Config {
	token := make([]byte, 32)
	if _, err := rand.Read(token); err != nil {
		panic(err)
	}
	tlsConf := &tls.Config{
		MinVersion: tls.VersionTLS13,
		NextProtos: []string{ALPN, "h3"},
	}
	if s.cfg.Certs != nil {
		tlsConf.GetCertificate = s.cfg.Certs.GetCertificate
	}
	return &quic.Config{
		TLSConfig:             tlsConf,
		MaxIdleTimeout:        idleTimeout,
		MaxDatagramFrameSize:  webtransport.DatagramFrameSize,
		MaxIncomingStreams:    8,
		MaxIncomingUniStreams: 8,
		MaxPendingDatagrams:   256,
		StatelessResetKey:     s.resets,
		TokenKey:              token,
		Multipath:             true,
		DatagramDedupKey:      wire.DedupKey,
		DatagramLossExempt:    true,
		MaxConnectionsPerIP:   32,
	}
}

func (s *Server) Serve(ctx context.Context, ep *quic.Endpoint) error {
	for {
		qc, err := ep.Accept(ctx)
		if err != nil {
			return err
		}
		go s.ServeConn(ctx, qc)
	}
}

func (s *Server) ServeConn(ctx context.Context, qc *quic.Conn) {
	switch qc.ConnectionState().NegotiatedProtocol {
	case ALPN:
		s.serve(ctx, newQUICLink(qc))
	case "h3":
		_ = s.wt.ServeConn(ctx, qc)
	default:
		qc.CloseWithError(0x0178, "no application protocol")
	}
}

func (s *Server) check(r *h3.Request) int {
	if r.Path != Path {
		return 404
	}
	if !s.cfg.AllowOrigin(r.Get("origin")) {
		return 403
	}
	if s.conns.Load() >= MaxConns || s.waiting.Load() >= MaxWaiting {
		return 503
	}
	return 200
}

func (s *Server) Rooms() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.rooms)
}

func (s *Server) Parked() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.parked
}

func (s *Server) Joins() int64 { return s.joins.Load() }

func (s *Server) Peers(roomID string) []string {
	s.mu.Lock()
	r := s.rooms[roomID]
	s.mu.Unlock()
	if r == nil {
		return nil
	}
	return r.core.Peers()
}

func (s *Server) Close() {
	s.mu.Lock()
	s.closed = true
	var links []link
	for l := range s.all {
		links = append(links, l)
	}
	s.mu.Unlock()
	for _, l := range links {
		l.close(0, "server closing")
	}
}

func readMessage(r io.Reader) (wire.Message, error) {
	head := make([]byte, wire.ControlHeaderLen)
	if _, err := io.ReadFull(r, head); err != nil {
		return nil, err
	}
	n := int(binary.BigEndian.Uint16(head[1:]))
	if n > wire.MaxControlBody {
		return nil, wire.ErrBound
	}
	raw := make([]byte, wire.ControlHeaderLen+n)
	copy(raw, head)
	if _, err := io.ReadFull(r, raw[wire.ControlHeaderLen:]); err != nil {
		return nil, err
	}
	m, _, err := wire.Decode(raw)
	return m, err
}

func (s *Server) serve(ctx context.Context, l link) {
	if s.conns.Add(1) > MaxConns {
		s.conns.Add(-1)
		l.close(0x10, "busy")
		return
	}
	defer s.conns.Add(-1)
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		l.close(0x10, "closing")
		return
	}
	s.all[l] = true
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		delete(s.all, l)
		s.mu.Unlock()
	}()
	if s.waiting.Add(1) > MaxWaiting {
		s.waiting.Add(-1)
		l.close(0x10, "busy")
		return
	}
	waiting := true
	defer func() {
		if waiting {
			s.waiting.Add(-1)
		}
	}()
	jctx, cancel := context.WithTimeout(ctx, joinTimeout)
	st, err := l.acceptStream(jctx)
	cancel()
	if err != nil {
		l.close(0x11, "join required")
		return
	}
	ctl := newControlWriter(st, l)
	defer ctl.close()
	_ = st.SetReadDeadline(time.Now().Add(joinTimeout))
	m, err := readMessage(st)
	join, ok := m.(wire.Join)
	if err != nil || !ok {
		s.refuse(ctl, l, sfu.ErrCodeAdmission, "join first")
		return
	}
	ticket, err := signal.VerifyTicket(s.cfg.Secret, join.Ticket, time.Now())
	if err != nil || ticket.Engine != signal.EngineFMM {
		s.refuse(ctl, l, sfu.ErrCodeAdmission, "ticket refused")
		return
	}
	if ticket.Standby {
		if !s.park(ticket.Acct) {
			s.refuse(ctl, l, sfu.ErrCodeFull, "standby full")
			return
		}
		waiting = false
		s.waiting.Add(-1)
		_ = st.SetReadDeadline(time.Now().Add(parkLifetime))
		m, err = readMessage(st)
		s.unpark(ticket.Acct)
		again, ok := m.(wire.Join)
		if err != nil || !ok || again.Ticket != join.Ticket {
			s.refuse(ctl, l, sfu.ErrCodeAdmission, "join first")
			return
		}
	}
	_ = st.SetReadDeadline(time.Time{})
	r, code, text := s.enter(ticket)
	if r == nil {
		s.refuse(ctl, l, code, text)
		return
	}
	left := false
	leave := func() {
		if !left {
			left = true
			s.leave(r, ticket.Acct)
		}
	}
	defer leave()
	if waiting {
		waiting = false
		s.waiting.Add(-1)
	}
	tr := &transport{link: l, ctl: ctl}
	peer := r.core.Connect(tr)
	r.amu.Lock()
	r.admits[join.Ticket] = ticket.Peer
	r.amu.Unlock()
	raw, _ := wire.Append(nil, wire.Join{Ticket: join.Ticket})
	peer.HandleControl(raw, s.micros())
	r.amu.Lock()
	delete(r.admits, join.Ticket)
	r.amu.Unlock()
	if peer.Closed() {
		leave()
		ctl.flush(errorGrace)
		l.waitClose(errorGrace)
		l.close(0, "refused")
		return
	}
	s.joins.Add(1)
	go s.datagrams(ctx, l, peer)
	buf := make([]byte, 4096)
	for {
		n, err := st.Read(buf)
		if n > 0 {
			peer.HandleControl(buf[:n], s.micros())
			if peer.Closed() {
				break
			}
		}
		if err != nil {
			break
		}
	}
	peer.Close(s.micros())
	leave()
	ctl.flush(errorGrace)
	l.close(0, "left")
}

func (s *Server) datagrams(ctx context.Context, l link, peer *sfu.Peer) {
	var start time.Time
	packets, bytes := 0, 0
	for {
		b, err := l.receive(ctx)
		if err != nil {
			return
		}
		now := time.Now()
		if now.Sub(start) > time.Second {
			start, packets, bytes = now, 0, 0
		}
		packets++
		bytes += len(b)
		if packets > packetBudget || bytes > byteBudget || len(b) > wire.MaxDatagramLen {
			continue
		}
		peer.HandleDatagram(b, s.micros())
	}
}

func (s *Server) refuse(ctl *controlWriter, l link, code int, text string) {
	if b, err := wire.Append(nil, wire.Error{Code: uint16(code), Text: text}); err == nil {
		_ = ctl.SendControl(b)
	}
	ctl.flush(errorGrace)
	l.waitClose(errorGrace)
	l.close(0, text)
}

func (s *Server) park(account string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed || s.parked >= MaxParked || s.parkedBy[account] >= MaxParkedPerAccount {
		return false
	}
	s.parked++
	s.parkedBy[account]++
	return true
}

func (s *Server) unpark(account string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.parked--
	if s.parkedBy[account]--; s.parkedBy[account] <= 0 {
		delete(s.parkedBy, account)
	}
}

func (s *Server) enter(t signal.Ticket) (*room, int, string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil, sfu.ErrCodeFull, "server closing"
	}
	if holder, err := s.cfg.Engines.Join(t.Room, signal.EngineFMM); err != nil {
		if errors.Is(err, signal.ErrRoomEngine) {
			return nil, ErrCodeEngine, "engine " + holder
		}
		return nil, sfu.ErrCodeFull, "server full"
	}
	r := s.rooms[t.Room]
	if r == nil {
		r = s.newRoom(t.Room)
		s.rooms[t.Room] = r
	}
	if r.accounts[t.Acct] >= s.cfg.MaxPerAccount {
		s.cfg.Engines.Leave(t.Room, signal.EngineFMM)
		s.dropIfEmptyLocked(r)
		return nil, sfu.ErrCodeFull, "account limit"
	}
	r.accounts[t.Acct]++
	r.members++
	return r, 0, ""
}

func (s *Server) leave(r *room, account string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if r.accounts[account]--; r.accounts[account] <= 0 {
		delete(r.accounts, account)
	}
	r.members--
	s.cfg.Engines.Leave(r.id, signal.EngineFMM)
	s.dropIfEmptyLocked(r)
}

func (s *Server) dropIfEmptyLocked(r *room) {
	if r.members > 0 || s.rooms[r.id] != r {
		return
	}
	delete(s.rooms, r.id)
	close(r.stop)
}

func (s *Server) newRoom(id string) *room {
	r := &room{
		id: id, server: s, accounts: map[string]int{}, stop: make(chan struct{}),
		admits: map[string]string{},
	}
	r.core = sfu.NewRoom(sfu.Config{
		Admit:        r.admit,
		MaxPeers:     s.cfg.MaxPeers,
		StartBitrate: s.cfg.StartBitrate,
		MinBitrate:   s.cfg.MinBitrate,
		MaxBitrate:   s.cfg.MaxBitrate,
	})
	go r.run()
	return r
}

func (r *room) admit(ticket string) (string, error) {
	r.amu.Lock()
	defer r.amu.Unlock()
	peer, ok := r.admits[ticket]
	if !ok {
		return "", sfu.ErrRejected
	}
	delete(r.admits, ticket)
	return peer, nil
}

func (r *room) run() {
	t := time.NewTicker(tickEvery)
	defer t.Stop()
	for {
		select {
		case <-r.stop:
			return
		case <-t.C:
			r.core.Tick(r.server.micros())
		}
	}
}
