package fmmmobile

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"net"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/fmmcall"
	"forkmesh.com/mirror-node/rtc/fmmserver"
	"forkmesh.com/mirror-node/rtc/quic"
	"forkmesh.com/mirror-node/rtc/signal"
)

var testSecret = []byte("fmm-mobile-test-secret-0123456789abcdef")

type core struct {
	server *fmmserver.Server
	certs  *fmmserver.CertStore
	ep     *quic.Endpoint
	cancel context.CancelFunc
	once   sync.Once
}

func startCore(t *testing.T) *core {
	t.Helper()
	certs, err := fmmserver.NewCertStore("", nil)
	if err != nil {
		t.Fatal(err)
	}
	srv, err := fmmserver.New(fmmserver.Config{Secret: testSecret, Certs: certs, AllowOrigin: func(string) bool { return true }})
	if err != nil {
		t.Fatal(err)
	}
	ep, err := quic.Listen("udp4", "127.0.0.1:0", srv.QUICConfig())
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	go srv.Serve(ctx, ep)
	c := &core{server: srv, certs: certs, ep: ep, cancel: cancel}
	t.Cleanup(c.stop)
	return c
}

func (c *core) stop() {
	c.once.Do(func() {
		c.cancel()
		c.server.Close()
		c.ep.Close()
	})
}

func (c *core) endpoint(room, peer, acct string, standby bool) *fmmcall.Endpoint {
	ticket := signal.MintTicket(testSecret, signal.Ticket{
		Room: room, Peer: peer, Acct: acct, Exp: time.Now().Add(signal.TicketLifetime).Unix(),
		Engine: signal.EngineFMM, Standby: standby,
	})
	return &fmmcall.Endpoint{Addr: c.ep.LocalAddr().String(), ALPN: fmmcall.ALPN, Hashes: c.certs.Hashes(), Ticket: ticket}
}

type chatRoom struct {
	mu     sync.Mutex
	phones []*phone
}

func (r *chatRoom) deliver(from *phone, kind string, data []byte) {
	r.mu.Lock()
	others := make([]*phone, 0, len(r.phones))
	for _, p := range r.phones {
		if p != from {
			others = append(others, p)
		}
	}
	r.mu.Unlock()
	for _, p := range others {
		go p.call.HandleChat(kind, append([]byte(nil), data...), from.kid, from.acct)
	}
}

type phone struct {
	t     *testing.T
	name  string
	idx   byte
	peer  string
	acct  string
	kid   string
	room  *chatRoom
	call  *Call
	video bool

	mu       sync.Mutex
	audioOut int
	videoOut int
	needKey  bool
	audioIn  map[int]string
	videoIn  map[int]string
	plays    map[string][]time.Time
	frames   map[string]int
	bad      int
	seq      uint32
	vseq     uint32
	tick     int
	moved    time.Time
	auth     map[string]int
	verify   int
	errors   []string
	stop     chan struct{}
	done     chan struct{}
}

func newPhone(t *testing.T, room *chatRoom, name string, idx byte, primary, standby *core, roomID string, video bool) *phone {
	t.Helper()
	p := &phone{
		t: t, name: name, idx: idx, acct: name, room: room, video: video,
		peer:    hex.EncodeToString(bytes.Repeat([]byte{idx}, 16)),
		audioIn: map[int]string{}, videoIn: map[int]string{}, plays: map[string][]time.Time{}, frames: map[string]int{},
		auth: map[string]int{}, stop: make(chan struct{}), done: make(chan struct{}),
	}
	ticketAcct := hex.EncodeToString(bytes.Repeat([]byte{idx ^ 0x5a}, 16))
	access := fmmcall.Access{Endpoint: *primary.endpoint(roomID, p.peer, ticketAcct, false)}
	if standby != nil {
		access.Standby = standby.endpoint(roomID, p.peer, ticketAcct, true)
	}
	raw, _ := json.Marshal(access)
	seed := make([]byte, 32)
	rand.Read(seed)
	call, err := NewCall(&Config{
		Access: string(raw), Call: "office-test-room", Peer: p.peer, Account: p.acct, Identity: seed,
		StartBitrate: 600_000, MaxBitrate: 1_500_000, LocalAddr: "127.0.0.1:0",
	}, p)
	if err != nil {
		t.Fatal(err)
	}
	p.call = call
	p.kid = call.IdentityKeyID()
	room.mu.Lock()
	room.phones = append(room.phones, p)
	room.mu.Unlock()
	return p
}

