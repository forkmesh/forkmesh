package quic

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"sort"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/netsim"
)

func mpServerConfig(t testing.TB) *Config {
	c := testServerConfig(t)
	c.Multipath = true
	return c
}

func mpClientConfig(t testing.TB) *Config {
	c := testClientConfig(t)
	c.Multipath = true
	return c
}

func openPath(t *testing.T, c *Conn, opts PathOptions) (*Path, *net.UDPConn) {
	t.Helper()
	sock := mustUDP(t)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	p, err := c.OpenPath(ctx, sock, opts)
	if err != nil {
		t.Fatalf("open path: %v", err)
	}
	return p, sock
}

func TestMultipathNegotiatedAndSecondPathCarriesData(t *testing.T) {
	p := newPair(t, mpServerConfig(t), mpClientConfig(t), nil)
	if !p.cliConn.MultipathEnabled() || !p.srvConn.MultipathEnabled() {
		t.Fatal("multipath not negotiated")
	}
	echoStreams(t, p.srvConn)
	path, sock := openPath(t, p.cliConn, PathOptions{})
	if path.ID() != 1 {
		t.Fatalf("first new path id %d", path.ID())
	}
	waitFor(t, "server to validate path 1", func() bool {
		info, ok := p.srvConn.PathInfo(1)
		return ok && info.State == PathActive
	})
	info, _ := p.srvConn.PathInfo(1)
	if info.RemoteAddr != sock.LocalAddr().(*net.UDPAddr).AddrPort() {
		t.Fatalf("server sees path 1 from %v", info.RemoteAddr)
	}
	if err := p.cliConn.AbandonPath(0); err != nil {
		t.Fatal(err)
	}
	s, err := p.cliConn.OpenStream()
	if err != nil {
		t.Fatal(err)
	}
	msg := make([]byte, 300<<10)
	rand.Read(msg)
	go func() {
		s.Write(msg)
		s.Close()
	}()
	s.SetReadDeadline(time.Now().Add(15 * time.Second))
	got, err := io.ReadAll(s)
	if err != nil || !bytes.Equal(got, msg) {
		t.Fatalf("echo over path 1: %d of %d bytes, %v", len(got), len(msg), err)
	}
	ci, _ := p.cliConn.PathInfo(1)
	if ci.PacketsSent == 0 || ci.PacketsReceived == 0 || !ci.Primary {
		t.Fatalf("client path 1 %+v", ci)
	}
	waitFor(t, "path 0 removed", func() bool {
		_, ok := p.cliConn.PathInfo(0)
		return !ok
	})
}

type mpBench struct {
	srv          *Endpoint
	relay        *netsim.Relay
	cli          *Endpoint
	cc, sc       *Conn
	sockA, sockB *net.UDPConn
	keyA, keyB   string
	pathB        *Path
}

func flowKey(c *net.UDPConn) string {
	return "addr:" + c.LocalAddr().(*net.UDPAddr).AddrPort().String()
}

func shapeBoth(r *netsim.Relay, key string, steps ...netsim.Step) {
	r.Shape(key, time.Now(), steps, steps)
}

func newMPBench(t *testing.T, delayA, delayB time.Duration, sconf, cconf *Config) *mpBench {
	t.Helper()
	srv, err := Listen("udp4", "127.0.0.1:0", sconf)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { srv.Close() })
	relay, err := netsim.Listen("127.0.0.1:0", srv.LocalAddr().(*net.UDPAddr).AddrPort(), netsim.Config{Seed: 7})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { relay.Close() })
	b := &mpBench{srv: srv, relay: relay, sockA: mustUDP(t), sockB: mustUDP(t)}
	b.keyA, b.keyB = flowKey(b.sockA), flowKey(b.sockB)
	shapeBoth(relay, b.keyA, netsim.Step{Profile: netsim.Profile{Delay: delayA}})
	shapeBoth(relay, b.keyB, netsim.Step{Profile: netsim.Profile{Delay: delayB}})
	b.cli, err = NewEndpoint(b.sockA, cconf, false)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { b.cli.Close() })
	ctx := ctxTimeout(t, 10*time.Second)
	accepted := make(chan *Conn, 1)
	go func() {
		c, _ := srv.Accept(ctx)
		accepted <- c
	}()
	b.cc, err = b.cli.Dial(ctx, relay.Addr(), cconf)
	if err != nil {
		t.Fatal(err)
	}
	b.sc = <-accepted
	if b.sc == nil {
		t.Fatal("no server connection")
	}
	if b.cc.MultipathEnabled() {
		b.pathB, err = b.cc.OpenPath(ctx, b.sockB, PathOptions{})
		if err != nil {
			t.Fatal(err)
		}
		waitFor(t, "server to validate path 1", func() bool {
			info, ok := b.sc.PathInfo(1)
			return ok && info.State == PathActive
		})
	}
	return b
}

