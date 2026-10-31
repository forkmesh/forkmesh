package quic

import (
	"bytes"
	"context"
	"net"
	"net/netip"
	"sort"
	"time"
)

type SendPolicy uint8

const (
	Fastest SendPolicy = iota
	Redundant
	Bulk
)

func (p SendPolicy) String() string {
	switch p {
	case Fastest:
		return "fastest"
	case Redundant:
		return "redundant"
	case Bulk:
		return "bulk"
	}
	return "unknown"
}

const (
	redundantCopyWait = 100 * time.Millisecond
	minSuspectTimeout = 15 * time.Millisecond
	suspectMargin     = 4 * time.Millisecond
	maxCIDsPerPath    = 2
	dedupWindow       = 4096
	lossAlpha         = 1.0 / 16
)

type outDatagram struct {
	data      []byte
	policy    SendPolicy
	sentOn    []uint64
	firstSent time.Time
}

type inDatagramRec struct {
	data []byte
	path uint64
}

type dedupSet struct {
	seen map[uint64]struct{}
	ring []uint64
	next int
}

func (d *dedupSet) add(k uint64) bool {
	if d.seen == nil {
		d.seen = make(map[uint64]struct{}, 256)
	}
	if _, ok := d.seen[k]; ok {
		return false
	}
	if len(d.ring) < dedupWindow {
		d.ring = append(d.ring, k)
	} else {
		delete(d.seen, d.ring[d.next])
		d.ring[d.next] = k
		d.next = (d.next + 1) % dedupWindow
	}
	d.seen[k] = struct{}{}
	return true
}

type pathCIDs struct {
	local     []*localCID
	nextSeq   uint64
	peer      []*peerCID
	peerNext  uint64
	retireMin uint64
	retire    []uint64
	dcidSeq   uint64
	hasDCID   bool
	blocked   bool
}

type pathCtx struct {
	id              uint64
	sp              *pnSpace
	rec             *recovery
	own             recovery
	addr            *path
	sock            *socket
	ownsSock        bool
	backup          bool
	statusDue       bool
	statusSeq       uint64
	statusNext      uint64
	peerStatusKnown bool
	peerStatusSeq   uint64
	peerBackup      bool
	abandoned       bool
	abandonAcked    bool
	removeAt        time.Time
	removed         bool
	lastHeard       time.Time
	lastAcked       time.Time
	lastElicit      time.Time
	loss            float64
	sent            uint64
	lost            uint64
	recv            uint64
	pingDue         bool
	crossRTT        bool
	validCh         chan struct{}
	validErr        error
	validDone       bool
	createdAt       time.Time
}

type abandonRec struct {
	id   uint64
	code uint64
}

type earlyStatus struct {
	seq    uint64
	backup bool
}

type multipath struct {
	localMax        uint64
	peerMax         uint64
	peerInitialMax  uint64
	maxDue          bool
	pathsBlockedDue bool
	paths           map[uint64]*pathCtx
	order           []*pathCtx
	cids            map[uint64]*pathCIDs
	floor           uint64
	consumed        map[uint64]bool
	abandonQ        []abandonRec
	early           map[uint64]earlyStatus
	primary         *pathCtx
	retained        int
	pendingRaise    int
}

func (m *multipath) isConsumed(id uint64) bool {
	return id < m.floor || m.consumed[id]
}

func (m *multipath) markConsumed(id uint64) {
	delete(m.early, id)
	if id < m.floor {
		return
	}
	m.consumed[id] = true
	for m.consumed[m.floor] {
		delete(m.consumed, m.floor)
		m.floor++
	}
}

func (m *multipath) cidSet(id uint64) *pathCIDs {
	set := m.cids[id]
	if set == nil {
		set = &pathCIDs{}
		m.cids[id] = set
	}
	return set
}

func (c *Conn) enableMultipath(peerMax uint64) {
	c.mp = &multipath{
		localMax:       uint64(c.config.MaxPaths - 1),
		peerMax:        peerMax,
		peerInitialMax: peerMax,
		paths:          make(map[uint64]*pathCtx),
		cids:           make(map[uint64]*pathCIDs),
		consumed:       make(map[uint64]bool),
		early:          make(map[uint64]earlyStatus),
		floor:          1,
	}
	c.mp.order = []*pathCtx{c.p0}
	c.mp.primary = c.p0
}

func (c *Conn) newPathCtx(id uint64, now time.Time) *pathCtx {
	pc := &pathCtx{id: id, sp: newSpace(), createdAt: now}
	pc.own.rtt = newRTT(c.config.InitialRTT)
	pc.own.cc = newCongestion(minInitialSize)
	pc.rec = &pc.own
	return pc
}

func (c *Conn) attachPathCtx(pc *pathCtx, now time.Time) {
	m := c.mp
	if es, ok := m.early[pc.id]; ok {
		pc.peerStatusKnown, pc.peerStatusSeq, pc.peerBackup = true, es.seq, es.backup
	}
	m.paths[pc.id] = pc
	m.order = append(m.order, pc)
	sort.Slice(m.order, func(i, j int) bool { return m.order[i].id < m.order[j].id })
	m.markConsumed(pc.id)
	c.stats.PathsOpened++
}

func (c *Conn) activeOf(pc *pathCtx) *path {
	if pc == nil {
		return nil
	}
	if pc == c.p0 {
		return c.active
	}
	return pc.addr
}

func (c *Conn) setActive(pc *pathCtx, p *path) {
	if pc == c.p0 {
		c.active = p
		return
	}
	pc.addr = p
}

