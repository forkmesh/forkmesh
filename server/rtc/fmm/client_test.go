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
	f.Fuzz(func(t *testing.T, dg, ctl []byte) {
		c := NewClient(&capture{}, nil)
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
