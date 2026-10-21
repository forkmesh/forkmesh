package quic

import (
	"time"
)

type pendingPacket struct {
	space     int
	typ       packetType
	pn        int64
	pnLen     int
	header    []byte
	lengthAt  int
	payload   []byte
	frames    []sentFrame
	eliciting bool
	padded    bool
	pathFrame bool
	dgrams    [][]byte
}

func (c *Conn) flush(now time.Time) {
	if c.state >= stateClosing || c.tls == nil {
		return
	}
	if c.mp != nil {
		c.prepareSchedule(now)
	}
	paths := c.sendPaths()
	busy := true
	for i := 0; i < 64; i++ {
		progress := false
		for _, pc := range paths {
			if c.sendOn(pc, now) {
				progress = true
			}
			if c.state >= stateClosing {
				return
			}
		}
		if !progress {
			busy = false
			break
		}
	}
	c.sendPathProbes(now)
	if busy {
		c.wake()
	}
}

func (c *Conn) anyProbes(pc *pathCtx) bool {
	for i := 0; i < numSpaces; i++ {
		sp := c.pathSpace(pc, i)
		if sp != nil && !sp.discarded && sp.probes > 0 {
			return true
		}
	}
	return false
}

func (c *Conn) hasPendingAppData() bool {
	return len(c.dgramOut) > 0 || len(c.sendQueue) > 0
}

func (c *Conn) pacingDeadline(now time.Time) time.Time {
	if c.state >= stateClosing || !c.hasPendingAppData() {
		return time.Time{}
	}
	var best time.Time
	for _, pc := range c.sendPaths() {
		if !c.usable(pc) {
			continue
		}
		if t := c.pathPacingDeadline(pc, now); !t.IsZero() && (best.IsZero() || t.Before(best)) {
			best = t
		}
	}
	return best
}

func (c *Conn) pathPacingDeadline(pc *pathCtx, now time.Time) time.Time {
	rec := pc.rec
	size := c.maxSendSize
	if rec.cc.inFlight+size > rec.cc.cwnd {
		return time.Time{}
	}
	rec.pacer.refill(now, rec.cc.cwnd, rec.rtt.smoothed)
	if rec.pacer.canSend(size) {
		return time.Time{}
	}
	return rec.pacer.nextSend(size, rec.cc.cwnd, rec.rtt.smoothed, now)
}

func (c *Conn) headerFor(s int, pn int64, pnLen int, dcid []byte, dst []byte) ([]byte, int, packetType) {
	switch s {
	case spaceInitial:
		h, at := appendLongHeader(dst[:0], packetInitial, dcid, c.localCIDs0(), c.token, pnLen)
		return h, at, packetInitial
	case spaceHandshake:
		h, at := appendLongHeader(dst[:0], packetHandshake, dcid, c.localCIDs0(), nil, pnLen)
		return h, at, packetHandshake
	}
	return appendShortHeader(dst[:0], dcid, pnLen, c.keys.writePhase&1 == 1), -1, packet1RTT
}

func (c *Conn) localCIDs0() []byte {
	if len(c.localCIDs) > 0 {
		return c.localCIDs[0].cid
	}
	return nil
}

