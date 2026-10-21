package quic

import (
	"bytes"
	"context"
	"errors"
	"net"
	"net/netip"
	"sync"
	"sync/atomic"
	"time"
)

const readBufferSize = 2048

type socket struct {
	pc  net.PacketConn
	udp *net.UDPConn
}

type Endpoint struct {
	config    *Config
	listening bool
	sock      atomic.Pointer[socket]

	mu          sync.Mutex
	conns       map[string]*Conn
	resetTokens map[[16]byte]*Conn
	all         map[*Conn]struct{}
	perIP       map[netip.Addr]int
	halfOpen    int
	closed      bool

	acceptQ  chan *Conn
	closeCh  chan struct{}
	resetKey []byte
	tokens   *tokenSealer
	limiter  rateLimiter
	pool     sync.Pool
	wg       sync.WaitGroup
}

func Listen(network, address string, config *Config) (*Endpoint, error) {
	addr, err := net.ResolveUDPAddr(network, address)
	if err != nil {
		return nil, err
	}
	pc, err := net.ListenUDP(network, addr)
	if err != nil {
		return nil, err
	}
	e, err := NewEndpoint(pc, config, true)
	if err != nil {
		pc.Close()
		return nil, err
	}
	return e, nil
}

func NewEndpoint(pc net.PacketConn, config *Config, listen bool) (*Endpoint, error) {
	conf := config.withDefaults()
	if listen && (conf.TLSConfig == nil || (len(conf.TLSConfig.Certificates) == 0 && conf.TLSConfig.GetCertificate == nil && conf.TLSConfig.GetConfigForClient == nil)) {
		return nil, errors.New("quic: a listening endpoint needs a TLS certificate")
	}
	e := &Endpoint{
		config:      conf,
		listening:   listen,
		conns:       make(map[string]*Conn),
		resetTokens: make(map[[16]byte]*Conn),
		all:         make(map[*Conn]struct{}),
		perIP:       make(map[netip.Addr]int),
		acceptQ:     make(chan *Conn, conf.AcceptQueue),
		closeCh:     make(chan struct{}),
	}
	e.resetKey = conf.StatelessResetKey
	if len(e.resetKey) == 0 {
		e.resetKey = randomBytes(32)
	}
	tk := conf.TokenKey
	if len(tk) == 0 {
		tk = randomBytes(32)
	}
	e.tokens = newTokenSealer(tk)
	e.limiter.rate = float64(conf.MaxStatelessResetsPerSecond)
	e.pool.New = func() any {
		b := make([]byte, readBufferSize)
		return &b
	}
	s := newSocket(pc)
	e.sock.Store(s)
	e.wg.Add(1)
	go e.readLoop(s)
	return e, nil
}

func newSocket(pc net.PacketConn) *socket {
	s := &socket{pc: pc}
	if u, ok := pc.(*net.UDPConn); ok {
		s.udp = u
	}
	return s
}

func (e *Endpoint) LocalAddr() net.Addr { return e.sock.Load().pc.LocalAddr() }

func (e *Endpoint) getBuf() *[]byte {
	bp := e.pool.Get().(*[]byte)
	*bp = (*bp)[:cap(*bp)]
	return bp
}

func (e *Endpoint) release(d *inDatagram) {
	if d != nil && d.buf != nil {
		e.pool.Put(d.buf)
		d.buf = nil
	}
}

func addrPortOf(a net.Addr) (netip.AddrPort, bool) {
	switch v := a.(type) {
	case *net.UDPAddr:
		return v.AddrPort(), true
	}
	ap, err := netip.ParseAddrPort(a.String())
	return ap, err == nil
}

func (e *Endpoint) readLoop(s *socket) {
	defer e.wg.Done()
	failures := 0
	for {
		bp := e.getBuf()
		var n int
		var ap netip.AddrPort
		var err error
		if s.udp != nil {
			n, ap, err = s.udp.ReadFromUDPAddrPort(*bp)
		} else {
			var addr net.Addr
			n, addr, err = s.pc.ReadFrom(*bp)
			if err == nil {
				var ok bool
				if ap, ok = addrPortOf(addr); !ok {
					e.pool.Put(bp)
					continue
				}
			}
		}
		if err != nil {
			e.pool.Put(bp)
			if e.sock.Load() != s || e.isClosed() || errors.Is(err, net.ErrClosed) {
				return
			}
			failures++
			if failures > 1000 {
				return
			}
			continue
		}
		failures = 0
		e.handle(bp, n, ap)
	}
}

