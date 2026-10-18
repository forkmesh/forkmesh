package mirrornode

import (
	"bufio"
	"context"
	"crypto/rand"
	"crypto/sha1"
	"crypto/tls"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

// EventSocket carries payload-free relay events over one authenticated
// WebSocket. Reconnects emit a catch-up event, and commit notifications replay
// the relay's newest retained refs generation; the daemon's slow reconciliation
// remains a final safety net. Small signed lease writes may share the socket;
// bulk repository data and commands remain on signed HTTPS routes.
type EventSocket struct {
	endpoint string
	owner    string
	identity *Identity
	watch    string

	// Handler receives relay pushes and one synthetic catch-up after each connect.
	Handler   func(Event)
	Connected func(bool)
	// Terminal receives the owner sign-in frames the relay forwards between
	// this node and the account's browser (terminal.go). Unlike an Event it is
	// addressed: the relay broadcasts to every node socket on the account, so
	// the receiver checks the frame's node name against its own.
	Terminal func(TerminalFrame)

	tlsConfig    *tls.Config
	dial         func(ctx context.Context, network, address string) (net.Conn, error)
	pingInterval time.Duration
	idleTimeout  time.Duration
	minBackoff   time.Duration
	maxBackoff   time.Duration

	stateMu     sync.Mutex
	writeMu     sync.Mutex
	conn        net.Conn
	accepts     map[string]bool
	pending     map[string]chan socketResult
	lastAppSend time.Time
}

type socketResult struct {
	status    int
	payload   []byte
	delivered bool
}

// Event is one payload-free relay push.
type Event struct {
	Topic      string
	Repo       string
	State      string
	Generation int64
	CatchUp    bool
}

const (
	websocketGUID       = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
	eventMaxFrameBytes  = 16 << 10
	eventHandshakeLimit = 32 << 10
)

// NewEventSocket builds the authenticated channel for one owner account.
func NewEventSocket(catalogURL, owner string, identity *Identity) (*EventSocket, error) {
	base, err := url.Parse(catalogURL)
	if err != nil || base.Host == "" {
		return nil, errors.New("catalogUrl must be an absolute URL")
	}
	// Plain ws:// is accepted only for a loopback relay so the tests can run a
	// real handshake; production is always wss:// over the public relay.
	loopback := base.Scheme == "http" && isLoopbackHost(base.Hostname())
	if base.Scheme != "https" && !loopback {
		return nil, errors.New("catalogUrl must be an absolute HTTPS URL")
	}
	if !nodePattern.MatchString(owner) {
		return nil, errors.New("event socket owner is invalid")
	}
	if identity == nil {
		return nil, errors.New("event socket requires an identity")
	}
	scheme := "wss"
	if loopback {
		scheme = "ws"
	}
	endpoint := (&url.URL{Scheme: scheme, Host: base.Host, Path: "/api/nodes/events"}).String()
	return &EventSocket{
		endpoint: endpoint,
		owner:    owner,
		identity: identity,
		// Endpoint leases normally write on this connection every four minutes.
		// Check after that cadence so the lease frame replaces the heartbeat;
		// nodes without a publishable endpoint still keep the socket alive.
		pingInterval: 5 * time.Minute,
		idleTimeout:  9 * time.Minute,
		minBackoff:   time.Second,
		maxBackoff:   time.Minute,
	}, nil
}

func isLoopbackHost(host string) bool {
	return host == "127.0.0.1" || host == "localhost" || host == "::1"
}

// Run keeps the channel connected until ctx is done.
func (s *EventSocket) Run(ctx context.Context) {
	backoff := s.minBackoff
	for ctx.Err() == nil {
		start := time.Now()
		if err := s.serve(ctx); err != nil && ctx.Err() == nil {
			if s.watch != "" {
				log.Printf("upstream watch %s: %v", s.endpoint+"?watch="+s.watch, err)
			} else {
				log.Printf("node event channel: %v", err)
			}
		}
		if time.Since(start) >= 2*s.pingInterval {
			backoff = s.minBackoff
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(backoff):
		}
		if backoff *= 2; backoff > s.maxBackoff {
			backoff = s.maxBackoff
		}
	}
}

// signedURL mints a fresh forkmesh-issues-pull-v1 drain token per attempt —
// the same canonical the inbox GET/DELETE routes and GET /api/sync verify, so
// the channel grants no authority this node does not already hold.
func (s *EventSocket) signedURL() string {
	if s.watch != "" {
		query := url.Values{}
		query.Set("watch", s.watch)
		return s.endpoint + "?" + query.Encode()
	}
	timestamp := strconv.FormatInt(time.Now().UnixMilli(), 10)
	canonical := "forkmesh-issues-pull-v1\n" + s.owner + "\n" + timestamp
	query := url.Values{}
	query.Set("owner", s.owner)
	query.Set("ts", timestamp)
	query.Set("sig", s.identity.Sign([]byte(canonical)))
	return s.endpoint + "?" + query.Encode()
}

func (s *EventSocket) serve(ctx context.Context) error {
	conn, reader, err := s.connect(ctx)
	if err != nil {
		return err
	}
	defer conn.Close()
	// Close the connection as soon as the daemon stops so the read below
	// unblocks; the hand-rolled framing has no other cancellation point.
	done := make(chan struct{})
	defer close(done)
	go func() {
		select {
		case <-ctx.Done():
			_ = conn.Close()
		case <-done:
		}
	}()

	s.openSession(conn)
	defer s.closeSession(conn)
	s.emit(Event{CatchUp: true})

	pings := time.NewTicker(s.pingInterval)
	defer pings.Stop()
	writeErr := make(chan error, 1)
	go func() {
		for {
			select {
			case <-done:
				return
			case <-pings.C:
				if err := s.keepAlive(conn); err != nil {
					select {
					case writeErr <- err:
					default:
					}
					_ = conn.Close()
					return
				}
			}
		}
	}()

	for {
		_ = conn.SetReadDeadline(time.Now().Add(s.idleTimeout))
		opcode, payload, err := readMessage(reader)
		if err != nil {
			select {
			case werr := <-writeErr:
				return werr
			default:
			}
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		switch opcode {
		case opcodeText:
			s.handleFrame(payload)
		case opcodePing:
			if err := s.writeFrame(conn, opcodePong, payload); err != nil {
				return err
			}
		case opcodeClose:
			return errors.New("relay closed the event channel")
		}
	}
}

func (s *EventSocket) handleFrame(payload []byte) {
	var frame struct {
		Type       string          `json:"type"`
		Topic      string          `json:"topic"`
		Repo       string          `json:"repo"`
		State      string          `json:"state"`
		Accepts    []string        `json:"accepts"`
		RequestID  string          `json:"requestId"`
		Status     int             `json:"status"`
		Response   json.RawMessage `json:"response"`
		Generation int64           `json:"generation"`
	}
	if err := json.Unmarshal(payload, &frame); err != nil {
		return
	}
	if frame.Type == "terminal" {
		s.handleTerminalFrame(payload)
		return
	}
	if frame.Type == "hello" {
		s.stateMu.Lock()
		s.accepts = make(map[string]bool, len(frame.Accepts))
		for _, kind := range frame.Accepts {
			s.accepts[kind] = true
		}
		s.stateMu.Unlock()
		return
	}
	if frame.Type == "endpoint-result" {
		s.resolve(frame.Type, frame.RequestID, socketResult{
			status: frame.Status, payload: append([]byte(nil), frame.Response...),
			delivered: true,
		})
		return
	}
	if frame.Type != "event" {
		return
	}
	state := strings.ToLower(strings.TrimSpace(frame.State))
	if !digestPattern.MatchString(state) {
		state = ""
	}
	s.emit(Event{
		Topic:      boundedText([]byte(frame.Topic), 40),
		Repo:       boundedText([]byte(frame.Repo), 200),
		State:      state,
		Generation: max(int64(0), frame.Generation),
	})
}

// handleTerminalFrame decodes one relayed sign-in frame. Nothing here trusts
// the relay: sizes are bounded, and the receiving relay in terminal.go decides
// whether the named node is this one and whether it will run anything at all.
func (s *EventSocket) handleTerminalFrame(payload []byte) {
	if s.Terminal == nil {
		return
	}
	var frame struct {
		Action   string `json:"action"`
		Session  string `json:"session"`
		Node     string `json:"node"`
		Provider string `json:"provider"`
		Data     string `json:"data"`
		From     string `json:"from"`
		Cols     int    `json:"cols"`
		Rows     int    `json:"rows"`
	}
	if err := json.Unmarshal(payload, &frame); err != nil {
		return
	}
	// Only the browser half of an account's own stream drives a node. A frame
	// the relay did not label as such is another node's output echoed back.
	if frame.From != "browser" {
		return
	}
	s.Terminal(TerminalFrame{
		Action:   boundedText([]byte(frame.Action), 20),
		Session:  boundedText([]byte(frame.Session), 64),
		Node:     strings.ToLower(boundedText([]byte(frame.Node), 63)),
		Provider: boundedText([]byte(frame.Provider), 20),
		Data:     boundedText([]byte(frame.Data), 12000),
		Cols:     min(max(frame.Cols, 0), 500),
		Rows:     min(max(frame.Rows, 0), 200),
	})
}

// SendTerminal carries one terminal frame back to the account's browser. It
// returns false when the socket is down: terminal bytes are worth nothing
// queued, and the browser re-announces its session on reconnect.
func (s *EventSocket) SendTerminal(frame map[string]any) bool {
	raw, err := json.Marshal(frame)
	if err != nil || len(raw) > eventMaxFrameBytes {
		return false
	}
	s.stateMu.Lock()
	conn := s.conn
	s.stateMu.Unlock()
	if conn == nil {
		return false
	}
	if err := s.writeApplicationFrame(conn, raw); err != nil {
		_ = conn.Close()
		return false
	}
	return true
}

func (s *EventSocket) emit(event Event) {
	if s.Handler != nil {
		s.Handler(event)
	}
}

func (s *EventSocket) setConnected(connected bool) {
	if s.Connected != nil {
		s.Connected(connected)
	}
}

func (s *EventSocket) openSession(conn net.Conn) {
	s.stateMu.Lock()
	s.conn = conn
	s.accepts = map[string]bool{}
	s.pending = map[string]chan socketResult{}
	s.lastAppSend = time.Now()
	s.stateMu.Unlock()
	s.setConnected(true)
}

func (s *EventSocket) closeSession(conn net.Conn) {
	var waiting []chan socketResult
	s.stateMu.Lock()
	if s.conn == conn {
		s.conn = nil
		s.accepts = nil
		for key, result := range s.pending {
			waiting = append(waiting, result)
			delete(s.pending, key)
		}
	}
	s.stateMu.Unlock()
	for _, result := range waiting {
		result <- socketResult{}
	}
	s.setConnected(false)
}

func (s *EventSocket) resolve(kind, requestID string, result socketResult) {
	key := kind + "\x00" + requestID
	s.stateMu.Lock()
	waiter := s.pending[key]
	delete(s.pending, key)
	s.stateMu.Unlock()
	if waiter != nil {
		waiter <- result
	}
}

// Request carries one independently signed write over the existing event
// socket. The bool is false only when this relay/socket cannot carry the
// frame, allowing the caller to use its HTTPS recovery path. An application
// error delivered by the relay remains a real result and must not be doubled
// by an immediate HTTP retry.
func (s *EventSocket) Request(
	ctx context.Context, kind, requestID string, payload map[string]any,
) (int, []byte, bool) {
	if kind == "" || requestID == "" {
		return 0, nil, false
	}
	frame := make(map[string]any, len(payload)+1)
	for key, value := range payload {
		frame[key] = value
	}
	frame["type"] = kind
	raw, err := json.Marshal(frame)
	if err != nil || len(raw) > eventMaxFrameBytes {
		return 0, nil, false
	}
	resultKey := kind + "-result\x00" + requestID
	waiter := make(chan socketResult, 1)
	s.stateMu.Lock()
	conn := s.conn
	if conn == nil || !s.accepts[kind] || s.pending[resultKey] != nil {
		s.stateMu.Unlock()
		return 0, nil, false
	}
	s.pending[resultKey] = waiter
	s.stateMu.Unlock()
	if err := s.writeApplicationFrame(conn, raw); err != nil {
		s.stateMu.Lock()
		delete(s.pending, resultKey)
		s.stateMu.Unlock()
		_ = conn.Close()
		return 0, nil, false
	}
	select {
	case <-ctx.Done():
		s.stateMu.Lock()
		delete(s.pending, resultKey)
		s.stateMu.Unlock()
		return 0, nil, false
	case result := <-waiter:
		return result.status, result.payload, result.delivered
	}
}

func (s *EventSocket) writeFrame(conn net.Conn, opcode int, payload []byte) error {
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	return writeFrame(conn, opcode, payload)
}

func (s *EventSocket) writeApplicationFrame(conn net.Conn, payload []byte) error {
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	return s.writeApplicationFrameLocked(conn, payload)
}

// writeApplicationFrameLocked keeps the successful write and its lease clock
// update atomic with respect to keepAlive. Otherwise a timer that inspected
// the old clock while an endpoint frame was being written could append a
// redundant ping immediately behind it.
func (s *EventSocket) writeApplicationFrameLocked(
	conn net.Conn, payload []byte,
) error {
	if err := writeFrame(conn, opcodeText, payload); err != nil {
		return err
	}
	s.stateMu.Lock()
	if s.conn == conn {
		s.lastAppSend = time.Now()
	}
	s.stateMu.Unlock()
	return nil
}

func (s *EventSocket) keepAlive(conn net.Conn) error {
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	s.stateMu.Lock()
	recent := s.conn == conn && time.Since(s.lastAppSend) < s.pingInterval
	s.stateMu.Unlock()
	if recent {
		return nil
	}
	return s.writeApplicationFrameLocked(conn, []byte(`{"type":"ping"}`))
}

func (s *EventSocket) connect(ctx context.Context) (net.Conn, *bufio.Reader, error) {
	target, err := url.Parse(s.signedURL())
	if err != nil {
		return nil, nil, err
	}
	address := target.Host
	if target.Port() == "" {
		if target.Scheme == "wss" {
			address = net.JoinHostPort(target.Hostname(), "443")
		} else {
			address = net.JoinHostPort(target.Hostname(), "80")
		}
	}
	dialCtx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	var conn net.Conn
	switch {
	case s.dial != nil:
		conn, err = s.dial(dialCtx, "tcp", address)
	case target.Scheme == "wss":
		dialer := &tls.Dialer{Config: s.tlsConfig}
		conn, err = dialer.DialContext(dialCtx, "tcp", address)
	default:
		conn, err = (&net.Dialer{}).DialContext(dialCtx, "tcp", address)
	}
	if err != nil {
		return nil, nil, err
	}
	ok := false
	defer func() {
		if !ok {
			_ = conn.Close()
		}
	}()

	nonce := make([]byte, 16)
	if _, err := rand.Read(nonce); err != nil {
		return nil, nil, err
	}
	key := base64.StdEncoding.EncodeToString(nonce)
	requestURI := target.RequestURI()
	handshake := "GET " + requestURI + " HTTP/1.1\r\n" +
		"Host: " + target.Host + "\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Key: " + key + "\r\n" +
		"Sec-WebSocket-Version: 13\r\n" +
		"User-Agent: ForkMesh-Mirror-Node\r\n\r\n"
	_ = conn.SetWriteDeadline(time.Now().Add(20 * time.Second))
	if _, err := io.WriteString(conn, handshake); err != nil {
		return nil, nil, err
	}
	_ = conn.SetReadDeadline(time.Now().Add(20 * time.Second))
	reader := bufio.NewReaderSize(conn, 4096)
	request, err := http.NewRequest(http.MethodGet, target.String(), nil)
	if err != nil {
		return nil, nil, err
	}
	response, err := http.ReadResponse(reader, request)
	if err != nil {
		return nil, nil, err
	}
	defer func() {
		_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, eventHandshakeLimit))
		_ = response.Body.Close()
	}()
	if response.StatusCode != http.StatusSwitchingProtocols {
		body, _ := io.ReadAll(io.LimitReader(response.Body, 512))
		return nil, nil, fmt.Errorf("upgrade returned HTTP %d: %s",
			response.StatusCode, boundedText(body, 200))
	}
	digest := sha1.Sum([]byte(key + websocketGUID))
	if !strings.EqualFold(response.Header.Get("Upgrade"), "websocket") ||
		response.Header.Get("Sec-WebSocket-Accept") !=
			base64.StdEncoding.EncodeToString(digest[:]) {
		return nil, nil, errors.New("relay returned an invalid websocket handshake")
	}
	_ = conn.SetWriteDeadline(time.Time{})
	ok = true
	return conn, reader, nil
}