func (c *Conn) pathSpace(pc *pathCtx, s int) *pnSpace {
	if pc == c.p0 {
		return c.spaces[s]
	}
	if s == spaceApp {
		return pc.sp
	}
	return nil
}

func (c *Conn) recvPath() *pathCtx {
	if c.rxPath != nil {
		return c.rxPath
	}
	return c.p0
}

func (c *Conn) pathByID(id uint64) *pathCtx {
	if id == 0 {
		if c.p0.removed {
			return nil
		}
		return c.p0
	}
	if c.mp == nil {
		return nil
	}
	return c.mp.paths[id]
}

func (c *Conn) sendPaths() []*pathCtx {
	if c.mp == nil {
		return c.p0list
	}
	return c.mp.order
}

func (c *Conn) usable(pc *pathCtx) bool {
	if pc == nil || pc.abandoned || pc.removed {
		return false
	}
	p := c.activeOf(pc)
	if p == nil {
		return false
	}
	if pc != c.p0 {
		_, ok := c.dcidOf(pc)
		return p.validated && ok
	}
	return true
}

func (c *Conn) openPaths() int {
	n := 0
	for _, pc := range c.sendPaths() {
		if !pc.abandoned && !pc.removed {
			n++
		}
	}
	return n
}

func (c *Conn) dcidOf(pc *pathCtx) ([]byte, bool) {
	if pc == c.p0 {
		return c.dcid, true
	}
	set := c.mp.cids[pc.id]
	if set == nil {
		return nil, false
	}
	if set.hasDCID {
		for _, q := range set.peer {
			if q.seq == set.dcidSeq {
				return q.cid, true
			}
		}
		set.hasDCID = false
	}
	var best *peerCID
	for _, q := range set.peer {
		if !q.inUse && (best == nil || q.seq < best.seq) {
			best = q
		}
	}
	if best == nil {
		return nil, false
	}
	best.inUse = true
	set.dcidSeq, set.hasDCID = best.seq, true
	return best.cid, true
}

func (c *Conn) mpLocalCIDFor(dcid []byte) (*localCID, uint64) {
	for id, set := range c.mp.cids {
		for _, l := range set.local {
			if !l.retired && bytes.Equal(l.cid, dcid) {
				return l, id
			}
		}
	}
	return nil, 0
}

func (c *Conn) peekPath(b []byte) *pathCtx {
	if len(b) == 0 {
		return nil
	}
	if isLongHeader(b[0]) {
		return c.p0
	}
	if len(b) < 1+localCIDLen {
		return nil
	}
	dcid := b[1 : 1+localCIDLen]
	if c.localCIDFor(dcid) != nil {
		return c.p0
	}
	if c.mp != nil {
		if l, id := c.mpLocalCIDFor(dcid); l != nil {
			return c.mp.paths[id]
		}
	}
	return nil
}

func (c *Conn) pathForCID(dcid []byte, now time.Time) (*pathCtx, *localCID, bool) {
	if l := c.localCIDFor(dcid); l != nil {
		if c.p0.removed {
			return nil, nil, false
		}
		return c.p0, l, false
	}
	if c.mp == nil {
		return nil, nil, false
	}
	l, id := c.mpLocalCIDFor(dcid)
	if l == nil {
		return nil, nil, false
	}
	if pc := c.mp.paths[id]; pc != nil {
		return pc, l, false
	}
	if !c.canAcceptPath(id) {
		return nil, nil, false
	}
	return c.newPathCtx(id, now), l, true
}

func (c *Conn) canAcceptPath(id uint64) bool {
	m := c.mp
	if c.isClient || !c.confirmed || id == 0 || id > m.localMax || m.isConsumed(id) {
		return false
	}
	return c.openPaths() < c.config.MaxPaths && len(m.paths) < 2*c.config.MaxPaths
}

func (c *Conn) pathCIDLimit() int {
	limit := int(c.peerParams.activeCIDLimit)
	if limit > c.config.ActiveConnectionIDLimit {
		limit = c.config.ActiveConnectionIDLimit
	}
	if limit > maxCIDsPerPath {
		limit = maxCIDsPerPath
	}
	if limit < 1 {
		limit = 1
	}
	return limit
}

func (c *Conn) issuePathCIDs() {
	if c.mp == nil || !c.handshakeDone || c.state >= stateClosing {
		return
	}
	m := c.mp
	max := m.localMax
	if m.peerMax < max {
		max = m.peerMax
	}
	limit := c.pathCIDLimit()
	ensure := func(id uint64) {
		set := m.cidSet(id)
		for len(set.local) < limit {
			cid := randomBytes(localCIDLen)
			l := &localCID{seq: set.nextSeq, cid: cid, token: c.ep.resetToken(cid), sendDue: true}
			set.nextSeq++
			set.local = append(set.local, l)
			c.ep.addCID(cid, c)
		}
	}
	for id := m.floor; id <= max && id >= m.floor; id++ {
		if m.isConsumed(id) {
			if pc := m.paths[id]; pc == nil || pc.abandoned {
				continue
			}
		}
		ensure(id)
	}
	for _, pc := range m.order {
		if pc != c.p0 && !pc.abandoned && !pc.removed && pc.id < m.floor {
			ensure(pc.id)
		}
	}
}

func (c *Conn) queuePathRetire(set *pathCIDs, seq uint64) *TransportError {
	for _, s := range set.retire {
		if s == seq {
			return nil
		}
	}
	if len(set.retire) >= maxPendingRetires {
		return transportErr(ConnectionIDLimitError, "too many path connection ids awaiting retirement")
	}
	set.retire = append(set.retire, seq)
	return nil
}