type audioResult struct {
	sent, received, lost, duplicates int
	maxGap                           time.Duration
	gapAt                            time.Duration
	lostSeqs                         []uint64
}

func seqKey(b []byte) (uint64, bool) {
	if len(b) < 8 {
		return 0, false
	}
	return binary.BigEndian.Uint64(b), true
}

func runAudio(t *testing.T, send, recv *Conn, policy SendPolicy, total, eventAt time.Duration, event func()) audioResult {
	t.Helper()
	type arrival struct {
		seq uint64
		at  time.Duration
	}
	var mu sync.Mutex
	first := map[uint64]time.Duration{}
	dups := 0
	start := time.Now()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			d, err := recv.ReceiveDatagram(ctx)
			if err != nil {
				return
			}
			seq, ok := seqKey(d)
			if !ok {
				continue
			}
			at := time.Since(start)
			mu.Lock()
			if _, seen := first[seq]; seen {
				dups++
			} else {
				first[seq] = at
			}
			mu.Unlock()
		}
	}()
	payload := make([]byte, 120)
	tick := time.NewTicker(20 * time.Millisecond)
	defer tick.Stop()
	var fired atomic.Bool
	var seq uint64
	for time.Since(start) < total {
		<-tick.C
		if !fired.Load() && time.Since(start) >= eventAt {
			fired.Store(true)
			event()
		}
		binary.BigEndian.PutUint64(payload, seq)
		if err := send.SendDatagramWith(payload, policy); err != nil {
			t.Fatalf("send %d: %v", seq, err)
		}
		seq++
	}
	time.Sleep(700 * time.Millisecond)
	cancel()
	<-done
	mu.Lock()
	defer mu.Unlock()
	res := audioResult{sent: int(seq), received: len(first), duplicates: dups}
	var arr []arrival
	for s, at := range first {
		arr = append(arr, arrival{s, at})
	}
	sort.Slice(arr, func(i, j int) bool { return arr[i].at < arr[j].at })
	for i := 1; i < len(arr); i++ {
		if g := arr[i].at - arr[i-1].at; g > res.maxGap && arr[i].at > eventAt-200*time.Millisecond {
			res.maxGap, res.gapAt = g, arr[i-1].at
		}
	}
	for s := uint64(0); s < seq; s++ {
		if _, ok := first[s]; !ok {
			res.lost++
			res.lostSeqs = append(res.lostSeqs, s)
		}
	}
	return res
}

const (
	benchDelayA = 15 * time.Millisecond
	benchDelayB = 25 * time.Millisecond
)

func benchConfigs(t *testing.T) (*Config, *Config) {
	s, c := mpServerConfig(t), mpClientConfig(t)
	s.DatagramDedupKey = seqKey
	c.DatagramDedupKey = seqKey
	return s, c
}

