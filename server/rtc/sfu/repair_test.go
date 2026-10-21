package sfu

import (
	"bytes"
	"crypto/rand"
	"encoding/binary"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/rtp"
	"forkmesh.com/mirror-node/rtc/srtp"
)

type testPeer struct {
	t    *testing.T
	p    *Participant
	send *srtp.Context
	recv *srtp.Context
	got  [][]byte
}

func newTestRoom() *Room {
	server := &Server{cfg: Config{VideoBitrate: defaultVideoBitrate, MaxParticipants: MaxParticipants}, epoch: time.Now(), rooms: map[string]*Room{}}
	return &Room{server: server, id: "room", participants: map[string]*Participant{}, used: map[uint32]bool{}}
}

func testContexts(t *testing.T) (*srtp.Context, *srtp.Context) {
	key, salt := make([]byte, 16), make([]byte, 12)
	rand.Read(key)
	rand.Read(salt)
	a, err := srtp.NewContext(srtp.ProfileAEADAES128GCM, key, salt)
	if err != nil {
		t.Fatal(err)
	}
	b, _ := srtp.NewContext(srtp.ProfileAEADAES128GCM, key, salt)
	return a, b
}

func addTestPeer(t *testing.T, r *Room, id string) *testPeer {
	peer := &testPeer{t: t}
	inbound, send := testContexts(t)
	outbound, recv := testContexts(t)
	p := &Participant{
		room: r, id: id, out: make(chan outgoing, sendQueue), sendSSRC: randomSSRC(), negotiated: map[*Track]bool{},
		lastKey: map[uint32]time.Time{}, arrivals: &arrivals{}, downlink: newDownlink(), down: map[*Track]*downstream{},
		inbound: inbound, outbound: outbound, ready: true, tccID: 3, tccDown: true,
	}
	p.write = func(b []byte) error {
		peer.got = append(peer.got, append([]byte(nil), b...))
		return nil
	}
	peer.p, peer.send, peer.recv = p, send, recv
	r.participants[id] = p
	r.order = append(r.order, id)
	return peer
}

func (peer *testPeer) addVideo(ssrc, rtx uint32) *Track {
	track := &Track{owner: peer.p, kind: "video", ssrc: ssrc, id: "video", payloadType: 96, rtxSSRC: rtx, rtxPayloadType: 97,
		cache: &packetCache{}, stats: newReceiveStats("video")}
	peer.p.tracks = append(peer.p.tracks, track)
	return track
}

func (peer *testPeer) addAudio(ssrc uint32) *Track {
	track := &Track{owner: peer.p, kind: "audio", ssrc: ssrc, id: "audio", payloadType: 111, stats: newReceiveStats("audio")}
	peer.p.tracks = append(peer.p.tracks, track)
	return track
}

func (peer *testPeer) receive(track *Track, rtx uint32) {
	peer.p.negotiated[track] = true
	peer.p.down[track] = &downstream{rtxSSRC: rtx, rtx: rtx != 0}
}

func (peer *testPeer) rtp(r *Room, packet rtp.Packet, now time.Time) {
	sealed, err := peer.send.EncryptRTP(nil, packet.Marshal(nil))
	if err != nil {
		peer.t.Fatal(err)
	}
	r.rtpLocked(peer.p, sealed, now)
}

func (peer *testPeer) rtcp(r *Room, packet []byte, now time.Time) {
	sealed, err := peer.send.EncryptRTCP(nil, packet)
	if err != nil {
		peer.t.Fatal(err)
	}
	r.rtcpLocked(peer.p, sealed, now)
}

type received struct {
	header rtp.Header
	raw    []byte
}

func (peer *testPeer) take() (media []received, control [][]byte) {
	for _, sealed := range peer.got {
		if rtp.IsRTCP(sealed) {
			plain, err := peer.recv.DecryptRTCP(nil, sealed)
			if err != nil {
				peer.t.Fatalf("rtcp does not decrypt: %v", err)
			}
			parts, err := rtp.SplitCompound(plain)
			if err != nil {
				peer.t.Fatal(err)
			}
			control = append(control, parts...)
			continue
		}
		plain, err := peer.recv.DecryptRTP(nil, sealed)
		if err != nil {
			peer.t.Fatalf("rtp does not decrypt: %v", err)
		}
		header, err := rtp.ParseHeader(plain)
		if err != nil {
			peer.t.Fatal(err)
		}
		media = append(media, received{header: header, raw: plain})
	}
	peer.got = nil
	return media, control
}