func (c *Conn) sendOn(pc *pathCtx, now time.Time) bool {
	p := c.activeOf(pc)
	if p == nil || !c.usable(pc) {
		return false
	}
	rec := pc.rec
	size := c.maxSendSize
	budget := size
	if !c.isClient {
		if b := p.amplificationBudget(); b < budget {
			budget = b
		}
	}
	if budget < 48 {
		return false
	}
	dcid, ok := c.dcidOf(pc)
	if !ok {
		return false
	}
	rec.pacer.refill(now, rec.cc.cwnd, rec.rtt.smoothed)
	probing := c.anyProbes(pc)
	ccAllows := rec.cc.inFlight+size <= rec.cc.cwnd
	paceAllows := rec.pacer.canSend(size)
	eliciting := probing || (ccAllows && paceAllows)
	c.maybeInitiateKeyUpdate()
	var pkts [numSpaces]pendingPacket
	n := 0
	used := 0
	for s := 0; s < numSpaces; s++ {
		sp := c.pathSpace(pc, s)
		if sp == nil {
			continue
		}
		if keys := c.spaces[s]; keys.discarded || keys.seal == nil {
			continue
		}
		pnLen := packetNumberLen(sp.nextPN, sp.largestAcked)
		hdr, lengthAt, typ := c.headerFor(s, sp.nextPN, pnLen, dcid, c.headerScratch[s][:0])
		c.headerScratch[s] = hdr
		overhead := len(hdr) + pnLen + aeadTagLen
		room := budget - used - overhead
		if room < 4 {
			continue
		}
		elic := eliciting || sp.probes > 0
		if s == spaceInitial && !c.isClient && budget < minInitialSize {
			elic = false
		}
		payload, frames, isElic, pathFrame := c.buildFrames(pc, s, room, elic, now)
		if len(payload) == 0 {
			continue
		}
		for pnLen+len(payload) < 4 {
			payload = append(payload, 0)
		}
		var dgrams [][]byte
		if len(c.builtDgrams) > 0 {
			dgrams = append(dgrams, c.builtDgrams...)
		}
		pkts[n] = pendingPacket{space: s, typ: typ, pn: sp.nextPN, pnLen: pnLen, header: hdr, lengthAt: lengthAt, payload: payload, frames: frames, eliciting: isElic, pathFrame: pathFrame, dgrams: dgrams}
		n++
		used += overhead + len(payload)
	}
	if n == 0 {
		return false
	}
	needPad := false
	for i := 0; i < n; i++ {
		if pkts[i].typ == packetInitial && (c.isClient || pkts[i].eliciting) {
			needPad = true
		}
		if pkts[i].pathFrame {
			needPad = true
		}
	}
	if needPad && used < minInitialSize {
		target := minInitialSize
		if target > budget {
			target = budget
		}
		if target > used {
			last := &pkts[n-1]
			for i := used; i < target; i++ {
				last.payload = append(last.payload, 0)
			}
			last.padded = true
			used = target
		}
	}
	out := c.sealAndRecord(pc, pkts[:n], now)
	c.write(p, out)
	if c.isClient {
		for i := 0; i < n; i++ {
			if pkts[i].space == spaceHandshake {
				c.discardSpace(spaceInitial)
				break
			}
		}
	}
	return true
}

func (c *Conn) sealAndRecord(pc *pathCtx, pkts []pendingPacket, now time.Time) []byte {
	rec := pc.rec
	out := c.datagramScratch[:0]
	anyEliciting := false
	inFlightBytes := 0
	for i := range pkts {
		pk := &pkts[i]
		sp := c.pathSpace(pc, pk.space)
		seal := c.spaces[pk.space].seal
		pathID := uint64(0)
		if pk.space == spaceApp {
			pathID = pc.id
		}
		start := len(out)
		out = append(out, pk.header...)
		if pk.lengthAt >= 0 {
			putVarint2(out[start+pk.lengthAt:], uint64(pk.pnLen+len(pk.payload)+aeadTagLen))
		}
		out = appendPacketNumber(out, pk.pn, pk.pnLen)
		pnOffset := len(out) - start - pk.pnLen
		hdrLen := len(out) - start
		out = append(out, pk.payload...)
		if cap(out)-len(out) < aeadTagLen {
			grown := make([]byte, len(out), len(out)+2*aeadTagLen+256)
			copy(grown, out)
			out = grown
		}
		sealed := seal.key.sealPath(pathID, out[start:], hdrLen, pk.pn)
		out = out[:start+len(sealed)]
		seal.hp.protect(out[start:], pnOffset, pk.pnLen)
		pktSize := len(out) - start
		sent := &sentPacket{
			pn:           pk.pn,
			sentAt:       now,
			size:         pktSize,
			ackEliciting: pk.eliciting,
			inFlight:     pk.eliciting || pk.padded,
			phase:        c.keys.writePhase,
			frames:       append([]sentFrame(nil), pk.frames...),
			dgrams:       pk.dgrams,
		}
		sp.sent = append(sp.sent, sent)
		sp.nextPN++
		c.stats.PacketsSent++
		if sent.inFlight {
			rec.cc.onSent(pktSize)
			inFlightBytes += pktSize
		}
		if pk.eliciting {
			anyEliciting = true
			sp.lastAckEliciting = now
			sp.elicitingInFlight++
			if sp.probes > 0 {
				sp.probes--
			}
			if pk.space == spaceApp {
				pc.lastElicit = now
				pc.pingDue = false
			}
		}
		if pk.space == spaceApp {
			c.keys.sentInPhase++
			pc.sent++
		}
	}
	c.datagramScratch = out[:0]
	if inFlightBytes > 0 {
		rec.pacer.onSent(inFlightBytes)
	}
	if anyEliciting {
		if !c.elicitingSince {
			c.idleStart = now
			c.elicitingSince = true
		}
		c.lastSent = now
		c.keepAliveDue = false
		c.setPathLossTimer(pc, now)
	}
	return out
}

