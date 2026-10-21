package ice

import (
	"crypto/rand"
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

	ConsentTimeout = 30 * time.Second
	MaxDatagram    = 1500
	iceChars       = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/"
)

var ErrNoPath = errors.New("ice: no selected path")

type Handler func(kind int, packet []byte)

type Session struct {
	mux         *Mux
	LocalUfrag  string
	LocalPwd    string
	remoteUfrag string
	handler     Handler
	selected    netip.AddrPort
	hasPath     bool
	lastCheck   time.Time
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

func (m *Mux) NewSession(remoteUfrag string, handler Handler) *Session {
	m.mu.Lock()
	defer m.mu.Unlock()
	session := &Session{
		mux:         m,
		LocalPwd:    randomString(24),
		remoteUfrag: remoteUfrag,
		handler:     handler,
		lastCheck:   m.now(),
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

func (s *Session) SetRemoteUfrag(ufrag string) {
	s.mux.mu.Lock()
	s.remoteUfrag = ufrag
	s.mux.mu.Unlock()
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
	if s.hasPath && s.mux.byAddr[s.selected] == s {
		delete(s.mux.byAddr, s.selected)
	}
}

func (s *Session) Expired(now time.Time) bool {
	s.mux.mu.Lock()
	defer s.mux.mu.Unlock()
	return s.closed || now.Sub(s.lastCheck) > ConsentTimeout
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
	m.mu.Unlock()
	if session == nil || session.handler == nil {
		return
	}
	session.handler(kind, packet)
}

func (m *Mux) handleSTUN(packet []byte, from netip.AddrPort) {
	if !stun.IsMessage(packet) {
		return
	}
	message, err := stun.Parse(packet)
	if err != nil || message.Type != stun.BindingRequest {
		return
	}
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
	m.mu.Lock()
	if session.closed || (session.remoteUfrag != "" && session.remoteUfrag != remote) {
		m.mu.Unlock()
		return
	}
	_, nominate := message.Get(stun.AttrUseCandidate)
	if nominate || !session.hasPath {
		if session.hasPath && m.byAddr[session.selected] == session {
			delete(m.byAddr, session.selected)
		}
		if other := m.byAddr[from]; other != nil && other != session {
			m.mu.Unlock()
			return
		}
		session.selected = from
		session.hasPath = true
		m.byAddr[from] = session
	}
	if from == session.selected {
		session.lastCheck = m.now()
	}
	m.mu.Unlock()
	response := &stun.Message{Type: stun.BindingSuccess, TransactionID: message.TransactionID}
	response.AddXORMappedAddress(from)
	_, _ = m.conn.WriteToUDPAddrPort(response.Encode([]byte(session.LocalPwd), true), from)
}
