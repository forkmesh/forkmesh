package quic

import (
	"bytes"
	"crypto/subtle"
)

type localCID struct {
	seq     uint64
	cid     []byte
	token   [16]byte
	sendDue bool
	retired bool
}

type peerCID struct {
	seq      uint64
	cid      []byte
	token    [16]byte
	hasToken bool
	inUse    bool
}

const maxPendingRetires = 32

func (c *Conn) addLocalCID(cid []byte, announce bool) *localCID {
	l := &localCID{seq: c.nextLocalSeq, cid: cid, token: c.ep.resetToken(cid), sendDue: announce}
	c.nextLocalSeq++
	c.localCIDs = append(c.localCIDs, l)
	c.ep.addCID(cid, c)
	return l
}

func (c *Conn) activeLocalCIDs() int {
	n := 0
	for _, l := range c.localCIDs {
		if !l.retired {
			n++
		}
	}
	return n
}

func (c *Conn) issueConnectionIDs() {
	if !c.havePeerParams {
		return
	}
	limit := int(c.peerParams.activeCIDLimit)
	if limit > c.config.ActiveConnectionIDLimit {
		limit = c.config.ActiveConnectionIDLimit
	}
	for c.activeLocalCIDs() < limit {
		c.addLocalCID(randomBytes(localCIDLen), true)
	}
}

func (c *Conn) localCIDFor(dcid []byte) *localCID {
	for _, l := range c.localCIDs {
		if !l.retired && bytes.Equal(l.cid, dcid) {
			return l
		}
	}
	return nil
}

func (c *Conn) isLocalCID(dcid []byte) bool {
	if c.localCIDFor(dcid) != nil {
		return true
	}
	return !c.isClient && !c.handshakeDone && bytes.Equal(dcid, c.odcid)
}

func (c *Conn) onRetireConnectionID(seq uint64, packetDCID []byte) *TransportError {
	if seq >= c.nextLocalSeq {
		return frameErr(ProtocolViolation, frameRetireConnectionID, "retiring unissued connection id")
	}
	for _, l := range c.localCIDs {
		if l.seq != seq || l.retired {
			continue
		}
		if bytes.Equal(l.cid, packetDCID) {
			return frameErr(ProtocolViolation, frameRetireConnectionID, "retiring the connection id in use")
		}
		l.retired = true
		l.sendDue = false
		c.ep.removeCID(l.cid)
	}
	kept := c.localCIDs[:0]
	for _, l := range c.localCIDs {
		if !l.retired {
			kept = append(kept, l)
		}
	}
	c.localCIDs = kept
	c.issueConnectionIDs()
	return nil
}

func (c *Conn) onNewConnectionID(f *frame) *TransportError {
	if len(c.dcid) == 0 {
		return frameErr(ProtocolViolation, frameNewConnectionID, "peer uses zero-length connection ids")
	}
	for _, p := range c.peerCIDs {
		if p.seq == f.seq {
			if !bytes.Equal(p.cid, f.data) || p.token != f.resetToken {
				return frameErr(ProtocolViolation, frameNewConnectionID, "connection id sequence reused")
			}
			return nil
		}
		if bytes.Equal(p.cid, f.data) {
			return frameErr(ProtocolViolation, frameNewConnectionID, "connection id reused")
		}
	}
	if f.seq < c.peerRetireMin {
		return c.queueRetire(f.seq)
	}
	p := &peerCID{seq: f.seq, cid: append([]byte(nil), f.data...), token: f.resetToken, hasToken: true}
	c.peerCIDs = append(c.peerCIDs, p)
	c.ep.addResetToken(p.token, c)
	if f.retirePrior > c.peerRetireMin {
		c.peerRetireMin = f.retirePrior
		kept := c.peerCIDs[:0]
		for _, q := range c.peerCIDs {
			if q.seq < c.peerRetireMin {
				if q.hasToken {
					c.ep.removeResetToken(q.token)
				}
				if err := c.queueRetire(q.seq); err != nil {
					return err
				}
				continue
			}
			kept = append(kept, q)
		}
		c.peerCIDs = kept
		if c.peerCIDBySeq(c.dcidSeq) == nil {
			next := c.unusedPeerCID()
			if next == nil {
				return frameErr(ProtocolViolation, frameNewConnectionID, "no connection id left after retirement")
			}
			c.useDCID(next)
		}
		for _, p := range c.paths {
			if c.peerCIDBySeq(p.dcidSeq) == nil {
				p.dcidSeq = c.dcidSeq
			}
		}
	}
	if len(c.peerCIDs) > c.config.ActiveConnectionIDLimit {
		return frameErr(ConnectionIDLimitError, frameNewConnectionID, "too many connection ids")
	}
	return nil
}

func (c *Conn) queueRetire(seq uint64) *TransportError {
	for _, s := range c.retireQueue {
		if s == seq {
			return nil
		}
	}
	if len(c.retireQueue) >= maxPendingRetires {
		return transportErr(ConnectionIDLimitError, "too many connection ids awaiting retirement")
	}
	c.retireQueue = append(c.retireQueue, seq)
	return nil
}

func (c *Conn) peerCIDBySeq(seq uint64) *peerCID {
	for _, p := range c.peerCIDs {
		if p.seq == seq {
			return p
		}
	}
	return nil
}

func (c *Conn) unusedPeerCID() *peerCID {
	var best *peerCID
	for _, p := range c.peerCIDs {
		if p.inUse {
			continue
		}
		if best == nil || p.seq < best.seq {
			best = p
		}
	}
	return best
}

func (c *Conn) useDCID(p *peerCID) {
	p.inUse = true
	c.dcidSeq = p.seq
	c.dcid = p.cid
}

func (c *Conn) retirePeerCID(seq uint64) {
	for _, p := range c.paths {
		if p.dcidSeq == seq {
			return
		}
	}
	if seq == c.dcidSeq {
		return
	}
	kept := c.peerCIDs[:0]
	for _, p := range c.peerCIDs {
		if p.seq == seq {
			if p.hasToken {
				c.ep.removeResetToken(p.token)
			}
			c.queueRetire(seq)
			continue
		}
		kept = append(kept, p)
	}
	c.peerCIDs = kept
}

func (c *Conn) dcidForPath(p *path) []byte {
	if q := c.peerCIDBySeq(p.dcidSeq); q != nil {
		return q.cid
	}
	return c.dcid
}

func (c *Conn) matchesResetToken(tail []byte) bool {
	for _, p := range c.peerCIDs {
		if p.hasToken && subtle.ConstantTimeCompare(p.token[:], tail) == 1 {
			return true
		}
	}
	return false
}
