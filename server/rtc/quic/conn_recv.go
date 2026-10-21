package quic

import (
	"bytes"
	"net/netip"
	"time"
)

type packetInfo struct {
	space      int
	pn         int64
	eliciting  bool
	nonProbing bool
	datagram   bool
	delayable  bool
	ok         bool
	localSeq   uint64
	pc         *pathCtx
}

func (c *Conn) handleDatagram(d *inDatagram) {
	defer c.ep.release(d)
	if c.state >= stateDraining {
		return
	}
	now := d.at
	if d.reset {
		if c.matchesResetToken(d.data[len(d.data)-16:]) {
			c.abort(ErrStatelessReset)
		}
		return
	}
	if c.state == stateClosing {
		if now.Sub(c.lastCloseSent) >= 50*time.Millisecond {
			c.sendClose(now)
		}
		return
	}
	b := d.data
	var tail [16]byte
	hasTail := len(b) >= 21
	if hasTail {
		copy(tail[:], b[len(b)-16:])
	}
	var p *path
	if pc0 := c.peekPath(b); pc0 != nil {
		p = c.pathFor(pc0, d.addr)
	}
	if p == nil {
		if c.isClient || !c.confirmed {
			c.stats.PacketsDropped++
			return
		}
	} else {
		p.recvBytes += uint64(len(b))
	}
	c.stats.BytesReceived += uint64(len(b))
	any := false
	var appInfo packetInfo
	for len(b) > 0 && c.state < stateClosing {
		n, info := c.handlePacket(b, d.addr, now)
		if n <= 0 {
			break
		}
		b = b[n:]
		if !info.ok {
			continue
		}
		any = true
		if info.space == spaceApp {
			if !appInfo.ok || info.pn > appInfo.pn {
				appInfo = info
			} else if info.nonProbing {
				appInfo.nonProbing = true
			}
		}
	}
	if !any {
		c.stats.PacketsDropped++
		if hasTail && c.matchesResetToken(tail[:]) {
			c.abort(ErrStatelessReset)
		}
		return
	}
	c.lastRecv = now
	c.idleStart = now
	c.elicitingSince = false
	if appInfo.ok && appInfo.pc != nil {
		appInfo.pc.lastHeard = now
	}
	if c.state >= stateClosing || c.isClient || !appInfo.ok {
		return
	}
	pc := appInfo.pc
	if pc == nil || pc.removed || pc.abandoned {
		return
	}
	p = c.pathFor(pc, d.addr)
	if p == nil {
		p = c.newPath(pc, d.addr, appInfo.localSeq, now)
		if p == nil {
			return
		}
		p.recvBytes += uint64(len(d.data))
		c.drainPendingResponses(p, d.addr)
	}
	largest := appInfo.pn == pc.sp.largestRecv
	c.onPeerAddress(pc, d.addr, p, appInfo.nonProbing, largest, now)
}

func (c *Conn) drainPendingResponses(p *path, addr netip.AddrPort) {
	if c.pendingResponseAddr != addr || len(c.pendingResponses) == 0 {
		return
	}
	for _, r := range c.pendingResponses {
		c.onPathChallenge(p, r)
	}
	c.pendingResponses = c.pendingResponses[:0]
}

