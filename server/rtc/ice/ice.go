package ice

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"net"
	"net/netip"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/mirror-node/rtc/stun"
)

const (
	KindDTLS = 1
	KindRTP  = 2

	ConsentTimeout   = 30 * time.Second
	ConsentInterval  = 5 * time.Second
	VerifyTimeout    = 3 * time.Second
	MaxDatagram      = 1500
	maxCandidates    = 8
	maxPendingChecks = 16
	iceChars         = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/"
)

var ErrNoPath = errors.New("ice: no selected path")

type Handler func(kind int, packet []byte)

type pendingCheck struct {
	addr     netip.AddrPort
	sent     time.Time
	nominate bool
}

type Session struct {
	mux         *Mux
	LocalUfrag  string
	LocalPwd    string
	remoteUfrag string
	remotePwd   string
	handler     Handler
	selected    netip.AddrPort
	hasPath     bool
	candidates  []netip.AddrPort
	pending     map[[12]byte]pendingCheck
	lastConsent time.Time
	lastProbe   time.Time
	tieBreaker  [8]byte
	closed      bool
}

type Mux struct {
	conn    *net.UDPConn
	mu      sync.Mutex
	byUfrag map[string]*Session
	byAddr  map[netip.AddrPort]*Session
	now     func() time.Time
}

func Listen(address string) (*Mux, error) {
	addr, err := net.ResolveUDPAddr("udp", address)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		return nil, err
	}
	_ = conn.SetReadBuffer(4 << 20)
	_ = conn.SetWriteBuffer(4 << 20)
	return &Mux{
		conn:    conn,
		byUfrag: map[string]*Session{},
		byAddr:  map[netip.AddrPort]*Session{},
		now:     time.Now,
	}, nil
}

func (m *Mux) Port() int {
	return m.conn.LocalAddr().(*net.UDPAddr).Port
}

func (m *Mux) Close() error {
	return m.conn.Close()
}

func (m *Mux) Candidates(addresses []netip.Addr) []string {
	port := strconv.Itoa(m.Port())
	lines := make([]string, 0, len(addresses))
	for index, address := range addresses {
		foundation := strconv.Itoa(index + 1)
		priority := strconv.FormatUint(uint64(2130706431-index), 10)
		lines = append(lines, foundation+" 1 udp "+priority+" "+address.Unmap().String()+" "+port+" typ host")
	}
	return lines
}

func randomString(n int) string {
	raw := make([]byte, n)
	if _, err := rand.Read(raw); err != nil {
		panic(err)
	}
	for i := range raw {
		raw[i] = iceChars[int(raw[i])%len(iceChars)]
	}
	return string(raw)
}

func (m *Mux) NewSession(remoteUfrag, remotePwd string, handler Handler) *Session {
	m.mu.Lock()
	defer m.mu.Unlock()
	session := &Session{
		mux:         m,
		LocalPwd:    randomString(24),
		remoteUfrag: remoteUfrag,
		remotePwd:   remotePwd,
		handler:     handler,
		pending:     map[[12]byte]pendingCheck{},
		lastConsent: m.now(),
	}
	if _, err := rand.Read(session.tieBreaker[:]); err != nil {
		panic(err)
	}
	for {
		session.LocalUfrag = randomString(8)
		if _, taken := m.byUfrag[session.LocalUfrag]; !taken {
			break
		}
	}
	m.byUfrag[session.LocalUfrag] = session
	return session
}

func (s *Session) Selected() (netip.AddrPort, bool) {
	s.mux.mu.Lock()
	defer s.mux.mu.Unlock()
	return s.selected, s.hasPath
}

func (s *Session) Send(packet []byte) error {
	s.mux.mu.Lock()
	target, ok := s.selected, s.hasPath && !s.closed
	s.mux.mu.Unlock()
	if !ok {
		return ErrNoPath
	}
	_, err := s.mux.conn.WriteToUDPAddrPort(packet, target)
	return err
}

func (s *Session) Close() {
	s.mux.mu.Lock()
	defer s.mux.mu.Unlock()
	s.closeLocked()
}

func (s *Session) closeLocked() {
	if s.closed {
		return
	}
	s.closed = true
	delete(s.mux.byUfrag, s.LocalUfrag)
	for _, candidate := range s.candidates {
		if s.mux.byAddr[candidate] == s {
			delete(s.mux.byAddr, candidate)
		}
	}
	s.candidates = nil
	s.pending = nil
}

func (s *Session) Expired(now time.Time) bool {
	s.mux.mu.Lock()
	defer s.mux.mu.Unlock()
	return s.closed || now.Sub(s.lastConsent) > ConsentTimeout
}

func (s *Session) Refresh(now time.Time) {
	s.mux.mu.Lock()
	if s.closed || !s.hasPath || now.Sub(s.lastProbe) < ConsentInterval {
		s.mux.mu.Unlock()
		return
	}
	s.lastProbe = now
	target := s.selected
	packet, ok := s.checkLocked(target, false, now)
	s.mux.mu.Unlock()
	if ok {
		_, _ = s.mux.conn.WriteToUDPAddrPort(packet, target)
	}
}

