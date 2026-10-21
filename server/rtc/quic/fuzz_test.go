package quic

import (
	"bytes"
	"net/netip"
	"sync"
	"testing"
	"time"
)

func frameSeeds() [][]byte {
	var seeds [][]byte
	seeds = append(seeds, appendAckFrame(nil, []ackRange{{10, 20}, {2, 5}, {0, 0}}, 30))
	seeds = append(seeds, appendStreamHeader(nil, 4, 100, 3, true, true), append(appendStreamHeader(nil, 0, 0, 5, false, true), "hello"...))
	seeds = append(seeds, appendCryptoHeader(nil, 0, 2), appendResetStream(nil, 4, 1, 99))
	seeds = append(seeds, appendNewConnectionID(nil, 1, 0, []byte{1, 2, 3, 4, 5, 6, 7, 8}, [16]byte{1}))
	seeds = append(seeds, appendConnectionClose(nil, false, 0x0a, 0x08, "bad"), appendConnectionClose(nil, true, 7, 0, "app"))
	seeds = append(seeds, appendDatagramFrame(nil, []byte("media")), []byte{0x30, 1, 2, 3})
	seeds = append(seeds, []byte{0x1a, 1, 2, 3, 4, 5, 6, 7, 8}, []byte{0x1b, 1, 2, 3, 4, 5, 6, 7, 8})
	seeds = append(seeds, appendOneVarintFrame(nil, frameMaxData, 1<<20), appendTwoVarintFrame(nil, frameMaxStreamData, 4, 1<<20))
	seeds = append(seeds, appendOneVarintFrame(nil, frameMaxStreamsBidi, 10), appendOneVarintFrame(nil, frameRetireConnectionID, 0))
	seeds = append(seeds, []byte{0x07, 3, 'a', 'b', 'c'}, []byte{0x1e}, []byte{0x01, 0x00, 0x00})
	seeds = append(seeds, []byte{0x03, 5, 0, 1, 0, 1, 2, 3}, []byte{0x02, 0x40, 0x10, 0, 0x3f, 0x10})
	return seeds
}

func FuzzParseFrames(f *testing.F) {
	for _, s := range frameSeeds() {
		f.Add(s)
	}
	f.Fuzz(func(t *testing.T, b []byte) {
		var fr frame
		rest := b
		for len(rest) > 0 {
			n, err := parseFrame(rest, &fr)
			if err != nil {
				return
			}
			if n <= 0 || n > len(rest) {
				t.Fatalf("parseFrame consumed %d of %d", n, len(rest))
			}
			if len(fr.data) > len(rest) {
				t.Fatal("frame data beyond input")
			}
			if fr.typ == frameAck || fr.typ == frameAckECN {
				prev := uint64(1<<63 - 1)
				for i, r := range fr.ranges {
					if r.smallest > r.largest || (i > 0 && r.largest+1 >= prev) {
						t.Fatalf("bad ack ranges %+v", fr.ranges)
					}
					prev = r.smallest
				}
			}
			rest = rest[n:]
		}
	})
}

func FuzzParsePacketHeaders(f *testing.F) {
	f.Add(rfcClientInitialProtected)
	f.Add(unhex("ff000000010008f067a5502a4262b5746f6b656e04a265ba2eff4d829058fb3f0f2496ba"))
	f.Add(unhex("4cfe4189655e5cd55c41f69080575d7999c25a5bfb"))
	f.Add(unhex("80000000000101ff00000001"))
	f.Fuzz(func(t *testing.T, b []byte) {
		if inv, ok := parseInvariant(b, localCIDLen); ok {
			if len(inv.dcid) > len(b) || len(inv.scid) > len(b) {
				t.Fatal("cid beyond input")
			}
		}
		if len(b) > 0 && isLongHeader(b[0]) {
			h, err := parseLongHeader(b)
			if err == nil && h.typ != packetVersionNegotiation {
				if h.pnOffset > len(b) || h.end > len(b) || h.end < h.pnOffset {
					t.Fatalf("header offsets %d %d of %d", h.pnOffset, h.end, len(b))
				}
			}
			parseVersionNegotiation(b)
			validRetryTag([]byte{1, 2, 3, 4, 5, 6, 7, 8}, b)
		}
		var largest int64 = -1
		for n := 1; n <= 4 && n <= len(b); n++ {
			pn := decodePacketNumber(largest, readTruncatedPN(b, n), n)
			if pn < 0 {
				t.Fatalf("negative packet number %d", pn)
			}
		}
	})
}

func FuzzTransportParams(f *testing.F) {
	p := defaultTransportParams()
	p.initialSCID, p.hasInitialSCID = []byte{1, 2, 3}, true
	p.originalDCID, p.hasOriginalDCID = []byte{4, 5, 6, 7, 8, 9, 10, 11}, true
	p.statelessResetToken = make([]byte, 16)
	p.maxDatagramFrameSize = 1200
	p.initialMaxData = 1 << 20
	f.Add(p.marshal(true), true)
	f.Add(p.marshal(false), false)
	f.Fuzz(func(t *testing.T, b []byte, fromServer bool) {
		got, err := parseTransportParams(b, fromServer)
		if err != nil {
			return
		}
		again, err := parseTransportParams(got.marshal(fromServer), fromServer)
		if err != nil {
			t.Fatalf("re-parse of marshalled params: %v", err)
		}
		if again.initialMaxData != got.initialMaxData || !bytes.Equal(again.initialSCID, got.initialSCID) || again.maxDatagramFrameSize != got.maxDatagramFrameSize {
			t.Fatal("params changed across a round trip")
		}
	})
}

