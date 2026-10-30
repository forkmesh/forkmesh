package quic

const MaxVarint = 1<<62 - 1

func VarintLen(v uint64) int {
	switch {
	case v < 1<<6:
		return 1
	case v < 1<<14:
		return 2
	case v < 1<<30:
		return 4
	default:
		return 8
	}
}

func AppendVarint(b []byte, v uint64) []byte {
	switch {
	case v < 1<<6:
		return append(b, byte(v))
	case v < 1<<14:
		return append(b, 0x40|byte(v>>8), byte(v))
	case v < 1<<30:
		return append(b, 0x80|byte(v>>24), byte(v>>16), byte(v>>8), byte(v))
	default:
		v &= MaxVarint
		return append(b, 0xc0|byte(v>>56), byte(v>>48), byte(v>>40), byte(v>>32), byte(v>>24), byte(v>>16), byte(v>>8), byte(v))
	}
}

func appendVarint2(b []byte, v uint64) []byte {
	return append(b, 0x40|byte(v>>8), byte(v))
}

func putVarint2(b []byte, v uint64) {
	b[0] = 0x40 | byte(v>>8)
	b[1] = byte(v)
}

func ReadVarint(b []byte) (uint64, int) {
	if len(b) == 0 {
		return 0, 0
	}
	n := 1 << (b[0] >> 6)
	if len(b) < n {
		return 0, 0
	}
	v := uint64(b[0] & 0x3f)
	for i := 1; i < n; i++ {
		v = v<<8 | uint64(b[i])
	}
	return v, n
}

func readMinimalVarint(b []byte) (uint64, int) {
	v, n := ReadVarint(b)
	if n > 1 && VarintLen(v) != n {
		return 0, -1
	}
	return v, n
}