func (c *Conn) handlePacket(b []byte, from netip.AddrPort, now time.Time) (int, packetInfo) {
	var info packetInfo
	if isLongHeader(b[0]) {
		h, err := parseLongHeader(b)
		if err != nil {
			return 0, info
		}
		switch h.typ {
		case packetVersionNegotiation:
			c.onVersionNegotiation(b)
			return len(b), info
		case packetRetry:
			c.onRetry(b, h)
			return len(b), info
		case packet0RTT:
			return h.end, info
		}
		space := spaceInitial
		if h.typ == packetHandshake {
			space = spaceHandshake
		}
		if !c.isLocalCID(h.dcid) {
			return h.end, info
		}
		if c.gotPeerSCID && !bytes.Equal(h.scid, c.peerInitialSCID) {
			return h.end, info
		}
		sp := c.spaces[space]
		if sp.open == nil || sp.discarded {
			return h.end, info
		}
		pkt := b[:h.end]
		pn, payload, ok := c.openLong(pkt, h.pnOffset, sp)
		if !ok {
			return h.end, info
		}
		if pkt[0]&0x0c != 0 {
			c.closeTransport(transportErr(ProtocolViolation, "reserved bits set"))
			return h.end, info
		}
		if c.isClient && !c.gotPeerSCID {
			c.gotPeerSCID = true
			c.peerInitialSCID = append([]byte(nil), h.scid...)
			c.dcid = c.peerInitialSCID
			c.peerCIDs = []*peerCID{{seq: 0, cid: c.peerInitialSCID, inUse: true}}
			c.dcidSeq = 0
		}
		info = c.processPayload(space, h.typ, pn, payload, from, h.dcid, now)
		if info.ok && !c.isClient && space == spaceHandshake {
			if c.active != nil {
				c.active.validated = true
			}
			c.discardSpace(spaceInitial)
		}
		return h.end, info
	}
	if len(b) < 1+localCIDLen {
		return len(b), info
	}
	if b[0]&0x40 == 0 {
		return len(b), info
	}
	dcid := b[1 : 1+localCIDLen]
	pc, l, fresh := c.pathForCID(dcid, now)
	if pc == nil {
		return len(b), info
	}
	sp := c.spaces[spaceApp]
	if sp.open == nil || sp.discarded || c.keys.readCur == nil {
		return len(b), info
	}
	pn, payload, ok := c.openShort(b, 1+localCIDLen, pc, now)
	if !ok {
		return len(b), info
	}
	if b[0]&0x18 != 0 {
		c.closeTransport(transportErr(ProtocolViolation, "reserved bits set"))
		return len(b), info
	}
	if fresh {
		c.attachPathCtx(pc, now)
	}
	c.rxPath = pc
	info = c.processPayload(spaceApp, packet1RTT, pn, payload, from, dcid, now)
	c.rxPath = nil
	info.localSeq = l.seq
	info.pc = pc
	if info.ok {
		pc.recv++
	}
	if info.ok && c.isClient && !c.confirmed {
		c.onConfirmed()
	}
	return len(b), info
}

func (c *Conn) openLong(pkt []byte, pnOffset int, sp *pnSpace) (int64, []byte, bool) {
	pnLen, ok := sp.open.hp.unprotect(pkt, pnOffset)
	if !ok {
		return 0, nil, false
	}
	hdrLen := pnOffset + pnLen
	pn := decodePacketNumber(sp.largestRecv, readTruncatedPN(pkt[pnOffset:], pnLen), pnLen)
	if len(pkt) < hdrLen+aeadTagLen {
		return 0, nil, false
	}
	payload, err := sp.open.key.open(pkt, hdrLen, pn)
	if err != nil {
		return 0, nil, false
	}
	if sp.isDuplicate(pn) {
		return 0, nil, false
	}
	return pn, payload, true
}

func (c *Conn) openShort(pkt []byte, pnOffset int, pc *pathCtx, now time.Time) (int64, []byte, bool) {
	sp := pc.sp
	k := &c.keys
	pnLen, ok := k.readCur.hp.unprotect(pkt, pnOffset)
	if !ok {
		return 0, nil, false
	}
	hdrLen := pnOffset + pnLen
	if len(pkt) < hdrLen+aeadTagLen {
		return 0, nil, false
	}
	pn := decodePacketNumber(sp.largestRecv, readTruncatedPN(pkt[pnOffset:], pnLen), pnLen)
	phase := pkt[0]&0x04 != 0
	curPhase := k.readPhase&1 == 1
	var keys *directionKeys
	update := false
	switch {
	case phase == curPhase:
		keys = k.readCur
	case c.mp != nil && k.readPrev != nil:
		keys = k.readPrev
	case c.mp == nil && k.readPrev != nil && k.firstRecvInPhase >= 0 && pn < k.firstRecvInPhase:
		keys = k.readPrev
	default:
		keys = k.readNext
		update = true
	}
	var saved []byte
	if keys == k.readPrev && c.mp != nil {
		saved = append(saved, pkt[hdrLen:]...)
	}
	payload, err := keys.key.openPath(pc.id, pkt, hdrLen, pn)
	if err != nil && saved != nil {
		copy(pkt[hdrLen:], saved)
		keys = k.readNext
		update = true
		payload, err = keys.key.openPath(pc.id, pkt, hdrLen, pn)
	}
	if err != nil {
		k.authFailures++
		limit := uint64(aeadIntegrityLimit)
		if keys.suite.chacha {
			limit = chachaIntegrityLimit
		}
		if k.authFailures >= limit {
			c.closeTransport(transportErr(AEADLimitReached, "integrity limit reached"))
		}
		return 0, nil, false
	}
	if sp.isDuplicate(pn) {
		return 0, nil, false
	}
	if update {
		if !c.confirmed && !c.isClient {
			c.closeTransport(transportErr(KeyUpdateError, "key update before confirmation"))
			return 0, nil, false
		}
		k.readPrev = k.readCur
		k.readCur = k.readNext
		k.readNext = k.readCur.nextGeneration()
		k.readPhase++
		k.firstRecvInPhase = pn
		k.prevUntil = now.Add(3 * c.maxPTO())
		if k.writePhase < k.readPhase {
			c.rotateWriteKeys()
		}
		c.stats.KeyUpdates++
	} else if c.mp == nil && keys == k.readCur && (k.firstRecvInPhase < 0 || pn < k.firstRecvInPhase) {
		k.firstRecvInPhase = pn
	}
	return pn, payload, true
}

