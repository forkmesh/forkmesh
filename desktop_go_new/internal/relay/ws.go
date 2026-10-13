package relay

import (
	"bufio"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"net/url"
	"strings"
	"time"
)

const (
	opContinuation = 0x0
	opText         = 0x1
	opClose        = 0x8
	opPing         = 0x9
	opPong         = 0xA

	MaxEventPayload    = 64 * 1024
	MaxOutboundPayload = 60 * 1024
	maxHandshakeBytes  = 32 * 1024
	writeTimeout       = 20 * time.Second
)

var errFrameTooLarge = errors.New("relay: frame too large")

type inbound struct {
	opcode  int
	payload []byte
	err     error
}

func randomKey() string {
	nonce := make([]byte, 16)
	_, _ = rand.Read(nonce)
	return base64.StdEncoding.EncodeToString(nonce)
}

func requestPath(target *url.URL) string {
	path := target.EscapedPath()
	if path == "" {
		path = "/"
	}
	if target.RawQuery != "" {
		path += "?" + target.RawQuery
	}
	return path
}

func hostHeader(target *url.URL) string {
	host := target.Hostname()
	if strings.Contains(host, ":") {
		host = "[" + host + "]"
	}
	if port := target.Port(); port != "" {
		host += ":" + port
	}
	return host
}

func handshakeRequest(target *url.URL, key, userAgent string) string {
	return "GET " + requestPath(target) + " HTTP/1.1\r\n" +
		"Host: " + hostHeader(target) + "\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Version: 13\r\n" +
		"Sec-WebSocket-Key: " + key + "\r\n" +
		"User-Agent: " + userAgent + "\r\n\r\n"
}

func readUpgrade(reader *bufio.Reader) (string, bool, error) {
	var header []byte
	for {
		line, err := reader.ReadSlice('\n')
		header = append(header, line...)
		if err != nil && !errors.Is(err, bufio.ErrBufferFull) {
			return "", false, err
		}
		if len(header) > maxHandshakeBytes {
			return "", false, errors.New("relay: upgrade header too large")
		}
		if strings.HasSuffix(string(header), "\r\n\r\n") {
			break
		}
	}
	text := string(header[:len(header)-4])
	status := text
	if end := strings.Index(text, "\r\n"); end >= 0 {
		status = text[:end]
	}
	ok := strings.HasPrefix(text, "HTTP/1.1 101") || strings.HasPrefix(text, "HTTP/1.0 101")
	if len(status) > 80 {
		status = status[:80]
	}
	return strings.TrimSpace(status), ok, nil
}

func readFrame(reader *bufio.Reader) (bool, int, []byte, error) {
	var head [2]byte
	if _, err := io.ReadFull(reader, head[:]); err != nil {
		return false, 0, nil, err
	}
	final := head[0]&0x80 != 0
	opcode := int(head[0] & 0x0f)
	length := uint64(head[1] & 0x7f)
	switch length {
	case 126:
		var extended [2]byte
		if _, err := io.ReadFull(reader, extended[:]); err != nil {
			return false, 0, nil, err
		}
		length = uint64(binary.BigEndian.Uint16(extended[:]))
	case 127:
		var extended [8]byte
		if _, err := io.ReadFull(reader, extended[:]); err != nil {
			return false, 0, nil, err
		}
		length = binary.BigEndian.Uint64(extended[:])
	}
	if length > MaxEventPayload {
		return false, 0, nil, errFrameTooLarge
	}
	var mask []byte
	if head[1]&0x80 != 0 {
		mask = make([]byte, 4)
		if _, err := io.ReadFull(reader, mask); err != nil {
			return false, 0, nil, err
		}
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(reader, payload); err != nil {
		return false, 0, nil, err
	}
	if mask != nil {
		for i := range payload {
			payload[i] ^= mask[i%4]
		}
	}
	return final, opcode, payload, nil
}

func readMessage(reader *bufio.Reader) (int, []byte, error) {
	var message []byte
	opcode := -1
	for {
		final, frameOpcode, payload, err := readFrame(reader)
		if err != nil {
			return 0, nil, err
		}
		if frameOpcode >= opClose {
			return frameOpcode, payload, nil
		}
		if frameOpcode != opContinuation {
			opcode = frameOpcode
			message = payload
		} else {
			message = append(message, payload...)
		}
		if len(message) > MaxEventPayload {
			return 0, nil, errFrameTooLarge
		}
		if final {
			return opcode, message, nil
		}
	}
}

func encodeFrame(opcode int, payload []byte) []byte {
	frame := make([]byte, 0, len(payload)+8)
	frame = append(frame, byte(0x80|(opcode&0x0f)))
	if len(payload) < 126 {
		frame = append(frame, byte(0x80|len(payload)))
	} else {
		frame = append(frame, 0x80|126, byte(len(payload)>>8), byte(len(payload)))
	}
	mask := make([]byte, 4)
	_, _ = rand.Read(mask)
	frame = append(frame, mask...)
	for i, b := range payload {
		frame = append(frame, b^mask[i%4])
	}
	return frame
}

func writeFrame(conn net.Conn, opcode int, payload []byte) error {
	if opcode >= opClose && len(payload) > 125 {
		return errFrameTooLarge
	}
	if len(payload) > MaxOutboundPayload {
		return errFrameTooLarge
	}
	_ = conn.SetWriteDeadline(time.Now().Add(writeTimeout))
	_, err := conn.Write(encodeFrame(opcode, payload))
	_ = conn.SetWriteDeadline(time.Time{})
	return err
}
