package rtp

import (
	"encoding/binary"
	"errors"
)

var (
	errShort     = errors.New("rtp: packet too short")
	errVersion   = errors.New("rtp: version is not 2")
	errPadding   = errors.New("rtp: invalid padding")
	errExtension = errors.New("rtp: extension overruns packet")
	errCSRC      = errors.New("rtp: csrc list overruns packet")
)

type Header struct {
	Marker           bool
	PayloadType      uint8
	SequenceNumber   uint16
	Timestamp        uint32
	SSRC             uint32
	CSRC             []uint32
	ExtensionProfile uint16
	Extension        []byte
	PayloadOffset    int
	PaddingLength    int
}

const headerLen = 12

func ParseHeader(b []byte) (Header, error) {
	var h Header
	if len(b) < headerLen {
		return h, errShort
	}
	if b[0]>>6 != 2 {
		return h, errVersion
	}
	padding := b[0]&0x20 != 0
	extension := b[0]&0x10 != 0
	cc := int(b[0] & 0x0f)
	h.Marker = b[1]&0x80 != 0
	h.PayloadType = b[1] & 0x7f
	h.SequenceNumber = binary.BigEndian.Uint16(b[2:4])
	h.Timestamp = binary.BigEndian.Uint32(b[4:8])
	h.SSRC = binary.BigEndian.Uint32(b[8:12])
	off := headerLen
	if len(b) < off+4*cc {
		return Header{}, errCSRC
	}
	if cc > 0 {
		h.CSRC = make([]uint32, cc)
		for i := range h.CSRC {
			h.CSRC[i] = binary.BigEndian.Uint32(b[off : off+4])
			off += 4
		}
	}
	if extension {
		if len(b) < off+4 {
			return Header{}, errExtension
		}
		h.ExtensionProfile = binary.BigEndian.Uint16(b[off : off+2])
		words := int(binary.BigEndian.Uint16(b[off+2 : off+4]))
		off += 4
		if len(b)-off < words*4 {
			return Header{}, errExtension
		}
		h.Extension = b[off : off+words*4 : off+words*4]
		off += words * 4
	}
	h.PayloadOffset = off
	if padding {
		if len(b) == off {
			return Header{}, errPadding
		}
		n := int(b[len(b)-1])
		if n == 0 || n > len(b)-off {
			return Header{}, errPadding
		}
		h.PaddingLength = n
	}
	return h, nil
}

func IsRTCP(b []byte) bool {
	return len(b) >= 2 && b[1] >= 192 && b[1] <= 223
}

func SetSSRC(b []byte, ssrc uint32) {
	if len(b) < headerLen {
		return
	}
	binary.BigEndian.PutUint32(b[8:12], ssrc)
}

func SetPayloadType(b []byte, pt uint8) {
	if len(b) < 2 {
		return
	}
	b[1] = b[1]&0x80 | pt&0x7f
}
