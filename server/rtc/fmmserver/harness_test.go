package fmmserver

import (
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"net"
	"net/netip"
	"sort"
	"sync"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/fmm"
	"forkmesh.com/mirror-node/rtc/fmmcall"
	"forkmesh.com/mirror-node/rtc/netsim"
	"forkmesh.com/mirror-node/rtc/quic"
	"forkmesh.com/mirror-node/rtc/signal"
)

const videoFPS = 50 / 3

var testSecret = []byte("fmm-test-secret-0123456789abcdef-0123456789")

type core struct {
	name   string
	url    string
	server *Server
	certs  *CertStore
	ep     *quic.Endpoint
	relay  *netsim.Relay
	cancel context.CancelFunc
}

func startCore(t testing.TB, name string, seed uint64) *core {
	t.Helper()
	certs, err := NewCertStore("", nil)
	if err != nil {
		t.Fatal(err)
	}
	srv, err := New(Config{Secret: testSecret, Certs: certs, AllowOrigin: func(string) bool { return true }})
	if err != nil {
		t.Fatal(err)
	}
	ep, err := quic.Listen("udp4", "127.0.0.1:0", srv.QUICConfig())
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	go srv.Serve(ctx, ep)
	relay, err := netsim.Listen("127.0.0.1:0", ep.LocalAddr().(*net.UDPAddr).AddrPort(), netsim.Config{Seed: seed})
	if err != nil {
		t.Fatal(err)
	}
	c := &core{name: name, url: "wss://" + name + ".test/calls/ws", server: srv, certs: certs, ep: ep, relay: relay, cancel: cancel}
	t.Cleanup(c.stop)
	return c
}

func (c *core) stop() {
	c.cancel()
	c.server.Close()
	c.ep.Close()
	c.relay.Close()
}

func rendezvous(room string, cores []*core) []*core {
	out := append([]*core(nil), cores...)
	score := func(c *core) string {
		sum := sha256.Sum256([]byte(room + "\n" + c.url))
		return hex.EncodeToString(sum[:])
	}
	sort.SliceStable(out, func(i, j int) bool { return score(out[i]) > score(out[j]) })
	return out
}

func ticketFor(room, peer, acct string, standby bool) string {
	return signal.MintTicket(testSecret, signal.Ticket{
		Room: room, Peer: peer, Acct: acct, Exp: time.Now().Add(signal.TicketLifetime).Unix(),
		Engine: signal.EngineFMM, Standby: standby,
	})
}

func endpointOf(c *core, ticket string) fmmcall.Endpoint {
	return fmmcall.Endpoint{
		Addr: c.relay.Addr().String(), ALPN: fmmcall.ALPN, Hashes: c.certs.Hashes(), Ticket: ticket,
	}
}

func testRoom(n int) string {
	return signal.RoomID(testSecret, "channel-"+string(rune('a'+n)))
}

type play struct {
	at   time.Time
	from uint8
	seq  uint32
}

type party struct {
	t     testing.TB
	idx   uint8
	peer  string
	video bool
	sock  *net.UDPConn
	ep    *quic.Endpoint
	call  *fmmcall.Call

	mu       sync.Mutex
	sess     *fmmcall.Session
	audioOut int
	videoOut int
	needKey  bool
	lastKey  time.Time
	audioIn  map[int]bool
	videoIn  map[int]bool
	plays    []play
	frames   int
	errors   []string
	seq      uint32
	vtick    int
	recover  int64
	stop     chan struct{}
	done     chan struct{}
}

func newParty(t testing.TB, idx uint8, video bool) *party {
	t.Helper()
	sock, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	ep, err := quic.NewEndpoint(sock, &quic.Config{}, false)
	if err != nil {
		t.Fatal(err)
	}
	p := &party{
		t: t, idx: idx, peer: hex.EncodeToString([]byte{idx, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, idx}),
		video: video, sock: sock, ep: ep, stop: make(chan struct{}), done: make(chan struct{}),
	}
	t.Cleanup(func() { ep.Close() })
	return p
}

func (p *party) key() string { return "addr:" + p.sock.LocalAddr().(*net.UDPAddr).AddrPort().String() }

func (p *party) acct() string {
	return hex.EncodeToString([]byte{p.idx, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, p.idx})
}