const (
	opcodeContinuation = 0x0
	opcodeText         = 0x1
	opcodeBinary       = 0x2
	opcodeClose        = 0x8
	opcodePing         = 0x9
	opcodePong         = 0xA
)

// readMessage joins continuation frames and rejects oversized messages.
func readMessage(reader *bufio.Reader) (int, []byte, error) {
	var (
		message []byte
		opcode  int
	)
	for {
		final, frameOpcode, payload, err := readFrame(reader)
		if err != nil {
			return 0, nil, err
		}
		if frameOpcode >= opcodeClose {
			return frameOpcode, payload, nil
		}
		if frameOpcode != opcodeContinuation {
			opcode = frameOpcode
			message = payload
		} else {
			message = append(message, payload...)
		}
		if len(message) > eventMaxFrameBytes {
			return 0, nil, errors.New("event frame too large")
		}
		if final {
			return opcode, message, nil
		}
	}
}

func readFrame(reader *bufio.Reader) (bool, int, []byte, error) {
	header := make([]byte, 2)
	if _, err := io.ReadFull(reader, header); err != nil {
		return false, 0, nil, err
	}
	final := header[0]&0x80 != 0
	opcode := int(header[0] & 0x0F)
	// RFC 6455 §5.1: a server must not mask the frames it sends.
	if header[1]&0x80 != 0 {
		return false, 0, nil, errors.New("relay sent a masked frame")
	}
	length := int64(header[1] & 0x7F)
	switch length {
	case 126:
		extended := make([]byte, 2)
		if _, err := io.ReadFull(reader, extended); err != nil {
			return false, 0, nil, err
		}
		length = int64(binary.BigEndian.Uint16(extended))
	case 127:
		extended := make([]byte, 8)
		if _, err := io.ReadFull(reader, extended); err != nil {
			return false, 0, nil, err
		}
		length = int64(binary.BigEndian.Uint64(extended) & 0x7FFFFFFFFFFFFFFF)
	}
	if length > eventMaxFrameBytes {
		return false, 0, nil, errors.New("event frame too large")
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(reader, payload); err != nil {
		return false, 0, nil, err
	}
	return final, opcode, payload, nil
}

// writeFrame emits one unfragmented client frame. Client frames are always
// masked (RFC 6455 §5.3); Cloudflare rejects an unmasked one outright.
func writeFrame(conn net.Conn, opcode int, payload []byte) error {
	if len(payload) > eventMaxFrameBytes {
		return errors.New("event frame too large")
	}
	header := []byte{byte(0x80 | opcode)}
	switch {
	case len(payload) < 126:
		header = append(header, byte(0x80|len(payload)))
	default:
		header = append(header, 0x80|126, 0, 0)
		binary.BigEndian.PutUint16(header[2:], uint16(len(payload)))
	}
	mask := make([]byte, 4)
	if _, err := rand.Read(mask); err != nil {
		return err
	}
	masked := make([]byte, len(payload))
	for i := range payload {
		masked[i] = payload[i] ^ mask[i%4]
	}
	_ = conn.SetWriteDeadline(time.Now().Add(20 * time.Second))
	defer conn.SetWriteDeadline(time.Time{})
	if _, err := conn.Write(append(append(header, mask...), masked...)); err != nil {
		return err
	}
	return nil
}
