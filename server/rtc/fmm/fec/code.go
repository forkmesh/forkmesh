package fec

import (
	"errors"
	"sync"
)

var (
	expTable [512]byte
	logTable [256]byte
	mulTable [256][256]byte
)

func init() {
	x := 1
	for i := range 255 {
		expTable[i] = byte(x)
		logTable[x] = byte(i)
		x <<= 1
		if x&0x100 != 0 {
			x ^= 0x11D
		}
	}
	for i := 255; i < 512; i++ {
		expTable[i] = expTable[i-255]
	}
	for a := 1; a < 256; a++ {
		for b := 1; b < 256; b++ {
			mulTable[a][b] = expTable[int(logTable[a])+int(logTable[b])]
		}
	}
}

func gfMul(a, b byte) byte { return mulTable[a][b] }

func gfInv(a byte) byte { return expTable[255-int(logTable[a])] }

func mulAdd(dst, src []byte, c byte) {
	switch c {
	case 0:
		return
	case 1:
		for i, v := range src {
			dst[i] ^= v
		}
	default:
		t := &mulTable[c]
		for i, v := range src {
			dst[i] ^= t[v]
		}
	}
}

var (
	ErrTooFew   = errors.New("fec: not enough symbols to recover")
	ErrGeometry = errors.New("fec: bad code geometry")
)

type matrix [][]byte

var (
	codeMu    sync.Mutex
	codeCache = map[int]matrix{}
)

func coefficients(k, m int) matrix {
	key := k<<8 | m
	codeMu.Lock()
	defer codeMu.Unlock()
	if c, ok := codeCache[key]; ok {
		return c
	}
	c := make(matrix, m)
	for j := range m {
		c[j] = make([]byte, k)
		for i := range k {
			c[j][i] = gfInv(byte(j) ^ byte(m+i))
		}
	}
	for i := range k {
		s := gfInv(c[0][i])
		for j := range m {
			c[j][i] = gfMul(c[j][i], s)
		}
	}
	codeCache[key] = c
	return c
}

func Encode(blocks [][]byte, m int) [][]byte {
	k := len(blocks)
	size := len(blocks[0])
	c := coefficients(k, m)
	out := make([][]byte, m)
	for j := range m {
		out[j] = make([]byte, size)
		for i, b := range blocks {
			mulAdd(out[j], b, c[j][i])
		}
	}
	return out
}

func Reconstruct(data, repair [][]byte) error {
	k, m := len(data), len(repair)
	if k == 0 || m == 0 || k+m > 256 {
		return ErrGeometry
	}
	var missing, rows []int
	size := -1
	for i, d := range data {
		if d == nil {
			missing = append(missing, i)
		} else if size < 0 {
			size = len(d)
		}
	}
	if len(missing) == 0 {
		return nil
	}
	for j, r := range repair {
		if r != nil && len(rows) < len(missing) {
			rows = append(rows, j)
			if size < 0 {
				size = len(r)
			}
		}
	}
	if len(rows) < len(missing) {
		return ErrTooFew
	}
	for _, d := range data {
		if d != nil && len(d) != size {
			return ErrGeometry
		}
	}
	for _, j := range rows {
		if len(repair[j]) != size {
			return ErrGeometry
		}
	}
	c := coefficients(k, m)
	e := len(missing)
	syn := make([][]byte, e)
	for q, j := range rows {
		syn[q] = append([]byte(nil), repair[j]...)
		for i, d := range data {
			if d != nil {
				mulAdd(syn[q], d, c[j][i])
			}
		}
	}
	a := make(matrix, e)
	for q, j := range rows {
		a[q] = make([]byte, e)
		for p, i := range missing {
			a[q][p] = c[j][i]
		}
	}
	inv, ok := invert(a)
	if !ok {
		return ErrGeometry
	}
	for p, i := range missing {
		d := make([]byte, size)
		for q := range e {
			mulAdd(d, syn[q], inv[p][q])
		}
		data[i] = d
	}
	return nil
}

func invert(a matrix) (matrix, bool) {
	n := len(a)
	w := make(matrix, n)
	for i := range n {
		w[i] = make([]byte, 2*n)
		copy(w[i], a[i])
		w[i][n+i] = 1
	}
	for col := range n {
		piv := -1
		for r := col; r < n; r++ {
			if w[r][col] != 0 {
				piv = r
				break
			}
		}
		if piv < 0 {
			return nil, false
		}
		w[col], w[piv] = w[piv], w[col]
		s := gfInv(w[col][col])
		for x := range w[col] {
			w[col][x] = gfMul(w[col][x], s)
		}
		for r := range n {
			if r != col && w[r][col] != 0 {
				mulAdd(w[r], w[col], w[r][col])
			}
		}
	}
	out := make(matrix, n)
	for i := range n {
		out[i] = w[i][n:]
	}
	return out, true
}