func (e *Endpoint) isClosed() bool {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.closed
}

func (e *Endpoint) handle(bp *[]byte, n int, from netip.AddrPort) {
	b := (*bp)[:n]
	now := time.Now()
	inv, ok := parseInvariant(b, localCIDLen)
	if !ok {
		e.pool.Put(bp)
		return
	}
	if inv.long && inv.version != Version1 && inv.version != 0 {
		if e.listening && n >= minInitialSize && e.limiter.allow(now) {
			e.writeTo(appendVersionNegotiation(nil, inv.scid, inv.dcid, []uint32{greaseVersion(), Version1}), from)
		}
		e.pool.Put(bp)
		return
	}
	e.mu.Lock()
	c := e.conns[string(inv.dcid)]
	e.mu.Unlock()
	if c != nil {
		if !c.deliver(&inDatagram{data: b, addr: from, at: now, buf: bp}) {
			e.pool.Put(bp)
		}
		return
	}
	if !inv.long {
		if n >= 21 {
			var tok [16]byte
			copy(tok[:], b[n-16:])
			e.mu.Lock()
			rc := e.resetTokens[tok]
			e.mu.Unlock()
			if rc != nil {
				if !rc.deliver(&inDatagram{data: b, addr: from, at: now, buf: bp, reset: true}) {
					e.pool.Put(bp)
				}
				return
			}
		}
		if e.listening && !e.config.DisableStatelessReset {
			e.sendStatelessReset(inv.dcid, n, from, now)
		}
		e.pool.Put(bp)
		return
	}
	if e.listening && inv.version == Version1 && longPacketType(b[0]) == packetInitial {
		if e.acceptInitial(b, bp, from, now) {
			return
		}
	}
	e.pool.Put(bp)
}

func (e *Endpoint) sendStatelessReset(dcid []byte, n int, to netip.AddrPort, now time.Time) {
	if n <= 42 || !e.limiter.allow(now) {
		return
	}
	size := n - 1
	if size > minInitialSize {
		size = minInitialSize
	}
	b := randomBytes(size)
	b[0] = 0x40 | (b[0] & 0x3f)
	tok := statelessResetToken(e.resetKey, dcid)
	copy(b[size-16:], tok[:])
	e.writeTo(b, to)
}

func (e *Endpoint) resetToken(cid []byte) [16]byte {
	return statelessResetToken(e.resetKey, cid)
}

func (e *Endpoint) acceptInitial(b []byte, bp *[]byte, from netip.AddrPort, now time.Time) bool {
	if len(b) < minInitialSize {
		return false
	}
	h, err := parseLongHeader(b)
	if err != nil || h.typ != packetInitial || len(h.dcid) < minDCIDFromClient {
		return false
	}
	ip := from.Addr()
	e.mu.Lock()
	if e.closed || len(e.all) >= e.config.MaxConnections || e.perIP[ip] >= e.config.MaxConnectionsPerIP {
		e.mu.Unlock()
		return false
	}
	halfOpen := e.halfOpen
	e.mu.Unlock()
	var odcid, rscid []byte
	retried := false
	if len(h.token) > 0 {
		if o, r, ok := e.tokens.openRetryToken(h.token, from, now); ok && bytes.Equal(r, h.dcid) {
			odcid, rscid, retried = append([]byte(nil), o...), append([]byte(nil), r...), true
		}
	}
	if !retried {
		if e.config.RequireAddressValidation || halfOpen >= e.config.RetryThreshold {
			if len(h.token) == 0 && e.limiter.allow(now) {
				scid := randomBytes(localCIDLen)
				tok := e.tokens.retryToken(from, h.dcid, scid, now)
				e.writeTo(appendRetry(nil, h.scid, scid, h.dcid, tok), from)
			}
			return false
		}
		odcid = append([]byte(nil), h.dcid...)
	}
	if !initialDecrypts(b[:h.end], h) {
		return false
	}
	scid := rscid
	if !retried {
		scid = randomBytes(localCIDLen)
	}
	c, err := e.newServerConn(from, h, odcid, scid, retried, now)
	if err != nil {
		return false
	}
	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		return false
	}
	e.all[c] = struct{}{}
	e.perIP[ip]++
	e.halfOpen++
	c.halfOpen = true
	if !retried {
		e.conns[string(odcid)] = c
	}
	e.mu.Unlock()
	go c.run()
	if !c.deliver(&inDatagram{data: b, addr: from, at: now, buf: bp}) {
		return false
	}
	return true
}

