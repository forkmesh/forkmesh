package fmm

import (
	"errors"
	"maps"
	"slices"
	"sync"

	"forkmesh.com/mirror-node/rtc/fmm/cc"
	"forkmesh.com/mirror-node/rtc/fmm/frag"
	"forkmesh.com/mirror-node/rtc/fmm/hop"
	"forkmesh.com/mirror-node/rtc/fmm/jitter"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

type Transport interface {
	SendDatagram(p []byte) error
	MaxDatagramSize() int
	SendControl(p []byte) error
}

const (
	KindAudio = wire.TrackAudio
	KindVideo = wire.TrackVideo

	EventJoined          = 1
	EventTrackAdded      = 2
	EventTrackRemoved    = 3
	EventKeyframeRequest = 4
	EventBitrateHint     = 5
	EventError           = 6

	AudioWaiting = jitter.StatusWaiting
	AudioPlay    = jitter.StatusPlay
	AudioConceal = jitter.StatusConceal
	AudioInsert  = jitter.StatusInsert

	TickIntervalUs = 5_000
	maxPublished   = 4
	maxEvents      = 256
	minVideoBps    = 30_000
	headerFactor   = 0.97
	nominalAudio   = 40_000
)

var (
	ErrNotJoined    = errors.New("fmm: not joined")
	ErrUnknownTrack = errors.New("fmm: unknown track")
	ErrTooMany      = errors.New("fmm: too many tracks")
	ErrClosed       = errors.New("fmm: client closed")
	ErrProtocol     = errors.New("fmm: control stream broken")
)

type Config struct {
	StartBitrate int
	MinBitrate   int
	MaxBitrate   int
}

type Event struct {
	Kind    int
	Stream  int
	Track   int
	Layers  int
	Peer    string
	Name    string
	Bitrate int
	Code    int
	Text    string
}

type AudioFrame struct {
	Status int
	Data   []byte
}

type VideoFrame struct {
	Stream    int
	FrameID   int64
	Keyframe  bool
	Layer     int
	Timestamp int64
	Data      []byte
}

type Stats struct {
	TargetBitrate    int
	VideoBitrate     int
	RTTUs            int64
	Loss             float64
	SentPackets      int
	ReceivedPackets  int
	RecoveredPackets int
	AudioPlayed      int
	AudioConcealed   int
	AudioInserted    int
	VideoReleased    int
	VideoUndecodable int
	KeyframeRequests int
	PacketsQueued    int
}

type published struct {
	kind   int
	layers int
	pk     *frag.Packetizer
	hint   int
}

type remote struct {
	kind       int
	layers     int
	peer       string
	name       string
	subscribed bool
	ra         *frag.Reassembler
	audio      *jitter.Audio
	video      *jitter.Video
}

type Client struct {
	mu      sync.Mutex
	t       Transport
	ctl     wire.Reader
	snd     *hop.Sender
	rcv     *hop.Receiver
	pubs    map[uint8]*published
	next    uint8
	remotes map[uint8]*remote
	events  []*Event
	joined  bool
	peer    string
	closed  bool
	err     error
}

func NewClient(t Transport, cfg *Config) *Client {
	var c cc.Config
	if cfg != nil {
		c = cc.Config{StartBps: cfg.StartBitrate, MinBps: cfg.MinBitrate, MaxBps: cfg.MaxBitrate}
	}
	return &Client{
		t: t, snd: hop.NewSender(t, c), rcv: hop.NewReceiver(),
		pubs: map[uint8]*published{}, remotes: map[uint8]*remote{}, next: 1,
	}
}

func (c *Client) send(m wire.Message) error {
	b, err := wire.Append(nil, m)
	if err != nil {
		return err
	}
	return c.t.SendControl(b)
}

func (c *Client) Join(ticket string, now int64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return ErrClosed
	}
	return c.send(wire.Join{Ticket: ticket})
}

func (c *Client) Leave(reason string, now int64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return ErrClosed
	}
	c.closed = true
	return c.send(wire.Leave{Reason: reason})
}

func (c *Client) PeerID() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.peer
}

func (c *Client) publish(kind, layers int, name string) (int, error) {
	if c.closed {
		return 0, ErrClosed
	}
	if len(c.pubs) >= maxPublished {
		return 0, ErrTooMany
	}
	id := c.next
	c.next++
	if err := c.send(wire.Publish{Stream: id, Kind: uint8(kind), Layers: uint8(layers), Name: name}); err != nil {
		return 0, err
	}
	c.pubs[id] = &published{kind: kind, layers: layers, pk: frag.NewPacketizer(id, c.snd.MaxPayload())}
	c.snd.AddStream(id, kind == KindAudio)
	return int(id), nil
}

func (c *Client) PublishAudio(name string, now int64) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.publish(KindAudio, 1, name)
}

