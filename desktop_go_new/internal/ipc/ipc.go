package ipc

import (
	"bufio"
	"bytes"
	"context"
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	MaxLineBytes   = 64 << 20
	sendQueueDepth = 4096
	callTimeout    = 2 * time.Minute
)

type Endpoint struct {
	PID      int    `json:"pid"`
	Version  string `json:"version"`
	Project  string `json:"project"`
	Unix     string `json:"unix,omitempty"`
	TCP      string `json:"tcp,omitempty"`
	Token    string `json:"token"`
	Started  int64  `json:"startedMs"`
	Protocol int    `json:"protocol"`
}

const (
	Protocol    = 2
	ProtocolMin = 1
)

var Features = []string{"cancel", "progress", "peer-uid", "token-always"}

const CancelMethod = "core.cancel"

type AdoptParams struct {
	Peer     string `json:"peer"`
	Pass     string `json:"pass"`
	Hello    string `json:"hello"`
	Rest     string `json:"rest"`
	Platform string `json:"platform,omitempty"`
}

type Server struct {
	Registry   *rpc.Registry
	Hub        *rpc.Hub
	Version    string
	Project    string
	Logger     *log.Logger
	OnAdopt    func(stream net.Conn, params AdoptParams)
	OnUI       func(conn *Conn, attached bool)
	HelloExtra func() map[string]any

	token     string
	listeners []net.Listener
	endpoint  Endpoint
	nextID    atomic.Int64
	mu        sync.Mutex
	ui        *Conn
	conns     map[*Conn]struct{}
	wg        sync.WaitGroup
}

func NewServer(registry *rpc.Registry, hub *rpc.Hub) *Server {
	return &Server{Registry: registry, Hub: hub, conns: map[*Conn]struct{}{}, Logger: log.Default()}
}

func randomToken() string {
	raw := make([]byte, 24)
	_, _ = rand.Read(raw)
	return hex.EncodeToString(raw)
}

func EndpointPath(dir string) string { return filepath.Join(dir, "endpoint.json") }

func SocketPath(dir string) string { return filepath.Join(dir, "core.sock") }

func ReadEndpoint(dir string) (Endpoint, error) {
	var endpoint Endpoint
	raw, err := os.ReadFile(EndpointPath(dir))
	if err != nil {
		return endpoint, err
	}
	err = json.Unmarshal(raw, &endpoint)
	return endpoint, err
}

func Dial(endpoint Endpoint, timeout time.Duration) (net.Conn, error) {
	if endpoint.Unix != "" {
		if conn, err := net.DialTimeout("unix", endpoint.Unix, timeout); err == nil {
			return conn, nil
		} else if endpoint.TCP == "" {
			return nil, err
		}
	}
	if endpoint.TCP == "" {
		return nil, errors.New("ipc: endpoint has no address")
	}
	return net.DialTimeout("tcp", endpoint.TCP, timeout)
}

func (s *Server) Listen(dir string) (Endpoint, error) {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return Endpoint{}, err
	}
	_ = os.Chmod(dir, 0o700)
	s.token = randomToken()
	endpoint := Endpoint{PID: os.Getpid(), Version: s.Version, Project: s.Project, Token: s.token,
		Started: time.Now().UnixMilli(), Protocol: Protocol}
	if runtime.GOOS != "windows" {
		socket := SocketPath(dir)
		if conn, err := net.DialTimeout("unix", socket, 300*time.Millisecond); err == nil {
			conn.Close()
			return Endpoint{}, fmt.Errorf("ipc: another core is serving %s", socket)
		}
		_ = os.Remove(socket)
		if listener, err := net.Listen("unix", socket); err != nil {
			s.Logger.Printf("ipc: no unix socket at %s (%v); serving loopback TCP only", socket, err)
		} else {
			_ = os.Chmod(socket, 0o600)
			s.listeners = append(s.listeners, listener)
			endpoint.Unix = socket
		}
	}
	tcp, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		if len(s.listeners) == 0 {
			return Endpoint{}, fmt.Errorf("ipc: no listener: %w", err)
		}
	} else {
		s.listeners = append(s.listeners, tcp)
		endpoint.TCP = tcp.Addr().String()
	}
	raw, _ := json.MarshalIndent(endpoint, "", "  ")
	temp := EndpointPath(dir) + ".tmp"
	if err := os.WriteFile(temp, raw, 0o600); err != nil {
		return Endpoint{}, err
	}
	if err := os.Rename(temp, EndpointPath(dir)); err != nil {
		return Endpoint{}, err
	}
	s.endpoint = endpoint
	return endpoint, nil
}

func (s *Server) Endpoint() Endpoint { return s.endpoint }