func TestMultipathPathFailureAudio(t *testing.T) {
	cases := []struct {
		name     string
		policy   SendPolicy
		abandon  bool
		downlink bool
	}{
		{"redundant-blackhole", Redundant, false, false},
		{"redundant-blackhole-downlink", Redundant, false, true},
		{"fastest-blackhole", Fastest, false, false},
		{"fastest-abandon", Fastest, true, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			retryTiming(t, func(fail func(string, ...any)) {
				sconf, cconf := benchConfigs(t)
				b := newMPBench(t, benchDelayA, benchDelayB, sconf, cconf)
				waitFor(t, "rtt samples on both paths", func() bool {
					for _, id := range []uint64{0, 1} {
						if info, ok := b.cc.PathInfo(id); !ok || info.SmoothedRTT > 200*time.Millisecond {
							return false
						}
					}
					return true
				})
				sender, receiver := b.cc, b.sc
				if tc.downlink {
					sender, receiver = b.sc, b.cc
				}
				var infoA, infoB PathInfo
				res := runAudio(t, sender, receiver, tc.policy, 2500*time.Millisecond, 1200*time.Millisecond, func() {
					infoA, _ = sender.PathInfo(0)
					infoB, _ = sender.PathInfo(1)
					if tc.abandon {
						if err := b.cc.AbandonPath(0); err != nil {
							t.Error(err)
						}
						return
					}
					shapeBoth(b.relay, b.keyA, netsim.Step{Profile: netsim.Profile{Delay: benchDelayA, Loss: 1}})
				})
				st := receiver.Stats()
				cs := sender.Stats()
				t.Logf("%s: rtt A %v B %v; sent %d received %d lost %d %v; max gap %v at %v (extra %v over 20ms); receiver dropped %d duplicates; sender reinjected %d, primary switches %d",
					tc.name, infoA.SmoothedRTT, infoB.SmoothedRTT, res.sent, res.received, res.lost, res.lostSeqs, res.maxGap, res.gapAt, res.maxGap-20*time.Millisecond, st.DatagramsDuplicate, cs.DatagramsReinjected, cs.PrimarySwitches)
				if res.lost != 0 {
					fail("%d datagrams lost", res.lost)
				}
				extra := res.maxGap - 20*time.Millisecond
				switch {
				case tc.policy == Redundant || tc.abandon:
					if extra > 2*benchDelayB {
						fail("gap %v beyond one round trip of the surviving path", res.maxGap)
					}
				default:
					if res.maxGap > 4*(benchDelayA+benchDelayB) {
						fail("failover gap %v", res.maxGap)
					}
				}
			})
		})
	}
}

func TestPathNonceMatchesDraftExample(t *testing.T) {
	k := &packetKey{}
	copy(k.iv[:], unhex("6b26114b9cba2b63a9e8dd4f"))
	n := k.pathNonce(3, 54321)
	if want := unhex("6b2611489cba2b63a9e8097e"); !bytes.Equal(n[:], want) {
		t.Fatalf("nonce %x, want %x", n, want)
	}
	if a, b := k.pathNonce(0, 77), k.nonce(77); a != b {
		t.Fatal("path 0 nonce differs from the RFC 9001 nonce")
	}
}

func TestInitialMaxPathIDParameter(t *testing.T) {
	p := defaultTransportParams()
	p.initialSCID, p.hasInitialSCID = []byte{1, 2, 3, 4}, true
	p.maxPathID, p.hasMaxPathID = 3, true
	got, err := parseTransportParams(p.marshal(false), false)
	if err != nil || !got.hasMaxPathID || got.maxPathID != 3 {
		t.Fatalf("round trip %+v %v", got, err)
	}
	p.hasMaxPathID = false
	if got, _ := parseTransportParams(p.marshal(false), false); got.hasMaxPathID {
		t.Fatal("absent parameter parsed as present")
	}
	bad := appendParamVarint(appendParamBytes(nil, tpInitialSCID, []byte{1}), tpInitialMaxPathID, 1<<32)
	if _, err := parseTransportParams(bad, false); err == nil || err.Code != TransportParameterError {
		t.Fatalf("initial_max_path_id above 2^32-1 accepted: %v", err)
	}
}

func multipathFrameSeeds() [][]byte {
	tok := [16]byte{9, 8, 7}
	return [][]byte{
		appendPathAckFrame(nil, 2, []ackRange{{10, 20}, {2, 5}}, 7),
		append(appendPathAckFrame(nil, 1, []ackRange{{0, 3}}, 1)[:0:0], 0x3f, 1, 3, 0, 0, 3, 1, 2, 3),
		appendTwoVarintFrame(nil, framePathAbandon, 1, pathAbandonUnstableOrPoor),
		appendTwoVarintFrame(nil, framePathStatusBackup, 2, 5),
		appendTwoVarintFrame(nil, framePathStatusAvailable, 2, 6),
		appendPathNewConnectionID(nil, 3, 1, 0, []byte{1, 2, 3, 4, 5, 6, 7, 8}, tok),
		appendTwoVarintFrame(nil, framePathRetireConnectionID, 3, 0),
		appendOneVarintFrame(nil, frameMaxPathID, 9),
		appendOneVarintFrame(nil, framePathsBlocked, 3),
		appendTwoVarintFrame(nil, framePathCIDsBlocked, 1, 2),
	}
}

