package quic

import (
	"net/netip"
	"time"
)

const (
	maxPaths             = 4
	maxOutstandingProbes = 4
	maxPendingResponses  = 4
	minChallengeInterval = 50 * time.Millisecond
)

type path struct {
	addr          netip.AddrPort
	validated     bool
	recvBytes     uint64
	sentBytes     uint64
	challenges    [][8]byte
	challengeDue  bool
	nextChallenge time.Time
	deadline      time.Time
	abandonAt     time.Time
	responses     [][8]byte
	migrate       bool
	dcidSeq       uint64
	localSeq      uint64
	lastSeen      time.Time
	pc            *pathCtx
}

func (p *path) amplificationBudget() int {
	if p.validated {
		return 1 << 30
	}
	allowed := 3 * p.recvBytes
	if p.sentBytes >= allowed {
		return 0
	}
	v := allowed - p.sentBytes
	if v > 1<<30 {
		v = 1 << 30
	}
	return int(v)
}

func (p *path) nextTimer() time.Time {
	var t time.Time
	consider := func(x time.Time) {
		if !x.IsZero() && (t.IsZero() || x.Before(t)) {
			t = x
		}
	}
	if len(p.challenges) > 0 {
		consider(p.deadline)
		consider(p.nextChallenge)
	}
	consider(p.abandonAt)
	return t
}

func (c *Conn) pathFor(pc *pathCtx, addr netip.AddrPort) *path {
	for _, p := range c.paths {
		if p.pc == pc && p.addr == addr {
			return p
		}
	}
	return nil
}

func (c *Conn) newPath(pc *pathCtx, addr netip.AddrPort, localSeq uint64, now time.Time) *path {
	count := 0
	victim := -1
	active := c.activeOf(pc)
	for i, p := range c.paths {
		if p.pc != pc {
			continue
		}
		count++
		if p == active {
			continue
		}
		if victim < 0 || p.lastSeen.Before(c.paths[victim].lastSeen) {
			victim = i
		}
	}
	if count >= maxPaths {
		if victim < 0 {
			return nil
		}
		c.removePath(c.paths[victim])
	}
	p := &path{addr: addr, localSeq: localSeq, lastSeen: now, dcidSeq: c.dcidSeq, pc: pc}
	if pc == c.p0 && c.active != nil && localSeq != c.active.localSeq {
		if q := c.unusedPeerCID(); q != nil {
			q.inUse = true
			p.dcidSeq = q.seq
		}
	}
	c.paths = append(c.paths, p)
	if pc != c.p0 && pc.addr == nil {
		pc.addr = p
		c.startValidation(p, now)
	}
	return p
}

func (c *Conn) removePath(p *path) {
	kept := c.paths[:0]
	for _, q := range c.paths {
		if q != p {
			kept = append(kept, q)
		}
	}
	c.paths = kept
	if p.pc == c.p0 && p.dcidSeq != c.dcidSeq {
		c.retirePeerCID(p.dcidSeq)
	}
}

func (c *Conn) initialPTO() time.Duration {
	r := newRTT(c.config.InitialRTT)
	return r.pto(c.peerParams.maxAckDelay)
}

func (c *Conn) startValidation(p *path, now time.Time) {
	if len(p.challenges) > 0 {
		return
	}
	p.challengeDue = true
	t := c.pto()
	if p.pc != nil {
		t = c.pathPTO(p.pc)
	}
	if ip := c.initialPTO(); ip > t {
		t = ip
	}
	p.deadline = now.Add(3 * t)
	c.stats.PathChallenges++
}