func (s *Server) Serve(ctx context.Context) {
	for _, listener := range s.listeners {
		listener := listener
		s.wg.Add(1)
		go func() {
			defer s.wg.Done()
			for {
				conn, err := listener.Accept()
				if err != nil {
					if ctx.Err() != nil {
						return
					}
					if errors.Is(err, net.ErrClosed) {
						return
					}
					time.Sleep(50 * time.Millisecond)
					continue
				}
				_, isTCP := conn.(*net.TCPConn)
				go s.serveConn(ctx, conn, isTCP)
			}
		}()
	}
	<-ctx.Done()
	for _, listener := range s.listeners {
		listener.Close()
	}
	s.mu.Lock()
	for conn := range s.conns {
		conn.Close()
	}
	s.mu.Unlock()
	s.wg.Wait()
}

func (s *Server) UI() *Conn {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.ui
}

func (s *Server) Connections() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.conns)
}

func readFirstLine(conn net.Conn) ([]byte, []byte, *os.File, error) {
	unix, isUnix := conn.(*net.UnixConn)
	var buffer []byte
	var passed *os.File
	chunk := make([]byte, 64<<10)
	oob := make([]byte, 256)
	_ = conn.SetReadDeadline(time.Now().Add(15 * time.Second))
	defer conn.SetReadDeadline(time.Time{})
	for {
		if index := bytes.IndexByte(buffer, '\n'); index >= 0 {
			return buffer[:index], buffer[index+1:], passed, nil
		}
		if len(buffer) > MaxLineBytes {
			return nil, nil, passed, errors.New("ipc: first line too long")
		}
		var n, oobn int
		var err error
		if isUnix {
			n, oobn, _, _, err = unix.ReadMsgUnix(chunk, oob)
			if oobn > 0 && passed == nil {
				passed = receivedFile(oob[:oobn])
			}
		} else {
			n, err = conn.Read(chunk)
		}
		buffer = append(buffer, chunk[:n]...)
		if err != nil {
			if index := bytes.IndexByte(buffer, '\n'); index >= 0 {
				return buffer[:index], buffer[index+1:], passed, nil
			}
			return nil, nil, passed, err
		}
	}
}

func (s *Server) serveConn(ctx context.Context, raw net.Conn, isTCP bool) {
	first, rest, passed, err := readFirstLine(raw)
	if err != nil {
		if passed != nil {
			passed.Close()
		}
		raw.Close()
		return
	}
	var hello rpc.Message
	if err := json.Unmarshal(first, &hello); err != nil {
		raw.Close()
		return
	}
	var auth struct {
		Token    string `json:"token"`
		Role     string `json:"role"`
		Name     string `json:"name"`
		PID      int    `json:"pid"`
		Protocol int    `json:"protocol"`
	}
	_ = json.Unmarshal(hello.Params, &auth)
	if !isTCP && !sameUserPeer(raw) {
		writeLine(raw, rpc.Response(hello.ID, nil, rpc.Fail("forbidden")))
		raw.Close()
		return
	}
	if subtle.ConstantTimeCompare([]byte(auth.Token), []byte(s.token)) != 1 {
		writeLine(raw, rpc.Response(hello.ID, nil, rpc.Fail("bad_token")))
		raw.Close()
		return
	}
	if auth.Protocol != 0 && auth.Protocol < ProtocolMin {
		writeLine(raw, rpc.Response(hello.ID, map[string]any{"protocol": Protocol, "protocolMin": ProtocolMin},
			rpc.Fail("protocol_unsupported")))
		raw.Close()
		return
	}
	switch hello.Method {
	case "control.adopt":
		s.adopt(raw, hello, rest, passed)
		return
	case "core.hello":
	default:
		raw.Close()
		return
	}
	if passed != nil {
		passed.Close()
	}
	role := rpc.CLI
	if auth.Role == "ui" {
		role = rpc.UI
	}
	conn := newConn(s, raw, role, auth.Name, auth.PID)
	conn.protocol = auth.Protocol
	s.mu.Lock()
	s.conns[conn] = struct{}{}
	previous := s.ui
	if role == rpc.UI {
		s.ui = conn
	}
	s.mu.Unlock()
	s.Hub.Add(conn)
	conn.start()
	reply := map[string]any{
		"version": s.Version, "project": s.Project, "pid": os.Getpid(), "protocol": Protocol,
		"protocolMin": ProtocolMin, "features": Features, "routes": s.Registry.Routes(),
	}
	if s.HelloExtra != nil {
		for key, value := range s.HelloExtra() {
			if _, taken := reply[key]; !taken {
				reply[key] = value
			}
		}
	}
	conn.SendRaw(rpc.Response(hello.ID, reply, nil))
	if role == rpc.UI && s.OnUI != nil {
		if previous != nil && previous != conn {
			s.OnUI(previous, false)
		}
		s.OnUI(conn, true)
	}
	conn.readLoop(ctx, rest)
	s.Hub.Remove(conn)
	s.mu.Lock()
	delete(s.conns, conn)
	wasUI := s.ui == conn
	if wasUI {
		s.ui = nil
	}
	s.mu.Unlock()
	if wasUI && s.OnUI != nil {
		s.OnUI(conn, false)
	}
}

