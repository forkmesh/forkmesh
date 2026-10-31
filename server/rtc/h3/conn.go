package h3

import (
	"context"
	"errors"
	"io"
	"strconv"
	"strings"
	"sync"

	"forkmesh.com/mirror-node/rtc/quic"
)

const (
	defaultMaxFieldSection = 16 << 10
	maxControlFrame        = 16 << 10
	maxIgnoredFrame        = 64 << 10
)

type Config struct {
	Settings            Settings
	MaxFieldSectionSize int
	Handler             func(*Request)
	StreamHandler       func(uni bool, sessionID uint64, s *quic.Stream)
	DatagramHandler     func(streamID uint64, payload []byte)
}

type Conn struct {
	qc       *quic.Conn
	isServer bool
	cfg      Config
	local    Settings

	mu          sync.Mutex
	peer        Settings
	peerReady   chan struct{}
	peerControl bool
	peerEncoder bool
	peerDecoder bool
	goAway      bool
	control     *quic.Stream
}

func NewConn(qc *quic.Conn, isServer bool, cfg *Config) *Conn {
	c := &Conn{qc: qc, isServer: isServer, peerReady: make(chan struct{})}
	if cfg != nil {
		c.cfg = *cfg
	}
	if c.cfg.MaxFieldSectionSize <= 0 {
		c.cfg.MaxFieldSectionSize = defaultMaxFieldSection
	}
	c.local = Settings{SettingMaxFieldSectionSize: uint64(c.cfg.MaxFieldSectionSize)}
	for k, v := range c.cfg.Settings {
		c.local[k] = v
	}
	return c
}

func (c *Conn) QUIC() *quic.Conn { return c.qc }

func (c *Conn) LocalSettings() Settings { return c.local }

func (c *Conn) fail(code ErrorCode, reason string) {
	c.qc.CloseWithError(uint64(code), reason)
}

func (c *Conn) Serve(ctx context.Context) error {
	if err := c.openControl(); err != nil {
		return err
	}
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	go func() {
		select {
		case <-c.qc.Done():
		case <-ctx.Done():
		}
		cancel()
	}()
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		c.acceptUni(ctx)
	}()
	go func() {
		defer wg.Done()
		c.readDatagrams(ctx)
	}()
	if c.isServer {
		for {
			s, err := c.qc.AcceptStream(ctx)
			if err != nil {
				break
			}
			go c.handleRequestStream(s)
		}
	} else {
		for {
			s, err := c.qc.AcceptStream(ctx)
			if err != nil {
				break
			}
			go c.handleServerBidi(s)
		}
	}
	cancel()
	wg.Wait()
	return c.qc.Err()
}

func (c *Conn) openControl() error {
	s, err := c.qc.OpenUniStream()
	if err != nil {
		return err
	}
	b := quic.AppendVarint(nil, StreamControl)
	b = appendFrame(b, FrameSettings, c.local.encode())
	if _, err := s.Write(b); err != nil {
		return err
	}
	c.mu.Lock()
	c.control = s
	c.mu.Unlock()
	return nil
}