func (c *Client) PublishVideo(name string, layers int, now int64) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.publish(KindVideo, min(max(layers, 1), wire.MaxLayer+1), name)
}

func (c *Client) Unpublish(stream int, now int64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	id := uint8(stream)
	if c.pubs[id] == nil {
		return ErrUnknownTrack
	}
	delete(c.pubs, id)
	c.snd.RemoveStream(id)
	return c.send(wire.Unpublish{Stream: id})
}

func (c *Client) SendAudio(stream int, data []byte, now int64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	p := c.pubs[uint8(stream)]
	if p == nil || p.kind != KindAudio {
		return ErrUnknownTrack
	}
	pkts, err := p.pk.Packetize(data, false, 0, true, uint32(now))
	if err != nil {
		return err
	}
	for _, b := range pkts {
		c.snd.Send(b, now)
	}
	return nil
}

func (c *Client) SendVideo(stream int, data []byte, keyframe bool, layer int, now int64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	p := c.pubs[uint8(stream)]
	if p == nil || p.kind != KindVideo {
		return ErrUnknownTrack
	}
	if layer >= p.layers {
		return frag.ErrLayer
	}
	pkts, err := p.pk.Packetize(data, keyframe, layer, false, uint32(now))
	if err != nil {
		return err
	}
	for _, b := range pkts {
		c.snd.Send(b, now)
	}
	return nil
}

func (c *Client) Subscribe(stream int, maxLayer int, now int64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	r := c.remotes[uint8(stream)]
	if r == nil {
		return ErrUnknownTrack
	}
	if !r.subscribed {
		r.subscribed = true
		r.ra = frag.NewReassembler()
		if r.kind == KindAudio {
			r.audio = jitter.NewAudio(jitter.AudioConfig{})
		} else {
			r.video = jitter.NewVideo(jitter.VideoConfig{})
			c.rcv.EnableNack(uint8(stream))
		}
	}
	return c.send(wire.Subscribe{Stream: uint8(stream), MaxLayer: uint8(min(max(maxLayer, 0), wire.MaxLayer))})
}

func (c *Client) Unsubscribe(stream int, now int64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	r := c.remotes[uint8(stream)]
	if r == nil {
		return ErrUnknownTrack
	}
	r.subscribed, r.ra, r.audio, r.video = false, nil, nil, nil
	c.rcv.RemoveStream(uint8(stream))
	return c.send(wire.Unsubscribe{Stream: uint8(stream)})
}

func (c *Client) SetReceiveLimit(bitrate int, now int64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.send(wire.BitrateHint{Stream: wire.AllStreams, Bitrate: uint32(max(bitrate, 0))})
}

func (c *Client) HandleDatagram(p []byte, now int64) {
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, b := range c.rcv.Handle(p, now) {
		m, err := wire.ParseMedia(b)
		if err != nil {
			continue
		}
		r := c.remotes[m.Stream]
		if r == nil || !r.subscribed {
			continue
		}
		f := r.ra.Add(&m, &b[0] != &p[0], now)
		if f == nil {
			continue
		}
		if r.audio != nil {
			r.audio.Push(f.ID, f.Timestamp, f.Data, now)
		} else if r.video != nil {
			r.video.Push(f, now)
		}
	}
}

func (c *Client) HandleControl(p []byte, now int64) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.ctl.Write(p) != nil {
		c.fail(ErrProtocol)
		return
	}
	for {
		m, err := c.ctl.Next()
		if err != nil {
			c.fail(ErrProtocol)
			return
		}
		if m == nil {
			return
		}
		c.dispatch(m, now)
	}
}

func (c *Client) fail(err error) {
	if c.err == nil {
		c.err = err
		c.event(&Event{Kind: EventError, Text: err.Error()})
	}
}

func (c *Client) event(e *Event) {
	if len(c.events) < maxEvents {
		c.events = append(c.events, e)
	}
}

func (c *Client) dispatch(m wire.Message, now int64) {
	switch v := m.(type) {
	case *wire.Report:
		c.snd.OnReport(v, now)
	case wire.Nack:
		c.snd.OnNack(v, now)
	case wire.Joined:
		c.joined, c.peer = true, v.Peer
		c.event(&Event{Kind: EventJoined, Peer: v.Peer})
	case wire.TrackAdded:
		if _, ok := c.remotes[v.Stream]; !ok {
			c.remotes[v.Stream] = &remote{kind: int(v.Kind), layers: int(v.Layers), peer: v.Peer, name: v.Name}
		}
		c.event(&Event{Kind: EventTrackAdded, Stream: int(v.Stream), Track: int(v.Kind), Layers: int(v.Layers), Peer: v.Peer, Name: v.Name})
	case wire.TrackRemoved:
		if r := c.remotes[v.Stream]; r != nil {
			delete(c.remotes, v.Stream)
			c.rcv.RemoveStream(v.Stream)
			c.event(&Event{Kind: EventTrackRemoved, Stream: int(v.Stream), Peer: r.peer, Name: r.name})
		}
	case wire.KeyframeRequest:
		if p := c.pubs[v.Stream]; p != nil && p.kind == KindVideo {
			c.event(&Event{Kind: EventKeyframeRequest, Stream: int(v.Stream)})
		}
	case wire.BitrateHint:
		if p := c.pubs[v.Stream]; p != nil {
			p.hint = int(v.Bitrate)
			c.event(&Event{Kind: EventBitrateHint, Stream: int(v.Stream), Bitrate: int(v.Bitrate)})
		}
	case wire.Error:
		c.event(&Event{Kind: EventError, Code: int(v.Code), Text: v.Text})
	}
}