func (s *Server) adopt(raw net.Conn, hello rpc.Message, rest []byte, passed *os.File) {
	var params AdoptParams
	_ = json.Unmarshal(hello.Params, &params)
	if s.OnAdopt == nil {
		if passed != nil {
			passed.Close()
		}
		raw.Close()
		return
	}
	if params.Pass == "fd" {
		if passed == nil {
			writeLine(raw, rpc.Response(hello.ID, nil, rpc.Fail("no_descriptor")))
			raw.Close()
			return
		}
		stream, err := net.FileConn(passed)
		passed.Close()
		if err != nil {
			writeLine(raw, rpc.Response(hello.ID, nil, rpc.Fail("bad_descriptor")))
			raw.Close()
			return
		}
		writeLine(raw, rpc.Response(hello.ID, map[string]any{"adopted": true}, nil))
		raw.Close()
		s.OnAdopt(stream, params)
		return
	}
	if passed != nil {
		passed.Close()
	}
	writeLine(raw, rpc.Response(hello.ID, map[string]any{"adopted": true, "pipe": true}, nil))
	s.OnAdopt(&prefixedConn{Conn: raw, prefix: rest}, params)
}

type prefixedConn struct {
	net.Conn
	prefix []byte
}

func (c *prefixedConn) Read(p []byte) (int, error) {
	if len(c.prefix) > 0 {
		n := copy(p, c.prefix)
		c.prefix = c.prefix[n:]
		return n, nil
	}
	return c.Conn.Read(p)
}

func writeLine(conn net.Conn, value any) {
	raw, err := json.Marshal(value)
	if err != nil {
		return
	}
	_ = conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	_, _ = conn.Write(append(raw, '\n'))
	_ = conn.SetWriteDeadline(time.Time{})
}

type pending struct {
	reply chan rpc.Message
}

type inflightCall struct {
	cancel context.CancelFunc
}

type Conn struct {
	server    *Server
	raw       net.Conn
	transport rpc.Transport
	name      string
	pid       int
	id        string
	out       chan []byte
	done      chan struct{}
	closeOnce sync.Once
	nextCall  atomic.Int64
	mu        sync.Mutex
	pending   map[int64]*pending
	cancels   map[int64]*inflightCall
	inflight  sync.WaitGroup
	protocol  int
}

func newConn(server *Server, raw net.Conn, transport rpc.Transport, name string, pid int) *Conn {
	id := server.nextID.Add(1)
	return &Conn{server: server, raw: raw, transport: transport, name: name, pid: pid,
		id: string(transport) + ":" + strconv.FormatInt(id, 10), out: make(chan []byte, sendQueueDepth),
		done: make(chan struct{}), pending: map[int64]*pending{}, cancels: map[int64]*inflightCall{}}
}

func (c *Conn) ID() string               { return c.id }
func (c *Conn) Transport() rpc.Transport { return c.transport }
func (c *Conn) Device() rpc.Device       { return rpc.Device{Name: c.name, Platform: "desktop"} }
func (c *Conn) Done() <-chan struct{}    { return c.done }
func (c *Conn) PID() int                 { return c.pid }
func (c *Conn) PeerPID() (int, bool)     { return peerPID(c.raw) }
func (c *Conn) Protocol() int            { return c.protocol }
func (c *Conn) Send(event string, data any) bool {
	return c.SendRaw(rpc.EventFrame(event, data))
}

func (c *Conn) SendRaw(value any) bool {
	raw, err := json.Marshal(value)
	if err != nil {
		return false
	}
	select {
	case <-c.done:
		return false
	default:
	}
	select {
	case c.out <- append(raw, '\n'):
		return true
	case <-c.done:
		return false
	case <-time.After(5 * time.Second):
		c.Close()
		return false
	}
}

func (c *Conn) Close() {
	c.closeOnce.Do(func() {
		close(c.done)
		c.raw.Close()
		c.mu.Lock()
		for id, call := range c.pending {
			close(call.reply)
			delete(c.pending, id)
		}
		c.mu.Unlock()
	})
}

func (c *Conn) start() {
	go func() {
		writer := bufio.NewWriterSize(c.raw, 256<<10)
		for {
			select {
			case line := <-c.out:
				if _, err := writer.Write(line); err != nil {
					c.Close()
					return
				}
				for drained := false; !drained; {
					select {
					case more := <-c.out:
						if _, err := writer.Write(more); err != nil {
							c.Close()
							return
						}
					default:
						drained = true
					}
				}
				if err := writer.Flush(); err != nil {
					c.Close()
					return
				}
			case <-c.done:
				return
			}
		}
	}()
}