func TestMultipathFramesRoundTrip(t *testing.T) {
	seeds := multipathFrameSeeds()
	want := []struct {
		typ          uint64
		path, v1, v2 uint64
	}{
		{framePathAck, 2, 20, 0},
		{framePathAckECN, 1, 3, 0},
		{framePathAbandon, 1, pathAbandonUnstableOrPoor, 0},
		{framePathStatusBackup, 2, 5, 0},
		{framePathStatusAvailable, 2, 6, 0},
		{framePathNewConnectionID, 3, 1, 0},
		{framePathRetireConnectionID, 3, 0, 0},
		{frameMaxPathID, 0, 9, 0},
		{framePathsBlocked, 0, 3, 0},
		{framePathCIDsBlocked, 1, 2, 0},
	}
	for i, b := range seeds {
		var f frame
		n, err := parseFrame(b, &f)
		if err != nil || n != len(b) {
			t.Fatalf("frame %d: n %d of %d, %v", i, n, len(b), err)
		}
		w := want[i]
		if f.typ != w.typ || f.pathID != w.path {
			t.Fatalf("frame %d: type %x path %d", i, f.typ, f.pathID)
		}
		var v uint64
		switch f.typ {
		case framePathAck, framePathAckECN:
			v = f.ranges[0].largest
		case framePathAbandon:
			v = f.code
		case frameMaxPathID, framePathsBlocked:
			v = f.max
		default:
			v = f.seq
		}
		if v != w.v1 {
			t.Fatalf("frame %d: value %d, want %d", i, v, w.v1)
		}
		if f.ackEliciting() == f.isAck() {
			t.Fatalf("frame %d: ack-eliciting %v", i, f.ackEliciting())
		}
		if !isMultipathFrame(f.typ) || frameAllowed(packetHandshake, f.typ) {
			t.Fatalf("frame %d allowed outside 1-RTT", i)
		}
	}
	var f frame
	pn := appendPathNewConnectionID(nil, 1, 1, 2, []byte{1, 2, 3, 4}, [16]byte{})
	if _, err := parseFrame(pn, &f); err == nil {
		t.Fatal("retire_prior_to above sequence accepted")
	}
}

func TestMultipathFramesRejectedWithoutNegotiation(t *testing.T) {
	p := newPair(t, testServerConfig(t), mpClientConfig(t), nil)
	if p.cliConn.MultipathEnabled() || p.srvConn.MultipathEnabled() {
		t.Fatal("multipath negotiated with a server that does not offer it")
	}
	c := p.srvConn
	c.mu.Lock()
	sp := c.spaces[spaceApp]
	c.processPayload(spaceApp, packet1RTT, sp.largestRecv+1, appendOneVarintFrame(nil, frameMaxPathID, 4), c.active.addr, c.localCIDs[0].cid, time.Now())
	err := c.closeErr
	c.mu.Unlock()
	var te *TransportError
	if !errors.As(err, &te) || te.Code != ProtocolViolation {
		t.Fatalf("multipath frame without negotiation: %v", err)
	}
}

func TestMultipathFallsBackWhenOneSideLacksIt(t *testing.T) {
	for _, tc := range []struct {
		name           string
		server, client *Config
	}{
		{"server-without", testServerConfig(t), mpClientConfig(t)},
		{"client-without", mpServerConfig(t), testClientConfig(t)},
	} {
		t.Run(tc.name, func(t *testing.T) {
			p := newPair(t, tc.server, tc.client, nil)
			if p.cliConn.MultipathEnabled() || p.srvConn.MultipathEnabled() {
				t.Fatal("multipath negotiated")
			}
			if _, err := p.cliConn.OpenPath(ctxTimeout(t, time.Second), mustUDP(t), PathOptions{}); err != ErrMultipathDisabled {
				t.Fatalf("open path without multipath: %v", err)
			}
			echoStreams(t, p.srvConn)
			waitFor(t, "spare connection ids", func() bool {
				p.cliConn.mu.Lock()
				defer p.cliConn.mu.Unlock()
				return p.cliConn.unusedPeerCID() != nil
			})
			fresh := mustUDP(t)
			if err := p.client.Rebind(fresh); err != nil {
				t.Fatal(err)
			}
			s, err := p.cliConn.OpenStream()
			if err != nil {
				t.Fatal(err)
			}
			s.Write([]byte("after rebind"))
			s.Close()
			s.SetReadDeadline(time.Now().Add(10 * time.Second))
			got, err := io.ReadAll(s)
			if err != nil || string(got) != "after rebind" {
				t.Fatalf("echo after single-path migration %q %v", got, err)
			}
			want := fresh.LocalAddr().(*net.UDPAddr).AddrPort()
			waitFor(t, "server to migrate", func() bool {
				return p.srvConn.RemoteAddr().(*net.UDPAddr).AddrPort() == want
			})
		})
	}
}