func (p *phone) OnEvent(e *Event) {
	switch e.Kind {
	case EventChat:
		p.room.deliver(p, e.Name, e.Data)
		return
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	switch e.Kind {
	case EventTrackAdded:
		if e.Track == KindAudio {
			p.audioIn[e.Stream] = e.Peer
			_ = p.call.Subscribe(e.Stream, 0)
		} else {
			p.videoIn[e.Stream] = e.Peer
			_ = p.call.Subscribe(e.Stream, max(e.Layers-1, 0))
		}
	case EventTrackRemoved:
		delete(p.audioIn, e.Stream)
		delete(p.videoIn, e.Stream)
	case EventKeyframeRequest, EventSendRepresentation:
		p.needKey = true
	case EventAuth:
		p.auth[e.Peer] = e.State
	case EventVerify:
		p.verify++
	case EventMoved:
		p.moved = time.Now()
	case EventError:
		p.errors = append(p.errors, e.Text)
	}
}

func (p *phone) start(t *testing.T) {
	t.Helper()
	if err := p.call.Connect(5000); err != nil {
		t.Fatalf("%s connect: %v", p.name, err)
	}
	audio, err := p.call.PublishAudio("mic")
	if err != nil {
		t.Fatal(err)
	}
	video := 0
	if p.video {
		if video, err = p.call.PublishVideo("cam", 1, 1, false); err != nil {
			t.Fatal(err)
		}
	}
	p.mu.Lock()
	p.audioOut, p.videoOut, p.needKey = audio, video, p.video
	p.mu.Unlock()
	go p.run()
	t.Cleanup(p.close)
}

func (p *phone) close() {
	select {
	case <-p.stop:
		return
	default:
	}
	close(p.stop)
	<-p.done
	p.call.Leave()
}

func (p *phone) run() {
	defer close(p.done)
	t := time.NewTicker(20 * time.Millisecond)
	defer t.Stop()
	for {
		select {
		case <-p.stop:
			return
		case <-t.C:
		}
		p.step()
	}
}

func payload(idx byte, seq uint32, size int) []byte {
	b := make([]byte, size)
	b[0] = idx
	binary.BigEndian.PutUint32(b[1:], seq)
	for i := 5; i < size; i++ {
		b[i] = byte(i) ^ idx
	}
	return b
}

func checkPayload(b []byte) bool {
	if len(b) < 5 {
		return false
	}
	for i := 5; i < len(b); i++ {
		if b[i] != byte(i)^b[0] {
			return false
		}
	}
	return true
}

func (p *phone) step() {
	p.mu.Lock()
	audioOut, videoOut, video := p.audioOut, p.videoOut, p.video
	p.seq++
	seq := p.seq
	p.tick++
	sendVideo := video && p.tick%3 == 0
	key := p.needKey || p.tick%150 == 0
	audioIn := make(map[int]string, len(p.audioIn))
	for k, v := range p.audioIn {
		audioIn[k] = v
	}
	videoIn := make(map[int]string, len(p.videoIn))
	for k, v := range p.videoIn {
		videoIn[k] = v
	}
	p.mu.Unlock()
	_ = p.call.SendAudio(audioOut, payload(p.idx, seq, 60))
	if sendVideo {
		if rate := p.call.VideoBitrate(videoOut); rate > 0 {
			size := max(rate/8/16, 200)
			flags := FlagLongTerm
			if key {
				flags |= FlagKeyframe
				size *= 3
			}
			p.vseq++
			if _, err := p.call.SendVideo(videoOut, payload(p.idx, p.vseq, size), flags, 0, -1); err == nil && key {
				p.mu.Lock()
				p.needKey = false
				p.mu.Unlock()
			}
		}
	}
	now := time.Now()
	for stream, peer := range audioIn {
		a := p.call.PullAudio(stream)
		if a.Status == AudioPlay {
			p.mu.Lock()
			if checkPayload(a.Data) {
				p.plays[peer] = append(p.plays[peer], now)
			} else {
				p.bad++
			}
			p.mu.Unlock()
		}
	}
	for stream, peer := range videoIn {
		for f := p.call.PollVideo(stream); f != nil; f = p.call.PollVideo(stream) {
			p.mu.Lock()
			if checkPayload(f.Data) {
				p.frames[peer]++
			} else {
				p.bad++
			}
			p.mu.Unlock()
		}
	}
}

func (p *phone) playsSince(peer string, since time.Time) int {
	p.mu.Lock()
	defer p.mu.Unlock()
	n := 0
	for _, at := range p.plays[peer] {
		if !at.Before(since) {
			n++
		}
	}
	return n
}

func (p *phone) firstPlayAfter(peer string, since time.Time) time.Time {
	p.mu.Lock()
	defer p.mu.Unlock()
	for _, at := range p.plays[peer] {
		if at.After(since) {
			return at
		}
	}
	return time.Time{}
}

func waitFor(t *testing.T, what string, timeout time.Duration, ok func() bool) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for !ok() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func TestTwoPhonesHoldAnEncryptedVerifiedCallAndMoveToTheStandby(t *testing.T) {
	primary, standby := startCore(t), startCore(t)
	roomID := signal.RoomID(testSecret, "channel-mobile")
	chat := &chatRoom{}
	before := runtime.NumGoroutine()
	alice := newPhone(t, chat, "alice", 1, primary, standby, roomID, true)
	bob := newPhone(t, chat, "bob", 2, primary, standby, roomID, true)
	joinedAt := time.Now()
	alice.start(t)
	bob.start(t)
	t.Logf("both joined in %v", time.Since(joinedAt))
	waitFor(t, "audio both ways", 5*time.Second, func() bool {
		return alice.playsSince(bob.peer, joinedAt) > 0 && bob.playsSince(alice.peer, joinedAt) > 0
	})
	firstAudio := time.Since(joinedAt)
	if err := alice.call.WaitStandby(3000); err != nil {
		t.Fatalf("alice standby: %v", err)
	}
	if err := bob.call.WaitStandby(3000); err != nil {
		t.Fatalf("bob standby: %v", err)
	}
	window := time.Now()
	time.Sleep(2 * time.Second)
	for _, pair := range [][2]*phone{{alice, bob}, {bob, alice}} {
		got := pair[0].playsSince(pair[1].peer, window)
		if got < 70 {
			t.Errorf("%s played %d of about 100 audio frames from %s", pair[0].name, got, pair[1].name)
		}
		pair[0].mu.Lock()
		frames, bad, auth := pair[0].frames[pair[1].peer], pair[0].bad, pair[0].auth[pair[1].peer]
		pair[0].mu.Unlock()
		if frames < 20 {
			t.Errorf("%s decoded %d video frames from %s", pair[0].name, frames, pair[1].name)
		}
		if bad > 0 {
			t.Errorf("%s got %d frames whose plaintext was wrong", pair[0].name, bad)
		}
		if auth != AuthVerified {
			t.Errorf("%s sees %s's media as %d, not verified", pair[0].name, pair[1].name, auth)
		}
		if st := pair[0].call.Auth(pair[1].peer); st.State != AuthVerified || st.Verified == 0 || st.Forged != 0 {
			t.Errorf("%s auth status of %s: %+v", pair[0].name, pair[1].name, st)
		}
	}
	va, vb := alice.call.Verification(), bob.call.Verification()
	if !va.HasCode || !vb.HasCode || va.Emoji != vb.Emoji || va.Digits != vb.Digits || va.Round != vb.Round {
		t.Fatalf("verification codes differ: alice %+v bob %+v", va, vb)
	}
	t.Logf("code %s (%s), digits %s", va.Emoji, va.EmojiNames, va.Digits)
	if err := alice.call.MarkVerified(va.Round); err != nil {
		t.Fatal(err)
	}
	if err := bob.call.MarkVerified(vb.Round); err != nil {
		t.Fatal(err)
	}
	if !alice.call.Verification().Verified || !bob.call.Verification().Verified {
		t.Fatal("marking the shown code did not verify the call")
	}
	st := alice.call.Stats()
	t.Logf("alice: sealed %d opened %d refused %d auth sent %d accepted %d rtt %dus target %d, first audio after %v",
		st.Sealed, st.Opened, st.Refused, st.AuthSent, st.AuthAccepted, st.RTTUs, st.TargetBitrate, firstAudio)
	t.Logf("goroutines for two phones and two cores: %d before, %d now", before, runtime.NumGoroutine())

	killed := time.Now()
	primary.stop()
	waitFor(t, "both phones to move", 5*time.Second, func() bool {
		alice.mu.Lock()
		a := !alice.moved.IsZero()
		alice.mu.Unlock()
		bob.mu.Lock()
		b := !bob.moved.IsZero()
		bob.mu.Unlock()
		return a && b
	})
	waitFor(t, "audio back on the standby core", 5*time.Second, func() bool {
		return !alice.firstPlayAfter(bob.peer, killed).IsZero() && !bob.firstPlayAfter(alice.peer, killed).IsZero()
	})
	gapA := alice.firstPlayAfter(bob.peer, killed).Sub(killed)
	gapB := bob.firstPlayAfter(alice.peer, killed).Sub(killed)
	t.Logf("core killed: audio back after %v (alice hears bob) and %v (bob hears alice)", gapA, gapB)
	if gapA > time.Second || gapB > time.Second {
		t.Errorf("audio took %v / %v to come back after the core died", gapA, gapB)
	}
	window = time.Now()
	time.Sleep(time.Second)
	if a, b := alice.playsSince(bob.peer, window), bob.playsSince(alice.peer, window); a < 35 || b < 35 {
		t.Errorf("after the move: alice played %d, bob %d of about 50 frames", a, b)
	}
	if !alice.call.Stats().Moved {
		t.Error("stats do not report the move")
	}
	for _, p := range []*phone{alice, bob} {
		p.mu.Lock()
		if len(p.errors) > 0 {
			t.Errorf("%s errors: %s", p.name, strings.Join(p.errors, "; "))
		}
		p.mu.Unlock()
	}
}

func TestASecondPathOpensFromASocketTheAppHandsIn(t *testing.T) {
	primary := startCore(t)
	roomID := signal.RoomID(testSecret, "channel-path")
	chat := &chatRoom{}
	alice := newPhone(t, chat, "alice", 3, primary, nil, roomID, false)
	alice.start(t)
	sock, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	f, err := sock.File()
	if err != nil {
		t.Fatal(err)
	}
	id, err := alice.call.AddPathFd(int(f.Fd()), false)
	f.Close()
	sock.Close()
	if err != nil {
		t.Fatalf("second path: %v", err)
	}
	waitFor(t, "two usable paths", 3*time.Second, func() bool { return alice.call.UsablePaths() == 2 })
	if alice.call.PathCount() != 2 {
		t.Fatalf("path count %d", alice.call.PathCount())
	}
	if err := alice.call.AbandonPath(id); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "one usable path", 3*time.Second, func() bool { return alice.call.UsablePaths() == 1 })
}

