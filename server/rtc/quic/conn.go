package quic

import (
	"context"
	"crypto/rand"
	"crypto/tls"
	"net"
	"net/netip"
	"sync"
	"time"
)

type connState int

const (
	stateHandshake connState = iota
	stateActive
	stateClosing
	stateDraining
	stateClosed
)

type notifier struct {
	ch chan struct{}
}

func (n *notifier) wait() <-chan struct{} {
	if n.ch == nil {
		n.ch = make(chan struct{})
	}
	return n.ch
}

func (n *notifier) broadcast() {
	if n.ch != nil {
		close(n.ch)
		n.ch = nil
	}
}

type inDatagram struct {
	data  []byte
	addr  netip.AddrPort
	at    time.Time
	buf   *[]byte
	reset bool
}

type Stats struct {
	PacketsSent       uint64
	PacketsReceived   uint64
	PacketsLost       uint64
	PacketsDropped    uint64
	BytesSent         uint64
	BytesReceived     uint64
	DatagramsSent     uint64
	DatagramsReceived uint64
	DatagramsDropped  uint64
	DatagramsLost     uint64
	SmoothedRTT       time.Duration
	MinRTT            time.Duration
	CongestionWindow  int
	BytesInFlight     int
	KeyUpdates        int
	Migrations        int
	PathChallenges    int

	DatagramsDuplicate  uint64
	DatagramsReinjected uint64
	PathsOpened         int
	PathsAbandoned      int
	PrimarySwitches     int
}

type Conn struct {
	ep        *Endpoint
	config    *Config
	isClient  bool
	ownsEP    bool
	halfOpen  bool
	firstAddr netip.AddrPort

	mu    sync.Mutex
	state connState
	tls   *tls.QUICConn

	inbox  chan *inDatagram
	wakeCh chan struct{}

	spaces [numSpaces]*pnSpace
	keys   appKeys

	odcid            []byte
	retrySCID        []byte
	retried          bool
	peerInitialSCID  []byte
	gotPeerSCID      bool
	token            []byte
	serverName       string
	localParams      transportParams
	peerParams       transportParams
	havePeerParams   bool
	handshakeDone    bool
	confirmed        bool
	handshakeDoneDue bool
	peerValidatedUs  bool
	createdAt        time.Time
	handshakeCh      chan struct{}
	handshakeErr     error

	localCIDs     []*localCID
	nextLocalSeq  uint64
	peerCIDs      []*peerCID
	peerRetireMin uint64
	retireQueue   []uint64
	dcidSeq       uint64
	dcid          []byte

	paths  []*path
	active *path

	recovery
	p0          *pathCtx
	p0list      []*pathCtx
	mp          *multipath
	rxPath      *pathCtx
	mpNotify    notifier
	builtDgrams [][]byte
	dedup       dedupSet

	maxSendSize   int
	peerMaxPacket int

	streams        map[uint64]*Stream
	sendQueue      []*Stream
	controlQueue   []*Stream
	nextBidi       uint64
	nextUni        uint64
	peerMaxBidi    uint64
	peerMaxUni     uint64
	openedPeerBidi uint64
	openedPeerUni  uint64
	maxPeerBidi    uint64
	maxPeerUni     uint64
	acceptBidi     []*Stream
	acceptUni      []*Stream
	acceptNotify   notifier
	openNotify     notifier
	streamsBlocked [2]uint64
	blockedDue     [2]bool

	sendMaxData     uint64
	sentData        uint64
	recvMaxData     uint64
	recvData        uint64
	recvConsumed    uint64
	maxDataDue      bool
	dataBlockedAt   uint64
	dataBlockedDue  bool
	peerMaxDatagram uint64

	dgramOut    []*outDatagram
	dgramIn     []inDatagramRec
	dgramNotify notifier

	idleTimeout         time.Duration
	lastRecv            time.Time
	idleStart           time.Time
	lastSent            time.Time
	elicitingSince      bool
	closeErr            error
	closeFrame          []byte
	closeApp            bool
	closeCode           uint64
	closeFrameType      uint64
	closeReason         string
	closeUntil          time.Time
	lastCloseSent       time.Time
	closedCh            chan struct{}
	finished            chan struct{}
	keepAliveDue        bool
	stats               Stats
	frameScratch        frame
	ackScratch          []ackRange
	payloadScratch      [numSpaces][]byte
	headerScratch       [numSpaces][]byte
	maxStreamsPending   [2]bool
	blockedSent         [2]bool
	pendingResponseAddr netip.AddrPort
	pendingResponses    [][8]byte
	framesScratch       [numSpaces][]sentFrame
	datagramScratch     []byte
	ctx                 context.Context
	cancel              context.CancelFunc
}

