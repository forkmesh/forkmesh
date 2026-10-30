package rtp

import (
	"encoding/binary"
	"errors"
)

const (
	TransportCCURI   = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"
	profileOneByte   = 0xBEDE
	profileTwoByte   = 0x1000
	profileTwoByteMk = 0xFFF0
	MaxPadding       = 255
)

var errRTXShort = errors.New("rtp: rtx payload too short")

func ExtensionElement(h Header, id uint8) ([]byte, bool) {
	b := h.Extension
	switch {
	case h.ExtensionProfile == profileOneByte:
		if id == 0 || id > 14 {
			return nil, false
		}
		for i := 0; i < len(b); {
			if b[i] == 0 {
				i++
				continue
			}
			eid := b[i] >> 4
			n := int(b[i]&0x0f) + 1
			if eid == 15 {
				return nil, false
			}
			if i+1+n > len(b) {
				return nil, false
			}
			if eid == id {
				return b[i+1 : i+1+n], true
			}
			i += 1 + n
		}
	case h.ExtensionProfile&profileTwoByteMk == profileTwoByte:
		if id == 0 {
			return nil, false
		}
		for i := 0; i < len(b); {
			if b[i] == 0 {
				i++
				continue
			}
			if i+2 > len(b) {
				return nil, false
			}
			eid, n := b[i], int(b[i+1])
			if i+2+n > len(b) {
				return nil, false
			}
			if eid == id {
				return b[i+2 : i+2+n], true
			}
			i += 2 + n
		}
	}
	return nil, false
}

func TransportSequence(h Header, id uint8) (uint16, bool) {
	v, ok := ExtensionElement(h, id)
	if !ok || len(v) != 2 {
		return 0, false
	}
	return binary.BigEndian.Uint16(v), true
}

func Payload(b []byte, h Header) []byte {
	end := len(b) - h.PaddingLength
	if end < h.PayloadOffset {
		return nil
	}
	return b[h.PayloadOffset:end]
}

type Packet struct {
	Marker         bool
	PayloadType    uint8
	SequenceNumber uint16
	Timestamp      uint32
	SSRC           uint32
	CSRC           []uint32
	TransportID    uint8
	TransportSeq   uint16
	Payload        []byte
	Padding        int
}

func (p *Packet) Size() int {
	n := headerLen + 4*len(p.CSRC) + len(p.Payload) + p.Padding
	if p.TransportID != 0 {
		n += 8
	}
	return n
}

func (p *Packet) Marshal(dst []byte) []byte {
	csrc := p.CSRC
	if len(csrc) > 15 {
		csrc = csrc[:15]
	}
	padding := p.Padding
	if padding > MaxPadding {
		padding = MaxPadding
	}
	first := byte(0x80) | byte(len(csrc))
	if padding > 0 {
		first |= 0x20
	}
	if p.TransportID != 0 {
		first |= 0x10
	}
	second := p.PayloadType & 0x7f
	if p.Marker {
		second |= 0x80
	}
	dst = append(dst[:0], first, second)
	dst = binary.BigEndian.AppendUint16(dst, p.SequenceNumber)
	dst = binary.BigEndian.AppendUint32(dst, p.Timestamp)
	dst = binary.BigEndian.AppendUint32(dst, p.SSRC)
	for _, c := range csrc {
		dst = binary.BigEndian.AppendUint32(dst, c)
	}
	if p.TransportID != 0 && p.TransportID <= 14 {
		dst = binary.BigEndian.AppendUint16(dst, profileOneByte)
		dst = binary.BigEndian.AppendUint16(dst, 1)
		dst = append(dst, p.TransportID<<4|1)
		dst = binary.BigEndian.AppendUint16(dst, p.TransportSeq)
		dst = append(dst, 0)
	} else if p.TransportID != 0 {
		dst = binary.BigEndian.AppendUint16(dst, profileTwoByte)
		dst = binary.BigEndian.AppendUint16(dst, 1)
		dst = append(dst, p.TransportID, 2)
		dst = binary.BigEndian.AppendUint16(dst, p.TransportSeq)
	}
	dst = append(dst, p.Payload...)
	if padding > 0 {
		for i := 1; i < padding; i++ {
			dst = append(dst, 0)
		}
		dst = append(dst, byte(padding))
	}
	return dst
}

func RTXPayload(dst []byte, originalSeq uint16, payload []byte) []byte {
	dst = binary.BigEndian.AppendUint16(dst[:0], originalSeq)
	return append(dst, payload...)
}

func ParseRTX(payload []byte) (uint16, []byte, error) {
	if len(payload) < 2 {
		return 0, nil, errRTXShort
	}
	return binary.BigEndian.Uint16(payload[:2]), payload[2:], nil
}
