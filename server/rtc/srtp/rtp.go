package srtp

import (
	"crypto/subtle"
)

func rtpHeaderLength(p []byte) (int, error) {
	if len(p) < rtpHeaderMin {
		return 0, errShortPacket
	}
	if p[0]>>6 != 2 {
		return 0, errVersion
	}
	n := rtpHeaderMin + 4*int(p[0]&0x0F)
	if len(p) < n {
		return 0, errShortPacket
	}
	if p[0]&0x10 != 0 {
		if len(p) < n+4 {
			return 0, errShortPacket
		}
		n += 4 + 4*(int(p[n+2])<<8|int(p[n+3]))
		if len(p) < n {
			return 0, errShortPacket
		}
	}
	return n, nil
}

func rtpSeqSSRC(p []byte) (uint16, uint32) {
	return uint16(p[2])<<8 | uint16(p[3]), be32(p[8:12])
}

func (c *Context) rtpTagLength() int {
	if c.profile == ProfileAEADAES128GCM {
		return gcmTagLength
	}
	return cmTagLength
}

func (c *Context) rtpAuthTag(out []byte, packet []byte, roc uint32) []byte {
	var r [4]byte
	put32(r[:], roc)
	c.rtpMAC.Reset()
	c.rtpMAC.Write(packet)
	c.rtpMAC.Write(r[:])
	return c.rtpMAC.Sum(out[:0])[:cmTagLength]
}

func (c *Context) EncryptRTP(dst, packet []byte) ([]byte, error) {
	hl, err := rtpHeaderLength(packet)
	if err != nil {
		return nil, err
	}
	seq, ssrc := rtpSeqSSRC(packet)

	c.mu.Lock()
	defer c.mu.Unlock()

	st, started := c.txRTP[ssrc]
	var highest uint64
	if started {
		highest = st.index
	} else if len(c.txRTP) >= maxStreams {
		return nil, errTooManyStreams
	}
	index, err := guessIndex(started, highest, seq)
	if err != nil {
		return nil, err
	}
	roc := uint32(index >> 16)

	n := len(packet)
	out := grow(dst, n+c.rtpTagLength())
	copy(out[:hl], packet[:hl])
	switch c.profile {
	case ProfileAEADAES128GCM:
		nonce := gcmNonce(c.rtpSalt, ssrc, roc, seq)
		c.rtpGCM.Seal(out[hl:hl], nonce[:], packet[hl:], out[:hl])
	default:
		cmXOR(c.rtpBlock, c.rtpSalt, ssrc, index, out[hl:n], packet[hl:])
		copy(out[n:], c.rtpAuthTag(c.macBuf[:], out[:n], roc))
	}

	if !started {
		c.txRTP[ssrc] = &txRTPState{index: index}
	} else if index > st.index {
		st.index = index
	}
	return out, nil
}

func (c *Context) DecryptRTP(dst, packet []byte) ([]byte, error) {
	tl := c.rtpTagLength()
	if len(packet) < rtpHeaderMin+tl {
		return nil, errShortPacket
	}
	n := len(packet) - tl
	hl, err := rtpHeaderLength(packet[:n])
	if err != nil {
		return nil, err
	}
	seq, ssrc := rtpSeqSSRC(packet)

	c.mu.Lock()
	defer c.mu.Unlock()

	st := c.rxRTP[ssrc]
	if st == nil && len(c.rxRTP) >= maxStreams {
		return nil, errTooManyStreams
	}
	var w replayWindow
	if st != nil {
		w = st.window
	}
	index, err := guessIndex(w.started, w.top, seq)
	if err != nil {
		return nil, err
	}
	if !w.fresh(index) {
		return nil, errReplay
	}
	roc := uint32(index >> 16)

	var out []byte
	switch c.profile {
	case ProfileAEADAES128GCM:
		nonce := gcmNonce(c.rtpSalt, ssrc, roc, seq)
		out = grow(dst, n)
		copy(out[:hl], packet[:hl])
		if _, err := c.rtpGCM.Open(out[hl:hl], nonce[:], packet[hl:], packet[:hl]); err != nil {
			return nil, errAuth
		}
	default:
		tag := c.rtpAuthTag(c.macBuf[:], packet[:n], roc)
		if subtle.ConstantTimeCompare(tag, packet[n:]) != 1 {
			return nil, errAuth
		}
		out = grow(dst, n)
		copy(out[:hl], packet[:hl])
		cmXOR(c.rtpBlock, c.rtpSalt, ssrc, index, out[hl:n], packet[hl:n])
	}

	if st == nil {
		st = &rxState{}
		c.rxRTP[ssrc] = st
	}
	st.window.mark(index)
	return out, nil
}
