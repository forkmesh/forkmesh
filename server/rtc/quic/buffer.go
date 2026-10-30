package quic

type sendBuf struct {
	data     []byte
	base     uint64
	sent     uint64
	lost     rangeSet
	acked    rangeSet
	fin      bool
	finSent  bool
	finAcked bool
}

func (s *sendBuf) end() uint64 { return s.base + uint64(len(s.data)) }

func (s *sendBuf) buffered() int { return len(s.data) }

func (s *sendBuf) write(p []byte) {
	s.data = append(s.data, p...)
}

func (s *sendBuf) slice(start, end uint64) []byte {
	return s.data[start-s.base : end-s.base]
}

func (s *sendBuf) hasLost() bool { return !s.lost.empty() }

func (s *sendBuf) nextLost(max int) (off uint64, data []byte, fin bool, ok bool) {
	if s.lost.empty() {
		return 0, nil, false, false
	}
	r := s.lost[0]
	end := r.end
	if end-r.start > uint64(max) {
		end = r.start + uint64(max)
	}
	return r.start, s.slice(r.start, end), false, true
}

func (s *sendBuf) markLostSent(start, end uint64) {
	s.lost.remove(start, end)
}

func (s *sendBuf) pendingNew(limit uint64) uint64 {
	e := s.end()
	if limit < e {
		e = limit
	}
	if e <= s.sent {
		return 0
	}
	return e - s.sent
}

func (s *sendBuf) needFin() bool {
	return s.fin && !s.finSent && s.sent == s.end()
}

func (s *sendBuf) onAck(start, end uint64, fin bool) {
	if fin {
		s.finAcked = true
		s.finSent = true
	}
	if end <= s.base {
		return
	}
	if start < s.base {
		start = s.base
	}
	s.acked.add(start, end)
	s.lost.remove(start, end)
	if len(s.acked) > 0 && s.acked[0].start <= s.base {
		newBase := s.acked[0].end
		drop := newBase - s.base
		if drop > uint64(len(s.data)) {
			drop = uint64(len(s.data))
			newBase = s.base + drop
		}
		s.data = s.data[drop:]
		s.base = newBase
		s.acked.removeBelow(newBase)
		s.lost.removeBelow(newBase)
		if cap(s.data) > 4096 && len(s.data) < cap(s.data)/4 {
			s.data = append([]byte(nil), s.data...)
		}
	}
}

func (s *sendBuf) onLost(start, end uint64, fin bool) {
	if fin && !s.finAcked {
		s.finSent = false
	}
	if end <= s.base {
		return
	}
	if start < s.base {
		start = s.base
	}
	cur := start
	for _, a := range s.acked {
		if a.end <= cur {
			continue
		}
		if a.start >= end {
			break
		}
		if a.start > cur {
			s.lost.add(cur, a.start)
		}
		cur = a.end
	}
	if cur < end {
		s.lost.add(cur, end)
	}
}

func (s *sendBuf) allAcked() bool {
	return s.fin && s.finAcked && len(s.data) == 0
}

type recvBuf struct {
	base    uint64
	data    []byte
	filled  rangeSet
	highest uint64
}

const maxRecvGaps = 1024

func (r *recvBuf) write(off uint64, p []byte) bool {
	end := off + uint64(len(p))
	if end > r.highest {
		r.highest = end
	}
	if end <= r.base || len(p) == 0 {
		return true
	}
	if off < r.base {
		p = p[r.base-off:]
		off = r.base
	}
	need := int(end - r.base)
	if need > len(r.data) {
		if need > cap(r.data) {
			c := 2 * cap(r.data)
			if c < need {
				c = need
			}
			if c < 1024 {
				c = 1024
			}
			nd := make([]byte, need, c)
			copy(nd, r.data)
			r.data = nd
		} else {
			r.data = r.data[:need]
		}
	}
	copy(r.data[off-r.base:], p)
	r.filled.add(off, end)
	return len(r.filled) <= maxRecvGaps
}

func (r *recvBuf) readable() int {
	if len(r.filled) == 0 || r.filled[0].start != r.base {
		return 0
	}
	return int(r.filled[0].end - r.base)
}

func (r *recvBuf) read(p []byte) int {
	n := r.readable()
	if n > len(p) {
		n = len(p)
	}
	copy(p, r.data[:n])
	r.consume(n)
	return n
}

func (r *recvBuf) peek() []byte {
	return r.data[:r.readable()]
}

func (r *recvBuf) consume(n int) {
	if n == 0 {
		return
	}
	r.data = r.data[n:]
	r.base += uint64(n)
	r.filled.removeBelow(r.base)
	if len(r.data) == 0 {
		r.data = nil
	} else if cap(r.data) > 4096 && len(r.data) < cap(r.data)/4 {
		r.data = append([]byte(nil), r.data...)
	}
}