func (s *Session) checkLocked(target netip.AddrPort, nominate bool, now time.Time) ([]byte, bool) {
	if s.remotePwd == "" || s.remoteUfrag == "" {
		return nil, false
	}
	for tid, check := range s.pending {
		if now.Sub(check.sent) > VerifyTimeout {
			delete(s.pending, tid)
		}
	}
	if len(s.pending) >= maxPendingChecks {
		return nil, false
	}
	request := &stun.Message{Type: stun.BindingRequest}
	if _, err := rand.Read(request.TransactionID[:]); err != nil {
		return nil, false
	}
	request.Add(stun.AttrUsername, []byte(s.remoteUfrag+":"+s.LocalUfrag))
	var priority [4]byte
	binary.BigEndian.PutUint32(priority[:], 1853824767)
	request.Add(stun.AttrPriority, priority[:])
	request.Add(stun.AttrICEControlled, s.tieBreaker[:])
	s.pending[request.TransactionID] = pendingCheck{addr: target, sent: now, nominate: nominate}
	return request.Encode([]byte(s.remotePwd), true), true
}

func (m *Mux) Serve() error {
	buffer := make([]byte, MaxDatagram+1)
	for {
		n, from, err := m.conn.ReadFromUDPAddrPort(buffer)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			continue
		}
		if n == 0 || n > MaxDatagram {
			continue
		}
		m.HandleDatagram(append([]byte(nil), buffer[:n]...), from)
	}
}

func (m *Mux) HandleDatagram(packet []byte, from netip.AddrPort) {
	if len(packet) == 0 {
		return
	}
	from = netip.AddrPortFrom(from.Addr().Unmap(), from.Port())
	first := packet[0]
	switch {
	case first <= 3:
		m.handleSTUN(packet, from)
	case first >= 20 && first <= 63:
		m.deliver(KindDTLS, packet, from)
	case first >= 128 && first <= 191:
		m.deliver(KindRTP, packet, from)
	}
}

func (m *Mux) deliver(kind int, packet []byte, from netip.AddrPort) {
	m.mu.Lock()
	session := m.byAddr[from]
	heard := session != nil && !session.closed && (!session.hasPath || session.selected == from)
	m.mu.Unlock()
	if !heard || session.handler == nil {
		return
	}
	session.handler(kind, packet)
}

func (m *Mux) handleSTUN(packet []byte, from netip.AddrPort) {
	if !stun.IsMessage(packet) {
		return
	}
	message, err := stun.Parse(packet)
	if err != nil {
		return
	}
	switch message.Type {
	case stun.BindingRequest:
		m.handleRequest(packet, message, from)
	case stun.BindingSuccess:
		m.handleResponse(packet, message, from)
	}
}

func (m *Mux) handleRequest(packet []byte, message *stun.Message, from netip.AddrPort) {
	username, ok := message.Get(stun.AttrUsername)
	if !ok {
		return
	}
	local, remote, found := strings.Cut(string(username), ":")
	if !found {
		return
	}
	m.mu.Lock()
	session := m.byUfrag[local]
	m.mu.Unlock()
	if session == nil {
		return
	}
	if _, ok := message.Get(stun.AttrFingerprint); ok && !stun.CheckFingerprint(packet) {
		return
	}
	if !stun.CheckIntegrity(packet, []byte(session.LocalPwd)) {
		return
	}
	_, nominate := message.Get(stun.AttrUseCandidate)
	now := m.now()
	m.mu.Lock()
	if session.closed || (session.remoteUfrag != "" && session.remoteUfrag != remote) {
		m.mu.Unlock()
		return
	}
	if owner := m.byAddr[from]; owner != nil && owner != session {
		m.mu.Unlock()
		return
	}
	if m.byAddr[from] == nil {
		if len(session.candidates) >= maxCandidates {
			oldest := session.candidates[0]
			if oldest != session.selected && m.byAddr[oldest] == session {
				delete(m.byAddr, oldest)
			}
			session.candidates = session.candidates[1:]
		}
		session.candidates = append(session.candidates, from)
		m.byAddr[from] = session
	}
	var verify []byte
	if (nominate || !session.hasPath) && (!session.hasPath || from != session.selected) {
		verify, _ = session.checkLocked(from, nominate, now)
	}
	m.mu.Unlock()
	response := &stun.Message{Type: stun.BindingSuccess, TransactionID: message.TransactionID}
	response.AddXORMappedAddress(from)
	_, _ = m.conn.WriteToUDPAddrPort(response.Encode([]byte(session.LocalPwd), true), from)
	if verify != nil {
		_, _ = m.conn.WriteToUDPAddrPort(verify, from)
	}
}

func (m *Mux) handleResponse(packet []byte, message *stun.Message, from netip.AddrPort) {
	m.mu.Lock()
	defer m.mu.Unlock()
	session := m.byAddr[from]
	if session == nil || session.closed {
		return
	}
	check, ok := session.pending[message.TransactionID]
	if !ok || check.addr != from {
		return
	}
	if !stun.CheckIntegrity(packet, []byte(session.remotePwd)) {
		return
	}
	delete(session.pending, message.TransactionID)
	now := m.now()
	if check.nominate || !session.hasPath {
		session.selected = from
		session.hasPath = true
	}
	if from == session.selected {
		session.lastConsent = now
	}
}
