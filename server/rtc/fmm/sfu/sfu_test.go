package sfu

import (
	"errors"
	"slices"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/frag"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

type fakeT struct {
	dgs [][]byte
	ctl []wire.Message
	r   wire.Reader
}

func (f *fakeT) SendDatagram(p []byte) error {
	f.dgs = append(f.dgs, slices.Clone(p))
	return nil
}

func (f *fakeT) MaxDatagramSize() int { return 1200 }

func (f *fakeT) SendControl(p []byte) error {
	f.r.Write(p)
	for {
		m, err := f.r.Next()
		if err != nil || m == nil {
			return err
		}
		f.ctl = append(f.ctl, m)
	}
}

func (f *fakeT) take() []wire.Message {
	var out []wire.Message
	for _, m := range f.ctl {
		if m.Type() != wire.MsgReport && m.Type() != wire.MsgBitrateHint {
			out = append(out, m)
		}
	}
	f.ctl = nil
	return out
}

func (f *fakeT) media() []wire.Media {
	var out []wire.Media
	for _, b := range f.dgs {
		if m, err := wire.ParseMedia(b); err == nil {
			out = append(out, m)
		}
	}
	f.dgs = nil
	return out
}

func ctl(t *testing.T, p *Peer, m wire.Message, now int64) {
	b, err := wire.Append(nil, m)
	if err != nil {
		t.Fatal(err)
	}
	p.HandleControl(b, now)
}

func admit(ticket string) (string, error) {
	if ticket == "bad" {
		return "", errors.New("no")
	}
	return ticket, nil
}

func joined(t *testing.T, r *Room, name string) (*Peer, *fakeT) {
	ft := &fakeT{}
	p := r.Connect(ft)
	ctl(t, p, wire.Join{Ticket: name}, 0)
	if len(ft.ctl) == 0 || ft.ctl[0] != (wire.Joined{Peer: name}) {
		t.Fatalf("join %s: %+v", name, ft.ctl)
	}
	ft.ctl = ft.ctl[1:]
	return p, ft
}

func TestAdmissionAndTrackAnnouncements(t *testing.T) {
	r := NewRoom(Config{Admit: admit, MaxPeers: 2})
	bad := &fakeT{}
	bp := r.Connect(bad)
	ctl(t, bp, wire.Join{Ticket: "bad"}, 0)
	if m := bad.take(); len(m) != 1 || m[0].(wire.Error).Code != ErrCodeAdmission {
		t.Fatalf("%+v", m)
	}
	early := &fakeT{}
	ctl(t, r.Connect(early), wire.Publish{Stream: 1, Kind: wire.TrackAudio, Layers: 1}, 0)
	if m := early.take(); len(m) != 1 || m[0].(wire.Error).Code != ErrCodeAdmission {
		t.Fatal("published before joining")
	}
	a, at := joined(t, r, "a")
	ctl(t, a, wire.Publish{Stream: 1, Kind: wire.TrackAudio, Layers: 1, Name: "mic"}, 0)
	b, bt := joined(t, r, "b")
	if m := bt.take(); len(m) != 1 || m[0].(wire.TrackAdded).Peer != "a" {
		t.Fatalf("late joiner did not learn the existing track: %+v", m)
	}
	ctl(t, b, wire.Publish{Stream: 1, Kind: wire.TrackVideo, Layers: 3, Name: "cam"}, 0)
	if m := at.take(); len(m) != 1 || m[0].(wire.TrackAdded).Kind != wire.TrackVideo {
		t.Fatalf("%+v", m)
	}
	ctl(t, b, wire.Publish{Stream: 1, Kind: wire.TrackAudio, Layers: 1}, 0)
	if m := bt.take(); len(m) != 1 || m[0].(wire.Error).Code != ErrCodeTrack {
		t.Fatal("a stream id was published twice")
	}
	full := &fakeT{}
	ctl(t, r.Connect(full), wire.Join{Ticket: "c"}, 0)
	if m := full.take(); len(m) != 1 || m[0].(wire.Error).Code != ErrCodeFull {
		t.Fatal("room over its limit")
	}
	dup := &fakeT{}
	ctl(t, r.Connect(dup), wire.Join{Ticket: "a"}, 0)
	if m := dup.take(); len(m) != 1 || m[0].(wire.Error).Code == 0 {
		t.Fatal("a peer id joined twice")
	}
	ctl(t, b, wire.Leave{}, 0)
	if m := at.take(); len(m) != 1 || m[0].Type() != wire.MsgTrackRemoved {
		t.Fatalf("leaving did not remove the track: %+v", m)
	}
	if got := r.Peers(); len(got) != 1 || got[0] != "a" {
		t.Fatal(got)
	}
}

func sendFrames(t *testing.T, p *Peer, pk *frag.Packetizer, n int, start int64, key bool) int64 {
	pattern := []int{0, 2, 1, 2}
	now := start
	for i := range n {
		layer := pattern[i%4]
		pkts, err := pk.Packetize(make([]byte, 300), key && i == 0, layer, false, uint32(now))
		if err != nil {
			t.Fatal(err)
		}
		for _, b := range pkts {
			p.HandleDatagram(b, now)
		}
		now += 33_333
		p.room.Tick(now)
	}
	return now
}

func TestSubscribersWaitForAKeyframeAndSeeNoGapsFromDroppedLayers(t *testing.T) {
	r := NewRoom(Config{Admit: admit})
	pub, pt := joined(t, r, "pub")
	ctl(t, pub, wire.Publish{Stream: 7, Kind: wire.TrackVideo, Layers: 3}, 0)
	sub, st := joined(t, r, "sub")
	added := st.take()[0].(wire.TrackAdded)
	pk := frag.NewPacketizer(7, 400)
	now := sendFrames(t, pub, pk, 4, 0, true)
	ctl(t, sub, wire.Subscribe{Stream: added.Stream, MaxLayer: 1}, now)
	if m := pt.take(); len(m) != 1 || m[0] != (wire.KeyframeRequest{Stream: 7}) {
		t.Fatalf("subscribing did not ask the publisher for a keyframe: %+v", m)
	}
	now = sendFrames(t, pub, pk, 4, now, false)
	if got := st.media(); len(got) != 0 {
		t.Fatalf("forwarded %d packets before a keyframe", len(got))
	}
	now = sendFrames(t, pub, pk, 12, now, true)
	for range 20 {
		now += 5_000
		r.Tick(now)
	}
	got := st.media()
	if len(got) == 0 || !got[0].Keyframe {
		t.Fatal("stream did not start on the keyframe")
	}
	for i, m := range got {
		if m.Layer > 1 {
			t.Fatalf("forwarded layer %d above the subscriber's maximum", m.Layer)
		}
		if m.Stream != added.Stream {
			t.Fatal("stream id not rewritten")
		}
		if i > 0 && m.Seq != got[i-1].Seq+1 {
			t.Fatalf("sequence gap %d -> %d after dropping a layer", got[i-1].Seq, m.Seq)
		}
	}
	ctl(t, sub, wire.KeyframeRequest{Stream: added.Stream}, now)
	ctl(t, sub, wire.KeyframeRequest{Stream: added.Stream}, now+1000)
	if m := pt.take(); len(m) != 1 {
		t.Fatalf("keyframe requests not rate limited: %d", len(m))
	}
}

func TestLayersAndPausesFollowTheSubscriberBudget(t *testing.T) {
	r := NewRoom(Config{Admit: admit, StartBitrate: 120_000, MaxBitrate: 120_000})
	pub, _ := joined(t, r, "pub")
	ctl(t, pub, wire.Publish{Stream: 1, Kind: wire.TrackVideo, Layers: 3}, 0)
	sub, st := joined(t, r, "sub")
	added := st.take()[0].(wire.TrackAdded)
	ctl(t, sub, wire.Subscribe{Stream: added.Stream, MaxLayer: 2}, 0)
	pk := frag.NewPacketizer(1, 1000)
	sizes := []int{1000, 333, 500, 333}
	now := int64(0)
	for i := range 120 {
		pkts, _ := pk.Packetize(make([]byte, sizes[i%4]), i == 0, []int{0, 2, 1, 2}[i%4], false, uint32(now))
		for _, b := range pkts {
			pub.HandleDatagram(b, now)
		}
		now += 33_333
		r.Tick(now)
	}
	f := sub.fwds[added.Stream]
	if f.want != 1 || f.paused {
		t.Fatalf("budget 120 kbps over TL0 %.0f, TL0+1 %.0f, all %.0f: want %d paused %v",
			f.src.encs[0].meter.upTo(0), f.src.encs[0].meter.upTo(1), f.src.encs[0].meter.upTo(2), f.want, f.paused)
	}
	for i := range 90 {
		pkts, _ := pk.Packetize(make([]byte, 4*sizes[i%4]), false, []int{0, 2, 1, 2}[i%4], false, uint32(now))
		for _, b := range pkts {
			pub.HandleDatagram(b, now)
		}
		now += 33_333
		r.Tick(now)
	}
	if !f.paused || f.Pauses == 0 {
		t.Fatalf("TL0 at %.0f bps over a 120 kbps budget did not pause the video", f.src.encs[0].meter.upTo(0))
	}
}

func FuzzPeerInput(f *testing.F) {
	pk := frag.NewPacketizer(1, 100)
	pkts, _ := pk.Packetize(make([]byte, 250), true, 0, false, 0)
	sub, _ := wire.Append(nil, wire.Subscribe{Stream: 1, MaxLayer: 2})
	f.Add(pkts[0], sub)
	ack, _ := wire.Append(nil, wire.Ack{Stream: 1, Frames: []uint16{0}})
	rec, _ := wire.Append(nil, wire.Recovery{Stream: 1, Frame: 1})
	enc, _ := wire.Append(nil, wire.Publish{Stream: 5, Kind: wire.TrackVideo, Layers: 1, Track: 1, Rank: 0})
	f.Add(wire.AppendFeedback(nil, append(ack, rec...)), append(enc, rec...))
	f.Fuzz(func(t *testing.T, dg, control []byte) {
		r := NewRoom(Config{Admit: admit})
		a := r.Connect(&fakeT{})
		b := r.Connect(&fakeT{})
		ctl(t, a, wire.Join{Ticket: "a"}, 0)
		ctl(t, b, wire.Join{Ticket: "b"}, 0)
		ctl(t, a, wire.Publish{Stream: 1, Kind: wire.TrackVideo, Layers: 3}, 0)
		ctl(t, b, wire.Subscribe{Stream: 1, MaxLayer: 2}, 0)
		a.HandleControl(control, 1)
		b.HandleControl(control, 1)
		a.HandleDatagram(dg, 2)
		b.HandleDatagram(dg, 2)
		for _, p := range pkts {
			a.HandleDatagram(p, 3)
		}
		r.Tick(10_000)
	})
}

func sendLongTerm(t *testing.T, p *Peer, pk *frag.Packetizer, n int, start int64, key bool) int64 {
	now := start
	for i := range n {
		pkts, err := pk.PacketizeFrame(make([]byte, 300), frag.Opts{Keyframe: key && i == 0, Ref: -1, LongTerm: true}, uint32(now))
		if err != nil {
			t.Fatal(err)
		}
		for _, b := range pkts {
			p.HandleDatagram(b, now)
		}
		now += 33_333
		p.room.Tick(now)
	}
	return now
}

func only[T wire.Message](ms []wire.Message) []T {
	var out []T
	for _, m := range ms {
		if v, ok := m.(T); ok {
			out = append(out, v)
		}
	}
	return out
}

func TestAcksAreIntersectedAndRecoveryIsAggregated(t *testing.T) {
	r := NewRoom(Config{Admit: admit})
	pub, pt := joined(t, r, "pub")
	ctl(t, pub, wire.Publish{Stream: 1, Kind: wire.TrackVideo, Layers: 1}, 0)
	a, at := joined(t, r, "a")
	b, bt := joined(t, r, "b")
	da := at.take()[0].(wire.TrackAdded).Stream
	db := bt.take()[0].(wire.TrackAdded).Stream
	ctl(t, a, wire.Subscribe{Stream: da, MaxLayer: 0}, 0)
	ctl(t, b, wire.Subscribe{Stream: db, MaxLayer: 0}, 0)
	pk := frag.NewPacketizer(1, 1000)
	now := sendLongTerm(t, pub, pk, 6, 0, true)
	ctl(t, a, wire.Ack{Stream: da, Frames: []uint16{0, 1, 2, 3, 4}}, now)
	ctl(t, b, wire.Ack{Stream: db, Frames: []uint16{0, 1, 2}}, now)
	pt.take()
	now += ackEveryUs
	r.Tick(now)
	if acks := only[wire.Ack](pt.take()); len(acks) != 1 || acks[0].Frames[0] != 2 {
		t.Fatalf("publisher was not told the newest frame both subscribers decoded: %+v", acks)
	}
	ctl(t, a, wire.Recovery{Stream: da, Frame: 5}, now)
	ctl(t, b, wire.Recovery{Stream: db, Frame: 4}, now+1000)
	got := pt.take()
	recs := only[wire.Recovery](got)
	if len(recs) != 1 || recs[0] != (wire.Recovery{Stream: 1, Frame: 2}) || len(only[wire.KeyframeRequest](got)) != 0 {
		t.Fatalf("want one recovery from frame 2 and no keyframe request, got %+v", got)
	}
	ctl(t, b, wire.Recovery{Stream: db, Frame: 4}, now+2_000_000)
	if keys := only[wire.KeyframeRequest](pt.take()); len(keys) != 1 {
		t.Fatal("a recovery request that repeats after the recovery should have landed did not fall back to a keyframe")
	}
	c, ct := joined(t, r, "c")
	dc := ct.take()[0].(wire.TrackAdded).Stream
	ctl(t, c, wire.Subscribe{Stream: dc, MaxLayer: 0}, now+3_000_000)
	pt.take()
	now = sendLongTerm(t, pub, pk, 2, now+3_000_000, true)
	ctl(t, a, wire.Recovery{Stream: da, Frame: 20}, now+keyframeGapUs)
	if keys := only[wire.KeyframeRequest](pt.take()); len(keys) != 1 {
		t.Fatal("with no frame every subscriber holds, recovery did not fall back to a keyframe")
	}
}

func TestSwitchingEncodingsKeepsSequencesAndFrameIDsContinuous(t *testing.T) {
	r := NewRoom(Config{Admit: admit})
	pub, pt := joined(t, r, "pub")
	ctl(t, pub, wire.Publish{Stream: 1, Kind: wire.TrackVideo, Layers: 1, Track: 1, Rank: 2}, 0)
	ctl(t, pub, wire.Publish{Stream: 2, Kind: wire.TrackVideo, Layers: 1, Track: 1, Rank: 1}, 0)
	ctl(t, pub, wire.Publish{Stream: 3, Kind: wire.TrackVideo, Layers: 1, Track: 1, Rank: 0}, 0)
	ctl(t, pub, wire.Publish{Stream: 4, Kind: wire.TrackVideo, Layers: 1, Track: 1, Rank: 1}, 0)
	if m := only[wire.Error](pt.take()); len(m) != 1 {
		t.Fatal("a second encoding with the same rank was accepted")
	}
	sub, st := joined(t, r, "sub")
	added := st.take()
	if len(added) != 1 {
		t.Fatalf("encodings were announced as separate tracks: %+v", added)
	}
	down := added[0].(wire.TrackAdded).Stream
	ctl(t, sub, wire.Subscribe{Stream: down, MaxLayer: 0}, 0)
	hi, lo := frag.NewPacketizer(1, 1000), frag.NewPacketizer(2, 1000)
	for range 40 {
		lo.Packetize([]byte{1}, true, 0, false, 0)
	}
	now := sendLongTerm(t, pub, hi, 3, 0, true)
	f := sub.fwds[down]
	f.target = pub.encs[2]
	now = sendLongTerm(t, pub, hi, 1, now, false)
	sendLongTerm(t, pub, lo, 3, now, true)
	got := st.media()
	if len(got) != 7 {
		t.Fatalf("forwarded %d packets, want 7", len(got))
	}
	for i, m := range got {
		if m.Seq != got[0].Seq+uint16(i) || m.FrameID != got[0].FrameID+uint16(i) || m.Stream != down {
			t.Fatalf("packet %d: seq %d frame %d stream %d after the switch", i, m.Seq, m.FrameID, m.Stream)
		}
	}
	if !got[4].Keyframe || f.cur != pub.encs[2] || f.Switches != 1 {
		t.Fatal("the switch did not happen on the lower encoding's keyframe")
	}
	ctl(t, sub, wire.Ack{Stream: down, Frames: []uint16{got[5].FrameID}}, now)
	if len(f.acked) != 1 || f.acked[0] != 41 {
		t.Fatalf("an acknowledgement was not mapped back to the encoding's own frame id: %v", f.acked)
	}
}

func TestFaceModeRoutesEverySubscriberToThePrimary(t *testing.T) {
	r := NewRoom(Config{Admit: admit})
	pub, _ := joined(t, r, "pub")
	ctl(t, pub, wire.Publish{Stream: 1, Kind: wire.TrackVideo, Layers: 3, Track: 1, Rank: 1}, 0)
	ctl(t, pub, wire.Publish{Stream: 2, Kind: wire.TrackVideo, Layers: 1, Track: 1, Rank: 0}, 0)
	sub, st := joined(t, r, "sub")
	down := st.take()[0].(wire.TrackAdded).Stream
	ctl(t, sub, wire.Subscribe{Stream: down, MaxLayer: 2}, 0)
	pk := frag.NewPacketizer(1, 1000)
	now := int64(0)
	for range 5 {
		pkts, _ := pk.PacketizeFrame(make([]byte, 60), frag.Opts{Face: true, Independent: true, Ref: -1}, uint32(now))
		pub.HandleDatagram(pkts[0], now)
		now += 33_333
		r.Tick(now)
	}
	f := sub.fwds[down]
	if opts := f.options(now); len(opts) != 1 || opts[0].e != pub.encs[1] {
		t.Fatalf("face mode on the primary did not make it the only option: %+v", opts)
	}
	if got := st.media(); len(got) != 5 || !got[0].Face {
		t.Fatalf("face frames did not start the stream without a keyframe: %d", len(got))
	}
	f.share = 50_000
	pkts, _ := pk.Packetize(make([]byte, 600), true, 0, false, uint32(now))
	pub.HandleDatagram(pkts[0], now)
	if opts := f.options(now); len(opts) != 1 || opts[0].e != pub.encs[2] {
		t.Fatalf("a subscriber at 50 kbps was not offered only the face encoding once the primary sent video: %+v", opts)
	}
	f.share = 400_000
	if opts := f.options(now); len(opts) != 3 || opts[0].e != pub.encs[1] {
		t.Fatalf("a subscriber at 400 kbps was not offered video: %+v", opts)
	}
}