func (c *Conn) write(p *path, b []byte) {
	p.sentBytes += uint64(len(b))
	c.stats.BytesSent += uint64(len(b))
	var s *socket
	if p.pc != nil {
		s = p.pc.sock
	}
	c.ep.writeVia(s, b, p.addr)
}

func (c *Conn) maybeInitiateKeyUpdate() {
	k := &c.keys
	if !c.confirmed || k.write == nil || k.readCur == nil {
		return
	}
	if k.writePhase != k.readPhase || !k.ackedInPhase {
		return
	}
	if k.sentInPhase < c.config.KeyUpdateInterval {
		return
	}
	c.rotateWriteKeys()
	c.stats.KeyUpdates++
}

func (c *Conn) buildFrames(pc *pathCtx, s int, room int, eliciting bool, now time.Time) ([]byte, []sentFrame, bool, bool) {
	sp := c.pathSpace(pc, s)
	b := c.payloadScratch[s][:0]
	frames := c.framesScratch[s][:0]
	c.builtDgrams = c.builtDgrams[:0]
	isElic := false
	pathFrame := false
	defer func() {
		c.framesScratch[s] = frames[:0]
	}()
	multi := c.mp != nil && s == spaceApp
	ackLen := 0
	var ackLargest uint64
	if sp.newSinceAck && len(sp.received) > 0 && (sp.ackDue(now) || eliciting) {
		ranges := sp.ackRanges(c.ackScratch)
		c.ackScratch = ranges
		var delay uint64
		if d := now.Sub(sp.largestRecvAt); d > 0 {
			delay = uint64(d/time.Microsecond) >> 3
		}
		flen := func(r []ackRange) int {
			if multi {
				return pathAckFrameLen(pc.id, r, delay)
			}
			return ackFrameLen(r, delay)
		}
		for len(ranges) > 1 && flen(ranges) > room {
			ranges = ranges[:len(ranges)-1]
		}
		if flen(ranges) <= room {
			if multi {
				b = appendPathAckFrame(b, pc.id, ranges, delay)
			} else {
				b = appendAckFrame(b, ranges, delay)
			}
			ackLen = len(b)
			ackLargest = ranges[0].largest
		}
	}
	if multi {
		b, frames = c.appendForeignAcks(pc, b, frames, room, now)
	}
	finishAck := func() {
		if ackLen == 0 {
			return
		}
		if isElic || sp.ackDue(now) {
			frames = append(frames, sentFrame{kind: sfAck, id: ackLargest, offset: pc.id})
			sp.onAckSent()
			return
		}
		b = b[ackLen:]
	}
	if !eliciting {
		finishAck()
		return b, frames, false, false
	}
	left := func() int { return room - len(b) }
	primary := c.isPrimary(pc)
	if s == spaceApp {
		if primary && c.handshakeDoneDue && left() >= 1 {
			b = append(b, frameHandshakeDone)
			frames = append(frames, sentFrame{kind: sfHandshakeDone})
			c.handshakeDoneDue = false
			isElic = true
		}
		if p := c.activeOf(pc); p != nil {
			for len(p.responses) > 0 && left() >= 9 {
				b = append(b, framePathResponse)
				b = append(b, p.responses[0][:]...)
				p.responses = p.responses[1:]
				isElic = true
				pathFrame = true
			}
			if p.challengeDue && left() >= 9 {
				d := c.newChallenge(p, now)
				b = append(b, framePathChallenge)
				b = append(b, d[:]...)
				isElic = true
				pathFrame = true
			}
		}
	}
	if s == spaceApp && primary {
		if c.maxDataDue {
			if l := 1 + VarintLen(c.recvMaxData); l <= left() {
				b = appendOneVarintFrame(b, frameMaxData, c.recvMaxData)
				frames = append(frames, sentFrame{kind: sfMaxData})
				c.maxDataDue = false
				isElic = true
			}
		}
		for k := 0; k < 2; k++ {
			if !c.maxStreamsPending[k] {
				continue
			}
			v, typ, kind := c.maxPeerBidi, uint64(frameMaxStreamsBidi), sfMaxStreamsBidi
			if k == 1 {
				v, typ, kind = c.maxPeerUni, frameMaxStreamsUni, sfMaxStreamsUni
			}
			if l := 1 + VarintLen(v); l <= left() {
				b = appendOneVarintFrame(b, typ, v)
				frames = append(frames, sentFrame{kind: kind})
				c.maxStreamsPending[k] = false
				isElic = true
			}
		}
		if c.dataBlockedDue {
			if l := 1 + VarintLen(c.sendMaxData); l <= left() {
				b = appendOneVarintFrame(b, frameDataBlocked, c.sendMaxData)
				frames = append(frames, sentFrame{kind: sfDataBlocked, offset: c.sendMaxData})
				c.dataBlockedDue = false
				isElic = true
			}
		}
		for k := 0; k < 2; k++ {
			if !c.blockedDue[k] {
				continue
			}
			typ, kind := uint64(frameStreamsBlockedBidi), sfStreamsBlockedBidi
			if k == 1 {
				typ, kind = frameStreamsBlockedUni, sfStreamsBlockedUni
			}
			v := c.streamsBlocked[k]
			if l := 1 + VarintLen(v); l <= left() {
				b = appendOneVarintFrame(b, typ, v)
				frames = append(frames, sentFrame{kind: kind})
				c.blockedDue[k] = false
				isElic = true
			}
		}
		for _, l := range c.localCIDs {
			if !l.sendDue || l.retired {
				continue
			}
			need := 1 + VarintLen(l.seq) + 1 + 1 + len(l.cid) + 16
			if need > left() {
				break
			}
			b = appendNewConnectionID(b, l.seq, 0, l.cid, l.token)
			frames = append(frames, sentFrame{kind: sfNewCID, id: l.seq})
			l.sendDue = false
			isElic = true
		}
		for len(c.retireQueue) > 0 {
			seq := c.retireQueue[0]
			if 1+VarintLen(seq) > left() {
				break
			}
			b = appendOneVarintFrame(b, frameRetireConnectionID, seq)
			frames = append(frames, sentFrame{kind: sfRetireCID, id: seq})
			c.retireQueue = c.retireQueue[1:]
			isElic = true
		}
		b, frames, isElic = c.appendStreamControl(b, frames, room, isElic)
		if c.mp != nil {
			b, frames, isElic = c.appendMultipathControl(b, frames, room, isElic)
		}
	}
	if s != spaceApp || primary {
		b, frames, isElic = c.appendCrypto(s, b, frames, room, isElic)
	}
	if s == spaceApp {
		if primary && c.keepAliveDue && left() >= 1 {
			b = append(b, framePing)
			frames = append(frames, sentFrame{kind: sfPing})
			c.keepAliveDue = false
			isElic = true
		}
		if pc.pingDue && !isElic && left() >= 1 {
			b = append(b, framePing)
			frames = append(frames, sentFrame{kind: sfPing})
			isElic = true
		}
		b, frames, isElic = c.appendDatagrams(pc, primary, b, frames, room, isElic, now)
		if primary {
			b, frames, isElic = c.appendStreams(b, frames, room, isElic)
		}
	}
	if !isElic && sp.probes > 0 && left() >= 1 {
		b = append(b, framePing)
		frames = append(frames, sentFrame{kind: sfPing})
		isElic = true
	}
	finishAck()
	return b, frames, isElic, pathFrame
}