func (c *Conn) rotateWriteKeys() {
	k := &c.keys
	k.write = k.write.nextGeneration()
	k.writePhase++
	k.ackedInPhase = false
	k.sentInPhase = 0
	c.spaces[spaceApp].seal = k.write
}

func frameAllowed(t packetType, typ uint64) bool {
	if t == packet1RTT {
		return true
	}
	switch typ {
	case framePadding, framePing, frameAck, frameAckECN, frameCrypto, frameConnectionClose:
		return true
	}
	return false
}

func (c *Conn) processPayload(space int, t packetType, pn int64, payload []byte, from netip.AddrPort, dcid []byte, now time.Time) packetInfo {
	info := packetInfo{space: space, pn: pn}
	if len(payload) == 0 {
		c.closeTransport(transportErr(ProtocolViolation, "packet without frames"))
		return info
	}
	f := &c.frameScratch
	for len(payload) > 0 {
		n, err := parseFrame(payload, f)
		if err != nil {
			c.closeTransport(err)
			return info
		}
		payload = payload[n:]
		if !frameAllowed(t, f.typ) {
			c.closeTransport(frameErr(ProtocolViolation, f.typ, "frame not allowed in "+t.String()+" packet"))
			return info
		}
		if f.ackEliciting() {
			info.eliciting = true
		}
		if !f.probing() {
			info.nonProbing = true
		}
		switch f.typ {
		case frameDatagram, frameDatagramLen:
			info.datagram = true
		case framePing, framePathChallenge, framePathResponse:
		default:
			if f.ackEliciting() {
				info.delayable = true
			}
		}
		if err := c.handleFrame(space, f, from, dcid, now); err != nil {
			c.closeTransport(err)
			return info
		}
		if c.state >= stateClosing {
			info.ok = true
			return info
		}
	}
	c.stats.PacketsReceived++
	sp := c.spaces[space]
	if space == spaceApp {
		sp = c.recvPath().sp
	}
	if !sp.discarded {
		delayable := space == spaceApp && (c.mp == nil || (info.delayable && !info.datagram))
		sp.onReceived(pn, info.eliciting, delayable, now, 20*time.Millisecond)
	}
	info.ok = true
	return info
}