func (c *Conn) onMultipathFrame(f *frame, dcid []byte, now time.Time) *TransportError {
	m := c.mp
	if m == nil {
		return frameErr(ProtocolViolation, f.typ, "multipath frame without negotiation")
	}
	switch f.typ {
	case frameMaxPathID:
		if f.max > maxPathIDValue || f.max < m.peerInitialMax {
			return frameErr(ProtocolViolation, f.typ, "invalid maximum path id")
		}
		if f.max > m.peerMax {
			m.peerMax = f.max
			c.issuePathCIDs()
			c.mpNotify.broadcast()
		}
		return nil
	case framePathsBlocked:
		if f.max > m.localMax {
			return frameErr(ProtocolViolation, f.typ, "paths_blocked above our maximum")
		}
		return nil
	}
	if f.pathID > m.localMax {
		return frameErr(ProtocolViolation, f.typ, "path id above the maximum")
	}
	switch f.typ {
	case framePathAck, framePathAckECN:
		return c.onAck(spaceApp, f, now)
	case framePathAbandon:
		c.onPathAbandonFrame(f.pathID, now)
		return nil
	case framePathStatusBackup, framePathStatusAvailable:
		c.onPathStatusFrame(f.pathID, f.seq, f.typ == framePathStatusBackup)
		return nil
	case framePathNewConnectionID:
		if f.pathID == 0 {
			return c.onNewConnectionID(f)
		}
		return c.onPathNewCID(f)
	case framePathRetireConnectionID:
		if f.pathID == 0 {
			return c.onRetireConnectionID(f.seq, dcid)
		}
		return c.onPathRetireCID(f.pathID, f.seq, dcid)
	case framePathCIDsBlocked:
		next := c.nextLocalSeq
		if f.pathID != 0 {
			next = 0
			if set := m.cids[f.pathID]; set != nil {
				next = set.nextSeq
			}
		}
		if f.seq > next {
			return frameErr(ProtocolViolation, f.typ, "path_cids_blocked beyond issued sequence")
		}
		c.issuePathCIDs()
		return nil
	}
	return frameErr(FrameEncodingError, f.typ, "unknown frame")
}

func (c *Conn) onPathNewCID(f *frame) *TransportError {
	m := c.mp
	id := f.pathID
	pc := m.paths[id]
	if (pc != nil && pc.abandoned) || (pc == nil && m.isConsumed(id)) {
		return nil
	}
	set := m.cidSet(id)
	for _, p := range set.peer {
		if p.seq == f.seq {
			if !bytes.Equal(p.cid, f.data) || p.token != f.resetToken {
				return frameErr(ProtocolViolation, f.typ, "path connection id sequence reused")
			}
			return nil
		}
		if bytes.Equal(p.cid, f.data) {
			return frameErr(ProtocolViolation, f.typ, "path connection id reused")
		}
	}
	if f.seq < set.retireMin {
		return c.queuePathRetire(set, f.seq)
	}
	p := &peerCID{seq: f.seq, cid: append([]byte(nil), f.data...), token: f.resetToken, hasToken: true}
	set.peer = append(set.peer, p)
	c.ep.addResetToken(p.token, c)
	if f.seq+1 > set.peerNext {
		set.peerNext = f.seq + 1
	}
	if f.retirePrior > set.retireMin {
		set.retireMin = f.retirePrior
		kept := set.peer[:0]
		for _, q := range set.peer {
			if q.seq < set.retireMin {
				if q.hasToken {
					c.ep.removeResetToken(q.token)
				}
				if err := c.queuePathRetire(set, q.seq); err != nil {
					return err
				}
				if set.hasDCID && set.dcidSeq == q.seq {
					set.hasDCID = false
				}
				continue
			}
			kept = append(kept, q)
		}
		set.peer = kept
	}
	if len(set.peer) > c.config.ActiveConnectionIDLimit {
		return frameErr(ConnectionIDLimitError, f.typ, "too many connection ids for the path")
	}
	set.blocked = false
	c.mpNotify.broadcast()
	return nil
}

func (c *Conn) onPathRetireCID(id, seq uint64, packetDCID []byte) *TransportError {
	set := c.mp.cids[id]
	if set == nil {
		return nil
	}
	if seq >= set.nextSeq {
		return frameErr(ProtocolViolation, framePathRetireConnectionID, "retiring an unissued path connection id")
	}
	kept := set.local[:0]
	for _, l := range set.local {
		if l.seq == seq {
			if bytes.Equal(l.cid, packetDCID) {
				return frameErr(ProtocolViolation, framePathRetireConnectionID, "retiring the connection id in use")
			}
			c.ep.removeCID(l.cid)
			continue
		}
		kept = append(kept, l)
	}
	set.local = kept
	if pc := c.mp.paths[id]; pc == nil || !pc.abandoned {
		c.issuePathCIDs()
	}
	return nil
}

func (c *Conn) onPathStatusFrame(id, seq uint64, backup bool) {
	m := c.mp
	pc := c.pathByID(id)
	if pc == nil {
		if id == 0 || m.isConsumed(id) {
			return
		}
		es, ok := m.early[id]
		if ok && es.seq >= seq {
			return
		}
		if !ok && len(m.early) >= 2*c.config.MaxPaths {
			return
		}
		m.early[id] = earlyStatus{seq: seq, backup: backup}
		return
	}
	if pc.peerStatusKnown && seq <= pc.peerStatusSeq {
		return
	}
	pc.peerStatusKnown, pc.peerStatusSeq, pc.peerBackup = true, seq, backup
}