func (c *Conn) appendCrypto(s int, b []byte, frames []sentFrame, room int, isElic bool) ([]byte, []sentFrame, bool) {
	cs := &c.spaces[s].crypto
	for {
		left := room - len(b)
		if left < 8 {
			return b, frames, isElic
		}
		if !cs.lost.empty() {
			r := cs.lost[0]
			hdr := cryptoHeaderLen(r.start, left)
			max := left - hdr
			if max <= 0 {
				return b, frames, isElic
			}
			n := r.end - r.start
			if n > uint64(max) {
				n = uint64(max)
			}
			b = appendCryptoHeader(b, r.start, int(n))
			b = append(b, cs.slice(r.start, r.start+n)...)
			cs.markLostSent(r.start, r.start+n)
			frames = append(frames, sentFrame{kind: sfCrypto, offset: r.start, length: n})
			isElic = true
			continue
		}
		pending := cs.end() - cs.sent
		if pending == 0 {
			return b, frames, isElic
		}
		hdr := cryptoHeaderLen(cs.sent, left)
		max := left - hdr
		if max <= 0 {
			return b, frames, isElic
		}
		n := pending
		if n > uint64(max) {
			n = uint64(max)
		}
		b = appendCryptoHeader(b, cs.sent, int(n))
		b = append(b, cs.slice(cs.sent, cs.sent+n)...)
		frames = append(frames, sentFrame{kind: sfCrypto, offset: cs.sent, length: n})
		cs.sent += n
		isElic = true
	}
}