func newConn(ep *Endpoint, config *Config, isClient bool, now time.Time) *Conn {
	c := &Conn{
		ep:            ep,
		config:        config,
		isClient:      isClient,
		inbox:         make(chan *inDatagram, 256),
		wakeCh:        make(chan struct{}, 1),
		streams:       make(map[uint64]*Stream),
		handshakeCh:   make(chan struct{}),
		closedCh:      make(chan struct{}),
		finished:      make(chan struct{}),
		createdAt:     now,
		lastRecv:      now,
		idleStart:     now,
		maxSendSize:   minInitialSize,
		peerMaxPacket: minInitialSize,
	}
	c.ctx, c.cancel = context.WithCancel(context.Background())
	for i := range c.spaces {
		c.spaces[i] = newSpace()
		c.payloadScratch[i] = make([]byte, 0, 2048)
		c.headerScratch[i] = make([]byte, 0, 64)
	}
	c.datagramScratch = make([]byte, 0, 2048)
	c.rtt = newRTT(config.InitialRTT)
	c.cc = newCongestion(minInitialSize)
	c.p0 = &pathCtx{id: 0, sp: c.spaces[spaceApp], rec: &c.recovery, createdAt: now}
	c.p0list = []*pathCtx{c.p0}
	c.keys.firstRecvInPhase = -1
	c.recvMaxData = config.ConnectionReceiveWindow
	c.maxPeerBidi = uint64(config.MaxIncomingStreams)
	c.maxPeerUni = uint64(config.MaxIncomingUniStreams)
	c.idleTimeout = config.MaxIdleTimeout
	return c
}

func randomBytes(n int) []byte {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return b
}

func (c *Conn) buildLocalParams(scid []byte, resetToken []byte) {
	p := defaultTransportParams()
	p.maxIdleTimeout = c.config.MaxIdleTimeout
	p.maxUDPPayloadSize = uint64(maxAllowedPacketSize)
	p.initialMaxData = c.config.ConnectionReceiveWindow
	p.initialMaxStreamDataBL = c.config.StreamReceiveWindow
	p.initialMaxStreamDataBR = c.config.StreamReceiveWindow
	p.initialMaxStreamDataUni = c.config.StreamReceiveWindow
	p.initialMaxStreamsBidi = uint64(c.config.MaxIncomingStreams)
	p.initialMaxStreamsUni = uint64(c.config.MaxIncomingUniStreams)
	p.ackDelayExponent = 3
	p.maxAckDelay = 25 * time.Millisecond
	p.activeCIDLimit = uint64(c.config.ActiveConnectionIDLimit)
	p.initialSCID = scid
	p.hasInitialSCID = true
	p.maxDatagramFrameSize = uint64(c.config.MaxDatagramFrameSize)
	if c.config.Multipath {
		p.maxPathID = uint64(c.config.MaxPaths - 1)
		p.hasMaxPathID = true
	}
	if !c.isClient {
		p.originalDCID = c.odcid
		p.hasOriginalDCID = true
		p.statelessResetToken = resetToken
		if c.retried {
			p.retrySCID = c.retrySCID
			p.hasRetrySCID = true
		}
	}
	c.localParams = p
}

func (c *Conn) tlsConfig() *tls.Config {
	conf := c.config.TLSConfig
	if conf == nil {
		conf = &tls.Config{}
	}
	conf = conf.Clone()
	conf.MinVersion = tls.VersionTLS13
	if c.isClient && conf.ServerName == "" {
		conf.ServerName = c.serverName
	}
	return conf
}

func (c *Conn) startTLS() error {
	qc := &tls.QUICConfig{TLSConfig: c.tlsConfig()}
	if c.isClient {
		c.tls = tls.QUICClient(qc)
	} else {
		c.tls = tls.QUICServer(qc)
	}
	c.tls.SetTransportParameters(c.localParams.marshal(!c.isClient))
	if err := c.tls.Start(c.ctx); err != nil {
		return err
	}
	return c.processTLSEvents()
}

func (c *Conn) wake() {
	select {
	case c.wakeCh <- struct{}{}:
	default:
	}
}

func (c *Conn) deliver(d *inDatagram) bool {
	select {
	case c.inbox <- d:
		return true
	default:
		return false
	}
}

