package quic

import (
	"context"
	"io"
	"time"
)

type Stream struct {
	conn *Conn
	id   uint64

	canSend bool
	canRecv bool

	send         sendBuf
	sendMax      uint64
	sendClosed   bool
	resetLocal   bool
	resetDue     bool
	resetAcked   bool
	resetCode    uint64
	resetSize    uint64
	stopReceived bool
	stopCode     uint64
	blockedDue   bool
	writeNotify  notifier
	writeDL      time.Time

	recv          recvBuf
	recvMax       uint64
	recvWindow    uint64
	finalKnown    bool
	finalSize     uint64
	recvReset     bool
	recvResetCode uint64
	readCancelled bool
	stopDue       bool
	stopSendCode  uint64
	maxDataDue    bool
	eofRead       bool
	readNotify    notifier
	readDL        time.Time

	inSendQueue    bool
	inControlQueue bool
	removed        bool
}

func (c *Conn) isLocalStream(id uint64) bool {
	serverInitiated := id&1 == 1
	return serverInitiated != c.isClient
}

func (c *Conn) newStream(id uint64) *Stream {
	s := &Stream{conn: c, id: id}
	local := c.isLocalStream(id)
	uni := id&2 != 0
	s.canSend = !uni || local
	s.canRecv = !uni || !local
	if s.canRecv {
		s.recvWindow = c.config.StreamReceiveWindow
		s.recvMax = s.recvWindow
	}
	c.applyInitialStreamLimits(s)
	c.streams[id] = s
	return s
}

func (c *Conn) applyInitialStreamLimits(s *Stream) {
	if !s.canSend || !c.havePeerParams {
		return
	}
	var limit uint64
	switch {
	case s.id&2 != 0:
		limit = c.peerParams.initialMaxStreamDataUni
	case c.isLocalStream(s.id):
		limit = c.peerParams.initialMaxStreamDataBR
	default:
		limit = c.peerParams.initialMaxStreamDataBL
	}
	if limit > s.sendMax {
		s.sendMax = limit
	}
}

func (c *Conn) streamFor(id uint64, typ uint64, receiving bool) (*Stream, *TransportError) {
	local := c.isLocalStream(id)
	uni := id&2 != 0
	if uni && local == receiving {
		return nil, frameErr(StreamStateError, typ, "frame on a stream in the wrong direction")
	}
	idx := id >> 2
	if local {
		next := c.nextBidi
		if uni {
			next = c.nextUni
		}
		if idx >= next {
			return nil, frameErr(StreamStateError, typ, "frame for a stream not yet opened")
		}
		return c.streams[id], nil
	}
	max, opened := c.maxPeerBidi, &c.openedPeerBidi
	if uni {
		max, opened = c.maxPeerUni, &c.openedPeerUni
	}
	if idx >= max {
		return nil, frameErr(StreamLimitError, typ, "stream limit exceeded")
	}
	for *opened <= idx {
		nid := *opened<<2 | id&3
		*opened++
		s := c.newStream(nid)
		if uni {
			c.acceptUni = append(c.acceptUni, s)
		} else {
			c.acceptBidi = append(c.acceptBidi, s)
		}
		c.acceptNotify.broadcast()
	}
	return c.streams[id], nil
}

func (c *Conn) checkStreamFrameTarget(id uint64, typ uint64, receiving bool) *TransportError {
	_, err := c.streamFor(id, typ, receiving)
	return err
}

func (c *Conn) addRecvData(s *Stream, end uint64, typ uint64) *TransportError {
	if end <= s.recv.highest {
		return nil
	}
	if end > s.recvMax {
		return frameErr(FlowControlError, typ, "stream flow control exceeded")
	}
	delta := end - s.recv.highest
	c.recvData += delta
	if c.recvData > c.recvMaxData {
		return frameErr(FlowControlError, typ, "connection flow control exceeded")
	}
	if s.readCancelled || s.recvReset {
		s.recv.highest = end
		c.recvConsumed += delta
		c.updateConnCredit()
	}
	return nil
}

func (c *Conn) onStreamFrame(f *frame) *TransportError {
	s, err := c.streamFor(f.streamID, f.typ, true)
	if err != nil || s == nil {
		return err
	}
	end := f.offset + uint64(len(f.data))
	if s.finalKnown && (end > s.finalSize || (f.fin && end != s.finalSize)) {
		return frameErr(FinalSizeError, f.typ, "data beyond final size")
	}
	if f.fin && end < s.recv.highest {
		return frameErr(FinalSizeError, f.typ, "final size below received data")
	}
	if err := c.addRecvData(s, end, f.typ); err != nil {
		return err
	}
	if f.fin && !s.finalKnown {
		s.finalKnown = true
		s.finalSize = end
		s.maxDataDue = false
	}
	if s.readCancelled || s.recvReset {
		c.maybeRemoveStream(s)
		return nil
	}
	if !s.recv.write(f.offset, f.data) {
		return frameErr(ProtocolViolation, f.typ, "stream data too fragmented")
	}
	s.readNotify.broadcast()
	return nil
}

