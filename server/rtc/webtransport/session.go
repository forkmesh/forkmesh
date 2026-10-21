package webtransport

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"sync"

	"forkmesh.com/mirror-node/rtc/h3"
	"forkmesh.com/mirror-node/rtc/quic"
)

const (
	capsuleCloseSession = 0x2843
	capsuleDrainSession = 0x78ae
	maxCloseMessage     = 1024
	maxCapsule          = 64 << 10

	codeSessionGone      = 0x170d7b68
	codeBufferedRejected = 0x3994bd84
	firstAppCode         = 0x52e4a40fa8db
	lastAppCode          = 0x52e5ac983162
)

var (
	ErrSessionClosed = errors.New("webtransport: session closed")
	ErrQueueFull     = errors.New("webtransport: queue full")
)

func HTTP3ErrorCode(code uint32) uint64 {
	return firstAppCode + uint64(code) + uint64(code)/0x1e
}

func AppErrorCode(h uint64) (uint32, bool) {
	if h < firstAppCode || h > lastAppCode || (h-0x21)%0x1f == 0 {
		return 0, false
	}
	shifted := h - firstAppCode
	return uint32(shifted - shifted/0x1f), true
}

type SessionError struct {
	Code    uint32
	Message string
	Remote  bool
}

func (e *SessionError) Error() string {
	if e.Remote {
		return "webtransport: session closed by peer: " + e.Message
	}
	return "webtransport: session closed: " + e.Message
}

type Session struct {
	id       uint64
	conn     *h3.Conn
	connect  *quic.Stream
	body     io.Reader
	Request  *h3.Request
	Response *h3.Response

	limits limits

	mu        sync.Mutex
	streams   map[*quic.Stream]struct{}
	bidi      []*quic.Stream
	uni       []*quic.Stream
	dgrams    [][]byte
	notify    chan struct{}
	closed    chan struct{}
	closeErr  error
	closeOnce sync.Once
	ctx       context.Context
	cancel    context.CancelFunc
	onClose   func()
}

type limits struct {
	pendingStreams   int
	pendingDatagrams int
}

func newSession(id uint64, conn *h3.Conn, connect *quic.Stream, body io.Reader, l limits) *Session {
	s := &Session{
		id:      id,
		conn:    conn,
		connect: connect,
		body:    body,
		limits:  l,
		streams: make(map[*quic.Stream]struct{}),
		notify:  make(chan struct{}),
		closed:  make(chan struct{}),
	}
	s.ctx, s.cancel = context.WithCancel(context.Background())
	return s
}

func (s *Session) ID() uint64 { return s.id }

func (s *Session) Context() context.Context { return s.ctx }

func (s *Session) Conn() *quic.Conn { return s.conn.QUIC() }

func (s *Session) broadcast() {
	close(s.notify)
	s.notify = make(chan struct{})
}

func (s *Session) addStream(uni bool, st *quic.Stream) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closeErr != nil {
		return false
	}
	q := &s.bidi
	if uni {
		q = &s.uni
	}
	if len(*q) >= s.limits.pendingStreams {
		return false
	}
	*q = append(*q, st)
	s.streams[st] = struct{}{}
	s.broadcast()
	return true
}

func (s *Session) addDatagram(b []byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closeErr != nil || len(s.dgrams) >= s.limits.pendingDatagrams {
		return
	}
	s.dgrams = append(s.dgrams, append([]byte(nil), b...))
	s.broadcast()
}

func (s *Session) wait(ctx context.Context, take func() bool) error {
	for {
		s.mu.Lock()
		if take() {
			s.mu.Unlock()
			return nil
		}
		if s.closeErr != nil {
			err := s.closeErr
			s.mu.Unlock()
			return err
		}
		ch := s.notify
		s.mu.Unlock()
		select {
		case <-ch:
		case <-ctx.Done():
			return ctx.Err()
		}
	}
}

func (s *Session) AcceptStream(ctx context.Context) (*quic.Stream, error) {
	var out *quic.Stream
	err := s.wait(ctx, func() bool {
		if len(s.bidi) == 0 {
			return false
		}
		out = s.bidi[0]
		s.bidi = s.bidi[1:]
		return true
	})
	return out, err
}

func (s *Session) AcceptUniStream(ctx context.Context) (*quic.Stream, error) {
	var out *quic.Stream
	err := s.wait(ctx, func() bool {
		if len(s.uni) == 0 {
			return false
		}
		out = s.uni[0]
		s.uni = s.uni[1:]
		return true
	})
	return out, err
}

