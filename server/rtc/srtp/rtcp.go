package srtp

import (
	"crypto/subtle"
)

const eFlag = 0x80000000

func rtcpCheck(p []byte) error {
	if len(p) < rtcpHeaderMin {
		return errShortPacket
	}
	if p[0]>>6 != 2 {
		return errVersion
	}
	return nil
}

func (c *Context) rtcpTagLength() int {
	if c.profile == ProfileAEADAES128GCM {
		return gcmTagLength
	}
	return cmTagLength
}

func (c *Context) rtcpAuthTag(out []byte, packet []byte) []byte {
	c.rtcpMAC.Reset()
	c.rtcpMAC.Write(packet)
	return c.rtcpMAC.Sum(out[:0])[:cmTagLength]
}

func (c *Context) EncryptRTCP(dst, packet []byte) ([]byte, error) {
	if err := rtcpCheck(packet); err != nil {
		return nil, err
	}
	ssrc := be32(packet[4:8])

	c.mu.Lock()
	defer c.mu.Unlock()

	index, started := c.txRTCP[ssrc]
	if !started && len(c.txRTCP) >= maxStreams {
		return nil, errTooManyStreams
	}
	if index > maxSRTCPIndex {
		return nil, errExhausted
	}
	word := eFlag | index

	n := len(packet)
	tl := c.rtcpTagLength()
	out := grow(dst, n+tl+srtcpIndexSize)
	copy(out[:rtcpHeaderMin], packet[:rtcpHeaderMin])
	switch c.profile {
	case ProfileAEADAES128GCM:
		nonce := gcmNonce(c.rtcpSalt, ssrc, index>>16, uint16(index))
		var aad [rtcpHeaderMin + srtcpIndexSize]byte
		copy(aad[:], packet[:rtcpHeaderMin])
		put32(aad[rtcpHeaderMin:], word)
		c.rtcpGCM.Seal(out[rtcpHeaderMin:rtcpHeaderMin], nonce[:], packet[rtcpHeaderMin:], aad[:])
		put32(out[n+tl:], word)
	default:
		cmXOR(c.rtcpBlk, c.rtcpSalt, ssrc, uint64(index), out[rtcpHeaderMin:n], packet[rtcpHeaderMin:])
		put32(out[n:], word)
		copy(out[n+srtcpIndexSize:], c.rtcpAuthTag(c.macBuf[:], out[:n+srtcpIndexSize]))
	}

	c.txRTCP[ssrc] = index + 1
	return out, nil
}

func (c *Context) DecryptRTCP(dst, packet []byte) ([]byte, error) {
	tl := c.rtcpTagLength()
	if len(packet) < rtcpHeaderMin+tl+srtcpIndexSize {
		return nil, errShortPacket
	}
	if err := rtcpCheck(packet); err != nil {
		return nil, err
	}
	ssrc := be32(packet[4:8])

	var wordAt, bodyEnd int
	if c.profile == ProfileAEADAES128GCM {
		wordAt = len(packet) - srtcpIndexSize
		bodyEnd = wordAt - tl
	} else {
		wordAt = len(packet) - tl - srtcpIndexSize
		bodyEnd = wordAt
	}
	word := be32(packet[wordAt : wordAt+srtcpIndexSize])
	encrypted := word&eFlag != 0
	index := word &^ eFlag

	c.mu.Lock()
	defer c.mu.Unlock()

	st := c.rxRTCP[ssrc]
	if st == nil && len(c.rxRTCP) >= maxStreams {
		return nil, errTooManyStreams
	}
	if st != nil && !st.window.fresh(uint64(index)) {
		return nil, errReplay
	}

	out := grow(dst, bodyEnd)
	switch c.profile {
	case ProfileAEADAES128GCM:
		nonce := gcmNonce(c.rtcpSalt, ssrc, index>>16, uint16(index))
		if encrypted {
			var aad [rtcpHeaderMin + srtcpIndexSize]byte
			copy(aad[:], packet[:rtcpHeaderMin])
			put32(aad[rtcpHeaderMin:], word)
			copy(out[:rtcpHeaderMin], packet[:rtcpHeaderMin])
			if _, err := c.rtcpGCM.Open(out[rtcpHeaderMin:rtcpHeaderMin], nonce[:], packet[rtcpHeaderMin:wordAt], aad[:]); err != nil {
				return nil, errAuth
			}
		} else {
			aad := make([]byte, bodyEnd+srtcpIndexSize)
			copy(aad, packet[:bodyEnd])
			put32(aad[bodyEnd:], word)
			if _, err := c.rtcpGCM.Open(nil, nonce[:], packet[bodyEnd:wordAt], aad); err != nil {
				return nil, errAuth
			}
			copy(out, packet[:bodyEnd])
		}
	default:
		tag := c.rtcpAuthTag(c.macBuf[:], packet[:wordAt+srtcpIndexSize])
		if subtle.ConstantTimeCompare(tag, packet[wordAt+srtcpIndexSize:]) != 1 {
			return nil, errAuth
		}
		copy(out[:rtcpHeaderMin], packet[:rtcpHeaderMin])
		if encrypted {
			cmXOR(c.rtcpBlk, c.rtcpSalt, ssrc, uint64(index), out[rtcpHeaderMin:bodyEnd], packet[rtcpHeaderMin:bodyEnd])
		} else {
			copy(out[rtcpHeaderMin:bodyEnd], packet[rtcpHeaderMin:bodyEnd])
		}
	}

	if st == nil {
		st = &rxState{}
		c.rxRTCP[ssrc] = st
	}
	st.window.mark(uint64(index))
	return out, nil
}
