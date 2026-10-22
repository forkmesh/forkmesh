package srtp

const replayWindowSize = 128

type replayWindow struct {
	started bool
	top     uint64
	lo      uint64
	hi      uint64
}

func (w *replayWindow) fresh(index uint64) bool {
	if !w.started || index > w.top {
		return true
	}
	d := w.top - index
	if d >= replayWindowSize {
		return false
	}
	if d < 64 {
		return w.lo&(1<<d) == 0
	}
	return w.hi&(1<<(d-64)) == 0
}

func (w *replayWindow) mark(index uint64) {
	if !w.started {
		w.started = true
		w.top = index
		w.lo = 1
		w.hi = 0
		return
	}
	if index > w.top {
		s := index - w.top
		switch {
		case s >= replayWindowSize:
			w.lo, w.hi = 0, 0
		case s >= 64:
			w.hi = w.lo << (s - 64)
			w.lo = 0
		default:
			w.hi = w.hi<<s | w.lo>>(64-s)
			w.lo <<= s
		}
		w.top = index
		w.lo |= 1
		return
	}
	d := w.top - index
	if d < 64 {
		w.lo |= 1 << d
	} else if d < replayWindowSize {
		w.hi |= 1 << (d - 64)
	}
}

func guessIndex(started bool, highest uint64, seq uint16) (uint64, error) {
	if !started {
		return uint64(seq), nil
	}
	roc := int64(highest >> 16)
	sl := int64(uint16(highest))
	s := int64(seq)
	v := roc
	if sl < 32768 {
		if s-sl > 32768 {
			v = roc - 1
		}
	} else if sl-32768 > s {
		v = roc + 1
	}
	if v < 0 {
		return 0, errReplay
	}
	if v > maxROC {
		return 0, errExhausted
	}
	return uint64(v)<<16 | uint64(seq), nil
}
