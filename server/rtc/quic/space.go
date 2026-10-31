package quic

import (
	"time"
)

const (
	spaceInitial = iota
	spaceHandshake
	spaceApp
	numSpaces
)

const (
	maxTrackedRanges = 64
	maxAckRangesSent = 32
	maxCryptoBuffer  = 64 << 10
)

type pnSpace struct {
	seal      *directionKeys
	open      *directionKeys
	discarded bool

	nextPN            int64
	largestAcked      int64
	sent              []*sentPacket
	lossTime          time.Time
	lastAckEliciting  time.Time
	elicitingInFlight int
	probes            int

	received         rangeSet
	floor            uint64
	largestRecv      int64
	largestRecvAt    time.Time
	ackImmediate     bool
	ackDeadline      time.Time
	unackedEliciting int
	ackedUpTo        int64
	newSinceAck      bool

	crypto     sendBuf
	cryptoRecv recvBuf
}

func newSpace() *pnSpace {
	return &pnSpace{largestAcked: -1, largestRecv: -1, ackedUpTo: -1}
}

func (sp *pnSpace) isDuplicate(pn int64) bool {
	return uint64(pn) < sp.floor || sp.received.contains(uint64(pn))
}

func (sp *pnSpace) onReceived(pn int64, eliciting, delayable bool, now time.Time, maxAckDelay time.Duration) {
	outOfOrder := false
	if pn > sp.largestRecv {
		if sp.largestRecv >= 0 && pn != sp.largestRecv+1 {
			outOfOrder = true
		}
		sp.largestRecv = pn
		sp.largestRecvAt = now
	} else {
		outOfOrder = true
	}
	sp.received.add(uint64(pn), uint64(pn)+1)
	for len(sp.received) > maxTrackedRanges {
		sp.floor = sp.received[0].end
		sp.received = sp.received[1:]
	}
	sp.newSinceAck = true
	if !eliciting {
		return
	}
	sp.unackedEliciting++
	if !delayable || outOfOrder || sp.unackedEliciting >= 2 {
		sp.ackImmediate = true
		return
	}
	if sp.ackDeadline.IsZero() {
		sp.ackDeadline = now.Add(maxAckDelay)
	}
}

func (sp *pnSpace) ackDue(now time.Time) bool {
	if sp.ackImmediate {
		return true
	}
	return !sp.ackDeadline.IsZero() && !now.Before(sp.ackDeadline)
}

func (sp *pnSpace) ackRanges(dst []ackRange) []ackRange {
	dst = dst[:0]
	for i := len(sp.received) - 1; i >= 0 && len(dst) < maxAckRangesSent; i-- {
		r := sp.received[i]
		if len(dst) > 0 && int64(r.end-1) <= sp.ackedUpTo {
			break
		}
		dst = append(dst, ackRange{smallest: r.start, largest: r.end - 1})
	}
	return dst
}

func (sp *pnSpace) onAckSent() {
	sp.ackImmediate = false
	sp.ackDeadline = time.Time{}
	sp.unackedEliciting = 0
	sp.newSinceAck = false
}

func (sp *pnSpace) discard(cc *congestion) {
	for _, p := range sp.sent {
		if p.inFlight {
			cc.remove(p.size)
		}
	}
	sp.discarded = true
	sp.sent = nil
	sp.lossTime = time.Time{}
	sp.elicitingInFlight = 0
	sp.probes = 0
	sp.crypto = sendBuf{}
	sp.cryptoRecv = recvBuf{}
	sp.ackImmediate = false
	sp.ackDeadline = time.Time{}
	sp.seal = nil
	sp.open = nil
}