var (
	fuzzPairMu sync.Mutex
	fuzzPair   *pair
)

type fuzzTB struct{ testing.TB }

func (fuzzTB) Cleanup(func()) {}

func livePair(t *testing.T) *pair {
	fuzzPairMu.Lock()
	defer fuzzPairMu.Unlock()
	if fuzzPair != nil {
		select {
		case <-fuzzPair.srvConn.Done():
			fuzzPair.server.Close()
			fuzzPair.client.Close()
			fuzzPair = nil
		default:
		}
	}
	if fuzzPair == nil {
		fuzzPair = newPair(fuzzTB{t}, testServerConfig(t), testClientConfig(t), nil)
	}
	return fuzzPair
}

func FuzzConnFrames(f *testing.F) {
	for _, s := range frameSeeds() {
		f.Add(s)
	}
	f.Add(append(appendStreamHeader(nil, 0, 1<<40, 1, false, true), 'x'))
	f.Add(append(appendStreamHeader(nil, 3, 0, 1, false, true), 'x'))
	f.Add(appendResetStream(nil, 0, 1, 1<<50))
	f.Add(appendTwoVarintFrame(nil, frameStopSending, 2, 1))
	f.Fuzz(func(t *testing.T, payload []byte) {
		if len(payload) == 0 {
			return
		}
		p := livePair(t)
		c := p.srvConn
		c.mu.Lock()
		if c.state < stateClosing {
			sp := c.spaces[spaceApp]
			pn := sp.largestRecv + 1
			var dcid []byte
			if len(c.localCIDs) > 0 {
				dcid = c.localCIDs[0].cid
			}
			c.processPayload(spaceApp, packet1RTT, pn, append([]byte(nil), payload...), c.active.addr, dcid, time.Now())
			c.flush(time.Now())
		}
		c.mu.Unlock()
		c.wake()
	})
}

func FuzzEndpointDatagram(f *testing.F) {
	f.Add(rfcClientInitialProtected)
	f.Add(pad(1200, unhex("c0000000010800000000000000000000")))
	f.Add(unhex("4cfe4189655e5cd55c41f69080575d7999c25a5bfb"))
	srvOnce := sync.OnceValue(func() *Endpoint {
		e, err := Listen("udp4", "127.0.0.1:0", testServerConfig(f))
		if err != nil {
			panic(err)
		}
		return e
	})
	from := netip.MustParseAddrPort("127.0.0.1:9")
	f.Fuzz(func(t *testing.T, b []byte) {
		if len(b) > readBufferSize {
			return
		}
		e := srvOnce()
		bp := e.getBuf()
		n := copy(*bp, b)
		e.handle(bp, n, from)
	})
}

func TestRangeSetMatchesModel(t *testing.T) {
	var s rangeSet
	model := make([]bool, 200)
	seed := uint32(1)
	next := func() int {
		seed = seed*1103515245 + 12345
		return int(seed>>16) % 200
	}
	for i := 0; i < 5000; i++ {
		a, b := next(), next()
		if a > b {
			a, b = b, a
		}
		switch next() % 3 {
		case 0, 1:
			s.add(uint64(a), uint64(b))
			for x := a; x < b; x++ {
				model[x] = true
			}
		default:
			s.remove(uint64(a), uint64(b))
			for x := a; x < b; x++ {
				model[x] = false
			}
		}
		for x := 0; x < 200; x++ {
			if s.contains(uint64(x)) != model[x] {
				t.Fatalf("step %d: contains(%d) = %v, model %v, set %v", i, x, !model[x], model[x], s)
			}
		}
		for j := 1; j < len(s); j++ {
			if s[j-1].end >= s[j].start {
				t.Fatalf("ranges not disjoint and separated: %v", s)
			}
		}
	}
}

func TestRecvBufReassembly(t *testing.T) {
	var r recvBuf
	msg := []byte("the quick brown fox jumps over the lazy dog")
	order := []int{30, 5, 0, 17, 40, 10, 25, 35, 20, 15}
	for _, off := range order {
		end := off + 5
		if end > len(msg) {
			end = len(msg)
		}
		r.write(uint64(off), msg[off:end])
		r.write(uint64(off), msg[off:end])
	}
	out := make([]byte, 100)
	n := r.read(out)
	if string(out[:n]) != string(msg) {
		t.Fatalf("reassembled %q", out[:n])
	}
	var g recvBuf
	ok := true
	for i := 0; i < maxRecvGaps+10 && ok; i++ {
		ok = g.write(uint64(2*i+1), []byte{1})
	}
	if ok {
		t.Fatal("fragmentation cap not enforced")
	}
}

func TestSendBufAckAndLoss(t *testing.T) {
	var s sendBuf
	s.write(make([]byte, 1000))
	s.sent = 1000
	s.onAck(200, 400, false)
	s.onLost(0, 600, false)
	if len(s.lost) != 2 || s.lost[0] != (span{0, 200}) || s.lost[1] != (span{400, 600}) {
		t.Fatalf("lost ranges %v", s.lost)
	}
	s.onAck(0, 200, false)
	if s.base != 400 || len(s.data) != 600 {
		t.Fatalf("base %d len %d", s.base, len(s.data))
	}
	s.fin = true
	s.onAck(400, 1000, true)
	if !s.allAcked() {
		t.Fatal("stream not fully acknowledged")
	}
}
