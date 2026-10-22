package fmmmobile

import (
	"context"
	"crypto/ed25519"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"runtime"
	"sync"
	"sync/atomic"
	"time"

	"forkmesh.com/mirror-node/rtc/e2ee"
	"forkmesh.com/mirror-node/rtc/fmm"
	"forkmesh.com/mirror-node/rtc/fmmcall"
	"forkmesh.com/mirror-node/rtc/quic"
)

const (
	pumpEvery      = 10 * time.Millisecond
	targetEvery    = 100 * time.Millisecond
	targetChange   = 0.05
	targetQuiet    = time.Second
	statusEvery    = 250 * time.Millisecond
	closedGrace    = 2 * time.Second
	audioCache     = 16
	maxEvents      = 512
	maxRefPicture  = 16_000
	pathTimeout    = 5 * time.Second
	defaultConnect = 10 * time.Second
)

type pubSpec struct {
	logical   int
	kind      int
	name      string
	layers    int
	encodings int
	face      bool
	encs      []int
	faceID    int
}

type openedFrame struct {
	counter uint64
	keyID   byte
	plain   []byte
}

type remoteInfo struct {
	kind  int
	peer  string
	name  string
	cache [audioCache]openedFrame
	next  int
}

type pathInfo struct {
	path  *quic.Path
	index int
}

type Call struct {
	cfg    Config
	access fmmcall.Access
	l      Listener
	family string
	keys   *keys
	tx     e2ee.FrameSender
	rx     e2ee.FrameReceiver

	mu         sync.Mutex
	ep         *quic.Endpoint
	call       *fmmcall.Call
	sess       *fmmcall.Session
	toSess     map[int]int
	toLogical  map[int]int
	specs      []*pubSpec
	authStream int
	remotes    map[int]*remoteInfo
	authBy     map[int]string
	paths      map[int64]pathInfo
	wantPaths  []pathInfo
	connected  bool
	closed     bool
	moved      bool
	joined     bool
	joinCode   int
	joinText   string
	joinedCh   chan struct{}
	doneSince  time.Time
	closedSent bool
	stop       chan struct{}
	done       chan struct{}

	qmu    sync.Mutex
	events []*Event

	lastTarget   int
	targetAt     time.Time
	targetCheck  time.Time
	authAt       time.Time
	statusAt     time.Time
	movedSeen    bool
	nextLogical  int
	sealed       atomic.Int64
	opened       atomic.Int64
	refused      atomic.Int64
	authSent     atomic.Int64
	authAccepted atomic.Int64
}

func NewCall(cfg *Config, l Listener) (*Call, error) {
	if cfg == nil || cfg.Access == "" || cfg.Call == "" || cfg.Peer == "" || cfg.Account == "" || len(cfg.Identity) != ed25519.SeedSize {
		return nil, ErrConfig
	}
	var access fmmcall.Access
	if err := json.Unmarshal([]byte(cfg.Access), &access); err != nil {
		return nil, fmt.Errorf("fmmmobile: access block: %w", err)
	}
	if access.Addr == "" || access.Ticket == "" || len(access.Hashes) == 0 || (access.ALPN != "" && access.ALPN != fmmcall.ALPN) {
		return nil, ErrConfig
	}
	if access.Standby != nil && (access.Standby.Addr == "" || access.Standby.Ticket == "") {
		access.Standby = nil
	}
	c := &Call{
		cfg: *cfg, access: access, l: l,
		toSess: map[int]int{}, toLogical: map[int]int{}, remotes: map[int]*remoteInfo{}, authBy: map[int]string{},
		paths: map[int64]pathInfo{}, joinedCh: make(chan struct{}), stop: make(chan struct{}), done: make(chan struct{}),
	}
	c.cfg.Identity = append([]byte(nil), cfg.Identity...)
	k, err := newKeys(c, e2ee.Config{
		Call: cfg.Call, Peer: cfg.Peer, Acct: cfg.Account, Identity: ed25519.NewKeyFromSeed(cfg.Identity),
	})
	if err != nil {
		return nil, fmt.Errorf("fmmmobile: e2ee session: %w", err)
	}
	c.keys = k
	c.rx.OnAuth = func(peer string, st e2ee.AuthState) {
		c.emit(&Event{Kind: EventAuth, Peer: peer, State: int(st)})
	}
	return c, nil
}