func (c *Conn) handleFrame(space int, f *frame, from netip.AddrPort, dcid []byte, now time.Time) *TransportError {
	switch {
	case f.typ == framePadding, f.typ == framePing:
		return nil
	case f.typ == frameAck || f.typ == frameAckECN:
		return c.onAck(space, f, now)
	case isMultipathFrame(f.typ):
		return c.onMultipathFrame(f, dcid, now)
	case f.typ == frameCrypto:
		return c.feedCrypto(space, f)
	case f.typ == frameConnectionClose || f.typ == frameApplicationClose:
		reason := string(f.data)
		if len(reason) > maxReasonLen {
			reason = reason[:maxReasonLen]
		}
		if f.typ == frameApplicationClose {
			c.enterDraining(&ApplicationError{Code: f.code, Reason: reason, Remote: true})
		} else {
			c.enterDraining(&TransportError{Code: TransportErrorCode(f.code), FrameType: f.closeFrame, Reason: reason, Remote: true})
		}
		return nil
	case f.typ == frameHandshakeDone:
		if !c.isClient {
			return frameErr(ProtocolViolation, f.typ, "HANDSHAKE_DONE from client")
		}
		c.onConfirmed()
		return nil
	case f.typ == frameNewToken:
		if !c.isClient {
			return frameErr(ProtocolViolation, f.typ, "NEW_TOKEN from client")
		}
		return nil
	case f.isStream():
		return c.onStreamFrame(f)
	case f.typ == frameResetStream:
		return c.onResetStream(f)
	case f.typ == frameStopSending:
		return c.onStopSending(f)
	case f.typ == frameMaxData:
		if f.max > c.sendMaxData {
			c.sendMaxData = f.max
			c.wakeSenders()
		}
		return nil
	case f.typ == frameMaxStreamData:
		return c.onMaxStreamData(f)
	case f.typ == frameMaxStreamsBidi, f.typ == frameMaxStreamsUni:
		c.onMaxStreams(f.typ == frameMaxStreamsUni, f.max)
		return nil
	case f.typ == frameDataBlocked, f.typ == frameStreamsBlockedBidi, f.typ == frameStreamsBlockedUni:
		return nil
	case f.typ == frameStreamDataBlocked:
		return c.checkStreamFrameTarget(f.streamID, f.typ, true)
	case f.typ == frameNewConnectionID:
		return c.onNewConnectionID(f)
	case f.typ == frameRetireConnectionID:
		return c.onRetireConnectionID(f.seq, dcid)
	case f.typ == framePathChallenge:
		p := c.pathFor(c.recvPath(), from)
		if p == nil {
			c.queueResponseForNewPath(from, f.pathData)
			return nil
		}
		c.onPathChallenge(p, f.pathData)
		return nil
	case f.typ == framePathResponse:
		c.onPathResponse(f.pathData, now)
		return nil
	case f.typ == frameDatagram || f.typ == frameDatagramLen:
		return c.onDatagram(f)
	}
	return frameErr(FrameEncodingError, f.typ, "unknown frame")
}

func (c *Conn) queueResponseForNewPath(from netip.AddrPort, data [8]byte) {
	if c.pendingResponseAddr != from {
		c.pendingResponseAddr = from
		c.pendingResponses = c.pendingResponses[:0]
	}
	if len(c.pendingResponses) < maxPendingResponses {
		c.pendingResponses = append(c.pendingResponses, data)
	}
}

func (c *Conn) onDatagram(f *frame) *TransportError {
	max := c.config.MaxDatagramFrameSize
	if max == 0 {
		return frameErr(ProtocolViolation, f.typ, "datagrams not negotiated")
	}
	size := 1 + len(f.data)
	if f.typ == frameDatagramLen {
		size = datagramFrameLen(len(f.data))
	}
	if size > max {
		return frameErr(ProtocolViolation, f.typ, "datagram exceeds max_datagram_frame_size")
	}
	c.stats.DatagramsReceived++
	if key := c.config.DatagramDedupKey; key != nil {
		if k, ok := key(f.data); ok && !c.dedup.add(k) {
			c.stats.DatagramsDuplicate++
			return nil
		}
	}
	if len(c.dgramIn) >= c.config.MaxPendingDatagrams {
		c.stats.DatagramsDropped++
		return nil
	}
	c.dgramIn = append(c.dgramIn, inDatagramRec{data: append([]byte(nil), f.data...), path: c.recvPath().id})
	c.dgramNotify.broadcast()
	return nil
}