func (c *Conn) onPathAbandonFrame(id uint64, now time.Time) {
	m := c.mp
	pc := c.pathByID(id)
	if pc == nil {
		if id != 0 && !m.isConsumed(id) {
			m.markConsumed(id)
			if set := m.cids[id]; set != nil {
				for _, l := range set.local {
					c.ep.removeCID(l.cid)
				}
				for _, p := range set.peer {
					if p.hasToken {
						c.ep.removeResetToken(p.token)
					}
				}
				delete(m.cids, id)
			}
			c.queueAbandon(id, pathAbandonNoError)
			m.retained++
			m.pendingRaise++
			c.releaseRetained()
		}
		return
	}
	if !pc.abandoned {
		c.abandonPath(pc, pathAbandonNoError, ErrPathAbandoned, now)
	}
	if c.primaryPath() == nil && c.state < stateClosing {
		c.closeTransport(transportErr(NoError, "last path abandoned"))
	}
}

func (c *Conn) releaseRetained() {
	m := c.mp
	if m.retained > 0 {
		m.retained--
	}
	if m.pendingRaise > 0 {
		m.pendingRaise--
		c.raiseMaxPathID()
	}
}

func (c *Conn) queueAbandon(id, code uint64) {
	for _, a := range c.mp.abandonQ {
		if a.id == id {
			return
		}
	}
	c.mp.abandonQ = append(c.mp.abandonQ, abandonRec{id: id, code: code})
}

func (c *Conn) onAbandonAcked(id uint64) {
	if pc := c.pathByID(id); pc != nil {
		pc.abandonAcked = true
	}
}

func (c *Conn) requeuePathFrame(f sentFrame) {
	m := c.mp
	if m == nil {
		return
	}
	switch f.kind {
	case sfPathAbandon:
		if pc := c.pathByID(f.id); pc != nil && pc.abandonAcked {
			return
		}
		c.queueAbandon(f.id, f.offset)
	case sfPathStatus:
		if pc := c.pathByID(f.id); pc != nil && !pc.abandoned && pc.statusSeq == f.offset {
			pc.statusDue = true
		}
	case sfPathNewCID:
		if set := m.cids[f.id]; set != nil {
			for _, l := range set.local {
				if l.seq == f.offset {
					l.sendDue = true
				}
			}
		}
	case sfPathRetireCID:
		if set := m.cids[f.id]; set != nil {
			c.queuePathRetire(set, f.offset)
		}
	case sfMaxPathID:
		if f.id == m.localMax {
			m.maxDue = true
		}
	}
}

func (c *Conn) abandonPath(pc *pathCtx, code uint64, err error, now time.Time) {
	if pc.abandoned || pc.removed {
		return
	}
	m := c.mp
	pc.abandoned = true
	c.stats.PathsAbandoned++
	c.queueAbandon(pc.id, code)
	if pc == c.p0 {
		for _, q := range c.peerCIDs {
			if q.hasToken {
				c.ep.removeResetToken(q.token)
				q.hasToken = false
			}
		}
	} else if set := m.cids[pc.id]; set != nil {
		for _, q := range set.peer {
			if q.hasToken {
				c.ep.removeResetToken(q.token)
			}
		}
		set.peer = nil
		set.retire = nil
		set.hasDCID = false
	}
	for _, p := range c.paths {
		if p.pc == pc {
			p.challenges = nil
			p.challengeDue = false
			p.responses = nil
			p.migrate = false
			p.deadline = time.Time{}
			p.nextChallenge = time.Time{}
		}
	}
	c.discardPathPackets(pc)
	pc.statusDue = false
	pc.pingDue = false
	pc.removeAt = now.Add(3 * c.maxPTO())
	m.retained++
	if m.primary == pc {
		m.primary = nil
	}
	c.raiseMaxPathID()
	c.finishValidation(pc, err)
	c.mpNotify.broadcast()
}

func (c *Conn) raiseMaxPathID() {
	m := c.mp
	if m.retained > c.config.MaxPaths {
		m.pendingRaise++
		return
	}
	if m.localMax >= maxPathIDValue {
		return
	}
	m.localMax++
	m.maxDue = true
	c.issuePathCIDs()
}

func (c *Conn) discardPathPackets(pc *pathCtx) {
	sp := pc.sp
	var re []*outDatagram
	for _, p := range sp.sent {
		if p.size < 0 {
			continue
		}
		if p.inFlight {
			pc.rec.cc.remove(p.size)
		}
		for _, f := range p.frames {
			switch f.kind {
			case sfDatagram, sfAck, sfPing:
			default:
				c.requeueFrame(spaceApp, f)
			}
		}
		for _, d := range p.dgrams {
			re = append(re, &outDatagram{data: d})
		}
		p.dgrams = nil
		p.size = -1
	}
	sp.sent = nil
	sp.elicitingInFlight = 0
	sp.lossTime = time.Time{}
	sp.probes = 0
	pc.rec.lossTimer = time.Time{}
	c.prependDatagrams(re)
}

func (c *Conn) prependDatagrams(re []*outDatagram) {
	if len(re) == 0 {
		return
	}
	c.stats.DatagramsReinjected += uint64(len(re))
	c.dgramOut = append(re, c.dgramOut...)
}