func (c *Call) resolve(addr string) (netip.AddrPort, error) {
	if ap, err := netip.ParseAddrPort(addr); err == nil {
		return ap, nil
	}
	network := "udp"
	if c.family != "" {
		network = c.family
	}
	ua, err := net.ResolveUDPAddr(network, addr)
	if err != nil {
		return netip.AddrPort{}, err
	}
	ap := ua.AddrPort()
	return netip.AddrPortFrom(ap.Addr().Unmap(), ap.Port()), nil
}

func (c *Call) listen() (net.PacketConn, error) {
	if c.cfg.LocalAddr != "" {
		ua, err := net.ResolveUDPAddr("udp", c.cfg.LocalAddr)
		if err != nil {
			return nil, err
		}
		c.family = "udp6"
		if ua.IP.To4() != nil {
			c.family = "udp4"
		}
		return net.ListenUDP(c.family, ua)
	}
	ap, err := c.resolve(c.access.Addr)
	if err != nil {
		return nil, err
	}
	c.family = "udp6"
	if ap.Addr().Is4() {
		c.family = "udp4"
	}
	return net.ListenUDP(c.family, nil)
}

func (c *Call) Connect(timeoutMs int) error {
	c.mu.Lock()
	if c.connected || c.closed {
		c.mu.Unlock()
		return ErrDuplicate
	}
	c.connected = true
	c.mu.Unlock()
	timeout := defaultConnect
	if timeoutMs > 0 {
		timeout = time.Duration(timeoutMs) * time.Millisecond
	}
	sock, err := c.listen()
	if err != nil {
		return err
	}
	ep, err := quic.NewEndpoint(sock, &quic.Config{}, false)
	if err != nil {
		sock.Close()
		return err
	}
	c.mu.Lock()
	c.ep = ep
	c.mu.Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	cc := &fmm.Config{StartBitrate: c.cfg.StartBitrate, MinBitrate: c.cfg.MinBitrate, MaxBitrate: c.cfg.MaxBitrate}
	call, err := fmmcall.Start(ctx, ep, c.access, fmmcall.Options{Client: cc, Resolve: c.resolve, OnSession: c.onSession})
	if err != nil {
		ep.Close()
		return err
	}
	c.mu.Lock()
	c.call = call
	c.mu.Unlock()
	go c.run()
	select {
	case <-c.joinedCh:
	case <-ctx.Done():
		c.Leave()
		return errors.New("fmmmobile: no answer from the call server")
	}
	c.mu.Lock()
	code, text := c.joinCode, c.joinText
	c.mu.Unlock()
	if code != 0 {
		c.Leave()
		return fmt.Errorf("fmmmobile: %d %s", code, text)
	}
	c.keys.start(time.Now())
	return nil
}

func (c *Call) JoinError() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.joinCode
}

func (c *Call) WaitStandby(timeoutMs int) error {
	c.mu.Lock()
	call := c.call
	c.mu.Unlock()
	if call == nil {
		return ErrNotLive
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(max(timeoutMs, 1))*time.Millisecond)
	defer cancel()
	return call.StandbyReady(ctx)
}

func (c *Call) HasStandby() bool { return c.access.Standby != nil }

