package quic

type span struct {
	start, end uint64
}

type rangeSet []span

func (s rangeSet) contains(v uint64) bool {
	lo, hi := 0, len(s)
	for lo < hi {
		m := (lo + hi) / 2
		switch {
		case v < s[m].start:
			hi = m
		case v >= s[m].end:
			lo = m + 1
		default:
			return true
		}
	}
	return false
}

func (s *rangeSet) add(start, end uint64) {
	if start >= end {
		return
	}
	r := *s
	i := 0
	for i < len(r) && r[i].end < start {
		i++
	}
	j := i
	for j < len(r) && r[j].start <= end {
		if r[j].start < start {
			start = r[j].start
		}
		if r[j].end > end {
			end = r[j].end
		}
		j++
	}
	if i == j {
		r = append(r, span{})
		copy(r[i+1:], r[i:])
		r[i] = span{start, end}
		*s = r
		return
	}
	r[i] = span{start, end}
	r = append(r[:i+1], r[j:]...)
	*s = r
}

func (s *rangeSet) remove(start, end uint64) {
	if start >= end {
		return
	}
	r := *s
	out := r[:0:0]
	for _, x := range r {
		if x.end <= start || x.start >= end {
			out = append(out, x)
			continue
		}
		if x.start < start {
			out = append(out, span{x.start, start})
		}
		if x.end > end {
			out = append(out, span{end, x.end})
		}
	}
	*s = out
}

func (s *rangeSet) removeBelow(v uint64) {
	r := *s
	i := 0
	for i < len(r) && r[i].end <= v {
		i++
	}
	r = r[i:]
	if len(r) > 0 && r[0].start < v {
		r[0].start = v
	}
	*s = r
}

func (s rangeSet) empty() bool { return len(s) == 0 }

func (s rangeSet) min() uint64 { return s[0].start }

func (s rangeSet) max() uint64 { return s[len(s)-1].end }

func (s rangeSet) prefixEnd(from uint64) uint64 {
	if len(s) > 0 && s[0].start <= from && s[0].end > from {
		return s[0].end
	}
	return from
}