func (c *Conn) onAck(space int, f *frame, now time.Time) *TransportError {
	pc := c.p0
	if f.typ == framePathAck || f.typ == framePathAckECN {
		pc = c.pathByID(f.pathID)
	} else if pc.removed {
		pc = nil
	}
	if pc == nil {
		return nil
	}
	sp := c.pathSpace(pc, space)
	if sp == nil || sp.discarded {
		return nil
	}
	largest := f.ranges[0].largest
	if largest >= uint64(sp.nextPN) {
		return frameErr(ProtocolViolation, f.typ, "ack for unsent packet")
	}
	var newly []*sentPacket
	j := 0
	for k := len(sp.sent) - 1; k >= 0; k-- {
		p := sp.sent[k]
		pn := uint64(p.pn)
		for j < len(f.ranges) && pn < f.ranges[j].smallest {
			j++
		}
		if j == len(f.ranges) {
			break
		}
		if pn <= f.ranges[j].largest {
			newly = append(newly, p)
		}
	}
	if len(newly) == 0 {
		return nil
	}
	if int64(largest) > sp.largestAcked {
		sp.largestAcked = int64(largest)
	}
	eliciting := false
	for _, p := range newly {
		if p.ackEliciting {
			eliciting = true
		}
	}
	rec := pc.rec
	if uint64(newly[0].pn) == largest && eliciting {
		var delay time.Duration
		if space == spaceApp {
			exp := c.peerParams.ackDelayExponent
			if f.ackDelay < 1<<40 {
				delay = time.Duration(f.ackDelay<<exp) * time.Microsecond
			}
			if c.confirmed && delay > c.peerParams.maxAckDelay {
				delay = c.peerParams.maxAckDelay
			}
		}
		sample := true
		if c.mp != nil && space == spaceApp {
			cross := c.recvPath() != pc
			switch {
			case cross && rec.rtt.hasSample:
				sample = false
			case !cross && pc.crossRTT:
				rec.rtt = newRTT(c.config.InitialRTT)
			}
			if sample {
				pc.crossRTT = cross
			}
		}
		if sample {
			rec.rtt.update(now.Sub(newly[0].sentAt), delay, now)
		}
	}
	if space == spaceHandshake && c.isClient {
		c.peerValidatedUs = true
	}
	for _, p := range newly {
		c.onPacketAcked(pc, space, p, now)
	}
	c.removeSent(sp)
	c.detectLost(pc, space, now)
	if c.peerCompletedAddressValidation() {
		rec.ptoCount = 0
	}
	if space == spaceApp {
		pc.lastAcked = now
	}
	c.setPathLossTimer(pc, now)
	return nil
}

func (c *Conn) removeSent(sp *pnSpace) {
	kept := sp.sent[:0]
	for _, p := range sp.sent {
		if p.size >= 0 {
			kept = append(kept, p)
		}
	}
	for i := len(kept); i < len(sp.sent); i++ {
		sp.sent[i] = nil
	}
	sp.sent = kept
}

func (c *Conn) onPacketAcked(pc *pathCtx, space int, p *sentPacket, now time.Time) {
	sp := c.pathSpace(pc, space)
	cc := &pc.rec.cc
	if p.inFlight {
		inFlightBefore := cc.inFlight
		cc.cwndLimited = inFlightBefore*2 >= cc.cwnd
		cc.onAcked(p)
	}
	if p.ackEliciting && p.inFlight {
		sp.elicitingInFlight--
	}
	if space == spaceApp {
		if p.phase == c.keys.writePhase {
			c.keys.ackedInPhase = true
		}
		if p.ackEliciting {
			pc.loss -= pc.loss * lossAlpha
		}
	}
	for _, f := range p.frames {
		c.onFrameAcked(space, f)
	}
	p.dgrams = nil
	p.size = -1
}

func (c *Conn) onFrameAcked(space int, f sentFrame) {
	switch f.kind {
	case sfAck:
		sp := c.spaces[space]
		if space == spaceApp {
			q := c.pathByID(f.offset)
			if q == nil {
				return
			}
			sp = q.sp
		}
		if int64(f.id) > sp.ackedUpTo {
			sp.ackedUpTo = int64(f.id)
		}
	case sfCrypto:
		c.spaces[space].crypto.onAck(f.offset, f.offset+f.length, false)
	case sfStream:
		if s := c.streams[f.id]; s != nil && !s.resetLocal {
			s.send.onAck(f.offset, f.offset+f.length, f.fin)
			s.writeNotify.broadcast()
			c.maybeRemoveStream(s)
		}
	case sfResetStream:
		if s := c.streams[f.id]; s != nil {
			s.resetAcked = true
			c.maybeRemoveStream(s)
		}
	case sfPathAbandon:
		c.onAbandonAcked(f.id)
	case sfRetireCID:
	}
}