func (c *Call) onSession(s *fmmcall.Session, moved bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.sess = s
	c.doneSince = time.Time{}
	now := s.Now()
	if id, err := s.Client.PublishAuth("auth", now); err == nil {
		c.authStream = id
	}
	if !moved {
		return
	}
	if !c.moved {
		c.nextLogical = 255
	}
	c.moved = true
	for id, r := range c.remotes {
		c.emit(&Event{Kind: EventTrackRemoved, Stream: id, Peer: r.peer, Name: r.name})
	}
	c.remotes = map[int]*remoteInfo{}
	c.authBy = map[int]string{}
	lost := c.paths
	c.paths = map[int64]pathInfo{}
	c.toSess, c.toLogical = map[int]int{}, map[int]int{}
	for _, sp := range c.specs {
		c.republishLocked(s, sp, now)
	}
	for _, p := range lost {
		c.emit(&Event{Kind: EventPath, Code: 0, Text: "path closed by the move", FrameID: int64(p.path.ID())})
	}
	if len(c.wantPaths) > 0 {
		want := append([]pathInfo(nil), c.wantPaths...)
		go func() {
			for _, p := range want {
				if p.index > 0 {
					_, _ = c.AddPathInterface(p.index, false)
				}
			}
		}()
	}
}

func (c *Call) mapLocked(logical, id int) {
	c.toSess[logical] = id
	c.toLogical[id] = logical
}

func (c *Call) republishLocked(s *fmmcall.Session, sp *pubSpec, now int64) {
	var id int
	var err error
	if sp.kind == KindAudio {
		id, err = s.Client.PublishAudio(sp.name, now)
	} else {
		id, err = s.Client.PublishVideoTrack(sp.name, sp.layers, sp.encodings, sp.face, now)
	}
	if err != nil {
		c.emit(&Event{Kind: EventError, Text: "republish: " + err.Error()})
		return
	}
	c.mapLocked(sp.logical, id)
	for i, e := range sp.encs {
		if n := s.Client.EncodingStream(id, i); n != 0 {
			c.mapLocked(e, n)
		}
	}
	if sp.faceID != 0 {
		if n := s.Client.FaceStream(id); n != 0 {
			c.mapLocked(sp.faceID, n)
		}
	}
}

func (c *Call) session() (*fmmcall.Session, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.sess == nil || c.closed {
		return nil, ErrNotLive
	}
	return c.sess, nil
}

func (c *Call) local(stream int) (*fmmcall.Session, int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.sess == nil || c.closed {
		return nil, 0, ErrNotLive
	}
	id, ok := c.toSess[stream]
	if !ok {
		return nil, 0, fmm.ErrUnknownTrack
	}
	return c.sess, id, nil
}

func (c *Call) PeerID() string { return c.cfg.Peer }

func (c *Call) PublishAudio(name string) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.sess == nil || c.closed {
		return 0, ErrNotLive
	}
	id, err := c.sess.Client.PublishAudio(name, c.sess.Now())
	if err != nil {
		return 0, err
	}
	logical := c.freshLogicalLocked(id)
	c.mapLocked(logical, id)
	c.specs = append(c.specs, &pubSpec{logical: logical, kind: KindAudio, name: name, layers: 1, encodings: 1})
	return logical, nil
}

func (c *Call) freshLogicalLocked(id int) int {
	if !c.moved {
		return id
	}
	c.nextLogical++
	return c.nextLogical
}

func (c *Call) PublishVideo(name string, layers, encodings int, face bool) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.sess == nil || c.closed {
		return 0, ErrNotLive
	}
	cl := c.sess.Client
	id, err := cl.PublishVideoTrack(name, layers, encodings, face, c.sess.Now())
	if err != nil {
		return 0, err
	}
	sp := &pubSpec{kind: KindVideo, name: name, layers: layers, encodings: encodings, face: face}
	sp.logical = c.freshLogicalLocked(id)
	c.mapLocked(sp.logical, id)
	for i := range max(encodings, 1) {
		n := cl.EncodingStream(id, i)
		if n == 0 {
			break
		}
		e := sp.logical
		if n != id {
			e = c.freshLogicalLocked(n)
			c.mapLocked(e, n)
		}
		sp.encs = append(sp.encs, e)
	}
	if f := cl.FaceStream(id); f != 0 {
		sp.faceID = c.freshLogicalLocked(f)
		c.mapLocked(sp.faceID, f)
	}
	c.specs = append(c.specs, sp)
	return sp.logical, nil
}