func TestMultipathRebindOfPathZeroStillMigrates(t *testing.T) {
	p := newPair(t, mpServerConfig(t), mpClientConfig(t), nil)
	echoStreams(t, p.srvConn)
	waitFor(t, "spare connection ids", func() bool {
		p.cliConn.mu.Lock()
		defer p.cliConn.mu.Unlock()
		return p.cliConn.unusedPeerCID() != nil
	})
	fresh := mustUDP(t)
	if err := p.client.Rebind(fresh); err != nil {
		t.Fatal(err)
	}
	s, _ := p.cliConn.OpenStream()
	s.Write([]byte("moved"))
	s.Close()
	s.SetReadDeadline(time.Now().Add(10 * time.Second))
	if got, err := io.ReadAll(s); err != nil || string(got) != "moved" {
		t.Fatalf("echo %q %v", got, err)
	}
	want := fresh.LocalAddr().(*net.UDPAddr).AddrPort()
	waitFor(t, "server to migrate path 0", func() bool {
		info, ok := p.srvConn.PathInfo(0)
		return ok && info.RemoteAddr == want
	})
}

func TestMultipathAbandonAndReAdd(t *testing.T) {
	p := newPair(t, mpServerConfig(t), mpClientConfig(t), nil)
	ctx := ctxTimeout(t, 30*time.Second)
	seen := make(chan uint64, 1024)
	go func() {
		for {
			_, id, err := p.srvConn.ReceiveDatagramPath(ctx)
			if err != nil {
				return
			}
			seen <- id
		}
	}()
	for round := 0; round < 6; round++ {
		path, _ := openPath(t, p.cliConn, PathOptions{})
		if want := uint64(round + 1); path.ID() != want {
			t.Fatalf("round %d: path id %d, want %d", round, path.ID(), want)
		}
		got := false
		for i := 0; i < 200 && !got; i++ {
			p.cliConn.SendDatagramWith([]byte("on every path"), Redundant)
			select {
			case id := <-seen:
				got = id == path.ID()
			case <-time.After(5 * time.Millisecond):
			}
		}
		if !got {
			t.Fatalf("round %d: nothing arrived on path %d", round, path.ID())
		}
		if err := path.Abandon(); err != nil {
			t.Fatal(err)
		}
		waitFor(t, "both sides to drop the path", func() bool {
			_, c := p.cliConn.PathInfo(path.ID())
			_, s := p.srvConn.PathInfo(path.ID())
			return !c && !s
		})
	}
	if err := p.cliConn.AbandonPath(0); err != ErrLastPath {
		t.Fatalf("abandoning the last path: %v", err)
	}
	p.cliConn.mu.Lock()
	max := p.cliConn.mp.peerMax
	p.cliConn.mu.Unlock()
	if max < 6 {
		t.Fatalf("peer max path id %d after six re-adds", max)
	}
	if st := p.srvConn.Stats(); st.PathsAbandoned != 6 || st.PathsOpened != 6 {
		t.Fatalf("server stats %+v", st)
	}
}

type spoofingConn struct {
	net.PacketConn
	spoof *net.UDPConn
	armed atomic.Bool
}

func (s *spoofingConn) WriteTo(b []byte, addr net.Addr) (int, error) {
	if len(b) > 100 && b[0]&0x80 == 0 && s.armed.CompareAndSwap(true, false) {
		s.spoof.WriteTo(b, addr)
		time.Sleep(2 * time.Millisecond)
	}
	return s.PacketConn.WriteTo(b, addr)
}

type countingConn struct {
	net.PacketConn
	in, out atomic.Int64
	deaf    bool
}

