package quic

const (
	framePadding            = 0x00
	framePing               = 0x01
	frameAck                = 0x02
	frameAckECN             = 0x03
	frameResetStream        = 0x04
	frameStopSending        = 0x05
	frameCrypto             = 0x06
	frameNewToken           = 0x07
	frameStream             = 0x08
	frameStreamMax          = 0x0f
	frameMaxData            = 0x10
	frameMaxStreamData      = 0x11
	frameMaxStreamsBidi     = 0x12
	frameMaxStreamsUni      = 0x13
	frameDataBlocked        = 0x14
	frameStreamDataBlocked  = 0x15
	frameStreamsBlockedBidi = 0x16
	frameStreamsBlockedUni  = 0x17
	frameNewConnectionID    = 0x18
	frameRetireConnectionID = 0x19
	framePathChallenge      = 0x1a
	framePathResponse       = 0x1b
	frameConnectionClose    = 0x1c
	frameApplicationClose   = 0x1d
	frameHandshakeDone      = 0x1e
	frameDatagram           = 0x30
	frameDatagramLen        = 0x31
)

const (
	maxAckRangesParsed = 256
	maxStreamsValue    = 1 << 60
	maxReasonLen       = 1024
)

type ackRange struct {
	smallest, largest uint64
}

type frame struct {
	typ           uint64
	streamID      uint64
	offset        uint64
	code          uint64
	finalSize     uint64
	max           uint64
	seq           uint64
	retirePrior   uint64
	closeFrame    uint64
	ackDelay      uint64
	data          []byte
	fin           bool
	ranges        []ackRange
	resetToken    [16]byte
	pathData      [8]byte
	ecn           [3]uint64
	rangesDropped bool
}

func (f *frame) isStream() bool { return f.typ >= frameStream && f.typ <= frameStreamMax }

func (f *frame) ackEliciting() bool {
	switch f.typ {
	case framePadding, frameAck, frameAckECN, frameConnectionClose, frameApplicationClose:
		return false
	}
	return true
}

func (f *frame) probing() bool {
	switch f.typ {
	case framePadding, framePathChallenge, framePathResponse, frameNewConnectionID:
		return true
	}
	return false
}

