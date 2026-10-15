package relay

import (
	"bufio"
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"math/rand/v2"
	"net"
	"net/url"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"forkmesh.com/desktop-core/internal/control"
)

const maxQueuedControlBytes = 16 * 1024 * 1024

type Timings struct {
	Ping          time.Duration
	AppPing       time.Duration
	StaleRx       time.Duration
	Connect       time.Duration
	ReconnectBase time.Duration
	ReconnectMax  time.Duration
	Jitter        time.Duration
}

func DefaultTimings() Timings {
	return Timings{
		Ping:          25 * time.Second,
		AppPing:       4 * time.Minute,
		StaleRx:       70 * time.Second,
		Connect:       30 * time.Second,
		ReconnectBase: 2 * time.Second,
		ReconnectMax:  2 * time.Minute,
		Jitter:        250 * time.Millisecond,
	}
}

type State struct {
	Running        bool     `json:"running"`
	Connected      bool     `json:"connected"`
	Accepts        []string `json:"accepts"`
	Relays         []string `json:"relays"`
	ControlBacklog int      `json:"controlBacklog"`
	BudgetWindowMs int64    `json:"budgetWindowMs"`
	BudgetLimit    int      `json:"budgetLimit"`
	Attempts       int      `json:"attempts"`
	Seq            int64    `json:"seq"`
}

var stateSeq atomic.Int64

type ErrRefused struct{ Status string }

func (e *ErrRefused) Error() string { return "relay: upgrade refused: " + e.Status }

const (
	phaseIdle = iota
	phaseConnecting
	phaseConnected
	phaseWaiting
)

type Client struct {
	SignedURL   func() (string, bool)
	UserAgent   func() string
	OnConnected func(connected bool)
	OnState     func(State)
	OnFrame     func(kind string, frame map[string]any, raw []byte)
	OnMessage   func(text string)
	Dial        func(ctx context.Context, network, address string) (net.Conn, error)
	TLSConfig   *tls.Config
	Timings     Timings

	mu             sync.Mutex
	writeMu        sync.Mutex
	running        bool
	cancel         context.CancelFunc
	done           chan struct{}
	kick           chan struct{}
	pump           chan struct{}
	phase          int
	attempts       int
	conn           net.Conn
	accepts        map[string]bool
	relaysTerminal bool
	relaysControl  bool
	budget         *control.SendBudget
	queue          [][]byte
	queuedBytes    int
	backlogShown   bool
	lastRx         time.Time
	lastAppSend    time.Time
}

func NewClient() *Client {
	return &Client{Timings: DefaultTimings(), budget: control.NewSendBudget(), accepts: map[string]bool{},
		kick: make(chan struct{}, 1), pump: make(chan struct{}, 1)}
}

func (c *Client) Start(ctx context.Context) {
	c.mu.Lock()
	if c.running {
		c.mu.Unlock()
		return
	}
	c.running = true
	c.attempts = 0
	runCtx, cancel := context.WithCancel(ctx)
	c.cancel = cancel
	done := make(chan struct{})
	c.done = done
	c.mu.Unlock()
	go c.run(runCtx, done)
}

func (c *Client) Stop() {
	c.mu.Lock()
	if !c.running {
		c.mu.Unlock()
		return
	}
	c.running = false
	cancel, done := c.cancel, c.done
	c.mu.Unlock()
	cancel()
	<-done
	c.emitState()
}

func (c *Client) Running() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.running
}

func (c *Client) ReconnectNow() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	if !c.running || c.phase != phaseWaiting {
		return false
	}
	c.attempts = 0
	select {
	case c.kick <- struct{}{}:
	default:
	}
	return true
}

func (c *Client) State() State {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.stateLocked()
}

func (c *Client) stateLocked() State {
	state := State{Running: c.running, Connected: c.phase == phaseConnected, Accepts: []string{}, Relays: []string{},
		ControlBacklog: len(c.queue), BudgetWindowMs: c.budget.WindowMs(), BudgetLimit: c.budget.Limit(), Attempts: c.attempts,
		Seq: stateSeq.Add(1)}
	if state.Connected {
		for kind := range c.accepts {
			state.Accepts = append(state.Accepts, kind)
		}
		sort.Strings(state.Accepts)
		if c.relaysControl {
			state.Relays = append(state.Relays, "control")
		}
		if c.relaysTerminal {
			state.Relays = append(state.Relays, "terminal")
		}
	}
	return state
}