func (c *countingConn) WriteTo(b []byte, addr net.Addr) (int, error) {
	c.out.Add(int64(len(b)))
	return c.PacketConn.WriteTo(b, addr)
}

func (c *countingConn) ReadFrom(b []byte) (int, net.Addr, error) {
	for {
		n, addr, err := c.PacketConn.ReadFrom(b)
		if err != nil {
			return n, addr, err
		}
		c.in.Add(int64(n))
		if !c.deaf {
			return n, addr, err
		}
	}
}

func TestMultipathSpoofedPathAttempts(t *testing.T) {
	p := newPair(t, mpServerConfig(t), mpClientConfig(t), nil)
	spoof := mustUDP(t)
	var spoofIn atomic.Int64
	go func() {
		buf := make([]byte, 2048)
		for {
			n, _, err := spoof.ReadFromUDPAddrPort(buf)
			if err != nil {
				return
			}
			spoofIn.Add(int64(n))
		}
	}()
	real := mustUDP(t)
	sc := &spoofingConn{PacketConn: real, spoof: spoof}
	ctx := ctxTimeout(t, 10*time.Second)
	path, err := p.cliConn.OpenPath(ctx, sc, PathOptions{})
	if err != nil {
		t.Fatal(err)
	}
	realAddr := real.LocalAddr().(*net.UDPAddr).AddrPort()
	waitFor(t, "server to validate path 1", func() bool {
		info, ok := p.srvConn.PathInfo(1)
		return ok && info.State == PathActive && info.RemoteAddr == realAddr
	})
	before := p.srvConn.Stats().PathChallenges
	stop := make(chan struct{})
	go func() {
		for {
			select {
			case <-stop:
				return
			case <-time.After(5 * time.Millisecond):
				p.cliConn.SendDatagramWith(make([]byte, 200), Redundant)
			}
		}
	}()
	time.Sleep(50 * time.Millisecond)
	sc.armed.Store(true)
	waitFor(t, "the spoofed copy", func() bool { return !sc.armed.Load() })
	time.Sleep(300 * time.Millisecond)
	close(stop)
	info, _ := p.srvConn.PathInfo(path.ID())
	if info.RemoteAddr != realAddr {
		t.Fatalf("server moved path 1 to %v after a spoofed copy", info.RemoteAddr)
	}
	if after := p.srvConn.Stats().PathChallenges; after <= before {
		t.Fatal("spoofed address was not challenged")
	}
	if in := spoofIn.Load(); in > 3*1500 {
		t.Fatalf("server sent %d bytes to the spoofed address", in)
	}

	p.srvConn.mu.Lock()
	var forgedCID []byte
	for id, set := range p.srvConn.mp.cids {
		if _, live := p.srvConn.mp.paths[id]; !live && len(set.local) > 0 {
			forgedCID = set.local[0].cid
		}
	}
	paths := len(p.srvConn.mp.paths)
	p.srvConn.mu.Unlock()
	if forgedCID == nil {
		t.Fatal("no connection id for an unused path")
	}
	pkt := make([]byte, 1200)
	rand.Read(pkt)
	pkt[0] = 0x40 | pkt[0]&0x3f
	copy(pkt[1:], forgedCID)
	attacker := mustUDP(t)
	var attackerIn atomic.Int64
	go func() {
		buf := make([]byte, 2048)
		for {
			n, _, err := attacker.ReadFromUDPAddrPort(buf)
			if err != nil {
				return
			}
			attackerIn.Add(int64(n))
		}
	}()
	for i := 0; i < 5; i++ {
		attacker.WriteToUDPAddrPort(pkt, p.srvAddr)
	}
	time.Sleep(200 * time.Millisecond)
	p.srvConn.mu.Lock()
	after := len(p.srvConn.mp.paths)
	p.srvConn.mu.Unlock()
	if after != paths || attackerIn.Load() != 0 {
		t.Fatalf("forged path packet: paths %d -> %d, %d bytes back to the attacker", paths, after, attackerIn.Load())
	}
}