func (c *Conn) appendStreamControl(b []byte, frames []sentFrame, room int, isElic bool) ([]byte, []sentFrame, bool) {
	kept := c.controlQueue[:0]
	for i, s := range c.controlQueue {
		left := room - len(b)
		if left < 24 {
			kept = append(kept, c.controlQueue[i:]...)
			break
		}
		if s.removed {
			s.inControlQueue = false
			continue
		}
		if s.resetDue {
			if l := 1 + VarintLen(s.id) + VarintLen(s.resetCode) + VarintLen(s.resetSize); l <= left {
				b = appendResetStream(b, s.id, s.resetCode, s.resetSize)
				frames = append(frames, sentFrame{kind: sfResetStream, id: s.id})
				s.resetDue = false
				isElic = true
			}
		}
		left = room - len(b)
		if s.stopDue {
			if l := 1 + VarintLen(s.id) + VarintLen(s.stopSendCode); l <= left {
				b = appendTwoVarintFrame(b, frameStopSending, s.id, s.stopSendCode)
				frames = append(frames, sentFrame{kind: sfStopSending, id: s.id})
				s.stopDue = false
				isElic = true
			}
		}
		left = room - len(b)
		if s.maxDataDue {
			if l := 1 + VarintLen(s.id) + VarintLen(s.recvMax); l <= left {
				b = appendTwoVarintFrame(b, frameMaxStreamData, s.id, s.recvMax)
				frames = append(frames, sentFrame{kind: sfMaxStreamData, id: s.id})
				s.maxDataDue = false
				isElic = true
			}
		}
		left = room - len(b)
		if s.blockedDue {
			if l := 1 + VarintLen(s.id) + VarintLen(s.sendMax); l <= left {
				b = appendTwoVarintFrame(b, frameStreamDataBlocked, s.id, s.sendMax)
				frames = append(frames, sentFrame{kind: sfStreamDataBlocked, id: s.id, offset: s.sendMax})
				s.blockedDue = false
				isElic = true
			}
		}
		if s.resetDue || s.stopDue || s.maxDataDue || s.blockedDue {
			kept = append(kept, s)
		} else {
			s.inControlQueue = false
		}
	}
	c.controlQueue = kept
	return b, frames, isElic
}