func (c *Conn) removePathCtx(pc *pathCtx) {
	m := c.mp
	pc.removed = true
	pc.removeAt = time.Time{}
	kept := c.paths[:0]
	for _, p := range c.paths {
		if p.pc != pc || p == c.active {
			kept = append(kept, p)
		}
	}
	for i := len(kept); i < len(c.paths); i++ {
		c.paths[i] = nil
	}
	c.paths = kept
	if pc == c.p0 {
		pc.sp.ackImmediate = false
		pc.sp.ackDeadline = time.Time{}
		pc.sp.newSinceAck = false
	} else {
		if set := m.cids[pc.id]; set != nil {
			for _, l := range set.local {
				c.ep.removeCID(l.cid)
			}
			for _, p := range set.peer {
				if p.hasToken {
					c.ep.removeResetToken(p.token)
				}
			}
			delete(m.cids, pc.id)
		}
		delete(m.paths, pc.id)
	}
	order := m.order[:0]
	for _, q := range m.order {
		if q != pc {
			order = append(order, q)
		}
	}
	m.order = order
	if m.primary == pc {
		m.primary = nil
	}
	if pc.ownsSock && pc.sock != nil {
		c.ep.closePathSocket(pc.sock)
		pc.ownsSock = false
	}
	c.releaseRetained()
	c.mpNotify.broadcast()
}

func (c *Conn) finishValidation(pc *pathCtx, err error) {
	if pc.validDone {
		return
	}
	pc.validDone = true
	pc.validErr = err
	if pc.validCh != nil {
		close(pc.validCh)
	}
}

func (c *Conn) onPathValidated(pc *pathCtx, now time.Time) {
	if pc.validDone {
		return
	}
	pc.lastElicit = now
	c.finishValidation(pc, nil)
	c.mpNotify.broadcast()
}

func (c *Conn) onPathValidationFailed(pc *pathCtx, now time.Time) {
	c.abandonPath(pc, pathAbandonNoError, ErrPathValidation, now)
}

func (c *Conn) primaryPath() *pathCtx {
	if c.mp == nil {
		return c.p0
	}
	if c.mp.primary != nil && c.usable(c.mp.primary) {
		return c.mp.primary
	}
	for _, pc := range c.mp.order {
		if c.usable(pc) {
			return pc
		}
	}
	return nil
}

func (c *Conn) isPrimary(pc *pathCtx) bool {
	return pc == c.primaryPath()
}

func (c *Conn) closePath() *pathCtx {
	if c.mp == nil {
		return c.p0
	}
	return c.primaryPath()
}

func (c *Conn) oldestUnacked(pc *pathCtx) *sentPacket {
	for _, p := range pc.sp.sent {
		if p.ackEliciting && p.inFlight && p.size >= 0 {
			return p
		}
	}
	return nil
}

func carriesDatagram(p *sentPacket) bool {
	for _, f := range p.frames {
		if f.kind == sfDatagram {
			return true
		}
	}
	return false
}

func (c *Conn) suspectTimeout(pc *pathCtx, p *sentPacket) time.Duration {
	r := &pc.rec.rtt
	v := 4 * r.variance
	if q := r.smoothed / 4; v < q {
		v = q
	}
	if v < suspectMargin {
		v = suspectMargin
	}
	t := r.smoothed + v
	if !carriesDatagram(p) {
		t += c.peerParams.maxAckDelay
	}
	if t < minSuspectTimeout {
		t = minSuspectTimeout
	}
	return t
}

func (c *Conn) healthy(pc *pathCtx, now time.Time) bool {
	if !c.usable(pc) || pc.rec.ptoCount > 0 {
		return false
	}
	if o := c.oldestUnacked(pc); o != nil && now.Sub(o.sentAt) > c.suspectTimeout(pc, o) {
		return false
	}
	return true
}

func (c *Conn) pathClass(pc *pathCtx, now time.Time) int {
	class := 0
	if !c.healthy(pc, now) {
		class += 2
	}
	if pc.backup || pc.peerBackup {
		class++
	}
	return class
}

func betterInClass(a, b *pathCtx, class int) bool {
	if class >= 2 {
		return a.lastAcked.After(b.lastAcked)
	}
	return a.rec.rtt.smoothed < b.rec.rtt.smoothed
}

func (c *Conn) prepareSchedule(now time.Time) {
	c.selectPrimary(now)
	c.pruneDatagrams(now)
}

func (c *Conn) selectPrimary(now time.Time) {
	m := c.mp
	cur := m.primary
	if cur != nil && !c.usable(cur) {
		cur = nil
	}
	var best *pathCtx
	bestClass := 0
	for _, pc := range m.order {
		if !c.usable(pc) {
			continue
		}
		cl := c.pathClass(pc, now)
		if best == nil || cl < bestClass || (cl == bestClass && betterInClass(pc, best, cl)) {
			best, bestClass = pc, cl
		}
	}
	if best == nil {
		m.primary = nil
		return
	}
	if cur != nil && cur != best && bestClass < 2 && c.pathClass(cur, now) == bestClass && cur.rec.rtt.smoothed*4 <= best.rec.rtt.smoothed*5 {
		best = cur
	}
	if best == m.primary {
		return
	}
	old := m.primary
	m.primary = best
	if old == nil {
		return
	}
	c.stats.PrimarySwitches++
	if c.usable(old) && !c.healthy(old, now) {
		c.reinject(old)
	}
}

func (c *Conn) reinject(pc *pathCtx) {
	var re []*outDatagram
	for _, p := range pc.sp.sent {
		if p.size < 0 || len(p.dgrams) == 0 {
			continue
		}
		for _, d := range p.dgrams {
			re = append(re, &outDatagram{data: d})
		}
		p.dgrams = nil
	}
	c.requeueUnacked(pc, spaceApp, -1)
	c.prependDatagrams(re)
}

func (c *Conn) pruneDatagrams(now time.Time) {
	if len(c.dgramOut) == 0 {
		return
	}
	kept := c.dgramOut[:0]
	for _, d := range c.dgramOut {
		if d.policy == Redundant && !d.firstSent.IsZero() && (now.Sub(d.firstSent) > redundantCopyWait || c.redundantDone(d)) {
			continue
		}
		kept = append(kept, d)
	}
	for i := len(kept); i < len(c.dgramOut); i++ {
		c.dgramOut[i] = nil
	}
	c.dgramOut = kept
}