func (p *party) onSession(s *fmmcall.Session, moved bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.sess = s
	p.audioIn, p.videoIn = map[int]bool{}, map[int]bool{}
	p.recover = -1
	p.audioOut, _ = s.Client.PublishAudio("mic", s.Now())
	if p.video {
		p.videoOut, _ = s.Client.PublishVideo("cam", 1, s.Now())
		p.needKey = true
	}
}

func (p *party) join(ctx context.Context, access fmmcall.Access) error {
	call, err := fmmcall.Start(ctx, p.ep, access, fmmcall.Options{
		Client:    &fmm.Config{StartBitrate: 400_000, MaxBitrate: 800_000},
		OnSession: p.onSession,
	})
	if err != nil {
		return err
	}
	p.call = call
	go p.run()
	return nil
}

func (p *party) run() {
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

func (p *party) step() {
	p.mu.Lock()
	defer p.mu.Unlock()
	s := p.sess
	if s == nil {
		return
	}
	c := s.Client
	now := s.Now()
	frame := make([]byte, 80)
	frame[0] = p.idx
	binary.BigEndian.PutUint32(frame[1:], p.seq)
	p.seq++
	_ = c.SendAudio(p.audioOut, frame, now)
	if p.video {
		p.vtick++
		if p.vtick%3 == 0 {
			key := p.needKey || time.Since(p.lastKey) > 3*time.Second
			if rate := c.VideoBitrate(p.videoOut); rate > 0 {
				size := max(rate/8/videoFPS, 200)
				f := &fmm.OutFrame{Keyframe: key, LongTerm: key || p.vtick%15 == 0}
				if key {
					size *= 4
				} else if p.recover >= 0 {
					f.Recover, f.RefFrame = true, p.recover
				}
				f.Data = make([]byte, size)
				if _, err := c.SendVideoFrame(p.videoOut, f, now); err == nil {
					p.recover = -1
					if key {
						p.needKey, p.lastKey = false, time.Now()
					}
				}
			}
		}
	}
	for e := c.PollEvent(); e != nil; e = c.PollEvent() {
		switch e.Kind {
		case fmm.EventTrackAdded:
			if e.Track == fmm.KindAudio {
				p.audioIn[e.Stream] = true
				_ = c.Subscribe(e.Stream, 0, now)
			} else {
				p.videoIn[e.Stream] = true
				_ = c.Subscribe(e.Stream, e.Layers-1, now)
			}
		case fmm.EventTrackRemoved:
			delete(p.audioIn, e.Stream)
			delete(p.videoIn, e.Stream)
		case fmm.EventKeyframeRequest:
			p.needKey = true
		case fmm.EventRecover:
			p.recover = e.FrameID
		case fmm.EventError:
			p.errors = append(p.errors, e.Text)
		}
	}
	at := time.Now()
	for stream := range p.audioIn {
		if a := c.PullAudio(stream, now); a.Status == fmm.AudioPlay && len(a.Data) >= 5 {
			p.plays = append(p.plays, play{at: at, from: a.Data[0], seq: binary.BigEndian.Uint32(a.Data[1:])})
		}
	}
	for stream := range p.videoIn {
		for f := c.PollVideo(stream, now); f != nil; f = c.PollVideo(stream, now) {
			p.frames++
		}
	}
}

func (p *party) leave() {
	select {
	case <-p.stop:
	default:
		close(p.stop)
		<-p.done
	}
	if p.call != nil {
		p.call.Close()
	}
}

func (p *party) playsFrom(from uint8, after time.Time) []play {
	p.mu.Lock()
	defer p.mu.Unlock()
	var out []play
	for _, pl := range p.plays {
		if pl.from == from && !pl.at.Before(after) {
			out = append(out, pl)
		}
	}
	return out
}

func (p *party) videoFrames() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.frames
}

func shape(r *netsim.Relay, key string, prof netsim.Profile) {
	steps := []netsim.Step{{Profile: prof}}
	r.Shape(key, time.Now(), steps, steps)
}

func addrOf(t testing.TB, s string) netip.AddrPort {
	ap, err := netip.ParseAddrPort(s)
	if err != nil {
		t.Fatal(err)
	}
	return ap
}
