package webtransport

import (
	"context"
	"errors"
	"net/netip"
	"sync"
	"time"

	"forkmesh.com/mirror-node/rtc/h3"
	"forkmesh.com/mirror-node/rtc/quic"
)

const (
	defaultMaxSessions      = 4
	defaultPendingStreams   = 32
	defaultPendingDatagrams = 256
	DatagramFrameSize       = 65535
)

type Server struct {
	Check            func(r *h3.Request) int
	Handler          func(*Session)
	MaxSessions      int
	PendingStreams   int
	PendingDatagrams int
}

func Settings(maxSessions int) h3.Settings {
	return h3.Settings{
		h3.SettingEnableConnectProtocol:   1,
		h3.SettingH3Datagram:              1,
		h3.SettingH3DatagramDraft04:       1,
		h3.SettingEnableWebTransport:      1,
		h3.SettingWebTransportMaxSessions: uint64(maxSessions),
	}
}

func (s *Server) limits() limits {
	l := limits{pendingStreams: s.PendingStreams, pendingDatagrams: s.PendingDatagrams}
	if l.pendingStreams <= 0 {
		l.pendingStreams = defaultPendingStreams
	}
	if l.pendingDatagrams <= 0 {
		l.pendingDatagrams = defaultPendingDatagrams
	}
	return l
}

func (s *Server) maxSessions() int {
	if s.MaxSessions <= 0 {
		return defaultMaxSessions
	}
	return s.MaxSessions
}

