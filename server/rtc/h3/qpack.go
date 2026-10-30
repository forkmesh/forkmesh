package h3

import (
	"errors"
	"strings"
)

type Field struct {
	Name  string
	Value string
}

var errQPACK = errors.New("h3: qpack decompression failed")

type huffNode struct {
	next [2]int16
	sym  int16
}

var huffTree = buildHuffTree()

func buildHuffTree() []huffNode {
	nodes := []huffNode{{next: [2]int16{-1, -1}, sym: -1}}
	add := func(code uint32, length uint8, sym int16) {
		n := 0
		for i := int(length) - 1; i >= 0; i-- {
			bit := (code >> uint(i)) & 1
			if nodes[n].next[bit] < 0 {
				nodes = append(nodes, huffNode{next: [2]int16{-1, -1}, sym: -1})
				nodes[n].next[bit] = int16(len(nodes) - 1)
			}
			n = int(nodes[n].next[bit])
		}
		nodes[n].sym = sym
	}
	for i := 0; i < 256; i++ {
		add(huffmanCodes[i], huffmanCodeLen[i], int16(i))
	}
	add(0x3fffffff, 30, 256)
	return nodes
}

func huffmanDecode(dst []byte, src []byte, max int) ([]byte, error) {
	n := 0
	depth := 0
	allOnes := true
	for _, c := range src {
		for i := 7; i >= 0; i-- {
			bit := (c >> uint(i)) & 1
			next := huffTree[n].next[bit]
			if next < 0 {
				return nil, errQPACK
			}
			n = int(next)
			depth++
			if bit == 0 {
				allOnes = false
			}
			if s := huffTree[n].sym; s >= 0 {
				if s == 256 {
					return nil, errQPACK
				}
				if len(dst) >= max {
					return nil, errQPACK
				}
				dst = append(dst, byte(s))
				n = 0
				depth = 0
				allOnes = true
			}
		}
	}
	if depth > 7 || !allOnes {
		return nil, errQPACK
	}
	return dst, nil
}

func readPrefixInt(b []byte, n uint) (uint64, []byte, bool) {
	if len(b) == 0 {
		return 0, nil, false
	}
	mask := byte(1<<n - 1)
	v := uint64(b[0] & mask)
	b = b[1:]
	if v < uint64(mask) {
		return v, b, true
	}
	var m uint
	for {
		if len(b) == 0 || m > 56 {
			return 0, nil, false
		}
		c := b[0]
		b = b[1:]
		v += uint64(c&0x7f) << m
		m += 7
		if c&0x80 == 0 {
			return v, b, true
		}
	}
}

func appendPrefixInt(b []byte, flags byte, n uint, v uint64) []byte {
	mask := uint64(1<<n - 1)
	if v < mask {
		return append(b, flags|byte(v))
	}
	b = append(b, flags|byte(mask))
	v -= mask
	for v >= 0x80 {
		b = append(b, byte(v)|0x80)
		v >>= 7
	}
	return append(b, byte(v))
}

func readString(b []byte, n uint, max int) (string, []byte, bool) {
	if len(b) == 0 {
		return "", nil, false
	}
	huff := b[0]&(1<<n) != 0
	l, rest, ok := readPrefixInt(b, n)
	if !ok || l > uint64(len(rest)) {
		return "", nil, false
	}
	raw := rest[:l]
	rest = rest[l:]
	if !huff {
		if len(raw) > max {
			return "", nil, false
		}
		return string(raw), rest, true
	}
	out, err := huffmanDecode(make([]byte, 0, len(raw)*8/5+1), raw, max)
	if err != nil {
		return "", nil, false
	}
	return string(out), rest, true
}

func DecodeFieldSection(b []byte, maxSize int) ([]Field, error) {
	ric, b, ok := readPrefixInt(b, 8)
	if !ok || ric != 0 {
		return nil, errQPACK
	}
	_, b, ok = readPrefixInt(b, 7)
	if !ok {
		return nil, errQPACK
	}
	var fields []Field
	size := 0
	for len(b) > 0 {
		var f Field
		c := b[0]
		switch {
		case c&0x80 != 0:
			if c&0x40 == 0 {
				return nil, errQPACK
			}
			var idx uint64
			idx, b, ok = readPrefixInt(b, 6)
			if !ok || idx >= uint64(len(staticTable)) {
				return nil, errQPACK
			}
			f = Field{staticTable[idx].name, staticTable[idx].value}
		case c&0xc0 == 0x40:
			if c&0x10 == 0 {
				return nil, errQPACK
			}
			var idx uint64
			idx, b, ok = readPrefixInt(b, 4)
			if !ok || idx >= uint64(len(staticTable)) {
				return nil, errQPACK
			}
			f.Name = staticTable[idx].name
			f.Value, b, ok = readString(b, 7, maxSize)
			if !ok {
				return nil, errQPACK
			}
		case c&0xe0 == 0x20:
			f.Name, b, ok = readString(b, 3, maxSize)
			if !ok {
				return nil, errQPACK
			}
			f.Value, b, ok = readString(b, 7, maxSize)
			if !ok {
				return nil, errQPACK
			}
		default:
			return nil, errQPACK
		}
		size += len(f.Name) + len(f.Value) + 32
		if size > maxSize {
			return nil, errHeaderTooLarge
		}
		fields = append(fields, f)
	}
	return fields, nil
}

var staticIndex = func() (map[staticEntry]int, map[string]int) {
	full := make(map[staticEntry]int)
	names := make(map[string]int)
	for i, e := range staticTable {
		if _, ok := full[e]; !ok {
			full[e] = i
		}
		if _, ok := names[e.name]; !ok {
			names[e.name] = i
		}
	}
	return full, names
}

var staticFull, staticNames = staticIndex()

func appendString(b []byte, flags byte, n uint, s string) []byte {
	b = appendPrefixInt(b, flags, n, uint64(len(s)))
	return append(b, s...)
}

func AppendFieldSection(b []byte, fields []Field) []byte {
	b = append(b, 0, 0)
	for _, f := range fields {
		name := strings.ToLower(f.Name)
		if i, ok := staticFull[staticEntry{name, f.Value}]; ok {
			b = appendPrefixInt(b, 0xc0, 6, uint64(i))
			continue
		}
		if i, ok := staticNames[name]; ok {
			b = appendPrefixInt(b, 0x50, 4, uint64(i))
			b = appendString(b, 0x00, 7, f.Value)
			continue
		}
		b = appendString(b, 0x20, 3, name)
		b = appendString(b, 0x00, 7, f.Value)
	}
	return b
}