func (c *Conn) run() {
	timer := time.NewTimer(time.Hour)
	defer timer.Stop()
	for {
		c.mu.Lock()
		now := time.Now()
		c.onTimers(now)
		c.flush(now)
		if c.state == stateClosed {
			c.mu.Unlock()
			c.teardown()
			return
		}
		next := c.nextDeadline(now)
		c.mu.Unlock()
		wait := next.Sub(time.Now())
		if wait < 0 {
			wait = 0
		}
		timer.Reset(wait)
		select {
		case d := <-c.inbox:
			c.mu.Lock()
			c.handleDatagram(d)
			for i := 0; i < 63; i++ {
				select {
				case d = <-c.inbox:
					c.handleDatagram(d)
					continue
				default:
				}
				break
			}
			c.mu.Unlock()
		case <-c.wakeCh:
		case <-timer.C:
		}
		if !timer.Stop() {
			select {
			case <-timer.C:
			default:
			}
		}
	}
}

func (c *Conn) teardown() {
	c.mu.Lock()
	var cids [][]byte
	for _, l := range c.localCIDs {
		cids = append(cids, l.cid)
	}
	var tokens [][16]byte
	for _, p := range c.peerCIDs {
		if p.hasToken {
			tokens = append(tokens, p.token)
		}
	}
	var socks []*socket
	if c.mp != nil {
		for _, set := range c.mp.cids {
			for _, l := range set.local {
				cids = append(cids, l.cid)
			}
			for _, p := range set.peer {
				if p.hasToken {
					tokens = append(tokens, p.token)
				}
			}
		}
		for _, pc := range c.mp.paths {
			if pc.ownsSock && pc.sock != nil {
				socks = append(socks, pc.sock)
			}
		}
	}
	tc := c.tls
	c.mu.Unlock()
	for _, s := range socks {
		c.ep.closePathSocket(s)
	}
	if tc != nil {
		tc.Close()
	}
	c.cancel()
	c.ep.removeConn(c, cids, tokens)
	for {
		select {
		case d := <-c.inbox:
			c.ep.release(d)
			continue
		default:
		}
		break
	}
	close(c.finished)
	if c.ownsEP {
		c.ep.Close()
	}
}

func (c *Conn) pto() time.Duration {
	return c.pathPTO(c.p0)
}

func (c *Conn) pathPTO(pc *pathCtx) time.Duration {
	ack := time.Duration(0)
	if c.confirmed {
		ack = c.peerParams.maxAckDelay
	}
	return pc.rec.rtt.pto(ack)
}

func (c *Conn) maxPTO() time.Duration {
	if c.mp == nil {
		return c.pto()
	}
	var m time.Duration
	for _, pc := range c.mp.order {
		if d := c.pathPTO(pc); d > m {
			m = d
		}
	}
	if m == 0 {
		m = c.pto()
	}
	return m
}

func (c *Conn) nextDeadline(now time.Time) time.Time {
	next := now.Add(time.Hour)
	consider := func(t time.Time) {
		if !t.IsZero() && t.Before(next) {
			next = t
		}
	}
	switch c.state {
	case stateClosing, stateDraining:
		consider(c.closeUntil)
		return next
	}
	consider(c.lossTimer)
	sp := c.spaces[spaceApp]
	if !sp.discarded {
		consider(sp.ackDeadline)
	}
	consider(c.idleDeadline())
	if !c.handshakeDone {
		consider(c.createdAt.Add(c.config.HandshakeTimeout))
	}
	if c.config.KeepAlivePeriod > 0 && c.handshakeDone {
		consider(c.lastSent.Add(c.config.KeepAlivePeriod))
	}
	for _, p := range c.paths {
		consider(p.nextTimer())
	}
	if c.mp != nil {
		c.multipathDeadlines(now, consider)
	}
	if !c.keys.prevUntil.IsZero() {
		consider(c.keys.prevUntil)
	}
	if t := c.pacingDeadline(now); !t.IsZero() {
		consider(t)
	}
	return next
}

func (c *Conn) idleDeadline() time.Time {
	d := c.idleTimeout
	if min := 3 * c.maxPTO(); d < min {
		d = min
	}
	return c.idleStart.Add(d)
}

func (c *Conn) onTimers(now time.Time) {
	switch c.state {
	case stateClosing, stateDraining:
		if !now.Before(c.closeUntil) {
			c.state = stateClosed
		}
		return
	case stateClosed:
		return
	}
	if !c.handshakeDone && !now.Before(c.createdAt.Add(c.config.HandshakeTimeout)) {
		c.abort(ErrHandshakeTimeout)
		return
	}
	if !now.Before(c.idleDeadline()) {
		c.abort(ErrIdleTimeout)
		return
	}
	if !c.lossTimer.IsZero() && !now.Before(c.lossTimer) {
		c.onLossTimer(c.p0, now)
	}
	if c.mp != nil {
		c.onMultipathTimers(now)
	}
	if c.config.KeepAlivePeriod > 0 && c.handshakeDone && !now.Before(c.lastSent.Add(c.config.KeepAlivePeriod)) {
		c.keepAliveDue = true
	}
	if !c.keys.prevUntil.IsZero() && !now.Before(c.keys.prevUntil) {
		c.keys.readPrev = nil
		c.keys.prevUntil = time.Time{}
	}
	c.onPathTimers(now)
}