func (s *Session) ReceiveDatagram(ctx context.Context) ([]byte, error) {
	var out []byte
	err := s.wait(ctx, func() bool {
		if len(s.dgrams) == 0 {
			return false
		}
		out = s.dgrams[0]
		s.dgrams = s.dgrams[1:]
		return true
	})
	return out, err
}

func (s *Session) SendDatagram(b []byte) error {
	s.mu.Lock()
	err := s.closeErr
	s.mu.Unlock()
	if err != nil {
		return err
	}
	return s.conn.SendDatagram(s.id, b)
}

func (s *Session) MaxDatagramSize() int { return s.conn.MaxDatagramPayload(s.id) }

func (s *Session) open(ctx context.Context, uni bool) (*quic.Stream, error) {
	s.mu.Lock()
	err := s.closeErr
	s.mu.Unlock()
	if err != nil {
		return nil, err
	}
	qc := s.conn.QUIC()
	var st *quic.Stream
	var prefix []byte
	if uni {
		st, err = qc.OpenUniStreamSync(ctx)
		prefix = quic.AppendVarint(nil, h3.StreamWebTransport)
	} else {
		st, err = qc.OpenStreamSync(ctx)
		prefix = quic.AppendVarint(nil, h3.SignalWebTransport)
	}
	if err != nil {
		return nil, err
	}
	prefix = quic.AppendVarint(prefix, s.id)
	if _, err := st.Write(prefix); err != nil {
		return nil, err
	}
	s.mu.Lock()
	if s.closeErr != nil {
		s.mu.Unlock()
		st.CancelWrite(codeSessionGone)
		return nil, s.closeErr
	}
	s.streams[st] = struct{}{}
	s.mu.Unlock()
	return st, nil
}

func (s *Session) OpenStream(ctx context.Context) (*quic.Stream, error) { return s.open(ctx, false) }

func (s *Session) OpenUniStream(ctx context.Context) (*quic.Stream, error) {
	return s.open(ctx, true)
}

func (s *Session) Done() <-chan struct{} { return s.closed }

func (s *Session) Err() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.closeErr
}

func (s *Session) CloseWithError(code uint32, msg string) error {
	if len(msg) > maxCloseMessage {
		msg = msg[:maxCloseMessage]
	}
	value := binary.BigEndian.AppendUint32(nil, code)
	value = append(value, msg...)
	capsule := quic.AppendVarint(nil, capsuleCloseSession)
	capsule = quic.AppendVarint(capsule, uint64(len(value)))
	capsule = append(capsule, value...)
	frame := quic.AppendVarint(nil, h3.FrameData)
	frame = quic.AppendVarint(frame, uint64(len(capsule)))
	frame = append(frame, capsule...)
	s.connect.Write(frame)
	s.connect.Close()
	s.terminate(&SessionError{Code: code, Message: msg})
	return nil
}

func (s *Session) Close() error { return s.CloseWithError(0, "") }

func (s *Session) terminate(err error) {
	s.closeOnce.Do(func() {
		s.mu.Lock()
		s.closeErr = err
		streams := s.streams
		s.streams = nil
		s.bidi, s.uni, s.dgrams = nil, nil, nil
		s.broadcast()
		s.mu.Unlock()
		for st := range streams {
			st.CancelRead(codeSessionGone)
			st.CancelWrite(codeSessionGone)
		}
		close(s.closed)
		s.cancel()
		if s.onClose != nil {
			s.onClose()
		}
	})
}

func (s *Session) readCapsules() {
	for {
		typ, err := h3.ReadVarint(s.body)
		if err != nil {
			s.connect.CancelWrite(uint64(h3.ErrNoError))
			s.terminate(&SessionError{Message: "connect stream ended", Remote: true})
			return
		}
		length, err := h3.ReadVarint(s.body)
		if err != nil || length > maxCapsule {
			s.connect.CancelRead(uint64(h3.ErrMessage))
			s.connect.CancelWrite(uint64(h3.ErrMessage))
			s.terminate(&SessionError{Message: "malformed capsule", Remote: true})
			return
		}
		value := make([]byte, length)
		if _, err := io.ReadFull(s.body, value); err != nil {
			s.terminate(&SessionError{Message: "truncated capsule", Remote: true})
			return
		}
		switch typ {
		case capsuleCloseSession:
			if len(value) < 4 || len(value) > 4+maxCloseMessage {
				s.terminate(&SessionError{Message: "malformed close capsule", Remote: true})
				return
			}
			s.connect.Close()
			s.terminate(&SessionError{Code: binary.BigEndian.Uint32(value), Message: string(value[4:]), Remote: true})
			return
		case capsuleDrainSession:
		}
	}
}