func (c *Client) emitState() {
	if c.OnState != nil {
		c.OnState(c.State())
	}
}

func (c *Client) message(format string, args ...any) {
	if c.OnMessage != nil {
		c.OnMessage(fmt.Sprintf(format, args...))
	}
}

func (c *Client) nextDelay() time.Duration {
	c.mu.Lock()
	shift := min(c.attempts, 9)
	c.attempts++
	c.mu.Unlock()
	base := c.Timings.ReconnectBase
	delay := min(base<<shift, c.Timings.ReconnectMax)
	if delay <= 0 {
		delay = base
	}
	spread := delay/4 + c.Timings.Jitter
	if spread <= 0 {
		return delay
	}
	return delay + time.Duration(rand.Int64N(int64(spread)))
}

func (c *Client) setPhase(phase int) {
	c.mu.Lock()
	c.phase = phase
	c.mu.Unlock()
}

func (c *Client) run(ctx context.Context, done chan struct{}) {
	defer close(done)
	defer c.setPhase(phaseIdle)
	for ctx.Err() == nil {
		c.setPhase(phaseConnecting)
		conn, reader, err := c.connect(ctx)
		if err == nil {
			c.serve(ctx, conn, reader)
		} else if refused := (*ErrRefused)(nil); errors.As(err, &refused) {
			status := refused.Status
			if status == "" {
				status = "no status"
			}
			c.message("Relay event socket upgrade refused (%s); falling back to the sync poll until it reconnects.", status)
		}
		if ctx.Err() != nil {
			return
		}
		delay := c.nextDelay()
		select {
		case <-c.kick:
		default:
		}
		c.setPhase(phaseWaiting)
		c.emitState()
		timer := time.NewTimer(delay)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		case <-c.kick:
			timer.Stop()
		}
	}
}

var errNoURL = errors.New("relay: not signed in")

func (c *Client) connect(ctx context.Context) (net.Conn, *bufio.Reader, error) {
	if c.SignedURL == nil {
		return nil, nil, errNoURL
	}
	raw, ok := c.SignedURL()
	if !ok {
		return nil, nil, errNoURL
	}
	target, err := url.Parse(raw)
	if err != nil || target.Hostname() == "" || (target.Scheme != "ws" && target.Scheme != "wss") {
		return nil, nil, errNoURL
	}
	attemptCtx, cancel := context.WithTimeout(ctx, c.Timings.Connect)
	defer cancel()
	port := target.Port()
	if port == "" {
		port = map[bool]string{true: "443", false: "80"}[target.Scheme == "wss"]
	}
	address := net.JoinHostPort(target.Hostname(), port)
	dial := c.Dial
	if dial == nil {
		dial = (&net.Dialer{}).DialContext
	}
	conn, err := dial(attemptCtx, "tcp", address)
	if err != nil {
		return nil, nil, err
	}
	if tcp, isTCP := conn.(*net.TCPConn); isTCP {
		_ = tcp.SetNoDelay(true)
	}
	ok = false
	defer func() {
		if !ok {
			_ = conn.Close()
		}
	}()
	plain := conn
	stop := context.AfterFunc(attemptCtx, func() { _ = plain.SetDeadline(time.Unix(1, 0)) })
	defer stop()
	if target.Scheme == "wss" {
		config := &tls.Config{}
		if c.TLSConfig != nil {
			config = c.TLSConfig.Clone()
		}
		if config.ServerName == "" {
			config.ServerName = target.Hostname()
		}
		secure := tls.Client(conn, config)
		if err := secure.HandshakeContext(attemptCtx); err != nil {
			return nil, nil, err
		}
		conn = secure
	}
	userAgent := "ForkMeshDesktop/dev (account=; node=)"
	if c.UserAgent != nil {
		userAgent = c.UserAgent()
	}
	if _, err := conn.Write([]byte(handshakeRequest(target, randomKey(), userAgent))); err != nil {
		return nil, nil, err
	}
	reader := bufio.NewReaderSize(conn, 8192)
	status, upgraded, err := readUpgrade(reader)
	if err != nil {
		return nil, nil, err
	}
	if !upgraded {
		return nil, nil, &ErrRefused{Status: status}
	}
	if !stop() {
		return nil, nil, attemptCtx.Err()
	}
	_ = conn.SetDeadline(time.Time{})
	ok = true
	return conn, reader, nil
}