func containsID(ids []uint64, id uint64) bool {
	for _, x := range ids {
		if x == id {
			return true
		}
	}
	return false
}

func (c *Conn) redundantDone(d *outDatagram) bool {
	for _, pc := range c.mp.order {
		if c.usable(pc) && !containsID(d.sentOn, pc.id) {
			return false
		}
	}
	return true
}

func (c *Conn) dgramEligible(d *outDatagram, pc *pathCtx, primary bool) bool {
	if c.mp == nil {
		return true
	}
	switch d.policy {
	case Redundant:
		return !containsID(d.sentOn, pc.id)
	case Bulk:
		return primary || !(pc.backup || pc.peerBackup)
	}
	return primary
}

func (c *Conn) dgramSent(d *outDatagram, pc *pathCtx, now time.Time) bool {
	if c.mp == nil || d.policy != Redundant {
		return false
	}
	d.sentOn = append(d.sentOn, pc.id)
	if d.firstSent.IsZero() {
		d.firstSent = now
	}
	return !c.redundantDone(d)
}

func (c *Conn) appendForeignAcks(pc *pathCtx, b []byte, frames []sentFrame, room int, now time.Time) ([]byte, []sentFrame) {
	for _, q := range c.mp.order {
		if q == pc || q.removed || c.usable(q) {
			continue
		}
		sp := q.sp
		if !sp.newSinceAck || len(sp.received) == 0 || !sp.ackDue(now) {
			continue
		}
		ranges := sp.ackRanges(c.ackScratch)
		c.ackScratch = ranges
		var delay uint64
		if d := now.Sub(sp.largestRecvAt); d > 0 {
			delay = uint64(d/time.Microsecond) >> 3
		}
		for len(ranges) > 1 && pathAckFrameLen(q.id, ranges, delay) > room-len(b) {
			ranges = ranges[:len(ranges)-1]
		}
		if pathAckFrameLen(q.id, ranges, delay) > room-len(b) {
			continue
		}
		b = appendPathAckFrame(b, q.id, ranges, delay)
		frames = append(frames, sentFrame{kind: sfAck, id: ranges[0].largest, offset: q.id})
		sp.onAckSent()
	}
	return b, frames
}

func (c *Conn) appendMultipathControl(b []byte, frames []sentFrame, room int, isElic bool) ([]byte, []sentFrame, bool) {
	m := c.mp
	left := func() int { return room - len(b) }
	if m.maxDue && VarintLen(frameMaxPathID)+VarintLen(m.localMax) <= left() {
		b = appendOneVarintFrame(b, frameMaxPathID, m.localMax)
		frames = append(frames, sentFrame{kind: sfMaxPathID, id: m.localMax})
		m.maxDue = false
		isElic = true
	}
	if m.pathsBlockedDue && VarintLen(framePathsBlocked)+VarintLen(m.peerMax) <= left() {
		b = appendOneVarintFrame(b, framePathsBlocked, m.peerMax)
		frames = append(frames, sentFrame{kind: sfPathsBlocked})
		m.pathsBlockedDue = false
		isElic = true
	}
	for len(m.abandonQ) > 0 {
		a := m.abandonQ[0]
		if twoVarintFrameLen(framePathAbandon, a.id, a.code) > left() {
			break
		}
		b = appendTwoVarintFrame(b, framePathAbandon, a.id, a.code)
		frames = append(frames, sentFrame{kind: sfPathAbandon, id: a.id, offset: a.code})
		m.abandonQ = m.abandonQ[1:]
		isElic = true
	}
	for _, pc := range m.order {
		if !pc.statusDue || pc.abandoned {
			continue
		}
		typ := uint64(framePathStatusAvailable)
		if pc.backup {
			typ = framePathStatusBackup
		}
		if twoVarintFrameLen(typ, pc.id, pc.statusSeq) > left() {
			break
		}
		b = appendTwoVarintFrame(b, typ, pc.id, pc.statusSeq)
		frames = append(frames, sentFrame{kind: sfPathStatus, id: pc.id, offset: pc.statusSeq})
		pc.statusDue = false
		isElic = true
	}
	for id, set := range m.cids {
		for _, l := range set.local {
			if !l.sendDue {
				continue
			}
			if pathNewConnectionIDLen(id, l.seq, len(l.cid)) > left() {
				break
			}
			b = appendPathNewConnectionID(b, id, l.seq, 0, l.cid, l.token)
			frames = append(frames, sentFrame{kind: sfPathNewCID, id: id, offset: l.seq})
			l.sendDue = false
			isElic = true
		}
		for len(set.retire) > 0 {
			seq := set.retire[0]
			if twoVarintFrameLen(framePathRetireConnectionID, id, seq) > left() {
				break
			}
			b = appendTwoVarintFrame(b, framePathRetireConnectionID, id, seq)
			frames = append(frames, sentFrame{kind: sfPathRetireCID, id: id, offset: seq})
			set.retire = set.retire[1:]
			isElic = true
		}
		if set.blocked && twoVarintFrameLen(framePathCIDsBlocked, id, set.peerNext) <= left() {
			b = appendTwoVarintFrame(b, framePathCIDsBlocked, id, set.peerNext)
			frames = append(frames, sentFrame{kind: sfPathCIDsBlocked})
			set.blocked = false
			isElic = true
		}
	}
	return b, frames, isElic
}