func frame(seq uint16, transport uint16) rtp.Packet {
	return rtp.Packet{PayloadType: 96, SequenceNumber: seq, Timestamp: uint32(seq) * 3000, SSRC: 1000,
		TransportID: 3, TransportSeq: transport, Payload: []byte{0x90, 0x80, byte(seq), 'f', 'r', 'a', 'm', 'e'}}
}

func feedbackOf(control [][]byte, typ, format uint8) [][]byte {
	var out [][]byte
	for _, part := range control {
		if rtp.PacketType(part) == typ && rtp.Count(part) == format {
			out = append(out, part)
		}
	}
	return out
}

func rembBitrate(p []byte) uint64 {
	exp := p[17] >> 2
	mantissa := uint64(p[17]&3)<<16 | uint64(p[18])<<8 | uint64(p[19])
	return mantissa << exp
}

func TestANackIsRepairedFromTheCacheForThatReceiverOnly(t *testing.T) {
	r := newTestRoom()
	sender, ana, ben := addTestPeer(t, r, "s"), addTestPeer(t, r, "a"), addTestPeer(t, r, "b")
	track := sender.addVideo(1000, 1001)
	ana.receive(track, 5000)
	ben.receive(track, 6000)
	now := time.Now()
	for seq := uint16(10); seq < 20; seq++ {
		sender.rtp(r, frame(seq, seq), now)
	}
	for _, peer := range []*testPeer{ana, ben} {
		media, _ := peer.take()
		if len(media) != 10 {
			t.Fatalf("forwarded %d packets", len(media))
		}
		for i, m := range media {
			seq, ok := rtp.TransportSequence(m.header, 3)
			if m.header.SSRC != 1000 || m.header.PayloadType != 96 || m.header.SequenceNumber != uint16(10+i) || !ok || seq != uint16(i) {
				t.Fatalf("forwarded %+v transport %d %v", m.header, seq, ok)
			}
			if !bytes.Equal(rtp.Payload(m.raw, m.header), frame(uint16(10+i), 0).Payload) {
				t.Fatal("payload changed")
			}
		}
	}
	sender.take()
	ana.rtcp(r, rtp.NewNACK(77, 1000, []uint16{12, 15, 99}), now)
	media, _ := ana.take()
	if len(media) != 2 {
		t.Fatalf("%d retransmissions", len(media))
	}
	for i, want := range []uint16{12, 15} {
		h := media[i].header
		osn, payload, err := rtp.ParseRTX(rtp.Payload(media[i].raw, h))
		if err != nil || h.SSRC != 5000 || h.PayloadType != videoRTXPayloadType || h.SequenceNumber != uint16(i) || osn != want ||
			!bytes.Equal(payload, frame(want, 0).Payload) || h.Timestamp != uint32(want)*3000 {
			t.Fatalf("rtx %+v osn %d err %v", h, osn, err)
		}
		if _, ok := rtp.TransportSequence(h, 3); !ok {
			t.Fatal("rtx without a transport sequence")
		}
	}
	if media, control := ben.take(); len(media)+len(control) != 0 {
		t.Fatal("another receiver got the repair")
	}
	if media, control := sender.take(); len(media)+len(control) != 0 {
		t.Fatal("the nack went to the sender")
	}
	ana.rtcp(r, rtp.NewNACK(77, 1000, []uint16{12}), now.Add(time.Millisecond))
	if media, _ := ana.take(); len(media) != 0 {
		t.Fatal("the same packet was resent too soon")
	}
	sent := 1
	for i := 1; i < 10; i++ {
		ana.rtcp(r, rtp.NewNACK(77, 1000, []uint16{12}), now.Add(time.Duration(i)*50*time.Millisecond))
		media, _ := ana.take()
		sent += len(media)
	}
	if sent != resendTimes {
		t.Fatalf("packet resent %d times", sent)
	}
	ana.rtcp(r, rtp.NewNACK(77, 1000, []uint16{13}), now.Add(2*cacheAge))
	if media, _ := ana.take(); len(media) != 0 {
		t.Fatal("an expired packet was resent")
	}
}