func (c *Conn) onResetStream(f *frame) *TransportError {
	s, err := c.streamFor(f.streamID, f.typ, true)
	if err != nil || s == nil {
		return err
	}
	if s.finalKnown && f.finalSize != s.finalSize {
		return frameErr(FinalSizeError, f.typ, "final size changed")
	}
	if f.finalSize < s.recv.highest {
		return frameErr(FinalSizeError, f.typ, "final size below received data")
	}
	if s.recvReset {
		return nil
	}
	if err := c.addRecvData(s, f.finalSize, f.typ); err != nil {
		return err
	}
	s.recvReset = true
	s.recvResetCode = f.code
	s.finalKnown = true
	s.finalSize = f.finalSize
	s.maxDataDue = false
	s.stopDue = false
	if !s.readCancelled {
		unread := f.finalSize - s.recv.base
		c.recvConsumed += unread
		c.updateConnCredit()
	}
	s.recv = recvBuf{base: f.finalSize, highest: f.finalSize}
	s.readNotify.broadcast()
	c.maybeRemoveStream(s)
	return nil
}

func (c *Conn) onStopSending(f *frame) *TransportError {
	s, err := c.streamFor(f.streamID, f.typ, false)
	if err != nil || s == nil {
		return err
	}
	if s.stopReceived {
		return nil
	}
	s.stopReceived = true
	s.stopCode = f.code
	if !s.resetLocal && !s.send.allAcked() {
		c.resetSend(s, f.code)
	}
	s.writeNotify.broadcast()
	return nil
}

func (c *Conn) onMaxStreamData(f *frame) *TransportError {
	s, err := c.streamFor(f.streamID, f.typ, false)
	if err != nil || s == nil {
		return err
	}
	if f.max > s.sendMax {
		s.sendMax = f.max
		c.queueSend(s)
		s.writeNotify.broadcast()
	}
	return nil
}

func (c *Conn) onMaxStreams(uni bool, max uint64) {
	if uni {
		if max > c.peerMaxUni {
			c.peerMaxUni = max
		}
	} else if max > c.peerMaxBidi {
		c.peerMaxBidi = max
	}
	c.openNotify.broadcast()
}

func (c *Conn) wakeSenders() {
	for _, s := range c.streams {
		if s.canSend && s.send.pendingNew(s.sendMax) > 0 {
			c.queueSend(s)
		}
	}
}

func (c *Conn) queueSend(s *Stream) {
	if s.inSendQueue || s.removed {
		return
	}
	s.inSendQueue = true
	c.sendQueue = append(c.sendQueue, s)
}

func (c *Conn) queueControl(s *Stream) {
	if s.inControlQueue || s.removed {
		return
	}
	s.inControlQueue = true
	c.controlQueue = append(c.controlQueue, s)
}

func (c *Conn) updateConnCredit() {
	w := c.config.ConnectionReceiveWindow
	if c.recvMaxData-c.recvConsumed < w/2 {
		c.recvMaxData = c.recvConsumed + w
		c.maxDataDue = true
	}
}

func (c *Conn) onStreamRead(s *Stream, n int) {
	c.recvConsumed += uint64(n)
	c.updateConnCredit()
	if !s.finalKnown && s.recvMax-s.recv.base < s.recvWindow/2 {
		s.recvMax = s.recv.base + s.recvWindow
		s.maxDataDue = true
		c.queueControl(s)
	}
	c.wake()
}

func (c *Conn) resetSend(s *Stream, code uint64) {
	if s.resetLocal || !s.canSend {
		return
	}
	s.resetLocal = true
	s.resetDue = true
	s.resetCode = code
	s.resetSize = s.send.sent
	s.send.data = nil
	s.send.lost = nil
	s.send.acked = nil
	s.blockedDue = false
	c.queueControl(s)
	c.wake()
}

func (c *Conn) maxStreamsDue(uni bool) {
	if uni {
		c.maxStreamsPending[1] = true
	} else {
		c.maxStreamsPending[0] = true
	}
}

func (c *Conn) maybeRemoveStream(s *Stream) {
	if s.removed {
		return
	}
	sendDone := !s.canSend || (s.resetLocal && s.resetAcked) || (!s.resetLocal && s.send.allAcked())
	recvDone := !s.canRecv || s.eofRead || s.recvReset || (s.readCancelled && s.finalKnown)
	if !sendDone || !recvDone {
		return
	}
	s.removed = true
	delete(c.streams, s.id)
	if !c.isLocalStream(s.id) {
		if s.id&2 != 0 {
			c.maxPeerUni++
			c.maxStreamsDue(true)
		} else {
			c.maxPeerBidi++
			c.maxStreamsDue(false)
		}
	}
}