func initialDecrypts(pkt []byte, h longHeader) bool {
	cp := append([]byte(nil), pkt...)
	_, open := initialKeys(h.dcid, false)
	pnLen, ok := open.hp.unprotect(cp, h.pnOffset)
	if !ok {
		return false
	}
	hdrLen := h.pnOffset + pnLen
	if len(cp) < hdrLen+aeadTagLen {
		return false
	}
	pn := decodePacketNumber(-1, readTruncatedPN(cp[h.pnOffset:], pnLen), pnLen)
	_, err := open.key.open(cp, hdrLen, pn)
	return err == nil
}

func (e *Endpoint) newServerConn(from netip.AddrPort, h longHeader, odcid, scid []byte, retried bool, now time.Time) (*Conn, error) {
	c := newConn(e, e.config, false, now)
	c.odcid = odcid
	c.retried = retried
	if retried {
		c.retrySCID = scid
	}
	c.peerInitialSCID = append([]byte(nil), h.scid...)
	c.gotPeerSCID = true
	c.firstAddr = from
	c.dcid = c.peerInitialSCID
	c.peerCIDs = []*peerCID{{seq: 0, cid: c.peerInitialSCID, inUse: true}}
	c.mu.Lock()
	defer c.mu.Unlock()
	l := c.addLocalCID(scid, false)
	c.active = &path{addr: from, validated: retried, lastSeen: now}
	c.paths = []*path{c.active}
	seal, open := initialKeys(h.dcid, false)
	c.spaces[spaceInitial].seal = seal
	c.spaces[spaceInitial].open = open
	c.buildLocalParams(scid, l.token[:])
	if err := c.startTLS(); err != nil {
		e.removeCIDs(c, [][]byte{scid}, nil)
		return nil, err
	}
	return c, nil
}

func (e *Endpoint) addCID(cid []byte, c *Conn) {
	e.mu.Lock()
	e.conns[string(cid)] = c
	e.mu.Unlock()
}

func (e *Endpoint) removeCID(cid []byte) {
	e.mu.Lock()
	delete(e.conns, string(cid))
	e.mu.Unlock()
}

func (e *Endpoint) addResetToken(t [16]byte, c *Conn) {
	e.mu.Lock()
	e.resetTokens[t] = c
	e.mu.Unlock()
}

func (e *Endpoint) removeResetToken(t [16]byte) {
	e.mu.Lock()
	delete(e.resetTokens, t)
	e.mu.Unlock()
}

func (e *Endpoint) removeCIDs(c *Conn, cids [][]byte, tokens [][16]byte) {
	e.mu.Lock()
	defer e.mu.Unlock()
	for _, cid := range cids {
		if e.conns[string(cid)] == c {
			delete(e.conns, string(cid))
		}
	}
	for _, t := range tokens {
		if e.resetTokens[t] == c {
			delete(e.resetTokens, t)
		}
	}
}

func (e *Endpoint) handshakeFinished(c *Conn) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if c.halfOpen {
		c.halfOpen = false
		e.halfOpen--
	}
}

func (e *Endpoint) enqueueAccept(c *Conn) bool {
	select {
	case e.acceptQ <- c:
		return true
	default:
		return false
	}
}

func (e *Endpoint) removeConn(c *Conn, cids [][]byte, tokens [][16]byte) {
	e.mu.Lock()
	for _, cid := range cids {
		if e.conns[string(cid)] == c {
			delete(e.conns, string(cid))
		}
	}
	if len(c.odcid) > 0 && e.conns[string(c.odcid)] == c {
		delete(e.conns, string(c.odcid))
	}
	for _, t := range tokens {
		if e.resetTokens[t] == c {
			delete(e.resetTokens, t)
		}
	}
	if _, ok := e.all[c]; ok {
		delete(e.all, c)
		if !c.isClient {
			ip := c.firstAddr.Addr()
			if e.perIP[ip] <= 1 {
				delete(e.perIP, ip)
			} else {
				e.perIP[ip]--
			}
		}
	}
	if c.halfOpen {
		c.halfOpen = false
		e.halfOpen--
	}
	e.mu.Unlock()
}