func (c *Conn) detectLost(pc *pathCtx, space int, now time.Time) {
	sp := c.pathSpace(pc, space)
	rec := pc.rec
	sp.lossTime = time.Time{}
	if sp.largestAcked < 0 {
		return
	}
	delay := rec.rtt.lossDelay()
	lostBefore := now.Add(-delay)
	var lost []*sentPacket
	for _, p := range sp.sent {
		if p.pn > sp.largestAcked {
			break
		}
		if !p.sentAt.After(lostBefore) || sp.largestAcked-p.pn >= packetThreshold {
			lost = append(lost, p)
			continue
		}
		t := p.sentAt.Add(delay)
		if sp.lossTime.IsZero() || t.Before(sp.lossTime) {
			sp.lossTime = t
		}
	}
	if len(lost) == 0 {
		return
	}
	var first, last time.Time
	anyInFlight := false
	for _, p := range lost {
		if p.inFlight {
			anyInFlight = true
			if first.IsZero() {
				first = p.sentAt
			}
			last = p.sentAt
		}
		c.onPacketLost(pc, space, p)
	}
	c.removeSent(sp)
	if anyInFlight {
		rec.cc.onCongestionEvent(last, now)
		limit := persistentThreshold * c.pathPTO(pc)
		if rec.rtt.hasSample && first.After(rec.rtt.firstAt) && last.Sub(first) > limit {
			rec.cc.onPersistentCongestion()
		}
	}
}

func (c *Conn) onPacketLost(pc *pathCtx, space int, p *sentPacket) {
	sp := c.pathSpace(pc, space)
	if p.inFlight {
		pc.rec.cc.remove(p.size)
		if p.ackEliciting {
			sp.elicitingInFlight--
		}
	}
	c.stats.PacketsLost++
	if space == spaceApp {
		pc.lost++
		if p.ackEliciting {
			pc.loss += (1 - pc.loss) * lossAlpha
		}
	}
	for _, f := range p.frames {
		c.requeueFrame(space, f)
	}
	p.dgrams = nil
	p.size = -1
}

func (c *Conn) requeueFrame(space int, f sentFrame) {
	switch f.kind {
	case sfCrypto:
		c.spaces[space].crypto.onLost(f.offset, f.offset+f.length, false)
	case sfStream:
		if s := c.streams[f.id]; s != nil && !s.resetLocal {
			s.send.onLost(f.offset, f.offset+f.length, f.fin)
			c.queueSend(s)
		}
	case sfMaxData:
		c.maxDataDue = true
	case sfMaxStreamData:
		if s := c.streams[f.id]; s != nil && !s.finalKnown && !s.readCancelled {
			s.maxDataDue = true
			c.queueControl(s)
		}
	case sfMaxStreamsBidi:
		c.maxStreamsDue(false)
	case sfMaxStreamsUni:
		c.maxStreamsDue(true)
	case sfResetStream:
		if s := c.streams[f.id]; s != nil && !s.resetAcked {
			s.resetDue = true
			c.queueControl(s)
		}
	case sfStopSending:
		if s := c.streams[f.id]; s != nil && !s.finalKnown && !s.recvReset {
			s.stopDue = true
			c.queueControl(s)
		}
	case sfNewCID:
		for _, l := range c.localCIDs {
			if l.seq == f.id && !l.retired {
				l.sendDue = true
			}
		}
	case sfRetireCID:
		c.queueRetire(f.id)
	case sfHandshakeDone:
		c.handshakeDoneDue = true
	case sfDataBlocked:
		if c.sentData >= c.sendMaxData && c.sendMaxData == f.offset {
			c.dataBlockedDue = true
		}
	case sfStreamDataBlocked:
		if s := c.streams[f.id]; s != nil && s.sendMax == f.offset {
			s.blockedDue = true
			c.queueControl(s)
		}
	case sfStreamsBlockedBidi:
		c.blockedDue[0] = true
	case sfStreamsBlockedUni:
		c.blockedDue[1] = true
	case sfDatagram:
		c.stats.DatagramsLost++
	case sfPathAbandon, sfPathStatus, sfPathNewCID, sfPathRetireCID, sfMaxPathID:
		c.requeuePathFrame(f)
	}
}

func (c *Conn) peerCompletedAddressValidation() bool {
	if !c.isClient {
		return true
	}
	return c.confirmed || c.peerValidatedUs
}