func parseFrame(b []byte, f *frame) (int, *TransportError) {
	ranges := f.ranges[:0]
	*f = frame{ranges: ranges}
	typ, n := readMinimalVarint(b)
	if n <= 0 {
		return 0, frameErr(FrameEncodingError, 0, "bad frame type")
	}
	f.typ = typ
	p := n
	fail := func(reason string) (int, *TransportError) {
		return 0, frameErr(FrameEncodingError, typ, reason)
	}
	readV := func() (uint64, bool) {
		v, m := ReadVarint(b[p:])
		if m == 0 {
			return 0, false
		}
		p += m
		return v, true
	}
	readBytes := func(l uint64) ([]byte, bool) {
		if l > uint64(len(b)-p) {
			return nil, false
		}
		out := b[p : p+int(l)]
		p += int(l)
		return out, true
	}
	switch {
	case typ == framePadding:
		for p < len(b) && b[p] == 0 {
			p++
		}
		return p, nil
	case typ == framePing, typ == frameHandshakeDone:
		return p, nil
	case typ == frameAck || typ == frameAckECN:
		largest, ok := readV()
		if !ok {
			return fail("short ack")
		}
		delay, ok := readV()
		if !ok {
			return fail("short ack")
		}
		count, ok := readV()
		if !ok {
			return fail("short ack")
		}
		first, ok := readV()
		if !ok || first > largest {
			return fail("bad first ack range")
		}
		f.ackDelay = delay
		smallest := largest - first
		f.ranges = append(f.ranges, ackRange{smallest: smallest, largest: largest})
		if count > uint64(len(b)) {
			return fail("too many ack ranges")
		}
		for i := uint64(0); i < count; i++ {
			gap, ok := readV()
			if !ok {
				return fail("short ack range")
			}
			length, ok := readV()
			if !ok {
				return fail("short ack range")
			}
			if smallest < gap+2 {
				return fail("ack range underflow")
			}
			hi := smallest - gap - 2
			if length > hi {
				return fail("ack range underflow")
			}
			smallest = hi - length
			if len(f.ranges) < maxAckRangesParsed {
				f.ranges = append(f.ranges, ackRange{smallest: smallest, largest: hi})
			} else {
				f.rangesDropped = true
			}
		}
		if typ == frameAckECN {
			for i := 0; i < 3; i++ {
				v, ok := readV()
				if !ok {
					return fail("short ecn counts")
				}
				f.ecn[i] = v
			}
		}
		return p, nil
	case typ == frameResetStream:
		var ok1, ok2, ok3 bool
		f.streamID, ok1 = readV()
		f.code, ok2 = readV()
		f.finalSize, ok3 = readV()
		if !ok1 || !ok2 || !ok3 {
			return fail("short reset_stream")
		}
		return p, nil
	case typ == frameStopSending:
		var ok1, ok2 bool
		f.streamID, ok1 = readV()
		f.code, ok2 = readV()
		if !ok1 || !ok2 {
			return fail("short stop_sending")
		}
		return p, nil
	case typ == frameCrypto:
		off, ok := readV()
		if !ok {
			return fail("short crypto")
		}
		l, ok := readV()
		if !ok {
			return fail("short crypto")
		}
		data, ok := readBytes(l)
		if !ok {
			return fail("short crypto data")
		}
		if off+l > MaxVarint {
			return 0, frameErr(CryptoBufferExceeded, typ, "crypto offset too large")
		}
		f.offset, f.data = off, data
		return p, nil
	case typ == frameNewToken:
		l, ok := readV()
		if !ok || l == 0 {
			return fail("bad new_token")
		}
		data, ok := readBytes(l)
		if !ok {
			return fail("short new_token")
		}
		f.data = data
		return p, nil
	case typ >= frameStream && typ <= frameStreamMax:
		id, ok := readV()
		if !ok {
			return fail("short stream")
		}
		f.streamID = id
		if typ&0x04 != 0 {
			f.offset, ok = readV()
			if !ok {
				return fail("short stream offset")
			}
		}
		var l uint64
		if typ&0x02 != 0 {
			l, ok = readV()
			if !ok {
				return fail("short stream length")
			}
		} else {
			l = uint64(len(b) - p)
		}
		data, ok := readBytes(l)
		if !ok {
			return fail("short stream data")
		}
		if f.offset+l > MaxVarint {
			return 0, frameErr(FlowControlError, typ, "stream offset too large")
		}
		f.data = data
		f.fin = typ&0x01 != 0
		return p, nil
	case typ == frameMaxData, typ == frameDataBlocked:
		v, ok := readV()
		if !ok {
			return fail("short frame")
		}
		f.max = v
		return p, nil
	case typ == frameMaxStreamData, typ == frameStreamDataBlocked:
		var ok1, ok2 bool
		f.streamID, ok1 = readV()
		f.max, ok2 = readV()
		if !ok1 || !ok2 {
			return fail("short frame")
		}
		return p, nil
	case typ >= frameMaxStreamsBidi && typ <= frameMaxStreamsUni, typ >= frameStreamsBlockedBidi && typ <= frameStreamsBlockedUni:
		v, ok := readV()
		if !ok {
			return fail("short frame")
		}
		if v > maxStreamsValue {
			return fail("stream count too large")
		}
		f.max = v
		return p, nil
	case typ == frameNewConnectionID:
		var ok1, ok2 bool
		f.seq, ok1 = readV()
		f.retirePrior, ok2 = readV()
		if !ok1 || !ok2 || p >= len(b) {
			return fail("short new_connection_id")
		}
		if f.retirePrior > f.seq {
			return fail("retire_prior_to above sequence")
		}
		l := int(b[p])
		p++
		if l < 1 || l > maxCIDLen {
			return fail("bad connection id length")
		}
		cid, ok := readBytes(uint64(l))
		if !ok {
			return fail("short connection id")
		}
		tok, ok := readBytes(16)
		if !ok {
			return fail("short reset token")
		}
		f.data = cid
		copy(f.resetToken[:], tok)
		return p, nil
	case typ == frameRetireConnectionID:
		v, ok := readV()
		if !ok {
			return fail("short retire_connection_id")
		}
		f.seq = v
		return p, nil
	case typ == framePathChallenge, typ == framePathResponse:
		d, ok := readBytes(8)
		if !ok {
			return fail("short path frame")
		}
		copy(f.pathData[:], d)
		return p, nil
	case typ == frameConnectionClose, typ == frameApplicationClose:
		code, ok := readV()
		if !ok {
			return fail("short connection_close")
		}
		f.code = code
		if typ == frameConnectionClose {
			f.closeFrame, ok = readV()
			if !ok {
				return fail("short connection_close")
			}
		}
		l, ok := readV()
		if !ok {
			return fail("short connection_close")
		}
		reason, ok := readBytes(l)
		if !ok {
			return fail("short close reason")
		}
		f.data = reason
		return p, nil
	case typ == frameDatagram:
		f.data = b[p:]
		return len(b), nil
	case typ == frameDatagramLen:
		l, ok := readV()
		if !ok {
			return fail("short datagram")
		}
		d, ok := readBytes(l)
		if !ok {
			return fail("short datagram data")
		}
		f.data = d
		return p, nil
	}
	return 0, frameErr(FrameEncodingError, typ, "unknown frame type")
}