func (s *Stream) StreamID() uint64 { return s.id }

func (s *Stream) Conn() *Conn { return s.conn }

func waitUntil(ch <-chan struct{}, deadline time.Time, ctxDone <-chan struct{}) bool {
	var timer *time.Timer
	var tc <-chan time.Time
	if !deadline.IsZero() {
		d := time.Until(deadline)
		if d <= 0 {
			return false
		}
		timer = time.NewTimer(d)
		tc = timer.C
		defer timer.Stop()
	}
	select {
	case <-ch:
		return true
	case <-tc:
		return false
	case <-ctxDone:
		return false
	}
}

func (s *Stream) Read(p []byte) (int, error) {
	c := s.conn
	c.mu.Lock()
	defer c.mu.Unlock()
	if !s.canRecv {
		return 0, ErrNotSupported
	}
	for {
		if s.readCancelled {
			return 0, &StreamError{StreamID: s.id, Code: s.stopSendCode}
		}
		if s.recvReset {
			return 0, &StreamError{StreamID: s.id, Code: s.recvResetCode, Remote: true}
		}
		if len(p) == 0 {
			return 0, nil
		}
		if n := s.recv.read(p); n > 0 {
			c.onStreamRead(s, n)
			return n, nil
		}
		if s.finalKnown && s.recv.base >= s.finalSize {
			s.eofRead = true
			c.maybeRemoveStream(s)
			return 0, io.EOF
		}
		if c.closeErr != nil {
			return 0, c.closeErr
		}
		if !s.readDL.IsZero() && !time.Now().Before(s.readDL) {
			return 0, ErrDeadlineExceeded
		}
		ch := s.readNotify.wait()
		dl := s.readDL
		c.mu.Unlock()
		waitUntil(ch, dl, nil)
		c.mu.Lock()
	}
}

func (s *Stream) Write(p []byte) (int, error) {
	c := s.conn
	c.mu.Lock()
	defer c.mu.Unlock()
	if !s.canSend {
		return 0, ErrNotSupported
	}
	total := 0
	for len(p) > 0 {
		if s.stopReceived {
			return total, &StreamError{StreamID: s.id, Code: s.stopCode, Remote: true}
		}
		if s.resetLocal {
			return total, &StreamError{StreamID: s.id, Code: s.resetCode}
		}
		if s.sendClosed {
			return total, ErrWriteClosed
		}
		if c.closeErr != nil {
			return total, c.closeErr
		}
		room := c.config.StreamSendBuffer - s.send.buffered()
		if room > 0 {
			n := room
			if n > len(p) {
				n = len(p)
			}
			s.send.write(p[:n])
			p = p[n:]
			total += n
			c.queueSend(s)
			c.wake()
			continue
		}
		if !s.writeDL.IsZero() && !time.Now().Before(s.writeDL) {
			return total, ErrDeadlineExceeded
		}
		ch := s.writeNotify.wait()
		dl := s.writeDL
		c.mu.Unlock()
		waitUntil(ch, dl, nil)
		c.mu.Lock()
	}
	return total, nil
}

func (s *Stream) Close() error {
	c := s.conn
	c.mu.Lock()
	defer c.mu.Unlock()
	if !s.canSend {
		s.cancelReadLocked(0)
		return nil
	}
	if s.sendClosed || s.resetLocal {
		return nil
	}
	s.sendClosed = true
	s.send.fin = true
	c.queueSend(s)
	c.wake()
	return nil
}

func (s *Stream) CancelWrite(code uint64) {
	c := s.conn
	c.mu.Lock()
	defer c.mu.Unlock()
	if !s.canSend || s.send.allAcked() {
		return
	}
	c.resetSend(s, code)
	s.writeNotify.broadcast()
}

func (s *Stream) CancelRead(code uint64) {
	c := s.conn
	c.mu.Lock()
	defer c.mu.Unlock()
	s.cancelReadLocked(code)
}

func (s *Stream) cancelReadLocked(code uint64) {
	c := s.conn
	if !s.canRecv || s.readCancelled || s.eofRead {
		return
	}
	s.readCancelled = true
	s.stopSendCode = code
	unread := s.recv.highest - s.recv.base
	c.recvConsumed += unread
	c.updateConnCredit()
	s.recv = recvBuf{base: s.recv.highest, highest: s.recv.highest}
	if !s.finalKnown && !s.recvReset {
		s.stopDue = true
		c.queueControl(s)
	}
	s.maxDataDue = false
	s.readNotify.broadcast()
	c.maybeRemoveStream(s)
	c.wake()
}

func (s *Stream) SetReadDeadline(t time.Time) error {
	c := s.conn
	c.mu.Lock()
	s.readDL = t
	s.readNotify.broadcast()
	c.mu.Unlock()
	return nil
}