func (e *Endpoint) writeTo(b []byte, to netip.AddrPort) {
	s := e.sock.Load()
	if s.udp != nil {
		s.udp.WriteToUDPAddrPort(b, to)
		return
	}
	s.pc.WriteTo(b, net.UDPAddrFromAddrPort(to))
}

func (e *Endpoint) Accept(ctx context.Context) (*Conn, error) {
	select {
	case c := <-e.acceptQ:
		return c, nil
	case <-e.closeCh:
		return nil, ErrEndpointClosed
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func (e *Endpoint) Dial(ctx context.Context, addr netip.AddrPort, config *Config) (*Conn, error) {
	conf := config.withDefaults()
	now := time.Now()
	c := newConn(e, conf, true, now)
	if conf.TLSConfig != nil {
		c.serverName = conf.TLSConfig.ServerName
	}
	if c.serverName == "" {
		c.serverName = addr.Addr().String()
	}
	c.odcid = randomBytes(minDCIDFromClient)
	c.dcid = c.odcid
	scid := randomBytes(localCIDLen)
	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		return nil, ErrEndpointClosed
	}
	e.all[c] = struct{}{}
	e.mu.Unlock()
	c.mu.Lock()
	c.addLocalCID(scid, false)
	c.active = &path{addr: addr, validated: true, lastSeen: now}
	c.paths = []*path{c.active}
	seal, open := initialKeys(c.odcid, true)
	c.spaces[spaceInitial].seal = seal
	c.spaces[spaceInitial].open = open
	c.buildLocalParams(scid, nil)
	err := c.startTLS()
	c.mu.Unlock()
	if err != nil {
		e.removeConn(c, [][]byte{scid}, nil)
		return nil, err
	}
	go c.run()
	if err := c.waitHandshake(ctx); err != nil {
		return nil, err
	}
	return c, nil
}

func Dial(ctx context.Context, network, address string, config *Config) (*Conn, error) {
	raddr, err := net.ResolveUDPAddr(network, address)
	if err != nil {
		return nil, err
	}
	local := &net.UDPAddr{}
	if raddr.IP.To4() != nil {
		network = "udp4"
	}
	pc, err := net.ListenUDP(network, local)
	if err != nil {
		return nil, err
	}
	e, err := NewEndpoint(pc, config, false)
	if err != nil {
		pc.Close()
		return nil, err
	}
	ap := raddr.AddrPort()
	ap = netip.AddrPortFrom(ap.Addr().Unmap(), ap.Port())
	c, err := e.Dial(ctx, ap, config)
	if err != nil {
		e.Close()
		return nil, err
	}
	c.mu.Lock()
	c.ownsEP = true
	c.mu.Unlock()
	return c, nil
}

func (e *Endpoint) Rebind(pc net.PacketConn) error {
	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		return ErrEndpointClosed
	}
	conns := make([]*Conn, 0, len(e.all))
	for c := range e.all {
		conns = append(conns, c)
	}
	e.mu.Unlock()
	s := newSocket(pc)
	old := e.sock.Swap(s)
	e.wg.Add(1)
	go e.readLoop(s)
	old.pc.Close()
	now := time.Now()
	for _, c := range conns {
		c.mu.Lock()
		c.onRebind(now)
		c.mu.Unlock()
		c.wake()
	}
	return nil
}

func (e *Endpoint) Close() error {
	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		return nil
	}
	e.closed = true
	conns := make([]*Conn, 0, len(e.all))
	for c := range e.all {
		conns = append(conns, c)
	}
	e.mu.Unlock()
	now := time.Now()
	for _, c := range conns {
		c.mu.Lock()
		c.closeWith(ErrEndpointClosed, true, 0, 0, "")
		c.closeUntil = now
		c.mu.Unlock()
		c.wake()
	}
	close(e.closeCh)
	err := e.sock.Load().pc.Close()
	deadline := time.Now().Add(2 * time.Second)
	for _, c := range conns {
		wait := time.Until(deadline)
		if wait < 0 {
			wait = 0
		}
		timer := time.NewTimer(wait)
		select {
		case <-c.finished:
		case <-timer.C:
		}
		timer.Stop()
	}
	e.wg.Wait()
	return err
}

func (e *Endpoint) ConnCount() int {
	e.mu.Lock()
	defer e.mu.Unlock()
	return len(e.all)
}