func ServerQUICConfig(base *quic.Config) *quic.Config {
	c := &quic.Config{}
	if base != nil {
		n := *base
		c = &n
	}
	c.MaxDatagramFrameSize = DatagramFrameSize
	if c.TLSConfig != nil {
		t := c.TLSConfig.Clone()
		if len(t.NextProtos) == 0 {
			t.NextProtos = []string{"h3"}
		}
		c.TLSConfig = t
	}
	return c
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

const (
	maxUnclaimedStreams = 8
	unclaimedLifetime   = 5 * time.Second
)

type unclaimed struct {
	uni bool
	sid uint64
	st  *quic.Stream
	at  time.Time
}

type connSessions struct {
	mu       sync.Mutex
	sessions map[uint64]*Session
	pending  []unclaimed
}

func newConnSessions() *connSessions {
	return &connSessions{sessions: make(map[uint64]*Session)}
}

func (cs *connSessions) get(id uint64) *Session {
	cs.mu.Lock()
	defer cs.mu.Unlock()
	return cs.sessions[id]
}

func rejectStream(uni bool, st *quic.Stream) {
	st.CancelRead(codeBufferedRejected)
	if !uni {
		st.CancelWrite(codeBufferedRejected)
	}
}

func (cs *connSessions) route(uni bool, sid uint64, st *quic.Stream) {
	now := time.Now()
	var drop []unclaimed
	cs.mu.Lock()
	sess := cs.sessions[sid]
	if sess == nil {
		kept := cs.pending[:0]
		for _, p := range cs.pending {
			if now.Sub(p.at) > unclaimedLifetime {
				drop = append(drop, p)
				continue
			}
			kept = append(kept, p)
		}
		cs.pending = kept
		if len(cs.pending) >= maxUnclaimedStreams {
			drop = append(drop, cs.pending[0])
			cs.pending = cs.pending[1:]
		}
		cs.pending = append(cs.pending, unclaimed{uni: uni, sid: sid, st: st, at: now})
	}
	cs.mu.Unlock()
	for _, p := range drop {
		rejectStream(p.uni, p.st)
	}
	if sess != nil && !sess.addStream(uni, st) {
		rejectStream(uni, st)
	}
}

func (cs *connSessions) register(sess *Session) {
	cs.mu.Lock()
	cs.sessions[sess.id] = sess
	var claimed []unclaimed
	kept := cs.pending[:0]
	for _, p := range cs.pending {
		if p.sid == sess.id {
			claimed = append(claimed, p)
			continue
		}
		kept = append(kept, p)
	}
	cs.pending = kept
	cs.mu.Unlock()
	for _, p := range claimed {
		if !sess.addStream(p.uni, p.st) {
			rejectStream(p.uni, p.st)
		}
	}
}

func (cs *connSessions) closeAll() {
	cs.mu.Lock()
	all := make([]*Session, 0, len(cs.sessions))
	for _, sess := range cs.sessions {
		all = append(all, sess)
	}
	pending := cs.pending
	cs.pending = nil
	cs.mu.Unlock()
	for _, p := range pending {
		rejectStream(p.uni, p.st)
	}
	for _, sess := range all {
		sess.terminate(ErrSessionClosed)
	}
}

func (s *Server) ServeConn(ctx context.Context, qc *quic.Conn) error {
	cs := newConnSessions()
	lim := s.limits()
	var hc *h3.Conn
	hc = h3.NewConn(qc, true, &h3.Config{
		Settings: Settings(s.maxSessions()),
		Handler: func(r *h3.Request) {
			s.handleConnect(cs, hc, r, lim)
		},
		StreamHandler: cs.route,
		DatagramHandler: func(streamID uint64, payload []byte) {
			if sess := cs.get(streamID); sess != nil {
				sess.addDatagram(payload)
			}
		},
	})
	err := hc.Serve(ctx)
	cs.closeAll()
	return err
}

func (s *Server) handleConnect(cs *connSessions, hc *h3.Conn, r *h3.Request, lim limits) {
	if r.Method != "CONNECT" || r.Protocol != "webtransport" {
		r.Respond(404, nil)
		r.Stream.Close()
		return
	}
	id := r.Stream.StreamID()
	if id&3 != 0 {
		r.Respond(400, nil)
		r.Stream.Close()
		return
	}
	cs.mu.Lock()
	full := len(cs.sessions) >= s.maxSessions()
	cs.mu.Unlock()
	if full {
		r.Respond(429, nil)
		r.Stream.Close()
		return
	}
	status := 200
	if s.Check != nil {
		status = s.Check(r)
	}
	if status < 200 || status > 299 {
		if status < 400 || status > 599 {
			status = 403
		}
		r.Respond(status, nil)
		r.Stream.Close()
		return
	}
	sess := newSession(id, hc, r.Stream, r.Body(), lim)
	sess.Request = r
	sess.onClose = func() {
		cs.mu.Lock()
		delete(cs.sessions, id)
		cs.mu.Unlock()
	}
	cs.register(sess)
	var header []h3.Field
	if r.Get("sec-webtransport-http3-draft02") == "1" {
		header = append(header, h3.Field{Name: "sec-webtransport-http3-draft", Value: "draft02"})
	}
	if err := r.Respond(200, header); err != nil {
		sess.terminate(err)
		return
	}
	if s.Handler != nil {
		go s.Handler(sess)
	}
	sess.readCapsules()
}

func Dial(ctx context.Context, addr netip.AddrPort, authority, path, origin string, config *quic.Config) (*Session, error) {
	conf := &quic.Config{}
	if config != nil {
		n := *config
		conf = &n
	}
	conf.MaxDatagramFrameSize = DatagramFrameSize
	if conf.TLSConfig != nil {
		t := conf.TLSConfig.Clone()
		if len(t.NextProtos) == 0 {
			t.NextProtos = []string{"h3"}
		}
		conf.TLSConfig = t
	}
	qc, err := quic.Dial(ctx, "udp", addr.String(), conf)
	if err != nil {
		return nil, err
	}
	sess, err := DialConn(ctx, qc, authority, path, origin)
	if err != nil {
		qc.CloseWithError(uint64(h3.ErrNoError), "")
		return nil, err
	}
	return sess, nil
}

func DialConn(ctx context.Context, qc *quic.Conn, authority, path, origin string) (*Session, error) {
	cs := newConnSessions()
	lim := (&Server{}).limits()
	hc := h3.NewConn(qc, false, &h3.Config{
		Settings:      Settings(1),
		StreamHandler: cs.route,
		DatagramHandler: func(streamID uint64, payload []byte) {
			if sess := cs.get(streamID); sess != nil {
				sess.addDatagram(payload)
			}
		},
	})
	go func() {
		hc.Serve(context.Background())
		cs.closeAll()
	}()
	settings, err := hc.PeerSettings(ctx)
	if err != nil {
		return nil, err
	}
	if settings[h3.SettingEnableConnectProtocol] != 1 || settings[h3.SettingH3Datagram] != 1 || (settings[h3.SettingWebTransportMaxSessions] == 0 && settings[h3.SettingEnableWebTransport] != 1) {
		return nil, errors.New("webtransport: server does not support WebTransport")
	}
	fields := []h3.Field{
		{Name: ":method", Value: "CONNECT"},
		{Name: ":protocol", Value: "webtransport"},
		{Name: ":scheme", Value: "https"},
		{Name: ":authority", Value: authority},
		{Name: ":path", Value: path},
	}
	if origin != "" {
		fields = append(fields, h3.Field{Name: "origin", Value: origin})
	}
	resp, err := hc.RoundTrip(ctx, fields)
	if err != nil {
		return nil, err
	}
	if resp.Status < 200 || resp.Status > 299 {
		resp.Stream.CancelRead(uint64(h3.ErrNoError))
		resp.Stream.Close()
		return nil, &SessionError{Code: uint32(resp.Status), Message: "rejected by server", Remote: true}
	}
	id := resp.Stream.StreamID()
	sess := newSession(id, hc, resp.Stream, resp.Body(), lim)
	sess.Response = resp
	sess.onClose = func() {
		cs.mu.Lock()
		delete(cs.sessions, id)
		cs.mu.Unlock()
	}
	cs.register(sess)
	go sess.readCapsules()
	return sess, nil
}