func (c *Call) EncodingStream(track, index int) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, sp := range c.specs {
		if sp.logical == track && index >= 0 && index < len(sp.encs) {
			return sp.encs[index]
		}
	}
	return 0
}

func (c *Call) FaceStream(track int) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, sp := range c.specs {
		if sp.logical == track {
			return sp.faceID
		}
	}
	return 0
}

func (c *Call) Unpublish(stream int) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.sess == nil || c.closed {
		return ErrNotLive
	}
	for i, sp := range c.specs {
		if sp.logical != stream {
			continue
		}
		id := c.toSess[stream]
		ids := append([]int{sp.logical, sp.faceID}, sp.encs...)
		for _, l := range ids {
			if n, ok := c.toSess[l]; ok {
				delete(c.toLogical, n)
				delete(c.toSess, l)
			}
		}
		c.specs = append(c.specs[:i], c.specs[i+1:]...)
		return c.sess.Client.Unpublish(id, c.sess.Now())
	}
	return fmm.ErrUnknownTrack
}

func (c *Call) SendAudio(stream int, frame []byte) error {
	s, id, err := c.local(stream)
	if err != nil {
		return err
	}
	sealed, err := c.tx.Seal(frame, 0)
	if err != nil {
		return ErrNoKey
	}
	c.sealed.Add(1)
	return s.Client.SendAudio(id, sealed, s.Now())
}

func (c *Call) SendVideo(stream int, frame []byte, flags, layer int, refFrame int64) (int64, error) {
	s, id, err := c.local(stream)
	if err != nil {
		return -1, err
	}
	face, key := flags&FlagFace != 0, flags&FlagKeyframe != 0
	if face && key && len(frame)+SealOverhead > maxRefPicture {
		return -1, fmm.ErrBadFrame
	}
	sealed, err := c.tx.Seal(frame, 0)
	if err != nil {
		return -1, ErrNoKey
	}
	c.sealed.Add(1)
	return s.Client.SendVideoFrame(id, &fmm.OutFrame{
		Data: sealed, Keyframe: key, Layer: layer, LongTerm: flags&FlagLongTerm != 0, Face: face,
		Recover: flags&FlagRecover != 0, RefFrame: refFrame,
	}, s.Now())
}

func (c *Call) SetAudioRedundancy(stream, ms int) error {
	s, id, err := c.local(stream)
	if err != nil {
		return err
	}
	return s.Client.SetAudioRedundancy(id, ms)
}

func (c *Call) AudioBitrate(stream int) int {
	s, id, err := c.local(stream)
	if err != nil {
		return 0
	}
	return s.Client.AudioBitrate(id)
}

func (c *Call) AudioRedundancyMs(stream int) int {
	s, id, err := c.local(stream)
	if err != nil {
		return 0
	}
	return s.Client.AudioRedundancyMs(id)
}

func (c *Call) AudioLoss() float64 {
	s, err := c.session()
	if err != nil {
		return 0
	}
	return s.Client.Stats().Loss
}

func (c *Call) VideoBitrate(stream int) int {
	s, id, err := c.local(stream)
	if err != nil {
		return 0
	}
	return s.Client.VideoBitrate(id)
}

func (c *Call) AckedFrame(stream int) int64 {
	s, id, err := c.local(stream)
	if err != nil {
		return -1
	}
	return s.Client.AckedFrame(id)
}

func (c *Call) SendingRepresentation(stream int) int {
	s, id, err := c.local(stream)
	if err != nil {
		return RepOff
	}
	return s.Client.SendingRepresentation(id)
}

func (c *Call) TargetBitrate() int {
	s, err := c.session()
	if err != nil {
		return 0
	}
	return s.Client.TargetBitrate()
}

func (c *Call) Subscribe(stream, maxLayer int) error {
	s, err := c.session()
	if err != nil {
		return err
	}
	return s.Client.Subscribe(stream, maxLayer, s.Now())
}

