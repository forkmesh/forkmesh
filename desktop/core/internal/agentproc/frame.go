package agentproc

import (
	"bufio"
	"encoding/binary"
	"errors"
	"io"
)

const (
	frameWrite  byte = 'w'
	frameClose  byte = 'c'
	frameSignal byte = 's'
	frameGroup  byte = 'g'
	frameKill   byte = 'k'
	frameStop   byte = 't'
	frameHello  byte = 'h'
	frameData   byte = 'n'
	frameExit   byte = 'x'

	maxFramePayload = 16 << 20
	shimProtocol    = 1
)

var errFrameTooLarge = errors.New("agentproc: frame too large")

func writeFrame(w io.Writer, kind byte, payload []byte) error {
	if len(payload) > maxFramePayload {
		return errFrameTooLarge
	}
	header := make([]byte, 5, 5+len(payload))
	header[0] = kind
	binary.BigEndian.PutUint32(header[1:], uint32(len(payload)))
	_, err := w.Write(append(header, payload...))
	return err
}

func readFrame(r *bufio.Reader) (byte, []byte, error) {
	var header [5]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return 0, nil, err
	}
	size := binary.BigEndian.Uint32(header[1:])
	if size > maxFramePayload {
		return 0, nil, errFrameTooLarge
	}
	payload := make([]byte, size)
	if _, err := io.ReadFull(r, payload); err != nil {
		return 0, nil, err
	}
	return header[0], payload, nil
}

func int32Payload(value int) []byte {
	out := make([]byte, 4)
	binary.BigEndian.PutUint32(out, uint32(int32(value)))
	return out
}

func payloadInt32(payload []byte) int {
	if len(payload) < 4 {
		return 0
	}
	return int(int32(binary.BigEndian.Uint32(payload)))
}

func lengthsPayload(out, err int64) []byte {
	payload := make([]byte, 16)
	binary.BigEndian.PutUint64(payload, uint64(out))
	binary.BigEndian.PutUint64(payload[8:], uint64(err))
	return payload
}

func payloadLengths(payload []byte) (int64, int64) {
	if len(payload) < 16 {
		return 0, 0
	}
	return int64(binary.BigEndian.Uint64(payload)), int64(binary.BigEndian.Uint64(payload[8:]))
}
