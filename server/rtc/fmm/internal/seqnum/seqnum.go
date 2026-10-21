package seqnum

type Unwrapper16 struct {
	last int64
	ok   bool
}

func (u *Unwrapper16) Unwrap(v uint16) int64 {
	if !u.ok {
		u.ok = true
		u.last = int64(v)
		return u.last
	}
	c := Near16(v, u.last)
	if c > u.last {
		u.last = c
	}
	return c
}

func (u *Unwrapper16) Last() (int64, bool) { return u.last, u.ok }

func Near16(v uint16, ref int64) int64 {
	return ref + int64(int16(v-uint16(ref)))
}

type Unwrapper32 struct {
	last int64
	ok   bool
}

func (u *Unwrapper32) Unwrap(v uint32) int64 {
	if !u.ok {
		u.ok = true
		u.last = int64(v)
		return u.last
	}
	c := Near32(v, u.last)
	if c > u.last {
		u.last = c
	}
	return c
}

func Near32(v uint32, ref int64) int64 {
	return ref + int64(int32(v-uint32(ref)))
}