func (c *Call) Unsubscribe(stream int) error {
	s, err := c.session()
	if err != nil {
		return err
	}
	return s.Client.Unsubscribe(stream, s.Now())
}

func (c *Call) SetReceiveLimit(bitrate int) error {
	s, err := c.session()
	if err != nil {
		return err
	}
	return s.Client.SetReceiveLimit(bitrate, s.Now())
}

func (c *Call) Representation(stream int) int {
	s, err := c.session()
	if err != nil {
		return RepOff
	}
	return s.Client.Representation(stream)
}

func (c *Call) remote(stream int) (*fmmcall.Session, *remoteInfo) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.sess == nil || c.closed {
		return nil, nil
	}
	return c.sess, c.remotes[stream]
}

func (c *Call) open(r *remoteInfo, sealed []byte, cache bool) ([]byte, error) {
	t, err := e2ee.ParseFrameTrailer(sealed)
	if err != nil {
		return nil, err
	}
	if cache {
		c.mu.Lock()
		for _, f := range r.cache {
			if f.plain != nil && f.counter == t.Counter && f.keyID == t.KeyID {
				c.mu.Unlock()
				return f.plain, nil
			}
		}
		c.mu.Unlock()
	}
	plain, err := c.rx.Open(r.peer, sealed, 0)
	if err != nil {
		c.refused.Add(1)
		if errors.Is(err, e2ee.ErrUnknownPeer) {
			c.keys.unknownKey(r.peer, time.Now())
		}
		return nil, err
	}
	c.opened.Add(1)
	if cache {
		c.mu.Lock()
		r.cache[r.next] = openedFrame{counter: t.Counter, keyID: t.KeyID, plain: plain}
		r.next = (r.next + 1) % audioCache
		c.mu.Unlock()
	}
	return plain, nil
}

func (c *Call) PullAudio(stream int) *AudioOut {
	s, r := c.remote(stream)
	if s == nil || r == nil {
		return &AudioOut{Status: AudioWaiting}
	}
	f := s.Client.PullAudio(stream, s.Now())
	out := &AudioOut{Status: f.Status, Offset: f.Offset}
	if len(f.Data) == 0 {
		return out
	}
	plain, err := c.open(r, f.Data, true)
	if err != nil {
		out.Status, out.Offset = AudioConceal, 0
		if f.Status == AudioInsert {
			out.Status = AudioInsert
		}
		return out
	}
	out.Data = plain
	return out
}

func (c *Call) PollVideo(stream int) *VideoIn {
	s, r := c.remote(stream)
	if s == nil || r == nil {
		return nil
	}
	for {
		f := s.Client.PollVideo(stream, s.Now())
		if f == nil {
			return nil
		}
		plain, err := c.open(r, f.Data, false)
		if err != nil {
			continue
		}
		return &VideoIn{
			Stream: stream, FrameID: f.FrameID, Keyframe: f.Keyframe, Layer: f.Layer, Timestamp: f.Timestamp,
			Face: f.Face, Recovery: f.Recovery, LongTerm: f.LongTerm, Data: plain,
		}
	}
}

func (c *Call) RemotePeer(stream int) string {
	c.mu.Lock()
	defer c.mu.Unlock()
	if r := c.remotes[stream]; r != nil {
		return r.peer
	}
	return ""
}

func (c *Call) Auth(peer string) *AuthStatus {
	st := c.rx.Status(peer)
	return &AuthStatus{State: int(st.State), Dropping: st.Dropping, Verified: int64(st.Verified), Missing: int64(st.Missing), Forged: int64(st.Forged)}
}

func (c *Call) emit(e *Event) {
	c.qmu.Lock()
	if len(c.events) < maxEvents {
		c.events = append(c.events, e)
	}
	c.qmu.Unlock()
}

func (c *Call) PollEvent() *Event {
	if c.l != nil {
		return nil
	}
	return c.take()
}

func (c *Call) take() *Event {
	c.qmu.Lock()
	defer c.qmu.Unlock()
	if len(c.events) == 0 {
		return nil
	}
	e := c.events[0]
	c.events = c.events[1:]
	return e
}