func (c *Client) write(conn net.Conn, opcode int, payload []byte) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	err := writeFrame(conn, opcode, payload)
	if err == nil && opcode == opText {
		c.mu.Lock()
		if c.conn == conn {
			c.lastAppSend = time.Now()
		}
		c.mu.Unlock()
	}
	if err != nil && !errors.Is(err, errFrameTooLarge) {
		_ = conn.Close()
	}
	return err
}

func (c *Client) serve(ctx context.Context, conn net.Conn, reader *bufio.Reader) {
	now := time.Now()
	c.mu.Lock()
	c.conn = conn
	c.phase = phaseConnected
	c.attempts = 0
	c.accepts = map[string]bool{}
	c.relaysTerminal = false
	c.relaysControl = false
	c.resetQueueLocked()
	c.lastRx = now
	c.lastAppSend = now
	c.mu.Unlock()
	if c.OnConnected != nil {
		c.OnConnected(true)
	}
	c.emitState()

	frames := make(chan inbound, 16)
	quit := make(chan struct{})
	go func() {
		for {
			opcode, payload, err := readMessage(reader)
			select {
			case frames <- inbound{opcode: opcode, payload: payload, err: err}:
			case <-quit:
				return
			}
			if err != nil {
				return
			}
		}
	}()
	ping := time.NewTicker(c.Timings.Ping)
	pace := time.NewTimer(time.Hour)
	pace.Stop()
	defer func() {
		ping.Stop()
		pace.Stop()
		close(quit)
		_ = conn.Close()
		c.mu.Lock()
		if c.conn == conn {
			c.conn = nil
		}
		c.phase = phaseConnecting
		c.accepts = map[string]bool{}
		c.relaysTerminal = false
		c.relaysControl = false
		c.resetQueueLocked()
		c.mu.Unlock()
		if c.OnConnected != nil {
			c.OnConnected(false)
		}
		c.emitState()
	}()
	for {
		select {
		case <-ctx.Done():
			c.writeMu.Lock()
			_ = writeFrame(conn, opClose, []byte{0x03, 0xE8})
			c.writeMu.Unlock()
			return
		case in := <-frames:
			if in.err != nil {
				return
			}
			c.mu.Lock()
			c.lastRx = time.Now()
			c.mu.Unlock()
			switch in.opcode {
			case opPing:
				if c.write(conn, opPong, in.payload) != nil {
					return
				}
			case opPong:
			case opClose:
				c.writeMu.Lock()
				_ = writeFrame(conn, opClose, in.payload)
				c.writeMu.Unlock()
				return
			case opText:
				c.handleText(in.payload)
			}
		case <-ping.C:
			c.mu.Lock()
			stale := time.Since(c.lastRx) > c.Timings.StaleRx
			appIdle := time.Since(c.lastAppSend) >= c.Timings.AppPing
			c.mu.Unlock()
			if stale {
				return
			}
			if c.write(conn, opPing, nil) != nil {
				return
			}
			if appIdle && c.write(conn, opText, []byte(`{"type":"ping"}`)) != nil {
				return
			}
		case <-c.pump:
			if wait, err := c.pumpControl(conn); err != nil {
				return
			} else if wait > 0 {
				pace.Reset(wait)
			}
		case <-pace.C:
			if wait, err := c.pumpControl(conn); err != nil {
				return
			} else if wait > 0 {
				pace.Reset(wait)
			}
		}
	}
}

func (c *Client) resetQueueLocked() {
	c.queue = nil
	c.queuedBytes = 0
	c.backlogShown = false
	c.budget.Reset()
}