func (c *Conn) multipathDeadlines(now time.Time, consider func(time.Time)) {
	future := func(t time.Time) {
		if t.After(now) {
			consider(t)
		}
	}
	usable := 0
	for _, pc := range c.mp.order {
		if c.usable(pc) {
			usable++
		}
	}
	ka := c.config.PathKeepAlivePeriod
	for _, pc := range c.mp.order {
		if pc != c.p0 {
			consider(pc.rec.lossTimer)
			future(pc.sp.ackDeadline)
		}
		future(pc.removeAt)
		if !c.usable(pc) {
			continue
		}
		if usable >= 2 && ka > 0 {
			last := pc.lastElicit
			if last.IsZero() {
				last = pc.createdAt
			}
			future(last.Add(ka))
		}
		if o := c.oldestUnacked(pc); o != nil {
			future(o.sentAt.Add(c.suspectTimeout(pc, o)))
		}
	}
	for _, d := range c.dgramOut {
		if d.policy == Redundant && !d.firstSent.IsZero() {
			future(d.firstSent.Add(redundantCopyWait))
			break
		}
	}
}

func (c *Conn) onMultipathTimers(now time.Time) {
	m := c.mp
	usable := 0
	for _, pc := range m.order {
		if c.usable(pc) {
			usable++
		}
	}
	ka := c.config.PathKeepAlivePeriod
	for _, pc := range append([]*pathCtx(nil), m.order...) {
		if pc != c.p0 && !pc.rec.lossTimer.IsZero() && !now.Before(pc.rec.lossTimer) {
			c.onLossTimer(pc, now)
		}
		if !pc.removeAt.IsZero() && !now.Before(pc.removeAt) {
			c.removePathCtx(pc)
			continue
		}
		if usable >= 2 && ka > 0 && c.usable(pc) {
			last := pc.lastElicit
			if last.IsZero() {
				last = pc.createdAt
			}
			if !now.Before(last.Add(ka)) {
				pc.pingDue = true
			}
		}
	}
}

func (c *Conn) pickNewPathID() (uint64, error) {
	m := c.mp
	if c.openPaths() >= c.config.MaxPaths || len(m.paths) >= 2*c.config.MaxPaths {
		return 0, ErrPathLimit
	}
	max := m.localMax
	if m.peerMax < max {
		max = m.peerMax
	}
	for id := m.floor; id <= max && id >= m.floor; id++ {
		if m.isConsumed(id) {
			continue
		}
		set := m.cids[id]
		if set == nil || len(set.local) == 0 {
			continue
		}
		free := false
		for _, q := range set.peer {
			if !q.inUse {
				free = true
			}
		}
		if !free {
			if !set.blocked {
				set.blocked = true
			}
			continue
		}
		return id, nil
	}
	if m.peerMax < m.localMax || max < m.floor {
		m.pathsBlockedDue = true
	}
	return 0, ErrNoPathAvailable
}

type PathOptions struct {
	Remote netip.AddrPort
	Backup bool
}

type PathState uint8

const (
	PathValidating PathState = iota
	PathActive
	PathAbandoned
)

func (s PathState) String() string {
	switch s {
	case PathValidating:
		return "validating"
	case PathActive:
		return "active"
	case PathAbandoned:
		return "abandoned"
	}
	return "unknown"
}

type PathInfo struct {
	ID               uint64
	State            PathState
	LocalAddr        net.Addr
	RemoteAddr       netip.AddrPort
	Primary          bool
	Healthy          bool
	Backup           bool
	PeerBackup       bool
	SmoothedRTT      time.Duration
	RTTVariance      time.Duration
	MinRTT           time.Duration
	LatestRTT        time.Duration
	LossRate         float64
	LastHeard        time.Time
	LastAcked        time.Time
	CongestionWindow int
	BytesInFlight    int
	PTOCount         int
	PacketsSent      uint64
	PacketsLost      uint64
	PacketsReceived  uint64
}

type Health struct {
	Closed        bool
	LastHeard     time.Time
	LastAcked     time.Time
	OldestUnacked time.Time
	PTOCount      int
	SmoothedRTT   time.Duration
	RTTVariance   time.Duration
	MaxAckDelay   time.Duration
	UsablePaths   int
	HealthyPaths  int
}

type Path struct {
	conn *Conn
	id   uint64
}

func (p *Path) ID() uint64 { return p.id }

func (p *Path) Info() (PathInfo, bool) { return p.conn.PathInfo(p.id) }

func (p *Path) SetBackup(backup bool) error { return p.conn.SetPathBackup(p.id, backup) }

func (p *Path) Abandon() error { return p.conn.AbandonPath(p.id) }

func (c *Conn) MultipathEnabled() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.mp != nil
}