func TestNackRepairsAreBoundedPerReceiver(t *testing.T) {
	r := newTestRoom()
	sender, ana := addTestPeer(t, r, "s"), addTestPeer(t, r, "a")
	track := sender.addVideo(1000, 1001)
	ana.receive(track, 5000)
	now := time.Now()
	for seq := uint16(0); seq < 400; seq++ {
		sender.rtp(r, frame(seq, seq), now)
	}
	ana.take()
	ana.p.downlink.estimate = 400_000
	total := 0
	for round := 0; round < 200; round++ {
		lost := make([]uint16, 64)
		for i := range lost {
			lost[i] = uint16((round*64 + i) % 400)
		}
		ana.rtcp(r, rtp.NewNACK(77, 1000, lost), now.Add(time.Duration(round)*time.Millisecond))
		media, _ := ana.take()
		for _, m := range media {
			total += len(m.raw)
		}
	}
	if limit := int(400_000*repairShare/8) + MaxPacketBytes; total > limit {
		t.Fatalf("a nack flood drew %d bytes of repair in a second, limit %d", total, limit)
	}
	if ana.p.repairNacks <= repairNackBudget || ana.p.repairCount > repairPacketBudget {
		t.Fatalf("budgets nacks %d packets %d", ana.p.repairNacks, ana.p.repairCount)
	}
}

func TestUpstreamLossIsNackedAndTheRecoveredPacketForwardedOnce(t *testing.T) {
	r := newTestRoom()
	sender, ana, ben := addTestPeer(t, r, "s"), addTestPeer(t, r, "a"), addTestPeer(t, r, "b")
	track := sender.addVideo(1000, 1001)
	ana.receive(track, 5000)
	ben.receive(track, 6000)
	now := time.Now()
	for i, seq := range []uint16{20, 21, 23} {
		sender.rtp(r, frame(seq, uint16(i)), now)
	}
	_, control := sender.take()
	nacks := feedbackOf(control, rtp.TypeRTPFB, rtp.FmtNACK)
	if len(nacks) != 1 {
		t.Fatalf("%d nacks to the sender", len(nacks))
	}
	if media, lost, _ := rtp.ParseNACK(nacks[0]); media != 1000 || len(lost) != 1 || lost[0] != 22 {
		t.Fatalf("nack for %d %v", media, lost)
	}
	ana.take()
	ben.take()
	retransmission := rtp.Packet{PayloadType: 97, SequenceNumber: 500, Timestamp: 22 * 3000, SSRC: 1001, TransportID: 3, TransportSeq: 3,
		Payload: rtp.RTXPayload(nil, 22, frame(22, 0).Payload)}
	sender.rtp(r, retransmission, now.Add(30*time.Millisecond))
	for _, peer := range []*testPeer{ana, ben} {
		media, _ := peer.take()
		if len(media) != 1 || media[0].header.SSRC != 1000 || media[0].header.SequenceNumber != 22 ||
			!bytes.Equal(rtp.Payload(media[0].raw, media[0].header), frame(22, 0).Payload) {
			t.Fatalf("recovered packet %+v", media)
		}
	}
	sender.rtp(r, frame(22, 4), now.Add(40*time.Millisecond))
	padding := rtp.Packet{PayloadType: 97, SequenceNumber: 501, SSRC: 1001, TransportID: 3, TransportSeq: 5, Padding: 200}
	sender.rtp(r, padding, now.Add(41*time.Millisecond))
	again := rtp.Packet{PayloadType: 97, SequenceNumber: 502, Timestamp: 21 * 3000, SSRC: 1001, TransportID: 3, TransportSeq: 6,
		Payload: rtp.RTXPayload(nil, 21, frame(21, 0).Payload)}
	sender.rtp(r, again, now.Add(42*time.Millisecond))
	for _, peer := range []*testPeer{ana, ben} {
		if media, _ := peer.take(); len(media) != 0 {
			t.Fatalf("a duplicate or padding was forwarded: %+v", media)
		}
	}
	r.feedbackLocked(sender.p, now.Add(time.Second))
	_, control = sender.take()
	reports := feedbackOf(control, rtp.TypeRR, 1)
	if len(reports) != 1 {
		t.Fatalf("%d receiver reports", len(reports))
	}
	blocks, err := rtp.ParseReportBlocks(reports[0])
	if err != nil || len(blocks) != 1 {
		t.Fatal(err)
	}
	if b := blocks[0]; b.SSRC != 1000 || b.HighestSeq != 23 || b.Lost != 1 || b.FractionLost != 64 {
		t.Fatalf("report block %+v", b)
	}
	transport := feedbackOf(control, rtp.TypeRTPFB, rtp.FmtTCC)
	if len(transport) != 1 {
		t.Fatalf("%d transport feedbacks", len(transport))
	}
	f, err := rtp.ParseTransportFeedback(transport[0])
	if err != nil {
		t.Fatal(err)
	}
	if f.BaseSequence != 0 || len(f.Statuses) != 7 || f.SenderSSRC != sender.p.sendSSRC || f.MediaSSRC != 1000 {
		t.Fatalf("transport feedback %+v", f)
	}
	for i, s := range f.Statuses {
		if !s.Received {
			t.Fatalf("packet %d reported lost", i)
		}
	}
}

