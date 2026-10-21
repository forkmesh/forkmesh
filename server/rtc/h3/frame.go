package h3

import (
	"errors"
	"fmt"
	"io"

	"forkmesh.com/mirror-node/rtc/quic"
)

const (
	FrameData        = 0x00
	FrameHeaders     = 0x01
	FrameCancelPush  = 0x03
	FrameSettings    = 0x04
	FramePushPromise = 0x05
	FrameGoAway      = 0x07
	FrameMaxPushID   = 0x0d
)

const (
	StreamControl      = 0x00
	StreamPush         = 0x01
	StreamQPACKEncoder = 0x02
	StreamQPACKDecoder = 0x03
	StreamWebTransport = 0x54
	SignalWebTransport = 0x41
)

const (
	SettingQPACKMaxTableCapacity   = 0x01
	SettingMaxFieldSectionSize     = 0x06
	SettingQPACKBlockedStreams     = 0x07
	SettingEnableConnectProtocol   = 0x08
	SettingH3Datagram              = 0x33
	SettingH3DatagramDraft04       = 0xffd277
	SettingEnableWebTransport      = 0x2b603742
	SettingWebTransportMaxSessions = 0xc671706a
)

type ErrorCode uint64

const (
	ErrNoError              ErrorCode = 0x100
	ErrGeneralProtocol      ErrorCode = 0x101
	ErrInternal             ErrorCode = 0x102
	ErrStreamCreation       ErrorCode = 0x103
	ErrClosedCriticalStream ErrorCode = 0x104
	ErrFrameUnexpected      ErrorCode = 0x105
	ErrFrame                ErrorCode = 0x106
	ErrExcessiveLoad        ErrorCode = 0x107
	ErrID                   ErrorCode = 0x108
	ErrSettings             ErrorCode = 0x109
	ErrMissingSettings      ErrorCode = 0x10a
	ErrRequestRejected      ErrorCode = 0x10b
	ErrRequestCancelled     ErrorCode = 0x10c
	ErrRequestIncomplete    ErrorCode = 0x10d
	ErrMessage              ErrorCode = 0x10e
	ErrConnect              ErrorCode = 0x10f
	ErrVersionFallback      ErrorCode = 0x110
	ErrQPACKDecompression   ErrorCode = 0x200
	ErrDatagram             ErrorCode = 0x33
)

type Error struct {
	Code   ErrorCode
	Reason string
}

func (e *Error) Error() string { return fmt.Sprintf("h3: error 0x%x: %s", uint64(e.Code), e.Reason) }

var (
	errHeaderTooLarge = errors.New("h3: field section too large")
	errFrameTooLarge  = &Error{ErrExcessiveLoad, "frame too large"}
)

func isReservedFrameType(t uint64) bool {
	return t >= 0x21 && (t-0x21)%0x1f == 0
}

type byteReader struct {
	r   io.Reader
	buf [1]byte
}

func (b *byteReader) ReadByte() (byte, error) {
	if _, err := io.ReadFull(b.r, b.buf[:]); err != nil {
		return 0, err
	}
	return b.buf[0], nil
}

func ReadVarint(r io.Reader) (uint64, error) {
	br := byteReader{r: r}
	first, err := br.ReadByte()
	if err != nil {
		return 0, err
	}
	n := 1 << (first >> 6)
	v := uint64(first & 0x3f)
	for i := 1; i < n; i++ {
		c, err := br.ReadByte()
		if err != nil {
			if err == io.EOF {
				err = io.ErrUnexpectedEOF
			}
			return 0, err
		}
		v = v<<8 | uint64(c)
	}
	return v, nil
}

func readFrameHeader(r io.Reader) (typ, length uint64, err error) {
	typ, err = ReadVarint(r)
	if err != nil {
		return 0, 0, err
	}
	length, err = ReadVarint(r)
	if err == io.EOF {
		err = io.ErrUnexpectedEOF
	}
	return typ, length, err
}

func readPayload(r io.Reader, length uint64, max int) ([]byte, error) {
	if length > uint64(max) {
		return nil, errFrameTooLarge
	}
	b := make([]byte, length)
	if _, err := io.ReadFull(r, b); err != nil {
		if err == io.EOF {
			err = io.ErrUnexpectedEOF
		}
		return nil, err
	}
	return b, nil
}

func discard(r io.Reader, length uint64) error {
	_, err := io.CopyN(io.Discard, r, int64(length))
	if err == io.EOF {
		err = io.ErrUnexpectedEOF
	}
	return err
}

func appendFrame(b []byte, typ uint64, payload []byte) []byte {
	b = quic.AppendVarint(b, typ)
	b = quic.AppendVarint(b, uint64(len(payload)))
	return append(b, payload...)
}

type Settings map[uint64]uint64

func (s Settings) encode() []byte {
	var b []byte
	for _, id := range sortedKeys(s) {
		b = quic.AppendVarint(b, id)
		b = quic.AppendVarint(b, s[id])
	}
	return b
}

func sortedKeys(s Settings) []uint64 {
	keys := make([]uint64, 0, len(s))
	for k := range s {
		keys = append(keys, k)
	}
	for i := 1; i < len(keys); i++ {
		for j := i; j > 0 && keys[j] < keys[j-1]; j-- {
			keys[j], keys[j-1] = keys[j-1], keys[j]
		}
	}
	return keys
}

const maxSettings = 64

func parseSettings(b []byte) (Settings, error) {
	s := Settings{}
	for len(b) > 0 {
		id, n := quic.ReadVarint(b)
		if n == 0 {
			return nil, &Error{ErrFrame, "truncated setting"}
		}
		b = b[n:]
		v, n := quic.ReadVarint(b)
		if n == 0 {
			return nil, &Error{ErrFrame, "truncated setting"}
		}
		b = b[n:]
		if id >= 0x02 && id <= 0x05 {
			return nil, &Error{ErrSettings, "HTTP/2 setting in HTTP/3"}
		}
		if _, dup := s[id]; dup {
			return nil, &Error{ErrSettings, "duplicate setting"}
		}
		if len(s) >= maxSettings {
			return nil, &Error{ErrExcessiveLoad, "too many settings"}
		}
		s[id] = v
	}
	return s, nil
}