func (c *Conn) PeerSettings(ctx context.Context) (Settings, error) {
	select {
	case <-c.peerReady:
		c.mu.Lock()
		defer c.mu.Unlock()
		return c.peer, nil
	case <-c.qc.Done():
		return nil, c.qc.Err()
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func (c *Conn) acceptUni(ctx context.Context) {
	for {
		s, err := c.qc.AcceptUniStream(ctx)
		if err != nil {
			return
		}
		go c.handleUni(s)
	}
}

func (c *Conn) handleUni(s *quic.Stream) {
	typ, err := ReadVarint(s)
	if err != nil {
		s.CancelRead(uint64(ErrStreamCreation))
		return
	}
	switch typ {
	case StreamControl:
		c.mu.Lock()
		dup := c.peerControl
		c.peerControl = true
		c.mu.Unlock()
		if dup {
			c.fail(ErrStreamCreation, "second control stream")
			return
		}
		c.readControl(s)
	case StreamQPACKEncoder, StreamQPACKDecoder:
		c.mu.Lock()
		flag := &c.peerEncoder
		if typ == StreamQPACKDecoder {
			flag = &c.peerDecoder
		}
		dup := *flag
		*flag = true
		c.mu.Unlock()
		if dup {
			c.fail(ErrStreamCreation, "second qpack stream")
			return
		}
		c.drainQPACK(s, typ == StreamQPACKEncoder)
	case StreamWebTransport:
		sid, err := ReadVarint(s)
		if err != nil || c.cfg.StreamHandler == nil {
			s.CancelRead(uint64(ErrStreamCreation))
			return
		}
		c.cfg.StreamHandler(true, sid, s)
	case StreamPush:
		if c.isServer {
			c.fail(ErrStreamCreation, "push stream from client")
		} else {
			c.fail(ErrID, "push without MAX_PUSH_ID")
		}
	default:
		s.CancelRead(uint64(ErrStreamCreation))
	}
}

func (c *Conn) drainQPACK(s *quic.Stream, encoder bool) {
	buf := make([]byte, 256)
	for {
		n, err := s.Read(buf)
		if encoder {
			for _, b := range buf[:n] {
				if b&0xe0 != 0x20 || b&0x1f != 0 {
					c.fail(ErrQPACKDecompression+1, "dynamic table instruction with zero capacity")
					return
				}
			}
		}
		if err != nil {
			select {
			case <-c.qc.Done():
			default:
				c.fail(ErrClosedCriticalStream, "qpack stream closed")
			}
			return
		}
	}
}

func (c *Conn) readControl(s *quic.Stream) {
	typ, length, err := readFrameHeader(s)
	if err != nil {
		c.controlClosed()
		return
	}
	if typ != FrameSettings {
		c.fail(ErrMissingSettings, "first control frame is not SETTINGS")
		return
	}
	payload, err := readPayload(s, length, maxControlFrame)
	if err != nil {
		c.fail(ErrFrame, "bad SETTINGS frame")
		return
	}
	settings, err := parseSettings(payload)
	if err != nil {
		var he *Error
		if errors.As(err, &he) {
			c.fail(he.Code, he.Reason)
		} else {
			c.fail(ErrSettings, err.Error())
		}
		return
	}
	for _, id := range []uint64{SettingEnableConnectProtocol, SettingH3Datagram} {
		if v, ok := settings[id]; ok && v > 1 {
			c.fail(ErrSettings, "boolean setting above 1")
			return
		}
	}
	c.mu.Lock()
	c.peer = settings
	close(c.peerReady)
	c.mu.Unlock()
	for {
		typ, length, err := readFrameHeader(s)
		if err != nil {
			c.controlClosed()
			return
		}
		switch typ {
		case FrameSettings, FrameData, FrameHeaders, FramePushPromise:
			c.fail(ErrFrameUnexpected, "unexpected frame on control stream")
			return
		case FrameGoAway:
			if _, err := readPayload(s, length, 16); err != nil {
				c.fail(ErrFrame, "bad GOAWAY")
				return
			}
			c.mu.Lock()
			c.goAway = true
			c.mu.Unlock()
		case FrameMaxPushID:
			if !c.isServer {
				c.fail(ErrFrameUnexpected, "MAX_PUSH_ID from server")
				return
			}
			if _, err := readPayload(s, length, 16); err != nil {
				c.fail(ErrFrame, "bad MAX_PUSH_ID")
				return
			}
		default:
			if length > maxIgnoredFrame {
				c.fail(ErrExcessiveLoad, "oversized unknown frame")
				return
			}
			if err := discard(s, length); err != nil {
				c.controlClosed()
				return
			}
		}
	}
}

func (c *Conn) controlClosed() {
	select {
	case <-c.qc.Done():
	default:
		c.fail(ErrClosedCriticalStream, "control stream closed")
	}
}

func (c *Conn) readDatagrams(ctx context.Context) {
	for {
		d, err := c.qc.ReceiveDatagram(ctx)
		if err != nil {
			return
		}
		qid, n := quic.ReadVarint(d)
		if n == 0 || qid > 1<<60-1 {
			c.fail(ErrDatagram, "malformed datagram")
			return
		}
		if c.cfg.DatagramHandler != nil {
			c.cfg.DatagramHandler(qid*4, d[n:])
		}
	}
}

func (c *Conn) SendDatagram(streamID uint64, payload []byte) error {
	b := make([]byte, 0, 8+len(payload))
	b = quic.AppendVarint(b, streamID/4)
	b = append(b, payload...)
	return c.qc.SendDatagram(b)
}

func (c *Conn) MaxDatagramPayload(streamID uint64) int {
	n := c.qc.MaxDatagramSize() - quic.VarintLen(streamID/4)
	if n < 0 {
		return 0
	}
	return n
}

type Request struct {
	Method    string
	Scheme    string
	Authority string
	Path      string
	Protocol  string
	Header    []Field
	Stream    *quic.Stream
	Conn      *Conn
	body      *dataReader
	responded bool
}

func (r *Request) Get(name string) string {
	for _, f := range r.Header {
		if f.Name == name {
			return f.Value
		}
	}
	return ""
}

func (r *Request) Respond(status int, header []Field) error {
	if r.responded {
		return errors.New("h3: response already sent")
	}
	r.responded = true
	fields := append([]Field{{":status", strconv.Itoa(status)}}, header...)
	block := AppendFieldSection(nil, fields)
	_, err := r.Stream.Write(appendFrame(nil, FrameHeaders, block))
	return err
}

func (r *Request) WriteData(b []byte) error {
	_, err := r.Stream.Write(appendFrame(nil, FrameData, b))
	return err
}

func (r *Request) Body() io.Reader {
	if r.body == nil {
		r.body = &dataReader{s: r.Stream, conn: r.Conn}
	}
	return r.body
}

type dataReader struct {
	s      *quic.Stream
	conn   *Conn
	remain uint64
	done   bool
}

func (d *dataReader) Read(p []byte) (int, error) {
	for d.remain == 0 {
		if d.done {
			return 0, io.EOF
		}
		typ, length, err := readFrameHeader(d.s)
		if err != nil {
			return 0, err
		}
		switch typ {
		case FrameData:
			d.remain = length
		case FrameHeaders:
			if err := discard(d.s, length); err != nil {
				return 0, err
			}
			d.done = true
		case FrameSettings, FrameGoAway, FrameMaxPushID, FrameCancelPush:
			d.conn.fail(ErrFrameUnexpected, "control frame on request stream")
			return 0, &Error{ErrFrameUnexpected, "control frame on request stream"}
		default:
			if length > maxIgnoredFrame {
				d.conn.fail(ErrExcessiveLoad, "oversized unknown frame")
				return 0, errFrameTooLarge
			}
			if err := discard(d.s, length); err != nil {
				return 0, err
			}
		}
	}
	if uint64(len(p)) > d.remain {
		p = p[:d.remain]
	}
	n, err := d.s.Read(p)
	d.remain -= uint64(n)
	if err == io.EOF && d.remain > 0 {
		err = io.ErrUnexpectedEOF
	}
	return n, err
}

func (c *Conn) handleRequestStream(s *quic.Stream) {
	for {
		typ, err := ReadVarint(s)
		if err != nil {
			s.CancelRead(uint64(ErrRequestIncomplete))
			s.CancelWrite(uint64(ErrRequestIncomplete))
			return
		}
		if typ == SignalWebTransport {
			sid, err := ReadVarint(s)
			if err != nil || c.cfg.StreamHandler == nil {
				s.CancelRead(uint64(ErrStreamCreation))
				s.CancelWrite(uint64(ErrStreamCreation))
				return
			}
			c.cfg.StreamHandler(false, sid, s)
			return
		}
		length, err := ReadVarint(s)
		if err != nil {
			s.CancelRead(uint64(ErrRequestIncomplete))
			s.CancelWrite(uint64(ErrRequestIncomplete))
			return
		}
		switch {
		case typ == FrameHeaders:
			c.serveRequest(s, length)
			return
		case typ == FrameData:
			c.fail(ErrFrameUnexpected, "DATA before HEADERS")
			return
		case typ == FrameSettings || typ == FrameGoAway || typ == FrameMaxPushID || typ == FrameCancelPush || typ == FramePushPromise:
			c.fail(ErrFrameUnexpected, "control frame on request stream")
			return
		default:
			if length > maxIgnoredFrame {
				c.fail(ErrExcessiveLoad, "oversized unknown frame")
				return
			}
			if err := discard(s, length); err != nil {
				return
			}
		}
	}
}

func (c *Conn) rejectStream(s *quic.Stream, code ErrorCode) {
	s.CancelRead(uint64(code))
	s.CancelWrite(uint64(code))
}

func (c *Conn) serveRequest(s *quic.Stream, length uint64) {
	if length > uint64(c.cfg.MaxFieldSectionSize) {
		c.rejectStream(s, ErrExcessiveLoad)
		return
	}
	payload, err := readPayload(s, length, c.cfg.MaxFieldSectionSize)
	if err != nil {
		c.rejectStream(s, ErrRequestIncomplete)
		return
	}
	fields, err := DecodeFieldSection(payload, c.cfg.MaxFieldSectionSize)
	if err == errHeaderTooLarge {
		c.rejectStream(s, ErrExcessiveLoad)
		return
	}
	if err != nil {
		c.fail(ErrQPACKDecompression, "bad field section")
		return
	}
	req, ok := parseRequest(fields)
	if !ok {
		c.rejectStream(s, ErrMessage)
		return
	}
	if req.Protocol != "" && c.local[SettingEnableConnectProtocol] != 1 {
		c.rejectStream(s, ErrMessage)
		return
	}
	req.Stream = s
	req.Conn = c
	if c.cfg.Handler == nil {
		req.Respond(404, nil)
		s.Close()
		return
	}
	c.cfg.Handler(req)
}

var connectionHeaders = map[string]bool{
	"connection": true, "keep-alive": true, "proxy-connection": true, "transfer-encoding": true, "upgrade": true,
}

func validFieldName(n string) bool {
	if n == "" {
		return false
	}
	for i := 0; i < len(n); i++ {
		ch := n[i]
		if ch >= 'A' && ch <= 'Z' {
			return false
		}
		if ch <= 0x20 || ch >= 0x7f || strings.IndexByte("\"(),/:;<=>?@[\\]{}", ch) >= 0 {
			return false
		}
	}
	return true
}

func validFieldValue(v string) bool {
	for i := 0; i < len(v); i++ {
		switch v[i] {
		case 0, '\r', '\n':
			return false
		}
	}
	return true
}

func parseRequest(fields []Field) (*Request, bool) {
	r := &Request{}
	seen := map[string]bool{}
	regular := false
	for _, f := range fields {
		if !validFieldValue(f.Value) {
			return nil, false
		}
		if strings.HasPrefix(f.Name, ":") {
			if regular || seen[f.Name] {
				return nil, false
			}
			seen[f.Name] = true
			switch f.Name {
			case ":method":
				r.Method = f.Value
			case ":scheme":
				r.Scheme = f.Value
			case ":authority":
				r.Authority = f.Value
			case ":path":
				r.Path = f.Value
			case ":protocol":
				r.Protocol = f.Value
			default:
				return nil, false
			}
			continue
		}
		regular = true
		if !validFieldName(f.Name) || connectionHeaders[f.Name] {
			return nil, false
		}
		if f.Name == "te" && f.Value != "trailers" {
			return nil, false
		}
		r.Header = append(r.Header, f)
	}
	if r.Method == "" {
		return nil, false
	}
	if r.Method == "CONNECT" && r.Protocol == "" {
		return r, r.Authority != "" && !seen[":scheme"] && !seen[":path"]
	}
	if r.Protocol != "" && r.Method != "CONNECT" {
		return nil, false
	}
	if r.Scheme == "" || r.Path == "" {
		return nil, false
	}
	if r.Protocol != "" && r.Authority == "" {
		return nil, false
	}
	return r, true
}

type Response struct {
	Status int
	Header []Field
	Stream *quic.Stream
	conn   *Conn
	body   *dataReader
}

func (r *Response) Get(name string) string {
	for _, f := range r.Header {
		if f.Name == name {
			return f.Value
		}
	}
	return ""
}

func (r *Response) Body() io.Reader {
	if r.body == nil {
		r.body = &dataReader{s: r.Stream, conn: r.conn}
	}
	return r.body
}

func (c *Conn) RoundTrip(ctx context.Context, fields []Field) (*Response, error) {
	if c.isServer {
		return nil, errors.New("h3: RoundTrip on a server connection")
	}
	s, err := c.qc.OpenStreamSync(ctx)
	if err != nil {
		return nil, err
	}
	block := AppendFieldSection(nil, fields)
	if _, err := s.Write(appendFrame(nil, FrameHeaders, block)); err != nil {
		return nil, err
	}
	for {
		typ, length, err := readFrameHeader(s)
		if err != nil {
			return nil, err
		}
		if typ != FrameHeaders {
			if typ == FrameData {
				c.fail(ErrFrameUnexpected, "DATA before HEADERS")
				return nil, &Error{ErrFrameUnexpected, "DATA before HEADERS"}
			}
			if err := discard(s, length); err != nil {
				return nil, err
			}
			continue
		}
		payload, err := readPayload(s, length, c.cfg.MaxFieldSectionSize)
		if err != nil {
			return nil, err
		}
		fs, err := DecodeFieldSection(payload, c.cfg.MaxFieldSectionSize)
		if err != nil {
			c.fail(ErrQPACKDecompression, "bad field section")
			return nil, err
		}
		resp := &Response{Stream: s, conn: c}
		for _, f := range fs {
			if f.Name == ":status" {
				resp.Status, _ = strconv.Atoi(f.Value)
				continue
			}
			resp.Header = append(resp.Header, f)
		}
		if resp.Status >= 100 && resp.Status < 200 {
			continue
		}
		if resp.Status < 200 || resp.Status > 599 {
			return nil, &Error{ErrMessage, "bad status"}
		}
		return resp, nil
	}
}

func (c *Conn) handleServerBidi(s *quic.Stream) {
	typ, err := ReadVarint(s)
	if err != nil || typ != SignalWebTransport || c.cfg.StreamHandler == nil {
		c.fail(ErrStreamCreation, "server-initiated bidirectional stream")
		return
	}
	sid, err := ReadVarint(s)
	if err != nil {
		c.rejectStream(s, ErrStreamCreation)
		return
	}
	c.cfg.StreamHandler(false, sid, s)
}