func (c *Conn) OpenPath(ctx context.Context, conn net.PacketConn, opts PathOptions) (*Path, error) {
	if !c.isClient {
		return nil, ErrClientOnly
	}
	c.mu.Lock()
	var id uint64
	for {
		if c.closeErr != nil {
			err := c.closeErr
			c.mu.Unlock()
			return nil, err
		}
		if c.mp == nil && c.havePeerParams {
			c.mu.Unlock()
			return nil, ErrMultipathDisabled
		}
		if c.mp != nil && c.confirmed {
			var err error
			id, err = c.pickNewPathID()
			if err == nil {
				break
			}
			if err != ErrNoPathAvailable {
				c.mu.Unlock()
				return nil, err
			}
		}
		ch := c.mpNotify.wait()
		c.mu.Unlock()
		c.wake()
		select {
		case <-ch:
		case <-c.closedCh:
		case <-ctx.Done():
			return nil, ctx.Err()
		}
		c.mu.Lock()
	}
	now := time.Now()
	sock, err := c.ep.addPathSocket(conn)
	if err != nil {
		c.mu.Unlock()
		return nil, err
	}
	pc := c.newPathCtx(id, now)
	pc.sock, pc.ownsSock = sock, true
	pc.validCh = make(chan struct{})
	if opts.Backup {
		pc.backup = true
		pc.statusSeq, pc.statusNext, pc.statusDue = 0, 1, true
	}
	c.attachPathCtx(pc, now)
	remote := opts.Remote
	if !remote.IsValid() {
		remote = c.active.addr
	}
	p := &path{addr: remote, lastSeen: now, pc: pc}
	c.paths = append(c.paths, p)
	pc.addr = p
	c.startValidation(p, now)
	ch := pc.validCh
	c.mu.Unlock()
	c.wake()
	select {
	case <-ch:
	case <-c.closedCh:
		return nil, c.Err()
	case <-ctx.Done():
		c.mu.Lock()
		c.abandonPath(pc, pathAbandonApplication, ctx.Err(), time.Now())
		c.mu.Unlock()
		c.wake()
		return nil, ctx.Err()
	}
	c.mu.Lock()
	err = pc.validErr
	c.mu.Unlock()
	if err != nil {
		return nil, err
	}
	return &Path{conn: c, id: id}, nil
}

func (c *Conn) AbandonPath(id uint64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.mp == nil {
		return ErrMultipathDisabled
	}
	pc := c.pathByID(id)
	if pc == nil {
		return ErrUnknownPath
	}
	if pc.abandoned {
		return nil
	}
	others := 0
	for _, q := range c.mp.order {
		if q != pc && c.usable(q) {
			others++
		}
	}
	if others == 0 {
		return ErrLastPath
	}
	c.abandonPath(pc, pathAbandonApplication, ErrPathAbandoned, time.Now())
	c.wake()
	return nil
}

func (c *Conn) SetPathBackup(id uint64, backup bool) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.mp == nil {
		return ErrMultipathDisabled
	}
	pc := c.pathByID(id)
	if pc == nil || pc.abandoned {
		return ErrUnknownPath
	}
	pc.backup = backup
	pc.statusSeq = pc.statusNext
	pc.statusNext++
	pc.statusDue = true
	c.wake()
	return nil
}

func (c *Conn) PathInfo(id uint64) (PathInfo, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	pc := c.pathByID(id)
	if pc == nil {
		return PathInfo{}, false
	}
	return c.pathInfo(pc, time.Now()), true
}

func (c *Conn) Paths() []PathInfo {
	c.mu.Lock()
	defer c.mu.Unlock()
	now := time.Now()
	var out []PathInfo
	for _, pc := range c.sendPaths() {
		if pc.removed {
			continue
		}
		out = append(out, c.pathInfo(pc, now))
	}
	return out
}

func (c *Conn) pathInfo(pc *pathCtx, now time.Time) PathInfo {
	r := pc.rec
	info := PathInfo{
		ID:               pc.id,
		Primary:          pc == c.primaryPath(),
		Healthy:          c.healthy(pc, now),
		Backup:           pc.backup,
		PeerBackup:       pc.peerBackup,
		SmoothedRTT:      r.rtt.smoothed,
		RTTVariance:      r.rtt.variance,
		MinRTT:           r.rtt.min,
		LatestRTT:        r.rtt.latest,
		LossRate:         pc.loss,
		LastHeard:        pc.lastHeard,
		LastAcked:        pc.lastAcked,
		CongestionWindow: r.cc.cwnd,
		BytesInFlight:    r.cc.inFlight,
		PTOCount:         r.ptoCount,
		PacketsSent:      pc.sent,
		PacketsLost:      pc.lost,
		PacketsReceived:  pc.recv,
	}
	switch {
	case pc.abandoned:
		info.State = PathAbandoned
	case c.usable(pc):
		info.State = PathActive
	default:
		info.State = PathValidating
	}
	if pc.sock != nil {
		info.LocalAddr = pc.sock.pc.LocalAddr()
	} else {
		info.LocalAddr = c.ep.LocalAddr()
	}
	if p := c.activeOf(pc); p != nil {
		info.RemoteAddr = p.addr
	}
	return info
}

func (c *Conn) Health() Health {
	c.mu.Lock()
	defer c.mu.Unlock()
	now := time.Now()
	h := Health{Closed: c.closeErr != nil, LastHeard: c.lastRecv, MaxAckDelay: c.peerParams.maxAckDelay, PTOCount: -1}
	for _, pc := range c.sendPaths() {
		if !c.usable(pc) {
			continue
		}
		h.UsablePaths++
		if pc.lastAcked.After(h.LastAcked) {
			h.LastAcked = pc.lastAcked
		}
		if c.healthy(pc, now) {
			h.HealthyPaths++
		}
		if o := c.oldestUnacked(pc); o != nil && (h.OldestUnacked.IsZero() || o.sentAt.Before(h.OldestUnacked)) {
			h.OldestUnacked = o.sentAt
		}
		if h.PTOCount < 0 || pc.rec.ptoCount < h.PTOCount {
			h.PTOCount = pc.rec.ptoCount
		}
		if pc.rec.rtt.smoothed > h.SmoothedRTT {
			h.SmoothedRTT = pc.rec.rtt.smoothed
		}
		if pc.rec.rtt.variance > h.RTTVariance {
			h.RTTVariance = pc.rec.rtt.variance
		}
	}
	if h.PTOCount < 0 {
		h.PTOCount = 0
	}
	return h
}

func (c *Conn) ping() {
	c.mu.Lock()
	if c.state < stateClosing && c.handshakeDone {
		c.keepAliveDue = true
	}
	c.mu.Unlock()
	c.wake()
}