func (c *Call) run() {
	defer close(c.done)
	t := time.NewTicker(pumpEvery)
	defer t.Stop()
	for {
		select {
		case <-c.stop:
			return
		case now := <-t.C:
			c.step(now)
			if c.l != nil {
				for e := c.take(); e != nil; e = c.take() {
					c.l.OnEvent(e)
				}
			}
		}
	}
}

func (c *Call) step(now time.Time) {
	c.mu.Lock()
	s, call := c.sess, c.call
	c.mu.Unlock()
	if s == nil || call == nil {
		return
	}
	for e := s.Client.PollEvent(); e != nil; e = s.Client.PollEvent() {
		c.translate(s, e)
	}
	c.watchSession(s, call, now)
	if now.Sub(c.authAt) >= e2ee.AuthInterval {
		c.authAt = now
		c.flushAuth(s)
	}
	c.pollAuth(s)
	if now.Sub(c.statusAt) >= statusEvery {
		c.statusAt = now
		for _, peer := range c.keys.peers() {
			c.rx.Status(peer)
		}
	}
	c.keys.tick(now)
	if now.Sub(c.targetCheck) >= targetEvery {
		c.targetCheck = now
		target := s.Client.TargetBitrate()
		diff := float64(target - c.lastTarget)
		if c.lastTarget == 0 || (diff*diff > targetChange*targetChange*float64(c.lastTarget*c.lastTarget) && now.Sub(c.targetAt) >= targetEvery) || now.Sub(c.targetAt) >= targetQuiet {
			if target != c.lastTarget {
				c.emit(&Event{Kind: EventTarget, Bitrate: target})
			}
			c.lastTarget, c.targetAt = target, now
		}
	}
}

func (c *Call) watchSession(s *fmmcall.Session, call *fmmcall.Call, now time.Time) {
	if !c.movedSeen {
		select {
		case <-call.Moved():
			c.movedSeen = true
			deadAt, movedAt, reason := call.Failover()
			c.emit(&Event{Kind: EventMoved, Text: reason.String(), FrameID: movedAt.Sub(deadAt).Milliseconds()})
		default:
		}
	}
	select {
	case <-s.Done():
	default:
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.sess != s || c.closedSent {
		return
	}
	if c.doneSince.IsZero() {
		c.doneSince = now
	}
	if c.access.Standby != nil && !c.movedSeen && now.Sub(c.doneSince) < closedGrace {
		return
	}
	c.closedSent = true
	text := "the call connection closed"
	if err := s.Conn.Err(); err != nil {
		text = err.Error()
	}
	c.emit(&Event{Kind: EventClosed, Text: text})
	if !c.joined {
		c.joinCode, c.joinText = 503, text
		c.signalJoinedLocked()
	}
}

func (c *Call) signalJoinedLocked() {
	select {
	case <-c.joinedCh:
	default:
		close(c.joinedCh)
	}
}

func (c *Call) translate(s *fmmcall.Session, e *fmm.Event) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.sess != s {
		return
	}
	logical := func(id int) (int, bool) {
		n, ok := c.toLogical[id]
		return n, ok
	}
	out := &Event{
		Kind: e.Kind, Stream: e.Stream, Track: e.Track, Layers: e.Layers, Peer: e.Peer, Name: e.Name,
		Bitrate: e.Bitrate, Code: e.Code, Text: e.Text, FrameID: e.FrameID, Rep: e.Rep, MaxBytes: e.MaxBytes,
	}
	switch e.Kind {
	case fmm.EventJoined:
		if !c.joined {
			c.joined = true
			if e.Peer != c.cfg.Peer {
				out.Kind, out.Text = EventError, "joined as "+e.Peer+", expected "+c.cfg.Peer
			}
			c.signalJoinedLocked()
		}
	case fmm.EventTrackAdded:
		if e.Track == fmm.KindAuth {
			c.authBy[e.Stream] = e.Peer
			_ = s.Client.Subscribe(e.Stream, 0, s.Now())
			return
		}
		c.remotes[e.Stream] = &remoteInfo{kind: e.Track, peer: e.Peer, name: e.Name}
	case fmm.EventTrackRemoved:
		if _, ok := c.authBy[e.Stream]; ok {
			delete(c.authBy, e.Stream)
			return
		}
		delete(c.remotes, e.Stream)
	case fmm.EventKeyframeRequest, fmm.EventBitrateHint, fmm.EventRecover, fmm.EventSendRepresentation, fmm.EventReferencePicture:
		if e.Stream == c.authStream {
			return
		}
		n, ok := logical(e.Stream)
		if !ok {
			return
		}
		out.Stream = n
		if e.Kind == fmm.EventReferencePicture {
			out.MaxBytes = max(e.MaxBytes-SealOverhead, 0)
		}
	case fmm.EventError:
		if !c.joined && e.Code != 0 {
			c.joinCode, c.joinText = e.Code, e.Text
			c.signalJoinedLocked()
		}
	}
	c.emit(out)
}

