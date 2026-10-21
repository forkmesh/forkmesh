package facestream

const (
	probBits  = 12
	probOne   = 1 << probBits
	probHalf  = probOne / 2
	moveBits  = 4
	topValue  = 1 << 24
	maxIntLen = 16
	maxInt    = 1<<maxIntLen - 1
)

type prob uint16

type rangeEncoder struct {
	low       uint64
	rng       uint32
	cache     byte
	cacheSize int
	out       []byte
	started   bool
}

func newRangeEncoder(out []byte) *rangeEncoder {
	return &rangeEncoder{rng: 0xFFFFFFFF, cacheSize: 1, out: out}
}

func (e *rangeEncoder) emit(b byte) {
	if !e.started {
		e.started = true
		return
	}
	e.out = append(e.out, b)
}

func (e *rangeEncoder) shiftLow() {
	if uint32(e.low) < 0xFF000000 || e.low>>32 != 0 {
		carry := byte(e.low >> 32)
		temp := e.cache
		for {
			e.emit(temp + carry)
			temp = 0xFF
			e.cacheSize--
			if e.cacheSize == 0 {
				break
			}
		}
		e.cache = byte(uint32(e.low) >> 24)
	}
	e.cacheSize++
	e.low = uint64(uint32(e.low) << 8)
}

func (e *rangeEncoder) normalize() {
	for e.rng < topValue {
		e.rng <<= 8
		e.shiftLow()
	}
}

func (e *rangeEncoder) bit(p *prob, bit int) {
	bound := (e.rng >> probBits) * uint32(*p)
	if bit == 0 {
		e.rng = bound
		*p += (probOne - *p) >> moveBits
	} else {
		e.low += uint64(bound)
		e.rng -= bound
		*p -= *p >> moveBits
	}
	e.normalize()
}

func (e *rangeEncoder) direct(value uint32, count int) {
	for i := count - 1; i >= 0; i-- {
		e.rng >>= 1
		if (value>>uint(i))&1 == 1 {
			e.low += uint64(e.rng)
		}
		e.normalize()
	}
}

func (e *rangeEncoder) finish() []byte {
	for _, k := range []uint{32, 24} {
		mask := uint64(1)<<k - 1
		v := (e.low + mask) &^ mask
		if v < e.low+uint64(e.rng) {
			e.low = v
			break
		}
	}
	for i := 0; i < 5; i++ {
		e.shiftLow()
	}
	end := len(e.out)
	for end > 0 && e.out[end-1] == 0 {
		end--
	}
	return e.out[:end]
}

type rangeDecoder struct {
	code uint32
	rng  uint32
	data []byte
	pos  int
}

func newRangeDecoder(data []byte) *rangeDecoder {
	d := &rangeDecoder{rng: 0xFFFFFFFF, data: data}
	for i := 0; i < 4; i++ {
		d.code = d.code<<8 | uint32(d.next())
	}
	return d
}

func (d *rangeDecoder) next() byte {
	var b byte
	if d.pos < len(d.data) {
		b = d.data[d.pos]
	}
	d.pos++
	return b
}

func (d *rangeDecoder) normalize() {
	for d.rng < topValue {
		d.rng <<= 8
		d.code = d.code<<8 | uint32(d.next())
	}
}

func (d *rangeDecoder) bit(p *prob) int {
	bound := (d.rng >> probBits) * uint32(*p)
	var bit int
	if d.code < bound {
		d.rng = bound
		*p += (probOne - *p) >> moveBits
	} else {
		d.code -= bound
		d.rng -= bound
		*p -= *p >> moveBits
		bit = 1
	}
	d.normalize()
	return bit
}

func (d *rangeDecoder) direct(count int) uint32 {
	var v uint32
	for i := 0; i < count; i++ {
		d.rng >>= 1
		b := uint32(0)
		if d.code >= d.rng {
			d.code -= d.rng
			b = 1
		}
		v = v<<1 | b
		d.normalize()
	}
	return v
}

func (d *rangeDecoder) overrun() bool {
	return d.pos > len(d.data)+8
}

type intModel struct {
	zero   [2]prob
	sign   prob
	length [maxIntLen]prob
	mant   [maxIntLen]prob
}

func newIntModel(zeroPrior prob) intModel {
	m := intModel{zero: [2]prob{zeroPrior, zeroPrior}, sign: probHalf}
	for i := range m.length {
		m.length[i] = probHalf
		m.mant[i] = probHalf
	}
	return m
}

func bitLen(v uint32) int {
	n := 0
	for v != 0 {
		n++
		v >>= 1
	}
	return n
}

func clampInt(v, low, high int) int {
	if v < low {
		return low
	}
	if v > high {
		return high
	}
	return v
}

func (e *rangeEncoder) uint(m *intModel, ctx int, v int) {
	v = clampInt(v, 0, maxInt)
	if v == 0 {
		e.bit(&m.zero[ctx], 0)
		return
	}
	e.bit(&m.zero[ctx], 1)
	e.magnitude(m, uint32(v))
}

func (e *rangeEncoder) int(m *intModel, ctx int, v int) {
	v = clampInt(v, -maxInt, maxInt)
	if v == 0 {
		e.bit(&m.zero[ctx], 0)
		return
	}
	e.bit(&m.zero[ctx], 1)
	if v < 0 {
		e.bit(&m.sign, 1)
		v = -v
	} else {
		e.bit(&m.sign, 0)
	}
	e.magnitude(m, uint32(v))
}

func (e *rangeEncoder) magnitude(m *intModel, a uint32) {
	n := bitLen(a)
	for i := 1; i < n; i++ {
		e.bit(&m.length[i-1], 1)
	}
	if n < maxIntLen {
		e.bit(&m.length[n-1], 0)
	}
	if n >= 2 {
		e.bit(&m.mant[n-2], int((a>>uint(n-2))&1))
		if n >= 3 {
			e.direct(a&(1<<uint(n-2)-1), n-2)
		}
	}
}

func (d *rangeDecoder) uint(m *intModel, ctx int) int {
	if d.bit(&m.zero[ctx]) == 0 {
		return 0
	}
	return int(d.magnitude(m))
}

func (d *rangeDecoder) int(m *intModel, ctx int) int {
	if d.bit(&m.zero[ctx]) == 0 {
		return 0
	}
	negative := d.bit(&m.sign) == 1
	a := int(d.magnitude(m))
	if negative {
		return -a
	}
	return a
}

func (d *rangeDecoder) magnitude(m *intModel) uint32 {
	n := 1
	for n < maxIntLen && d.bit(&m.length[n-1]) == 1 {
		n++
	}
	a := uint32(1)
	if n >= 2 {
		a = a<<1 | uint32(d.bit(&m.mant[n-2]))
		if n >= 3 {
			a = a<<uint(n-2) | d.direct(n-2)
		}
	}
	return a
}