func (c *Conn) earliestLossTime(pc *pathCtx) (time.Time, int) {
	var t time.Time
	s := -1
	for i := 0; i < numSpaces; i++ {
		sp := c.pathSpace(pc, i)
		if sp == nil || sp.discarded || sp.lossTime.IsZero() {
			continue
		}
		if t.IsZero() || sp.lossTime.Before(t) {
			t, s = sp.lossTime, i
		}
	}
	return t, s
}

func (c *Conn) elicitingInFlight(pc *pathCtx) bool {
	for i := 0; i < numSpaces; i++ {
		sp := c.pathSpace(pc, i)
		if sp != nil && !sp.discarded && sp.elicitingInFlight > 0 {
			return true
		}
	}
	return false
}

func (c *Conn) ptoTimeAndSpace(pc *pathCtx, now time.Time) (time.Time, int) {
	rec := pc.rec
	backoff := rec.ptoCount
	if backoff > maxPTOBackoff {
		backoff = maxPTOBackoff
	}
	dur := rec.rtt.pto(0) << uint(backoff)
	if !c.elicitingInFlight(pc) {
		if pc != c.p0 {
			return time.Time{}, -1
		}
		if hs := c.spaces[spaceHandshake]; hs.seal != nil && !hs.discarded {
			return now.Add(dur), spaceHandshake
		}
		return now.Add(dur), spaceInitial
	}
	var best time.Time
	space := -1
	for i := 0; i < numSpaces; i++ {
		sp := c.pathSpace(pc, i)
		if sp == nil || sp.discarded || sp.elicitingInFlight == 0 {
			continue
		}
		d := dur
		if i == spaceApp {
			if !c.confirmed {
				break
			}
			d += c.peerParams.maxAckDelay << uint(backoff)
		}
		t := sp.lastAckEliciting.Add(d)
		if best.IsZero() || t.Before(best) {
			best, space = t, i
		}
	}
	return best, space
}

func (c *Conn) setLossTimer(now time.Time) {
	c.setPathLossTimer(c.p0, now)
}

func (c *Conn) setPathLossTimer(pc *pathCtx, now time.Time) {
	rec := pc.rec
	if t, _ := c.earliestLossTime(pc); !t.IsZero() {
		rec.lossTimer = t
		return
	}
	if p := c.activeOf(pc); !c.isClient && p != nil && !p.validated && p.amplificationBudget() == 0 {
		rec.lossTimer = time.Time{}
		return
	}
	if !c.elicitingInFlight(pc) && c.peerCompletedAddressValidation() {
		rec.lossTimer = time.Time{}
		return
	}
	t, _ := c.ptoTimeAndSpace(pc, now)
	rec.lossTimer = t
}

func (c *Conn) onLossTimer(pc *pathCtx, now time.Time) {
	rec := pc.rec
	if t, s := c.earliestLossTime(pc); !t.IsZero() {
		c.detectLost(pc, s, now)
		c.setPathLossTimer(pc, now)
		return
	}
	_, s := c.ptoTimeAndSpace(pc, now)
	rec.ptoCount++
	if s < 0 {
		c.setPathLossTimer(pc, now)
		return
	}
	if s != spaceApp {
		for _, i := range []int{spaceInitial, spaceHandshake} {
			sp := c.spaces[i]
			if sp.discarded || sp.seal == nil {
				continue
			}
			c.requeueUnacked(pc, i, -1)
			if i == s || len(sp.sent) > 0 {
				sp.probes = 1
			}
		}
		c.spaces[s].probes = 2
	} else {
		c.requeueUnacked(pc, spaceApp, 2)
		pc.sp.probes = 2
	}
	rec.lossTimer = time.Time{}
	t, _ := c.ptoTimeAndSpace(pc, now)
	rec.lossTimer = t
}

func (c *Conn) requeueUnacked(pc *pathCtx, space int, limit int) {
	sp := c.pathSpace(pc, space)
	n := 0
	for _, p := range sp.sent {
		if !p.ackEliciting {
			continue
		}
		for _, f := range p.frames {
			switch f.kind {
			case sfCrypto, sfStream, sfHandshakeDone, sfResetStream, sfNewCID, sfPathNewCID, sfPathAbandon, sfPathStatus:
				c.requeueFrame(space, f)
			}
		}
		n++
		if limit > 0 && n >= limit {
			return
		}
	}
}