func (s *Stream) SetWriteDeadline(t time.Time) error {
	c := s.conn
	c.mu.Lock()
	s.writeDL = t
	s.writeNotify.broadcast()
	c.mu.Unlock()
	return nil
}

func (s *Stream) SetDeadline(t time.Time) error {
	s.SetReadDeadline(t)
	return s.SetWriteDeadline(t)
}

func (c *Conn) OpenStream() (*Stream, error) {
	return c.openStream(false)
}

func (c *Conn) OpenUniStream() (*Stream, error) {
	return c.openStream(true)
}

func (c *Conn) openStream(uni bool) (*Stream, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.openStreamLocked(uni)
}

func (c *Conn) openStreamLocked(uni bool) (*Stream, error) {
	if c.closeErr != nil {
		return nil, c.closeErr
	}
	next, max, kind := &c.nextBidi, c.peerMaxBidi, 0
	if uni {
		next, max, kind = &c.nextUni, c.peerMaxUni, 1
	}
	if *next >= max {
		if c.streamsBlocked[kind] != max || !c.blockedSent[kind] {
			c.streamsBlocked[kind] = max
			c.blockedDue[kind] = true
			c.blockedSent[kind] = true
			c.wake()
		}
		return nil, ErrStreamLimit
	}
	id := *next << 2
	if !c.isClient {
		id |= 1
	}
	if uni {
		id |= 2
	}
	*next++
	return c.newStream(id), nil
}

func (c *Conn) OpenStreamSync(ctx context.Context) (*Stream, error) {
	return c.openStreamSync(ctx, false)
}

func (c *Conn) OpenUniStreamSync(ctx context.Context) (*Stream, error) {
	return c.openStreamSync(ctx, true)
}

func (c *Conn) openStreamSync(ctx context.Context, uni bool) (*Stream, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	for {
		s, err := c.openStreamLocked(uni)
		if err != ErrStreamLimit {
			return s, err
		}
		ch := c.openNotify.wait()
		c.mu.Unlock()
		select {
		case <-ch:
		case <-ctx.Done():
			c.mu.Lock()
			return nil, ctx.Err()
		}
		c.mu.Lock()
	}
}

func (c *Conn) AcceptStream(ctx context.Context) (*Stream, error) {
	return c.accept(ctx, false)
}

func (c *Conn) AcceptUniStream(ctx context.Context) (*Stream, error) {
	return c.accept(ctx, true)
}

func (c *Conn) accept(ctx context.Context, uni bool) (*Stream, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	for {
		q := &c.acceptBidi
		if uni {
			q = &c.acceptUni
		}
		if len(*q) > 0 {
			s := (*q)[0]
			(*q)[0] = nil
			*q = (*q)[1:]
			return s, nil
		}
		if c.closeErr != nil {
			return nil, c.closeErr
		}
		ch := c.acceptNotify.wait()
		c.mu.Unlock()
		select {
		case <-ch:
		case <-ctx.Done():
			c.mu.Lock()
			return nil, ctx.Err()
		}
		c.mu.Lock()
	}
}

func (c *Conn) SendDatagram(b []byte) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closeErr != nil {
		return c.closeErr
	}
	if c.peerMaxDatagram == 0 {
		return ErrDatagramsDisabled
	}
	if len(b) > c.maxDatagramPayloadLocked() {
		return ErrDatagramTooLarge
	}
	if len(c.dgramOut) >= c.config.MaxPendingDatagrams {
		c.stats.DatagramsDropped++
		return ErrDatagramQueueFull
	}
	c.dgramOut = append(c.dgramOut, append([]byte(nil), b...))
	c.wake()
	return nil
}

func (c *Conn) MaxDatagramSize() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.maxDatagramPayloadLocked()
}

func (c *Conn) maxDatagramPayloadLocked() int {
	if c.peerMaxDatagram == 0 {
		return 0
	}
	room := c.maxSendSize - (1 + maxCIDLen + 4 + aeadTagLen) - 3
	if lim := int(c.peerMaxDatagram) - 3; c.peerMaxDatagram < 1<<20 && lim < room {
		room = lim
	}
	if room < 0 {
		return 0
	}
	return room
}

func (c *Conn) ReceiveDatagram(ctx context.Context) ([]byte, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	for {
		if len(c.dgramIn) > 0 {
			d := c.dgramIn[0]
			c.dgramIn[0] = nil
			c.dgramIn = c.dgramIn[1:]
			return d, nil
		}
		if c.closeErr != nil {
			return nil, c.closeErr
		}
		ch := c.dgramNotify.wait()
		c.mu.Unlock()
		select {
		case <-ch:
		case <-ctx.Done():
			c.mu.Lock()
			return nil, ctx.Err()
		}
		c.mu.Lock()
	}
}