func TestReceiverReportsCarryJitterAndSenderReportTiming(t *testing.T) {
	r := newTestRoom()
	sender := addTestPeer(t, r, "s")
	sender.addAudio(2000)
	now := time.Now()
	for seq := uint16(0); seq < 50; seq++ {
		at := now.Add(time.Duration(seq) * 20 * time.Millisecond)
		if seq%2 == 1 {
			at = at.Add(5 * time.Millisecond)
		}
		sender.rtp(r, rtp.Packet{PayloadType: 111, SequenceNumber: seq, Timestamp: uint32(seq) * 960, SSRC: 2000, Payload: []byte{1, 2, 3}}, at)
	}
	sr := make([]byte, 28)
	sr[0], sr[1], sr[3] = 0x80, rtp.TypeSR, 6
	binary.BigEndian.PutUint32(sr[4:8], 2000)
	binary.BigEndian.PutUint64(sr[8:16], 0x0102030405060708)
	sender.rtcp(r, sr, now.Add(time.Second))
	r.feedbackLocked(sender.p, now.Add(1500*time.Millisecond))
	_, control := sender.take()
	reports := feedbackOf(control, rtp.TypeRR, 1)
	if len(reports) != 1 {
		t.Fatalf("%d reports", len(reports))
	}
	blocks, _ := rtp.ParseReportBlocks(reports[0])
	b := blocks[0]
	if b.SSRC != 2000 || b.Lost != 0 || b.HighestSeq != 49 || b.LSR != 0x03040506 {
		t.Fatalf("block %+v", b)
	}
	if b.DLSR < 32000 || b.DLSR > 33600 {
		t.Fatalf("dlsr %d for half a second", b.DLSR)
	}
	if b.Jitter < 120 || b.Jitter > 240 {
		t.Fatalf("jitter %d for 5 ms alternating at 48 kHz", b.Jitter)
	}
}