func appendAckFrame(b []byte, ranges []ackRange, delay uint64) []byte {
	b = append(b, frameAck)
	b = AppendVarint(b, ranges[0].largest)
	b = AppendVarint(b, delay)
	b = AppendVarint(b, uint64(len(ranges)-1))
	b = AppendVarint(b, ranges[0].largest-ranges[0].smallest)
	prev := ranges[0].smallest
	for _, r := range ranges[1:] {
		b = AppendVarint(b, prev-r.largest-2)
		b = AppendVarint(b, r.largest-r.smallest)
		prev = r.smallest
	}
	return b
}

func ackFrameLen(ranges []ackRange, delay uint64) int {
	n := 1 + VarintLen(ranges[0].largest) + VarintLen(delay) + VarintLen(uint64(len(ranges)-1)) + VarintLen(ranges[0].largest-ranges[0].smallest)
	prev := ranges[0].smallest
	for _, r := range ranges[1:] {
		n += VarintLen(prev-r.largest-2) + VarintLen(r.largest-r.smallest)
		prev = r.smallest
	}
	return n
}

func appendCryptoHeader(b []byte, offset uint64, length int) []byte {
	b = append(b, frameCrypto)
	b = AppendVarint(b, offset)
	return AppendVarint(b, uint64(length))
}

func cryptoHeaderLen(offset uint64, length int) int {
	return 1 + VarintLen(offset) + VarintLen(uint64(length))
}

func appendStreamHeader(b []byte, id, offset uint64, length int, fin, withLen bool) []byte {
	t := byte(frameStream)
	if offset > 0 {
		t |= 0x04
	}
	if withLen {
		t |= 0x02
	}
	if fin {
		t |= 0x01
	}
	b = append(b, t)
	b = AppendVarint(b, id)
	if offset > 0 {
		b = AppendVarint(b, offset)
	}
	if withLen {
		b = AppendVarint(b, uint64(length))
	}
	return b
}

func streamHeaderLen(id, offset uint64, length int, withLen bool) int {
	n := 1 + VarintLen(id)
	if offset > 0 {
		n += VarintLen(offset)
	}
	if withLen {
		n += VarintLen(uint64(length))
	}
	return n
}

func appendTwoVarintFrame(b []byte, typ, a, c uint64) []byte {
	b = AppendVarint(b, typ)
	b = AppendVarint(b, a)
	return AppendVarint(b, c)
}

func appendOneVarintFrame(b []byte, typ, a uint64) []byte {
	b = AppendVarint(b, typ)
	return AppendVarint(b, a)
}

func appendResetStream(b []byte, id, code, finalSize uint64) []byte {
	b = append(b, frameResetStream)
	b = AppendVarint(b, id)
	b = AppendVarint(b, code)
	return AppendVarint(b, finalSize)
}

func appendNewConnectionID(b []byte, seq, retirePrior uint64, cid []byte, token [16]byte) []byte {
	b = append(b, frameNewConnectionID)
	b = AppendVarint(b, seq)
	b = AppendVarint(b, retirePrior)
	b = append(b, byte(len(cid)))
	b = append(b, cid...)
	return append(b, token[:]...)
}

func appendConnectionClose(b []byte, app bool, code, frameType uint64, reason string) []byte {
	if len(reason) > maxReasonLen {
		reason = reason[:maxReasonLen]
	}
	if app {
		b = append(b, frameApplicationClose)
		b = AppendVarint(b, code)
	} else {
		b = append(b, frameConnectionClose)
		b = AppendVarint(b, code)
		b = AppendVarint(b, frameType)
	}
	b = AppendVarint(b, uint64(len(reason)))
	return append(b, reason...)
}

func appendDatagramFrame(b []byte, data []byte) []byte {
	b = append(b, frameDatagramLen)
	b = AppendVarint(b, uint64(len(data)))
	return append(b, data...)
}

func datagramFrameLen(n int) int {
	return 1 + VarintLen(uint64(n)) + n
}