func TestMultipathNewPathAmplificationAndValidationTimeout(t *testing.T) {
	p := newPair(t, mpServerConfig(t), mpClientConfig(t), nil)
	deaf := &countingConn{PacketConn: mustUDP(t), deaf: true}
	ctx, cancel := context.WithTimeout(context.Background(), 1500*time.Millisecond)
	defer cancel()
	_, err := p.cliConn.OpenPath(ctx, deaf, PathOptions{})
	if err != context.DeadlineExceeded {
		t.Fatalf("open path over a deaf socket: %v", err)
	}
	time.Sleep(100 * time.Millisecond)
	in, out := deaf.in.Load(), deaf.out.Load()
	t.Logf("unvalidated path: client sent %d bytes, server answered %d bytes", out, in)
	if out == 0 || in == 0 || in > 3*out {
		t.Fatalf("server sent %d bytes for %d received on an unvalidated path", in, out)
	}
	info, ok := p.cliConn.PathInfo(1)
	if ok && info.State != PathAbandoned {
		t.Fatalf("failed path still %v", info.State)
	}
	if info, ok := p.srvConn.PathInfo(1); ok && info.State == PathActive {
		t.Fatal("server uses a path it never validated")
	}
	path, _ := openPath(t, p.cliConn, PathOptions{})
	if path.ID() != 2 {
		t.Fatalf("path id after a failed attempt %d", path.ID())
	}
}

func TestMultipathBackupPathIsNotPrimary(t *testing.T) {
	sconf, cconf := benchConfigs(t)
	b := newMPBench(t, 25*time.Millisecond, 25*time.Millisecond, sconf, cconf)
	shapeBoth(b.relay, b.keyB, netsim.Step{Profile: netsim.Profile{Delay: 5 * time.Millisecond}})
	if err := b.pathB.SetBackup(true); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "server to learn the backup status", func() bool {
		info, ok := b.sc.PathInfo(1)
		return ok && info.PeerBackup
	})
	stop := make(chan struct{})
	defer close(stop)
	go func() {
		for {
			select {
			case <-stop:
				return
			case <-time.After(20 * time.Millisecond):
				b.cc.SendDatagram(make([]byte, 100))
			}
		}
	}()
	time.Sleep(1500 * time.Millisecond)
	ia, _ := b.cc.PathInfo(0)
	ib, _ := b.cc.PathInfo(1)
	if !ia.Primary || ib.Primary {
		t.Fatalf("backup path chosen: A %+v B %+v", ia, ib)
	}
	if err := b.pathB.SetBackup(false); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "the faster path to become primary", func() bool {
		info, _ := b.cc.PathInfo(1)
		return info.Primary
	})
}

func TestMultipathBulkSpreadsAndKeysUpdate(t *testing.T) {
	sconf, cconf := mpServerConfig(t), mpClientConfig(t)
	sconf.KeyUpdateInterval, cconf.KeyUpdateInterval = 200, 200
	sconf.MaxPendingDatagrams = 1024
	p := newPair(t, sconf, cconf, nil)
	openPath(t, p.cliConn, PathOptions{})
	ctx := ctxTimeout(t, 20*time.Second)
	var got atomic.Int64
	go func() {
		for {
			if _, err := p.srvConn.ReceiveDatagram(ctx); err != nil {
				return
			}
			got.Add(1)
		}
	}()
	before := map[uint64]uint64{}
	for _, info := range p.cliConn.Paths() {
		before[info.ID] = info.PacketsSent
	}
	const n = 3000
	sent := 0
	for sent < n {
		if err := p.cliConn.SendDatagramWith(make([]byte, 1000), Bulk); err == ErrDatagramQueueFull {
			time.Sleep(time.Millisecond)
			continue
		} else if err != nil {
			t.Fatal(err)
		}
		sent++
	}
	waitFor(t, "most datagrams", func() bool { return got.Load() > n*9/10 })
	var per []uint64
	for _, info := range p.cliConn.Paths() {
		per = append(per, info.PacketsSent-before[info.ID])
	}
	t.Logf("bulk: %d of %d datagrams, packets per path %v, key updates client %d server %d", got.Load(), n, per, p.cliConn.Stats().KeyUpdates, p.srvConn.Stats().KeyUpdates)
	if len(per) != 2 || per[0] < n/10 || per[1] < n/10 {
		t.Fatalf("bulk did not use both paths: %v", per)
	}
	if p.cliConn.Stats().KeyUpdates == 0 || p.srvConn.Stats().KeyUpdates == 0 {
		t.Fatal("no key update across paths")
	}
	if err := p.srvConn.Err(); err != nil {
		t.Fatal(err)
	}
}
