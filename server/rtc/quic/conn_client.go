package quic

import (
	"bytes"
	"time"
)

func (c *Conn) onVersionNegotiation(b []byte) {
	if !c.isClient || c.gotPeerSCID || c.retried || c.stats.PacketsReceived > 0 {
		return
	}
	dcid, scid, versions, ok := parseVersionNegotiation(b)
	if !ok || !bytes.Equal(dcid, c.localCIDs0()) || !bytes.Equal(scid, c.odcid) {
		return
	}
	for _, v := range versions {
		if v == Version1 {
			return
		}
	}
	c.abort(ErrNoCompatibleVersion)
}

func (c *Conn) onRetry(b []byte, h longHeader) {
	if !c.isClient || c.retried || c.gotPeerSCID {
		return
	}
	if !bytes.Equal(h.dcid, c.localCIDs0()) || bytes.Equal(h.scid, c.odcid) {
		return
	}
	if len(b) < h.pnOffset+aeadTagLen+1 || !validRetryTag(c.odcid, b) {
		return
	}
	token := b[h.pnOffset : len(b)-aeadTagLen]
	c.retried = true
	c.retrySCID = append([]byte(nil), h.scid...)
	c.token = append([]byte(nil), token...)
	c.dcid = c.retrySCID
	sp := c.spaces[spaceInitial]
	seal, open := initialKeys(c.dcid, true)
	sp.seal, sp.open = seal, open
	for _, p := range sp.sent {
		if p.inFlight {
			c.cc.remove(p.size)
		}
	}
	sp.sent = nil
	sp.elicitingInFlight = 0
	sp.lossTime = time.Time{}
	sp.crypto.lost = nil
	sp.crypto.acked = nil
	sp.crypto.sent = sp.crypto.base
	c.ptoCount = 0
	c.setLossTimer(time.Now())
}