func TestARefusedTicketFailsTheConnectWithItsCode(t *testing.T) {
	primary := startCore(t)
	ep := primary.endpoint(signal.RoomID(testSecret, "channel-refused"), strings.Repeat("ab", 16), strings.Repeat("cd", 16), false)
	ep.Ticket = "forged.ticket"
	raw, _ := json.Marshal(fmmcall.Access{Endpoint: *ep})
	seed := make([]byte, 32)
	call, err := NewCall(&Config{Access: string(raw), Call: "office-test-room", Peer: strings.Repeat("ab", 16), Account: "carol", Identity: seed, LocalAddr: "127.0.0.1:0"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer call.Leave()
	if err := call.Connect(3000); err == nil || call.JoinError() != 401 {
		t.Fatalf("connect with a forged ticket: %v, code %d", err, call.JoinError())
	}
}

func TestConfigIsChecked(t *testing.T) {
	if _, err := NewCall(&Config{}, nil); err != ErrConfig {
		t.Fatalf("empty config: %v", err)
	}
	if _, err := NewCall(&Config{Access: `{"addr":"x:1","ticket":"t","hashes":["h"],"alpn":"h3"}`, Call: "c", Peer: "p", Account: "a", Identity: make([]byte, 32)}, nil); err != ErrConfig {
		t.Fatalf("wrong alpn: %v", err)
	}
}