func (c *Conn) abort(err error) {
	if c.state >= stateClosing {
		c.state = stateClosed
		return
	}
	c.setCloseErr(err)
	c.state = stateClosed
}

func (c *Conn) setCloseErr(err error) {
	if c.closeErr != nil {
		return
	}
	c.closeErr = err
	close(c.closedCh)
	if !c.handshakeDone && c.handshakeErr == nil {
		c.handshakeErr = err
		close(c.handshakeCh)
	}
	c.notifyAll()
}

func (c *Conn) notifyAll() {
	c.acceptNotify.broadcast()
	c.openNotify.broadcast()
	c.dgramNotify.broadcast()
	c.mpNotify.broadcast()
	for _, s := range c.streams {
		s.readNotify.broadcast()
		s.writeNotify.broadcast()
	}
	for _, s := range c.acceptBidi {
		s.readNotify.broadcast()
		s.writeNotify.broadcast()
	}
	for _, s := range c.acceptUni {
		s.readNotify.broadcast()
	}
}

func (c *Conn) closeWith(err error, app bool, code, frameType uint64, reason string) {
	if c.state >= stateClosing {
		return
	}
	c.setCloseErr(err)
	c.state = stateClosing
	c.closeApp = app
	c.closeCode = code
	c.closeFrameType = frameType
	c.closeReason = reason
	c.closeUntil = time.Now().Add(3 * c.maxPTO())
	c.sendClose(time.Now())
}

func (c *Conn) closeTransport(e *TransportError) {
	c.closeWith(e, false, uint64(e.Code), e.FrameType, e.Reason)
}

func (c *Conn) enterDraining(err error) {
	if c.state >= stateClosing {
		return
	}
	c.setCloseErr(err)
	c.state = stateDraining
	c.closeUntil = time.Now().Add(3 * c.maxPTO())
}

func (c *Conn) CloseWithError(code uint64, reason string) error {
	c.mu.Lock()
	c.closeWith(&ApplicationError{Code: code, Reason: reason}, true, code, 0, reason)
	c.mu.Unlock()
	c.wake()
	return nil
}

func (c *Conn) Close() error {
	return c.CloseWithError(0, "")
}

func (c *Conn) Context() context.Context { return c.ctx }

func (c *Conn) Done() <-chan struct{} { return c.closedCh }

func (c *Conn) Err() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.closeErr
}

func (c *Conn) Finished() <-chan struct{} { return c.finished }

func (c *Conn) HandshakeComplete() <-chan struct{} { return c.handshakeCh }

func (c *Conn) ConnectionState() tls.ConnectionState {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.tls == nil {
		return tls.ConnectionState{}
	}
	return c.tls.ConnectionState()
}

func (c *Conn) RemoteAddr() net.Addr {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.mp != nil {
		if pc := c.primaryPath(); pc != nil && c.activeOf(pc) != nil {
			return net.UDPAddrFromAddrPort(c.activeOf(pc).addr)
		}
	}
	return net.UDPAddrFromAddrPort(c.active.addr)
}

func (c *Conn) LocalAddr() net.Addr { return c.ep.LocalAddr() }

func (c *Conn) Stats() Stats {
	c.mu.Lock()
	defer c.mu.Unlock()
	s := c.stats
	rec := &c.recovery
	if c.mp != nil {
		if pc := c.primaryPath(); pc != nil {
			rec = pc.rec
		}
		s.BytesInFlight = 0
		for _, pc := range c.mp.order {
			s.BytesInFlight += pc.rec.cc.inFlight
		}
	} else {
		s.BytesInFlight = rec.cc.inFlight
	}
	s.SmoothedRTT = rec.rtt.smoothed
	s.MinRTT = rec.rtt.min
	s.CongestionWindow = rec.cc.cwnd
	return s
}

func (c *Conn) waitHandshake(ctx context.Context) error {
	select {
	case <-c.handshakeCh:
	case <-ctx.Done():
		c.CloseWithError(0, "")
		return ctx.Err()
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.handshakeErr
}

func (c *Conn) completeHandshake() {
	if c.handshakeErr == nil {
		select {
		case <-c.handshakeCh:
		default:
			close(c.handshakeCh)
		}
	}
}

func (c *Conn) Retried() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.retried
}