func (c *Call) flushAuth(s *fmmcall.Session) {
	c.mu.Lock()
	id := c.authStream
	c.mu.Unlock()
	for _, m := range c.tx.Flush() {
		if id != 0 && s.Client.SendAuth(id, m, s.Now()) == nil {
			c.authSent.Add(1)
		}
	}
}

func (c *Call) pollAuth(s *fmmcall.Session) {
	c.mu.Lock()
	streams := make(map[int]string, len(c.authBy))
	for id, peer := range c.authBy {
		streams[id] = peer
	}
	c.mu.Unlock()
	for id, peer := range streams {
		for m := s.Client.PollAuth(id); m != nil; m = s.Client.PollAuth(id) {
			if c.rx.AcceptAuth(peer, m) == nil {
				c.authAccepted.Add(1)
			}
		}
	}
}

func (c *Call) Stats() *Stats {
	out := &Stats{
		Sealed: int(c.sealed.Load()), Opened: int(c.opened.Load()), Refused: int(c.refused.Load()),
		AuthSent: int(c.authSent.Load()), AuthAccepted: int(c.authAccepted.Load()), Goroutines: runtime.NumGoroutine(),
	}
	c.mu.Lock()
	s, call, moved := c.sess, c.call, c.moved
	out.Paths = len(c.paths)
	c.mu.Unlock()
	if s == nil {
		return out
	}
	st := s.Client.Stats()
	out.TargetBitrate, out.VideoBitrate, out.RTTUs, out.Loss = st.TargetBitrate, st.VideoBitrate, st.RTTUs, st.Loss
	out.SentPackets, out.ReceivedPackets, out.RecoveredPackets = st.SentPackets, st.ReceivedPackets, st.RecoveredPackets
	out.AudioPlayed, out.AudioConcealed, out.AudioFEC, out.AudioDRED, out.AudioInserted = st.AudioPlayed, st.AudioConcealed, st.AudioFEC, st.AudioDRED, st.AudioInserted
	out.VideoReleased, out.VideoUndecodable, out.KeyframeRequests = st.VideoReleased, st.VideoUndecodable, st.KeyframeRequests
	out.RecoveryRequests, out.RecoveryFrames, out.Recoveries = st.RecoveryRequests, st.RecoveryFrames, st.Recoveries
	out.UsablePaths = s.Conn.Health().UsablePaths
	out.Moved = moved
	if moved && call != nil {
		deadAt, movedAt, _ := call.Failover()
		out.MovedAfterMs = movedAt.Sub(deadAt).Milliseconds()
	}
	return out
}

func (c *Call) Leave() {
	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return
	}
	c.closed = true
	call, ep, s := c.call, c.ep, c.sess
	c.signalJoinedLocked()
	c.mu.Unlock()
	if call != nil {
		close(c.stop)
		<-c.done
		if s != nil {
			c.flushAuth(s)
		}
		call.Close()
	}
	if ep != nil {
		ep.Close()
	}
	c.keys.close()
}