func (c *Conn) Call(ctx context.Context, method string, params any) (json.RawMessage, error) {
	id := c.nextCall.Add(1)
	call := &pending{reply: make(chan rpc.Message, 1)}
	c.mu.Lock()
	c.pending[id] = call
	c.mu.Unlock()
	encoded, err := json.Marshal(params)
	if err != nil {
		return nil, err
	}
	if !c.SendRaw(map[string]any{"id": id, "method": method, "params": json.RawMessage(encoded)}) {
		c.forget(id)
		return nil, rpc.Fail("ui_gone")
	}
	timer := time.NewTimer(callTimeout)
	defer timer.Stop()
	select {
	case reply, ok := <-call.reply:
		if !ok {
			return nil, rpc.Fail("ui_gone")
		}
		if reply.OK != nil && !*reply.OK {
			return nil, rpc.Fail(reply.Error)
		}
		return reply.Result, nil
	case <-ctx.Done():
		c.forget(id)
		return nil, ctx.Err()
	case <-timer.C:
		c.forget(id)
		return nil, rpc.Fail("ui_timeout")
	}
}

func (c *Conn) Notify(method string, params any) bool {
	return c.SendRaw(map[string]any{"method": method, "params": params})
}

func (c *Conn) forget(id int64) {
	c.mu.Lock()
	delete(c.pending, id)
	c.mu.Unlock()
}

func (c *Conn) readLoop(ctx context.Context, rest []byte) {
	defer c.Close()
	reader := bufio.NewReaderSize(io.MultiReader(bytes.NewReader(rest), c.raw), 256<<10)
	for {
		line, err := readLine(reader)
		if len(line) > 0 {
			c.handleLine(ctx, line)
		}
		if err != nil {
			c.inflight.Wait()
			return
		}
	}
}

func readLine(reader *bufio.Reader) ([]byte, error) {
	var line []byte
	for {
		chunk, isPrefix, err := reader.ReadLine()
		line = append(line, chunk...)
		if len(line) > MaxLineBytes {
			return nil, errors.New("ipc: line too long")
		}
		if err != nil {
			return line, err
		}
		if !isPrefix {
			return line, nil
		}
	}
}

func (c *Conn) handleLine(ctx context.Context, line []byte) {
	var message rpc.Message
	if err := json.Unmarshal(line, &message); err != nil {
		return
	}
	switch {
	case message.IsResponse() && message.Method == "":
		c.mu.Lock()
		call := c.pending[message.ID]
		delete(c.pending, message.ID)
		c.mu.Unlock()
		if call != nil {
			call.reply <- message
		}
	case message.Method == CancelMethod:
		var params struct {
			ID int64 `json:"id"`
		}
		_ = json.Unmarshal(message.Params, &params)
		cancelled := c.cancel(params.ID)
		if message.ID > 0 {
			c.SendRaw(rpc.Response(message.ID, map[string]any{"id": params.ID, "cancelled": cancelled}, nil))
		}
	case message.Method != "":
		request := &rpc.Request{Peer: c, ID: message.ID, Method: message.Method, Params: message.Params}
		if request.ID <= 0 {
			_, _ = c.server.Registry.Dispatch(ctx, request)
			return
		}
		requestCtx, cancel := context.WithCancel(ctx)
		call := &inflightCall{cancel: cancel}
		c.mu.Lock()
		if previous := c.cancels[request.ID]; previous != nil {
			previous.cancel()
		}
		c.cancels[request.ID] = call
		c.mu.Unlock()
		c.inflight.Add(1)
		go func() {
			defer c.inflight.Done()
			defer c.finish(request.ID, call)
			defer func() {
				if recovered := recover(); recovered != nil {
					c.server.Logger.Printf("ipc: %s panicked: %v", request.Method, recovered)
					if request.ID > 0 {
						c.SendRaw(rpc.Response(request.ID, nil, rpc.Fail("internal")))
					}
				}
			}()
			result, err := c.server.Registry.Dispatch(requestCtx, request)
			if err != nil && errors.Is(requestCtx.Err(), context.Canceled) && ctx.Err() == nil {
				err = rpc.Fail("cancelled")
			}
			c.SendRaw(rpc.Response(request.ID, result, err))
		}()
	}
}

func (c *Conn) cancel(id int64) bool {
	c.mu.Lock()
	call := c.cancels[id]
	c.mu.Unlock()
	if call == nil {
		return false
	}
	call.cancel()
	return true
}

func (c *Conn) finish(id int64, call *inflightCall) {
	call.cancel()
	c.mu.Lock()
	if c.cancels[id] == call {
		delete(c.cancels, id)
	}
	c.mu.Unlock()
}

func (c *Conn) Inflight() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.cancels)
}