func (c *Conn) connCredit() uint64 {
	if c.sentData >= c.sendMaxData {
		return 0
	}
	return c.sendMaxData - c.sentData
}

func (c *Conn) streamLimit(s *Stream) uint64 {
	limit := s.sendMax
	if l := s.send.sent + c.connCredit(); l < limit {
		limit = l
	}
	return limit
}

func (c *Conn) streamHasPending(s *Stream) bool {
	if s.resetLocal || s.removed {
		return false
	}
	return s.send.hasLost() || s.send.pendingNew(c.streamLimit(s)) > 0 || s.send.needFin()
}

func (c *Conn) appendStreams(b []byte, frames []sentFrame, room int, isElic bool) ([]byte, []sentFrame, bool) {
	if len(c.sendQueue) == 0 {
		return b, frames, isElic
	}
	var again []*Stream
	processed := 0
	for processed < len(c.sendQueue) {
		if room-len(b) < 16 {
			break
		}
		s := c.sendQueue[processed]
		processed++
		var wrote bool
		b, frames, wrote = c.writeStream(s, b, frames, room)
		if wrote {
			isElic = true
		}
		if c.streamHasPending(s) {
			again = append(again, s)
		} else {
			s.inSendQueue = false
		}
	}
	rest := make([]*Stream, 0, len(c.sendQueue)-processed+len(again))
	rest = append(rest, c.sendQueue[processed:]...)
	rest = append(rest, again...)
	c.sendQueue = rest
	return b, frames, isElic
}

func (c *Conn) writeStream(s *Stream, b []byte, frames []sentFrame, room int) ([]byte, []sentFrame, bool) {
	wrote := false
	if s.resetLocal || s.removed {
		return b, frames, false
	}
	sb := &s.send
	for {
		left := room - len(b)
		if left < 12 {
			return b, frames, wrote
		}
		if !sb.lost.empty() {
			r := sb.lost[0]
			hdr := streamHeaderLen(s.id, r.start, left, true)
			max := left - hdr
			if max <= 0 {
				return b, frames, wrote
			}
			n := r.end - r.start
			if n > uint64(max) {
				n = uint64(max)
			}
			fin := sb.fin && !sb.finSent && sb.sent == sb.end() && r.start+n == sb.end()
			b = appendStreamHeader(b, s.id, r.start, int(n), fin, true)
			b = append(b, sb.slice(r.start, r.start+n)...)
			sb.markLostSent(r.start, r.start+n)
			if fin {
				sb.finSent = true
			}
			frames = append(frames, sentFrame{kind: sfStream, id: s.id, offset: r.start, length: n, fin: fin})
			wrote = true
			continue
		}
		limit := c.streamLimit(s)
		n := sb.pendingNew(limit)
		if n == 0 {
			if sb.needFin() {
				hdr := streamHeaderLen(s.id, sb.sent, 0, true)
				if hdr > left {
					return b, frames, wrote
				}
				b = appendStreamHeader(b, s.id, sb.sent, 0, true, true)
				sb.finSent = true
				frames = append(frames, sentFrame{kind: sfStream, id: s.id, offset: sb.sent, fin: true})
				wrote = true
				return b, frames, wrote
			}
			if sb.end() > sb.sent {
				if s.sendMax <= sb.sent && !s.blockedDue {
					s.blockedDue = true
					c.queueControl(s)
				}
				if c.connCredit() == 0 && c.dataBlockedAt != c.sendMaxData {
					c.dataBlockedAt = c.sendMaxData
					c.dataBlockedDue = true
				}
			}
			return b, frames, wrote
		}
		hdr := streamHeaderLen(s.id, sb.sent, left, true)
		max := left - hdr
		if max <= 0 {
			return b, frames, wrote
		}
		if n > uint64(max) {
			n = uint64(max)
		}
		fin := sb.fin && sb.sent+n == sb.end()
		off := sb.sent
		b = appendStreamHeader(b, s.id, off, int(n), fin, true)
		b = append(b, sb.slice(off, off+n)...)
		sb.sent += n
		c.sentData += n
		if fin {
			sb.finSent = true
		}
		frames = append(frames, sentFrame{kind: sfStream, id: s.id, offset: off, length: n, fin: fin})
		wrote = true
	}
}