func TestSendersAreCappedByTheSlowestReceiversShare(t *testing.T) {
	r := newTestRoom()
	sender, ana, ben := addTestPeer(t, r, "s"), addTestPeer(t, r, "a"), addTestPeer(t, r, "b")
	video := sender.addVideo(1000, 1001)
	audio := sender.addAudio(2000)
	other := ben.addVideo(3000, 3001)
	for _, track := range []*Track{video, audio} {
		ana.receive(track, 5000)
		ben.receive(track, 6000)
	}
	ana.receive(other, 7000)
	now := time.Now()
	ceiling := r.ceilingLocked()
	if ceiling != defaultVideoBitrate {
		t.Fatalf("ceiling %v for three people", ceiling)
	}
	r.capSenderLocked(sender.p, now, ceiling)
	_, control := sender.take()
	remb := feedbackOf(control, rtp.TypePSFB, rtp.FmtREMB)
	if len(remb) != 1 || rembBitrate(remb[0]) < 1_490_000 {
		t.Fatalf("uncapped remb %v", remb)
	}
	ana.p.downlink.estimate = 600_000
	r.capSenderLocked(sender.p, now.Add(10*time.Millisecond), ceiling)
	_, control = sender.take()
	remb = feedbackOf(control, rtp.TypePSFB, rtp.FmtREMB)
	if len(remb) != 1 || rembBitrate(remb[0]) > 300_000 || rembBitrate(remb[0]) < 295_000 {
		t.Fatalf("remb not cut to ana's share: %v", remb)
	}
	r.capSenderLocked(sender.p, now.Add(20*time.Millisecond), ceiling)
	if _, control = sender.take(); len(control) != 0 {
		t.Fatal("an unchanged cap was resent at once")
	}
	ana.p.downlink.estimate = 100_000
	for i := 0; i <= 12; i++ {
		r.adaptReceiverLocked(ana.p, now.Add(time.Duration(i)*100*time.Millisecond), ceiling)
	}
	if !ana.p.videoPaused {
		t.Fatal("video not paused for a receiver far below the floor")
	}
	sender.take()
	ana.take()
	ben.take()
	sender.rtp(r, frame(1, 1), now.Add(1300*time.Millisecond))
	sender.rtp(r, rtp.Packet{PayloadType: 111, SequenceNumber: 1, SSRC: 2000, TransportID: 3, TransportSeq: 2, Payload: []byte{9}}, now.Add(1300*time.Millisecond))
	if media, _ := ana.take(); len(media) != 1 || media[0].header.SSRC != 2000 {
		t.Fatalf("paused receiver got %+v", media)
	}
	if media, _ := ben.take(); len(media) != 2 {
		t.Fatalf("other receiver got %d", len(media))
	}
	r.capSenderLocked(sender.p, now.Add(1400*time.Millisecond), ceiling)
	_, control = sender.take()
	if remb = feedbackOf(control, rtp.TypePSFB, rtp.FmtREMB); len(remb) != 1 || rembBitrate(remb[0]) < 1_490_000 {
		t.Fatal("a paused receiver still caps the sender")
	}
	ana.p.downlink.estimate = 400_000
	r.adaptReceiverLocked(ana.p, now.Add(3*time.Second), ceiling)
	if ana.p.videoPaused {
		t.Fatal("video not resumed")
	}
	_, control = sender.take()
	if len(feedbackOf(control, rtp.TypePSFB, rtp.FmtPLI)) != 1 {
		t.Fatal("no keyframe request on resume")
	}
}

func TestTheCeilingFallsAsTheRoomGrows(t *testing.T) {
	r := newTestRoom()
	for i, want := range []float64{defaultVideoBitrate, defaultVideoBitrate, defaultVideoBitrate, defaultVideoBitrate, 1_000_000, 750_000, 600_000, 500_000} {
		if got := r.ceilingLocked(); got != want {
			t.Fatalf("%d people: ceiling %v want %v", i, got, want)
		}
		addTestPeer(t, r, string(rune('a'+i)))
	}
	for i := 8; i < MaxParticipants; i++ {
		addTestPeer(t, r, string(rune('a'+i)))
	}
	if got := r.ceilingLocked(); got != minCeiling {
		t.Fatalf("16 people: ceiling %v", got)
	}
}

func TestUpstreamNacksAreLimitedToAShareOfWhatArrives(t *testing.T) {
	stats := newReceiveStats("video")
	now := time.Now()
	epoch := now
	stats.arrived(stats.ext(0), 0, now, epoch, false)
	stats.arrived(stats.ext(200), 0, now, epoch, false)
	first := stats.nacks(now)
	if len(first) != nackFloor {
		t.Fatalf("first nack named %d packets", len(first))
	}
	if more := stats.nacks(now.Add(100 * time.Millisecond)); len(more) != 0 {
		t.Fatalf("the floor was exceeded within the second: %d", len(more))
	}
	for seq := uint16(201); seq < 601; seq++ {
		stats.arrived(stats.ext(seq), 0, now.Add(200*time.Millisecond), epoch, false)
	}
	later := stats.nacks(now.Add(300 * time.Millisecond))
	if len(later) != 402/nackShare-nackFloor {
		t.Fatalf("after 400 arrivals a nack named %d packets", len(later))
	}
	for _, seq := range first {
		if seq < 1 || seq > 199 {
			t.Fatalf("nacked %d", seq)
		}
	}
}
