package fmm

import (
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/frag"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

type capture struct {
	dgs [][]byte
	ctl []wire.Message
	r   wire.Reader
}

func (c *capture) SendDatagram(p []byte) error {
	c.dgs = append(c.dgs, append([]byte(nil), p...))
	return nil
}

func (c *capture) MaxDatagramSize() int { return 1200 }

func (c *capture) SendControl(p []byte) error {
	c.r.Write(p)
	for {
		m, err := c.r.Next()
		if err != nil || m == nil {
			return err
		}
		c.ctl = append(c.ctl, m)
	}
}

func control(t *testing.T, c *Client, m wire.Message, now int64) {
	b, err := wire.Append(nil, m)
	if err != nil {
		t.Fatal(err)
	}
	c.HandleControl(b, now)
}

func TestClientPublishesSubscribesAndHandsOutFrames(t *testing.T) {
	tr := &capture{}
	c := NewClient(tr, &Config{StartBitrate: 500_000})
	if err := c.Join("ticket", 0); err != nil {
		t.Fatal(err)
	}
	control(t, c, wire.Joined{Peer: "me"}, 0)
	if e := c.PollEvent(); e == nil || e.Kind != EventJoined || c.PeerID() != "me" {
		t.Fatalf("%+v", e)
	}
	audio, _ := c.PublishAudio("mic", 0)
	video, _ := c.PublishVideo("cam", 3, 0)
	if err := c.SendVideo(video, []byte{1}, false, 0, 0); err == nil {
		t.Fatal("delta frame before any keyframe accepted")
	}
	if err := c.SendVideo(video, make([]byte, 5000), true, 0, 0); err != nil {
		t.Fatal(err)
	}
	if err := c.SendVideo(video, []byte{1}, false, 3, 0); err == nil {
		t.Fatal("layer above the published count accepted")
	}
	if err := c.SendAudio(video, []byte{1}, 0); err != ErrUnknownTrack {
		t.Fatal("audio on a video track accepted")
	}
	c.SendAudio(audio, make([]byte, 80), 0)
	for now := int64(5_000); now < 50_000; now += 5_000 {
		c.Tick(now)
	}
	if len(tr.dgs) < 7 {
		t.Fatalf("%d datagrams sent", len(tr.dgs))
	}
	if b := c.VideoBitrate(video); b <= 0 || b > 500_000 {
		t.Fatalf("video bitrate %d", b)
	}
	control(t, c, wire.BitrateHint{Stream: uint8(video), Bitrate: 100_000}, 0)
	if c.VideoBitrate(video) > 100_000 {
		t.Fatal("bitrate hint ignored")
	}
	control(t, c, wire.KeyframeRequest{Stream: uint8(video)}, 0)
	control(t, c, wire.TrackAdded{Stream: 9, Kind: wire.TrackVideo, Layers: 3, Peer: "p2", Name: "cam"}, 0)
	control(t, c, wire.TrackAdded{Stream: 10, Kind: wire.TrackAudio, Layers: 1, Peer: "p2", Name: "mic"}, 0)
	kinds := map[int]bool{}
	for e := c.PollEvent(); e != nil; e = c.PollEvent() {
		kinds[e.Kind] = true
	}
	if !kinds[EventKeyframeRequest] || !kinds[EventTrackAdded] || !kinds[EventBitrateHint] {
		t.Fatalf("%v", kinds)
	}
	if c.Subscribe(9, 2, 0) != nil || c.Subscribe(10, 0, 0) != nil || c.Subscribe(11, 0, 0) != ErrUnknownTrack {
		t.Fatal("subscribe")
	}
	pk := frag.NewPacketizer(9, 1000)
	pkts, _ := pk.Packetize(make([]byte, 2500), true, 0, false, 0)
	for _, b := range pkts {
		c.HandleDatagram(b, 60_000)
	}
	f := c.PollVideo(9, 60_000)
	if f == nil || !f.Keyframe || len(f.Data) != 2500 {
		t.Fatalf("%+v", f)
	}
	ak := frag.NewPacketizer(10, 1000)
	for i := range 10 {
		b, _ := ak.Packetize([]byte{byte(i)}, false, 0, true, uint32(i*20_000))
		c.HandleDatagram(b[0], int64(i*20_000)+60_000)
	}
	played := 0
	for now := int64(60_000); now < 400_000; now += 20_000 {
		if a := c.PullAudio(10, now); a.Status == AudioPlay {
			played++
		}
	}
	if played < 9 {
		t.Fatalf("played %d of 10 audio frames", played)
	}
	if s := c.Stats(); s.AudioPlayed != played || s.VideoReleased != 1 {
		t.Fatalf("%+v", s)
	}
	control(t, c, wire.TrackRemoved{Stream: 9}, 0)
	if c.PollVideo(9, 0) != nil {
		t.Fatal("removed track still delivers")
	}
	c.HandleControl([]byte{0x77, 0, 0}, 0)
	broken := false
	for e := c.PollEvent(); e != nil; e = c.PollEvent() {
		broken = broken || e.Kind == EventError
	}
	if !broken {
		t.Fatal("broken control stream not reported")
	}
}

func FuzzClientInput(f *testing.F) {
	pk := frag.NewPacketizer(9, 100)
	pkts, _ := pk.Packetize(make([]byte, 250), true, 0, false, 0)
	added, _ := wire.Append(nil, wire.TrackAdded{Stream: 9, Kind: wire.TrackVideo, Layers: 3, Peer: "p"})
	f.Add(pkts[0], added)
	rec, _ := wire.Append(nil, wire.Recovery{Stream: 1, Frame: 0})
	ack, _ := wire.Append(nil, wire.Ack{Stream: 1, Frames: []uint16{0}})
	ref, _ := pk.PacketizeFrame([]byte{0, 1, 0, 2, 9, 9}, frag.Opts{Keyframe: true, Face: true, Ref: -1}, 0)
	f.Add(ref[0], append(rec, ack...))
	f.Add(wire.AppendFeedback(nil, ack), rec)
	f.Fuzz(func(t *testing.T, dg, ctl []byte) {
		c := NewClient(&capture{}, nil)
		v, _ := c.PublishVideoTrack("cam", 3, 2, true, 0)
		c.SendVideoFrame(v, &OutFrame{Data: make([]byte, 40), Keyframe: true, LongTerm: true}, 0)
		control(t, c, wire.TrackAdded{Stream: 9, Kind: wire.TrackVideo, Layers: 3, Peer: "p"}, 0)
		control(t, c, wire.TrackAdded{Stream: 10, Kind: wire.TrackAudio, Layers: 1, Peer: "p"}, 0)
		c.Subscribe(9, 2, 0)
		c.Subscribe(10, 0, 0)
		c.HandleControl(ctl, 1)
		c.HandleDatagram(dg, 2)
		for _, p := range pkts {
			c.HandleDatagram(p, 3)
		}
		c.Tick(10_000)
		c.PollVideo(9, 20_000)
		c.PullAudio(10, 20_000)
	})
}

func publisher(t *testing.T, cfg *Config) (*Client, *capture) {
	tr := &capture{}
	c := NewClient(tr, cfg)
	c.Join("t", 0)
	control(t, c, wire.Joined{Peer: "me"}, 0)
	c.PollEvent()
	return c, tr
}

func drain(c *Client) []*Event {
	var out []*Event
	for e := c.PollEvent(); e != nil; e = c.PollEvent() {
		out = append(out, e)
	}
	return out
}

func TestRecoveryRequestBecomesALongTermPredictedFrame(t *testing.T) {
	c, tr := publisher(t, nil)
	v, _ := c.PublishVideo("cam", 3, 0)
	for i := range 8 {
		layer := []int{0, 2, 1, 2}[i%4]
		if _, err := c.SendVideoFrame(v, &OutFrame{Data: make([]byte, 100), Keyframe: i == 0, Layer: layer, LongTerm: layer == 0}, 0); err != nil {
			t.Fatal(err)
		}
	}
	control(t, c, wire.Ack{Stream: uint8(v), Frames: []uint16{4}}, 0)
	if c.AckedFrame(v) != 4 {
		t.Fatal("acknowledgement not recorded")
	}
	control(t, c, wire.Recovery{Stream: uint8(v), Frame: 4}, 0)
	ev := drain(c)
	if len(ev) != 1 || ev[0].Kind != EventRecover || ev[0].FrameID != 4 {
		t.Fatalf("%+v", ev)
	}
	tr.dgs = nil
	id, err := c.SendVideoFrame(v, &OutFrame{Data: make([]byte, 100), Layer: 2, Recover: true}, 0)
	if err != nil || id != 8 {
		t.Fatal(id, err)
	}
	for now := int64(5_000); now < 50_000; now += 5_000 {
		c.Tick(now)
	}
	var m wire.Media
	for _, b := range tr.dgs {
		if x, err := wire.ParseMedia(b); err == nil && x.FrameID == 8 {
			m = x
		}
	}
	if !m.Recovery || m.Layer != 0 || m.RefDelta != 4 {
		t.Fatalf("recovery frame on the wire: %+v", m)
	}
	control(t, c, wire.Recovery{Stream: uint8(v), Frame: 3}, 0)
	if ev := drain(c); len(ev) != 1 || ev[0].Kind != EventKeyframeRequest {
		t.Fatalf("a recovery from a frame that was never long-term did not ask for a keyframe: %+v", ev)
	}
}

func TestPublisherSwitchesToFaceAndSendsReferencePictures(t *testing.T) {
	c, tr := publisher(t, &Config{StartBitrate: 100_000, MaxBitrate: 100_000})
	v, _ := c.PublishVideoTrack("cam", 3, 1, true, 0)
	face := c.FaceStream(v)
	if face == 0 || c.EncodingStream(v, 0) != v {
		t.Fatal("face stream not published")
	}
	var pubs []wire.Publish
	for _, m := range tr.ctl {
		if p, ok := m.(wire.Publish); ok {
			pubs = append(pubs, p)
		}
	}
	if len(pubs) != 2 || pubs[0].Rank != 1 || pubs[1].Rank != 0 || pubs[1].Track != uint8(v) {
		t.Fatalf("%+v", pubs)
	}
	var rep, ref *Event
	for now := int64(0); now < 1_500_000 && (rep == nil || ref == nil); now += 5_000 {
		c.Tick(now)
		for _, e := range drain(c) {
			switch {
			case e.Kind == EventSendRepresentation && e.Stream == v:
				rep = e
			case e.Kind == EventReferencePicture:
				ref = e
			}
		}
	}
	if rep == nil || rep.Rep != RepFace || ref == nil || ref.Stream != v || ref.MaxBytes < minRefBytes {
		t.Fatalf("below 150 kbps for a second the camera did not switch to face: %+v %+v", rep, ref)
	}
	pic := make([]byte, 1300)
	for i := range pic {
		pic[i] = byte(i)
	}
	if _, err := c.SendVideoFrame(v, &OutFrame{Data: pic, Keyframe: true, Face: true}, 1_500_000); err != nil {
		t.Fatal(err)
	}
	rx, _ := publisher(t, nil)
	control(t, rx, wire.TrackAdded{Stream: 9, Kind: wire.TrackVideo, Layers: 3, Peer: "p"}, 0)
	rx.Subscribe(9, 2, 0)
	tr.dgs = nil
	for now := int64(1_500_000); now < 4_000_000; now += 5_000 {
		c.SendVideoFrame(v, &OutFrame{Data: make([]byte, 60), Face: true}, now)
		c.Tick(now)
		for _, b := range tr.dgs {
			if m, err := wire.ParseMedia(b); err == nil && m.Stream == uint8(v) {
				wire.SetStream(b, 9)
				rx.HandleDatagram(b, now)
			}
		}
		tr.dgs = nil
		for f := rx.PollVideo(9, now); f != nil; f = rx.PollVideo(9, now) {
		}
	}
	got := rx.FaceReference(9)
	if len(got) != len(pic) || got[1299] != pic[1299] || rx.Representation(9) != RepFace {
		t.Fatalf("reference picture %d bytes, representation %d", len(got), rx.Representation(9))
	}
	kinds := map[int]int{}
	for _, e := range drain(rx) {
		kinds[e.Kind]++
	}
	if kinds[EventRepresentation] != 1 || kinds[EventFaceReference] != 1 {
		t.Fatalf("%v", kinds)
	}
}

func TestAudioCarriesItsRedundancyDepthAndFeedbackRidesDatagrams(t *testing.T) {
	c, tr := publisher(t, nil)
	a, _ := c.PublishAudio("mic", 0)
	c.SetAudioRedundancy(a, 500)
	c.SendAudio(a, make([]byte, 40), 0)
	c.Tick(5_000)
	var m wire.Media
	for _, b := range tr.dgs {
		if x, err := wire.ParseMedia(b); err == nil {
			m = x
		}
	}
	if m.RefDelta != 50 {
		t.Fatalf("redundancy depth byte %d, want 50 (500 ms in 10 ms units)", m.RefDelta)
	}
	control(t, c, wire.TrackAdded{Stream: 10, Kind: wire.TrackAudio, Layers: 1, Peer: "p"}, 0)
	c.Subscribe(10, 0, 0)
	pk := frag.NewPacketizer(10, 1000)
	tr.dgs = nil
	var dred *AudioFrame
	fb := false
	for i := range 40 {
		now := int64(i) * 20_000
		b, _ := pk.PacketizeFrame([]byte{byte(i)}, frag.Opts{Independent: true, Ref: -1, Aux: 100}, uint32(now))
		if i != 15 && i != 16 {
			c.HandleDatagram(b[0], now+20_000)
		}
		c.Tick(now + 20_000)
		if f := c.PullAudio(10, now+25_000); f.Status == AudioDRED && dred == nil {
			dred = f
		}
	}
	for _, b := range tr.dgs {
		if k, _ := wire.Kind(b); k == wire.KindFeedback {
			fb = true
		}
	}
	if !fb {
		t.Fatal("receiver report not sent as a feedback datagram")
	}
	if dred == nil || dred.Data[0] != 17 || (17-dred.Offset != 15 && 17-dred.Offset != 16) {
		t.Fatalf("a lost frame was not handed out for DRED decoding from packet 17: %+v", dred)
	}
}