func (c *Conn) appendDatagrams(pc *pathCtx, primary bool, b []byte, frames []sentFrame, room int, isElic bool, now time.Time) ([]byte, []sentFrame, bool) {
	if len(c.dgramOut) == 0 {
		return b, frames, isElic
	}
	kept := c.dgramOut[:0]
	full := false
	limit := c.maxSendSize - (1 + maxCIDLen + 4 + aeadTagLen)
	for _, d := range c.dgramOut {
		if full || !c.dgramEligible(d, pc, primary) {
			kept = append(kept, d)
			continue
		}
		fl := datagramFrameLen(len(d.data))
		if fl > room-len(b) {
			if fl > limit {
				c.stats.DatagramsDropped++
				continue
			}
			full = true
			kept = append(kept, d)
			continue
		}
		b = appendDatagramFrame(b, d.data)
		frames = append(frames, sentFrame{kind: sfDatagram})
		c.stats.DatagramsSent++
		isElic = true
		if c.mp != nil && d.policy != Redundant {
			c.builtDgrams = append(c.builtDgrams, d.data)
		}
		if c.dgramSent(d, pc, now) {
			kept = append(kept, d)
		}
	}
	for i := len(kept); i < len(c.dgramOut); i++ {
		c.dgramOut[i] = nil
	}
	c.dgramOut = kept
	return b, frames, isElic
}

func (c *Conn) sendPathProbes(now time.Time) {
	if c.keys.write == nil || c.spaces[spaceApp].discarded {
		return
	}
	for _, p := range c.paths {
		pc := p.pc
		if pc == nil || pc.removed || pc.abandoned {
			continue
		}
		if p == c.activeOf(pc) && c.usable(pc) {
			continue
		}
		if !p.challengeDue && len(p.responses) == 0 {
			continue
		}
		budget := c.maxSendSize
		if !c.isClient && !p.validated {
			if a := p.amplificationBudget(); a < budget {
				budget = a
			}
		}
		dcid, ok := c.dcidForPath(p)
		if !ok {
			continue
		}
		sp := pc.sp
		pnLen := packetNumberLen(sp.nextPN, sp.largestAcked)
		hdr := appendShortHeader(c.headerScratch[spaceApp][:0], dcid, pnLen, c.keys.writePhase&1 == 1)
		c.headerScratch[spaceApp] = hdr
		overhead := len(hdr) + pnLen + aeadTagLen
		if budget < overhead+9 {
			continue
		}
		payload := c.payloadScratch[spaceApp][:0]
		for len(p.responses) > 0 && budget-overhead-len(payload) >= 9 {
			payload = append(payload, framePathResponse)
			payload = append(payload, p.responses[0][:]...)
			p.responses = p.responses[1:]
		}
		if p.challengeDue && budget-overhead-len(payload) >= 9 {
			d := c.newChallenge(p, now)
			payload = append(payload, framePathChallenge)
			payload = append(payload, d[:]...)
		}
		target := minInitialSize
		if target > budget {
			target = budget
		}
		for overhead+len(payload) < target {
			payload = append(payload, 0)
		}
		pk := pendingPacket{space: spaceApp, typ: packet1RTT, pn: sp.nextPN, pnLen: pnLen, header: hdr, lengthAt: -1, payload: payload, eliciting: true}
		out := c.sealAndRecordProbe(pc, pk, now)
		c.payloadScratch[spaceApp] = payload[:0]
		c.write(p, out)
	}
}