func (c *Conn) onPathTimers(now time.Time) {
	for _, p := range append([]*path(nil), c.paths...) {
		active := c.activeOf(p.pc)
		if len(p.challenges) > 0 && !p.deadline.IsZero() && !now.Before(p.deadline) {
			p.challenges = nil
			p.challengeDue = false
			p.migrate = false
			p.deadline = time.Time{}
			if p == active && !p.validated && p.pc != c.p0 {
				c.onPathValidationFailed(p.pc, now)
				continue
			}
			if p != active && !p.validated {
				c.removePath(p)
				continue
			}
		}
		if len(p.challenges) > 0 && !p.nextChallenge.IsZero() && !now.Before(p.nextChallenge) {
			p.challengeDue = true
			p.nextChallenge = time.Time{}
		}
		if !p.abandonAt.IsZero() && !now.Before(p.abandonAt) {
			p.abandonAt = time.Time{}
			if p != active {
				c.removePath(p)
			}
		}
	}
}

func (c *Conn) newChallenge(p *path, now time.Time) [8]byte {
	var d [8]byte
	copy(d[:], randomBytes(8))
	if len(p.challenges) >= maxOutstandingProbes {
		p.challenges = p.challenges[1:]
	}
	p.challenges = append(p.challenges, d)
	p.challengeDue = false
	iv := c.pto()
	if p.pc != nil {
		iv = c.pathPTO(p.pc)
	}
	if iv < minChallengeInterval {
		iv = minChallengeInterval
	}
	p.nextChallenge = now.Add(iv)
	return d
}

func (c *Conn) onPathChallenge(p *path, data [8]byte) {
	if p == nil {
		return
	}
	if len(p.responses) >= maxPendingResponses {
		p.responses = p.responses[1:]
	}
	p.responses = append(p.responses, data)
}

func (c *Conn) onPathResponse(data [8]byte, now time.Time) {
	for _, p := range c.paths {
		for _, ch := range p.challenges {
			if ch != data {
				continue
			}
			p.validated = true
			p.challenges = nil
			p.challengeDue = false
			p.deadline = time.Time{}
			p.nextChallenge = time.Time{}
			if p.migrate && p != c.activeOf(p.pc) {
				p.migrate = false
				c.switchPath(p.pc, p, now)
			}
			if p.pc != c.p0 && p == c.activeOf(p.pc) {
				c.onPathValidated(p.pc, now)
			}
			return
		}
	}
}

func (c *Conn) switchPath(pc *pathCtx, p *path, now time.Time) {
	old := c.activeOf(pc)
	c.setActive(pc, p)
	p.abandonAt = time.Time{}
	c.stats.Migrations++
	if old != nil {
		old.migrate = false
		old.abandonAt = now.Add(3 * c.pathPTO(pc))
		if old.addr.Addr() != p.addr.Addr() {
			pc.rec.cc.reset()
			pc.rec.rtt = newRTT(c.config.InitialRTT)
			pc.rec.pacer = pacer{}
		}
	}
	if pc == c.p0 {
		if q := c.peerCIDBySeq(p.dcidSeq); q != nil && p.dcidSeq != c.dcidSeq {
			c.useDCID(q)
		}
	}
	c.setPathLossTimer(pc, now)
}

func (c *Conn) onPeerAddress(pc *pathCtx, from netip.AddrPort, p *path, nonProbing, largest bool, now time.Time) {
	p.lastSeen = now
	if !nonProbing || !largest {
		return
	}
	if p == c.activeOf(pc) {
		for _, q := range c.paths {
			if q != p && q.pc == pc {
				q.migrate = false
			}
		}
		return
	}
	if p.validated {
		c.switchPath(pc, p, now)
		return
	}
	p.migrate = true
	c.startValidation(p, now)
}

func (c *Conn) onRebind(now time.Time) {
	if c.state >= stateClosing || !c.handshakeDone {
		return
	}
	if next := c.unusedPeerCID(); next != nil && c.confirmed {
		old := c.dcidSeq
		c.useDCID(next)
		c.active.dcidSeq = next.seq
		c.retirePeerCID(old)
	}
	c.cc.reset()
	c.pacer = pacer{}
	c.active.challenges = nil
	c.startValidation(c.active, now)
	c.keepAliveDue = true
}