func (c *Client) pumpControl(conn net.Conn) (time.Duration, error) {
	changed := false
	defer func() {
		if changed {
			c.emitState()
		}
	}()
	for {
		c.mu.Lock()
		if c.conn != conn {
			c.mu.Unlock()
			return 0, nil
		}
		if len(c.queue) == 0 {
			if c.backlogShown {
				c.backlogShown = false
				changed = true
			}
			c.mu.Unlock()
			return 0, nil
		}
		now := time.Now().UnixMilli()
		if wait := c.budget.WaitMs(now); wait > 0 {
			if !c.backlogShown {
				c.backlogShown = true
				changed = true
			}
			c.mu.Unlock()
			return time.Duration(min(wait, 60000)) * time.Millisecond, nil
		}
		c.budget.Spend(now)
		payload := c.queue[0]
		c.queue[0] = nil
		c.queue = c.queue[1:]
		c.queuedBytes -= len(payload)
		c.mu.Unlock()
		if err := c.write(conn, opText, payload); err != nil {
			return 0, err
		}
	}
}

func (c *Client) handleText(payload []byte) {
	var frame map[string]any
	if json.Unmarshal(payload, &frame) != nil || frame == nil {
		return
	}
	kind, _ := frame["type"].(string)
	if kind == "hello" {
		c.applyHello(frame)
		c.emitState()
		return
	}
	if c.OnFrame != nil {
		c.OnFrame(kind, frame, payload)
	}
}

func (c *Client) applyHello(frame map[string]any) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.accepts = map[string]bool{}
	if list, ok := frame["accepts"].([]any); ok {
		for _, value := range list {
			if text, ok := value.(string); ok {
				if kind := strings.TrimSpace(text); kind != "" {
					c.accepts[kind] = true
				}
			}
		}
	}
	c.relaysTerminal = false
	c.relaysControl = false
	if list, ok := frame["relays"].([]any); ok {
		for _, value := range list {
			text, _ := value.(string)
			switch strings.TrimSpace(text) {
			case "terminal":
				c.relaysTerminal = true
			case "control":
				c.relaysControl = true
			}
		}
	}
	budget, _ := frame["controlBudget"].(map[string]any)
	windowMs, _ := budget["windowMs"].(float64)
	maximum, _ := budget["max"].(float64)
	if int64(windowMs) > 0 && int(maximum) > 0 {
		c.budget.Configure(int64(windowMs), int(maximum))
	} else {
		c.budget.Configure(control.DefaultRelayControlWindowMs, control.DefaultRelayControlMax)
	}
}

func encodeCompact(frame map[string]any) ([]byte, bool) {
	var buffer bytes.Buffer
	encoder := json.NewEncoder(&buffer)
	encoder.SetEscapeHTML(false)
	if encoder.Encode(frame) != nil {
		return nil, false
	}
	raw := bytes.TrimRight(buffer.Bytes(), "\n")
	return raw, len(raw) <= MaxOutboundPayload
}

func (c *Client) current() net.Conn {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.phase != phaseConnected {
		return nil
	}
	return c.conn
}

func (c *Client) Accepts(kind string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.phase == phaseConnected && c.accepts[kind]
}

func (c *Client) SendSigned(frame map[string]any) bool {
	kind, _ := frame["type"].(string)
	if kind == "" || !c.Accepts(kind) {
		return false
	}
	conn := c.current()
	if conn == nil {
		return false
	}
	payload, ok := encodeCompact(frame)
	if !ok {
		return false
	}
	return c.write(conn, opText, payload) == nil
}

func (c *Client) SendTerminal(frame map[string]any) bool {
	conn := c.current()
	if conn == nil {
		return false
	}
	payload, ok := encodeCompact(frame)
	if !ok {
		return false
	}
	return c.write(conn, opText, payload) == nil
}

func (c *Client) SendOwnerControl(frame map[string]any) bool {
	payload, ok := encodeCompact(frame)
	if !ok {
		return false
	}
	c.mu.Lock()
	if c.phase != phaseConnected || c.conn == nil || !c.relaysControl {
		c.mu.Unlock()
		return false
	}
	if c.queuedBytes+len(payload) > maxQueuedControlBytes {
		c.mu.Unlock()
		return false
	}
	c.queue = append(c.queue, payload)
	c.queuedBytes += len(payload)
	c.mu.Unlock()
	select {
	case c.pump <- struct{}{}:
	default:
	}
	return true
}

func (c *Client) ControlBacklog() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.queue)
}