func (c *Conn) sealAndRecordProbe(pc *pathCtx, pk pendingPacket, now time.Time) []byte {
	sp := pc.sp
	seal := c.spaces[spaceApp].seal
	out := c.datagramScratch[:0]
	out = append(out, pk.header...)
	out = appendPacketNumber(out, pk.pn, pk.pnLen)
	hdrLen := len(out)
	pnOffset := hdrLen - pk.pnLen
	out = append(out, pk.payload...)
	if cap(out)-len(out) < aeadTagLen {
		grown := make([]byte, len(out), len(out)+aeadTagLen+256)
		copy(grown, out)
		out = grown
	}
	out = seal.key.sealPath(pc.id, out, hdrLen, pk.pn)
	seal.hp.protect(out, pnOffset, pk.pnLen)
	sp.sent = append(sp.sent, &sentPacket{pn: pk.pn, sentAt: now, size: len(out), ackEliciting: true, phase: c.keys.writePhase})
	sp.nextPN++
	c.stats.PacketsSent++
	c.keys.sentInPhase++
	c.datagramScratch = out[:0]
	return out
}

func (c *Conn) sendClose(now time.Time) {
	c.lastCloseSent = now
	pc := c.closePath()
	if pc == nil || c.tls == nil {
		return
	}
	p := c.activeOf(pc)
	if p == nil {
		return
	}
	dcid, ok := c.dcidOf(pc)
	if !ok {
		return
	}
	budget := c.maxSendSize
	if !c.isClient {
		if b := p.amplificationBudget(); b < budget {
			budget = b
		}
	}
	var pkts [numSpaces]pendingPacket
	n := 0
	used := 0
	useApp := c.handshakeDone && c.spaces[spaceApp].seal != nil && !c.spaces[spaceApp].discarded
	for s := 0; s < numSpaces; s++ {
		sp := c.pathSpace(pc, s)
		if sp == nil {
			continue
		}
		if keys := c.spaces[s]; keys.discarded || keys.seal == nil {
			continue
		}
		if s == spaceApp && !useApp {
			continue
		}
		if s != spaceApp && useApp && c.confirmed {
			continue
		}
		pnLen := packetNumberLen(sp.nextPN, sp.largestAcked)
		hdr, lengthAt, typ := c.headerFor(s, sp.nextPN, pnLen, dcid, c.headerScratch[s][:0])
		c.headerScratch[s] = hdr
		app := c.closeApp
		code, ft, reason := c.closeCode, c.closeFrameType, c.closeReason
		if s != spaceApp && app {
			app = false
			code, ft, reason = uint64(ApplicationErrorCode), 0, ""
		}
		payload := appendConnectionClose(c.payloadScratch[s][:0], app, code, ft, reason)
		overhead := len(hdr) + pnLen + aeadTagLen
		if used+overhead+len(payload) > budget {
			payload = appendConnectionClose(c.payloadScratch[s][:0], app, code, ft, "")
			if used+overhead+len(payload) > budget {
				continue
			}
		}
		for pnLen+len(payload) < 4 {
			payload = append(payload, 0)
		}
		c.payloadScratch[s] = payload
		pkts[n] = pendingPacket{space: s, typ: typ, pn: sp.nextPN, pnLen: pnLen, header: hdr, lengthAt: lengthAt, payload: payload}
		n++
		used += overhead + len(payload)
	}
	if n == 0 {
		return
	}
	needPad := false
	for i := 0; i < n; i++ {
		if pkts[i].typ == packetInitial && c.isClient {
			needPad = true
		}
	}
	if needPad && used < minInitialSize && budget >= minInitialSize {
		last := &pkts[n-1]
		for i := used; i < minInitialSize; i++ {
			last.payload = append(last.payload, 0)
		}
	}
	out := c.sealAndRecord(pc, pkts[:n], now)
	c.write(p, out)
}