func (c *Client) Tick(now int64) int64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.snd.Tick(now)
	if rep := c.rcv.Report(now); rep != nil {
		c.t.SendControl(rep)
	}
	c.rcv.SetRTT(c.snd.Controller().RTT())
	for _, n := range c.rcv.Nacks(now) {
		c.t.SendControl(n)
	}
	for _, id := range slices.Sorted(maps.Keys(c.remotes)) {
		r := c.remotes[id]
		if r.video == nil {
			continue
		}
		for _, l := range r.ra.Expire(now) {
			r.video.Lost(l, now)
		}
		if r.video.KeyframeNeeded(now) {
			c.send(wire.KeyframeRequest{Stream: id})
		}
	}
	return now + TickIntervalUs
}

func (c *Client) PullAudio(stream int, now int64) *AudioFrame {
	c.mu.Lock()
	defer c.mu.Unlock()
	r := c.remotes[uint8(stream)]
	if r == nil || r.audio == nil {
		return &AudioFrame{Status: AudioWaiting}
	}
	d, st := r.audio.Pull(now)
	return &AudioFrame{Status: st, Data: d}
}

func (c *Client) PollVideo(stream int, now int64) *VideoFrame {
	c.mu.Lock()
	defer c.mu.Unlock()
	r := c.remotes[uint8(stream)]
	if r == nil || r.video == nil {
		return nil
	}
	f := r.video.Pop(now)
	if f == nil {
		return nil
	}
	return &VideoFrame{Stream: stream, FrameID: f.ID, Keyframe: f.Keyframe, Layer: f.Layer, Timestamp: f.Timestamp, Data: f.Data}
}

func (c *Client) PollEvent() *Event {
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(c.events) == 0 {
		return nil
	}
	e := c.events[0]
	c.events = c.events[1:]
	return e
}

func (c *Client) TargetBitrate() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.snd.Target()
}

func (c *Client) VideoBitrate(stream int) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.videoBitrate(uint8(stream))
}

func (c *Client) videoBitrate(id uint8) int {
	p := c.pubs[id]
	if p == nil || p.kind != KindVideo {
		return 0
	}
	avail := float64(c.snd.Target())
	videos := 0
	for sid, q := range c.pubs {
		if q.kind == KindAudio {
			media, repair := c.snd.StreamRate(sid)
			if media == 0 {
				media, repair = nominalAudio, nominalAudio*c.snd.FECRatio(sid)
			}
			avail -= media + repair + c.snd.StreamOverheadBps(sid)
		} else {
			videos++
			avail -= c.snd.StreamOverheadBps(sid)
		}
	}
	share := avail / float64(max(videos, 1)) / (1 + c.snd.FECRatio(id)) * headerFactor
	if p.hint > 0 {
		share = min(share, float64(p.hint))
	}
	return max(int(share), minVideoBps)
}

func (c *Client) Stats() *Stats {
	c.mu.Lock()
	defer c.mu.Unlock()
	ctl := c.snd.Controller()
	s := &Stats{
		TargetBitrate: ctl.Target(), RTTUs: ctl.RTT(), Loss: ctl.Loss(),
		SentPackets: c.snd.Sent, ReceivedPackets: c.rcv.Media, RecoveredPackets: c.rcv.Recovered,
		PacketsQueued: c.snd.Pacer().QueuedBytes(),
	}
	for id, p := range c.pubs {
		if p.kind == KindVideo {
			s.VideoBitrate += c.videoBitrate(id)
		}
	}
	for _, r := range c.remotes {
		if r.audio != nil {
			s.AudioPlayed += r.audio.Stats.Played
			s.AudioConcealed += r.audio.Stats.Concealed
			s.AudioInserted += r.audio.Stats.Inserted
		}
		if r.video != nil {
			s.VideoReleased += r.video.Stats.Released
			s.VideoUndecodable += r.video.Stats.Undecodable
			s.KeyframeRequests += r.video.Stats.KeyframeRequests
		}
	}
	return s
}
