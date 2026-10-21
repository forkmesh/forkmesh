package signal

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

const (
	MaxMessageBytes  = 64 * 1024
	JoinMessageBytes = 4 * 1024
	websocketGUID    = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
	opContinuation   = 0x0
	opText           = 0x1
	opBinary         = 0x2
	opClose          = 0x8
	opPing           = 0x9
	opPong           = 0xA
)

var (
	ErrNotWebSocket  = errors.New("not a websocket upgrade")
	ErrOrigin        = errors.New("origin not allowed")
	ErrProtocol      = errors.New("websocket protocol violation")
	ErrMessageTooBig = errors.New("websocket message too big")
	ErrClosed        = errors.New("websocket closed")
)

type Conn struct {
	conn    net.Conn
	reader  *bufio.Reader
	writeMu sync.Mutex
	closed  bool
	limit   int
}

func Upgrade(w http.ResponseWriter, r *http.Request, allowOrigin func(string) bool) (*Conn, error) {
	if r.Method != http.MethodGet ||
		!headerHasToken(r.Header, "Connection", "upgrade") ||
		!strings.EqualFold(r.Header.Get("Upgrade"), "websocket") ||
		r.Header.Get("Sec-WebSocket-Version") != "13" {
		http.Error(w, "websocket upgrade required", http.StatusBadRequest)
		return nil, ErrNotWebSocket
	}
	key := strings.TrimSpace(r.Header.Get("Sec-WebSocket-Key"))
	if decoded, err := base64.StdEncoding.DecodeString(key); err != nil || len(decoded) != 16 {
		http.Error(w, "bad websocket key", http.StatusBadRequest)
		return nil, ErrNotWebSocket
	}
	if allowOrigin != nil && !allowOrigin(r.Header.Get("Origin")) {
		http.Error(w, "origin not allowed", http.StatusForbidden)
		return nil, ErrOrigin
	}
	hijacker, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "websocket unsupported", http.StatusInternalServerError)
		return nil, ErrNotWebSocket
	}
	conn, buffered, err := hijacker.Hijack()
	if err != nil {
		return nil, err
	}
	digest := sha1.Sum([]byte(key + websocketGUID))
	response := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + base64.StdEncoding.EncodeToString(digest[:]) + "\r\n\r\n"
	if _, err := buffered.WriteString(response); err != nil {
		conn.Close()
		return nil, err
	}
	if err := buffered.Flush(); err != nil {
		conn.Close()
		return nil, err
	}
	return &Conn{conn: conn, reader: buffered.Reader, limit: JoinMessageBytes}, nil
}

func (c *Conn) SetLimit(bytes int) {
	if bytes > 0 && bytes <= MaxMessageBytes {
		c.limit = bytes
	}
}

func headerHasToken(header http.Header, name, token string) bool {
	for _, value := range header.Values(name) {
		for _, part := range strings.Split(value, ",") {
			if strings.EqualFold(strings.TrimSpace(part), token) {
				return true
			}
		}
	}
	return false
}

func (c *Conn) SetReadDeadline(t time.Time) error {
	return c.conn.SetReadDeadline(t)
}

func (c *Conn) ReadJSON(v any) error {
	message, err := c.readMessage()
	if err != nil {
		return err
	}
	return json.Unmarshal(message, v)
}

func (c *Conn) WriteJSON(v any) error {
	payload, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return c.writeFrame(opText, payload)
}

func (c *Conn) Close() error {
	c.writeMu.Lock()
	already := c.closed
	c.closed = true
	c.writeMu.Unlock()
	if !already {
		_ = c.writeRaw(opClose, []byte{0x03, 0xE8})
	}
	return c.conn.Close()
}

func (c *Conn) readMessage() ([]byte, error) {
	var message []byte
	var messageOp byte
	for {
		fin, op, payload, err := c.readFrame()
		if err != nil {
			return nil, err
		}
		switch op {
		case opPing:
			if err := c.writeFrame(opPong, payload); err != nil {
				return nil, err
			}
			continue
		case opPong:
			continue
		case opClose:
			c.Close()
			return nil, ErrClosed
		case opText, opBinary:
			if messageOp != 0 {
				return nil, ErrProtocol
			}
			messageOp = op
			message = append(message[:0], payload...)
		case opContinuation:
			if messageOp == 0 {
				return nil, ErrProtocol
			}
			if len(message)+len(payload) > c.limit {
				return nil, ErrMessageTooBig
			}
			message = append(message, payload...)
		default:
			return nil, ErrProtocol
		}
		if fin {
			if messageOp != opText {
				return nil, ErrProtocol
			}
			return message, nil
		}
	}
}

func (c *Conn) readFrame() (bool, byte, []byte, error) {
	var head [2]byte
	if _, err := io.ReadFull(c.reader, head[:]); err != nil {
		return false, 0, nil, err
	}
	fin := head[0]&0x80 != 0
	if head[0]&0x70 != 0 {
		return false, 0, nil, ErrProtocol
	}
	op := head[0] & 0x0F
	if head[1]&0x80 == 0 {
		return false, 0, nil, ErrProtocol
	}
	length := uint64(head[1] & 0x7F)
	switch length {
	case 126:
		var ext [2]byte
		if _, err := io.ReadFull(c.reader, ext[:]); err != nil {
			return false, 0, nil, err
		}
		length = uint64(binary.BigEndian.Uint16(ext[:]))
	case 127:
		var ext [8]byte
		if _, err := io.ReadFull(c.reader, ext[:]); err != nil {
			return false, 0, nil, err
		}
		length = binary.BigEndian.Uint64(ext[:])
	}
	if op >= opClose && (length > 125 || !fin) {
		return false, 0, nil, ErrProtocol
	}
	if length > uint64(c.limit) {
		return false, 0, nil, ErrMessageTooBig
	}
	var mask [4]byte
	if _, err := io.ReadFull(c.reader, mask[:]); err != nil {
		return false, 0, nil, err
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(c.reader, payload); err != nil {
		return false, 0, nil, err
	}
	for i := range payload {
		payload[i] ^= mask[i%4]
	}
	return fin, op, payload, nil
}

func (c *Conn) writeFrame(op byte, payload []byte) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	if c.closed {
		return ErrClosed
	}
	return c.writeRawLocked(op, payload)
}

func (c *Conn) writeRaw(op byte, payload []byte) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	return c.writeRawLocked(op, payload)
}

func (c *Conn) writeRawLocked(op byte, payload []byte) error {
	header := make([]byte, 0, 10)
	header = append(header, 0x80|op)
	switch {
	case len(payload) < 126:
		header = append(header, byte(len(payload)))
	case len(payload) <= 0xFFFF:
		header = append(header, 126, byte(len(payload)>>8), byte(len(payload)))
	default:
		header = append(header, 127)
		header = binary.BigEndian.AppendUint64(header, uint64(len(payload)))
	}
	_ = c.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
	if _, err := c.conn.Write(append(header, payload...)); err != nil {
		return err
	}
	return nil
}
