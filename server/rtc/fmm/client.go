package fmm

import (
	"encoding/binary"
	"errors"
	"maps"
	"slices"
	"sync"

	"forkmesh.com/mirror-node/rtc/fmm/cc"
	"forkmesh.com/mirror-node/rtc/fmm/frag"
	"forkmesh.com/mirror-node/rtc/fmm/hop"
	"forkmesh.com/mirror-node/rtc/fmm/internal/seqnum"
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
	KindAuth  = wire.TrackAuth

	EventJoined             = 1
	EventTrackAdded         = 2
	EventTrackRemoved       = 3
	EventKeyframeRequest    = 4
	EventBitrateHint        = 5
	EventError              = 6
	EventRecover            = 7
	EventSendRepresentation = 8
	EventReferencePicture   = 9
	EventRepresentation     = 10
	EventFaceReference      = 11

	RepOff   = 0
	RepVideo = 1
	RepFace  = 2

	AudioWaiting = jitter.StatusWaiting
	AudioPlay    = jitter.StatusPlay
	AudioConceal = jitter.StatusConceal
	AudioInsert  = jitter.StatusInsert
	AudioDRED    = jitter.StatusDRED

	TickIntervalUs  = 5_000
	maxPublished    = 8
	maxEvents       = 256
	minVideoBps     = 30_000
	headerFactor    = 0.97
	nominalAudio    = 40_000
	nominalAuth     = 8_000
	maxAuthQueue    = 64
	maxLongTerm     = 32
	ackRepeat       = 2
	trackEveryUs    = 100_000
	faceDownBps     = 150_000
	faceUpBps       = 250_000
	faceHoldUs      = 1_000_000
	faceBps         = 32_000
	refEveryUs      = 2_000_000
	refShare        = 0.5
	refPartBytes    = 500
	refPrefix       = 4
	minRefBytes     = 600
	maxRefBytes     = 16_000
	minAudioBps     = 16_000
	maxAudioBps     = 32_000
	audioShare      = 0.2
	recoverRepeatUs = 200_000

	minRedundancyMs = 200
	maxRedundancyMs = 1000
	redundancyLoss  = 0.01
	redundancyShare = 0.03
	dredFixedBps    = 2_400
	dredBpsPerMs    = 10
)

var (
	ErrNotJoined    = errors.New("fmm: not joined")
	ErrUnknownTrack = errors.New("fmm: unknown track")
	ErrTooMany      = errors.New("fmm: too many tracks")
	ErrClosed       = errors.New("fmm: client closed")
	ErrProtocol     = errors.New("fmm: control stream broken")
	ErrBadFrame     = errors.New("fmm: bad frame options")
)

var simulcastWeights = [][]float64{{1}, {0.25, 0.75}, {0.12, 0.28, 0.6}}

type Config struct {
	StartBitrate      int
	MinBitrate        int
	MaxBitrate        int
	FeedbackOnControl bool
	NoRecovery        bool
	NoLookAhead       bool
	FaceDownBitrate   int
	FaceUpBitrate     int
}

type Event struct {
	Kind     int
	Stream   int
	Track    int
	Layers   int
	Peer     string
	Name     string
	Bitrate  int
	Code     int
	Text     string
	FrameID  int64
	Rep      int
	MaxBytes int
}

type AudioFrame struct {
	Status int
	Data   []byte
	Offset int
}

type VideoFrame struct {
	Stream    int
	FrameID   int64
	Keyframe  bool
	Layer     int
	Timestamp int64
	Face      bool
	Recovery  bool
	LongTerm  bool
	Data      []byte
}

type OutFrame struct {
	Data     []byte
	Keyframe bool
	Layer    int
	LongTerm bool
	Face     bool
	Recover  bool
	RefFrame int64
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
	AudioFEC         int
	AudioDRED        int
	AudioInserted    int
	VideoReleased    int
	VideoUndecodable int
	KeyframeRequests int
	RecoveryRequests int
	RecoveryFrames   int
	Escalations      int
	PacketsQueued    int
	Recoveries       int
}

type published struct {
	kind     int
	layers   int
	pk       *frag.Packetizer
	hint     int
	hinted   bool
	track    *vtrack
	depth    int
	longTerm []int64
	acked    int64
	hasAck   bool
	recRef   int64
	recAt    int64
	sending  int
	refParts [][]byte
	refID    uint16
	refBytes float64
}

type vtrack struct {
	id        uint8
	encs      []uint8
	face      uint8
	rep       int
	lowSince  int64
	highSince int64
	refAt     int64
	lastEval  int64
}

type refPicture struct {
	id    uint16
	parts [][]byte
	have  int
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
	rep        int
	faceRef    []byte
	building   *refPicture
	acks       []int64
	ackSent    []int
	authOn     bool
	auth       [][]byte
}

type Client struct {
	mu      sync.Mutex
	cfg     Config
	t       Transport
	ctl     wire.Reader
	snd     *hop.Sender
	rcv     *hop.Receiver
	pubs    map[uint8]*published
	tracks  map[uint8]*vtrack
	next    uint8
	remotes map[uint8]*remote
	events  []*Event
	joined  bool
	peer    string
	closed  bool
	err     error
	last    int64
	recs    int
}

func NewClient(t Transport, cfg *Config) *Client {
	var c cc.Config
	var conf Config
	if cfg != nil {
		conf = *cfg
		c = cc.Config{StartBps: cfg.StartBitrate, MinBps: cfg.MinBitrate, MaxBps: cfg.MaxBitrate}
	}
	if conf.FaceDownBitrate <= 0 {
		conf.FaceDownBitrate = faceDownBps
	}
	if conf.FaceUpBitrate <= 0 {
		conf.FaceUpBitrate = faceUpBps
	}
	cl := &Client{
		cfg: conf, t: t, snd: hop.NewSender(t, c), rcv: hop.NewReceiver(),
		pubs: map[uint8]*published{}, tracks: map[uint8]*vtrack{}, remotes: map[uint8]*remote{}, next: 1,
	}
	if !conf.FeedbackOnControl {
		cl.rcv.SetRepeat(1)
	}
	return cl
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

func (c *Client) publish(kind, layers int, name string, track uint8, rank int) (uint8, error) {
	if c.closed {
		return 0, ErrClosed
	}
	if len(c.pubs) >= maxPublished {
		return 0, ErrTooMany
	}
	id := c.next
	if track == 0xFF {
		track = id
	}
	if err := c.send(wire.Publish{Stream: id, Kind: uint8(kind), Layers: uint8(layers), Name: name, Track: track, Rank: uint8(rank)}); err != nil {
		return 0, err
	}
	c.next++
	c.pubs[id] = &published{kind: kind, layers: layers, pk: frag.NewPacketizer(id, c.snd.MaxPayload()), sending: RepVideo, recRef: -1}
	c.snd.AddStream(id, kind != KindVideo)
	return id, nil
}

func (c *Client) PublishAudio(name string, now int64) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	id, err := c.publish(KindAudio, 1, name, 0, 0)
	return int(id), err
}

func (c *Client) PublishAuth(name string, now int64) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	id, err := c.publish(KindAuth, 1, name, 0, 0)
	return int(id), err
}

func (c *Client) SendAuth(stream int, data []byte, now int64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	p := c.pubs[uint8(stream)]
	if p == nil || p.kind != KindAuth {
		return ErrUnknownTrack
	}
	pkts, err := p.pk.PacketizeFrame(data, frag.Opts{Independent: true, Ref: -1}, uint32(now))
	if err != nil {
		return err
	}
	for _, b := range pkts {
		c.snd.Send(b, now)
	}
	return nil
}

func (c *Client) PollAuth(stream int) []byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	r := c.remotes[uint8(stream)]
	if r == nil || len(r.auth) == 0 {
		return nil
	}
	b := r.auth[0]
	r.auth = r.auth[1:]
	return b
}

func (c *Client) PublishVideo(name string, layers int, now int64) (int, error) {
	return c.PublishVideoTrack(name, layers, 1, false, now)
}

func (c *Client) PublishVideoTrack(name string, layers, encodings int, face bool, now int64) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	layers = min(max(layers, 1), wire.MaxLayer+1)
	encodings = min(max(encodings, 1), len(simulcastWeights))
	extra := encodings - 1
	if face {
		extra++
	}
	if len(c.pubs)+1+extra > maxPublished {
		return 0, ErrTooMany
	}
	base := 0
	if extra > 0 {
		base = 1
	}
	marker := uint8(0)
	if extra > 0 {
		marker = 0xFF
	}
	id, err := c.publish(KindVideo, layers, name, marker, base+encodings-1)
	if err != nil {
		return 0, err
	}
	vt := &vtrack{id: id, rep: RepVideo}
	c.tracks[id] = vt
	c.pubs[id].track = vt
	for i := range encodings - 1 {
		e, err := c.publish(KindVideo, layers, name, id, base+i)
		if err != nil {
			return int(id), err
		}
		c.pubs[e].track = vt
		vt.encs = append(vt.encs, e)
	}
	vt.encs = append(vt.encs, id)
	if face {
		f, err := c.publish(KindVideo, 1, name, id, 0)
		if err != nil {
			return int(id), err
		}
		c.pubs[f].track = vt
		c.pubs[f].sending = RepOff
		vt.face = f
	}
	return int(id), nil
}

func (c *Client) EncodingStream(track, index int) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	vt := c.tracks[uint8(track)]
	if vt == nil || index < 0 || index >= len(vt.encs) {
		return 0
	}
	return int(vt.encs[index])
}

func (c *Client) FaceStream(track int) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	if vt := c.tracks[uint8(track)]; vt != nil {
		return int(vt.face)
	}
	return 0
}

func (c *Client) Unpublish(stream int, now int64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	id := uint8(stream)
	p := c.pubs[id]
	if p == nil {
		return ErrUnknownTrack
	}
	ids := []uint8{id}
	if vt := c.tracks[id]; vt != nil {
		ids = append(slices.DeleteFunc(slices.Clone(vt.encs), func(x uint8) bool { return x == id }), id)
		if vt.face != 0 {
			ids = append(ids, vt.face)
		}
		delete(c.tracks, id)
	}
	for _, x := range ids {
		delete(c.pubs, x)
		c.snd.RemoveStream(x)
	}
	return c.send(wire.Unpublish{Stream: id})
}

func (c *Client) SetAudioRedundancy(stream int, ms int) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	p := c.pubs[uint8(stream)]
	if p == nil || p.kind != KindAudio {
		return ErrUnknownTrack
	}
	p.depth = min(max(ms*1000/wire.DepthUnitUs, 0), 255)
	return nil
}

func (c *Client) SendAudio(stream int, data []byte, now int64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	p := c.pubs[uint8(stream)]
	if p == nil || p.kind != KindAudio {
		return ErrUnknownTrack
	}
	pkts, err := p.pk.PacketizeFrame(data, frag.Opts{Independent: true, Ref: -1, Aux: uint8(p.depth)}, uint32(now))
	if err != nil {
		return err
	}
	for _, b := range pkts {
		c.snd.Send(b, now)
	}
	return nil
}

func (c *Client) AudioBitrate(stream int) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return min(max(int(audioShare*float64(c.snd.Target())), minAudioBps), maxAudioBps)
}

func (c *Client) AudioRedundancyMs(stream int) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	ctl := c.snd.Controller()
	if ctl.Loss() < redundancyLoss {
		return minRedundancyMs
	}
	ms := (redundancyShare*float64(ctl.Target()) - dredFixedBps) / dredBpsPerMs
	return min(max(int(ms), minRedundancyMs), maxRedundancyMs)
}

func (c *Client) SendVideo(stream int, data []byte, keyframe bool, layer int, now int64) error {
	_, err := c.SendVideoFrame(stream, &OutFrame{Data: data, Keyframe: keyframe, Layer: layer}, now)
	return err
}

func (c *Client) SendVideoFrame(stream int, f *OutFrame, now int64) (int64, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	p := c.pubs[uint8(stream)]
	if p == nil || p.kind != KindVideo {
		return -1, ErrUnknownTrack
	}
	if f == nil || f.Layer < 0 || f.Layer >= p.layers {
		return -1, frag.ErrLayer
	}
	if f.Face && f.Keyframe {
		return -1, c.queueReference(p, f.Data)
	}
	o := frag.Opts{Keyframe: f.Keyframe, Layer: f.Layer, LongTerm: f.LongTerm, Face: f.Face, Ref: -1, Independent: f.Face}
	if f.Recover && !f.Keyframe && !f.Face {
		ref := p.recRef
		if f.RefFrame > 0 {
			ref = f.RefFrame
		}
		if ref < 0 || !slices.Contains(p.longTerm, ref) {
			return -1, ErrBadFrame
		}
		o.Ref, o.Layer = ref, 0
	}
	if f.Keyframe {
		p.longTerm = p.longTerm[:0]
	}
	pkts, err := p.pk.PacketizeFrame(f.Data, o, uint32(now))
	if err != nil {
		return -1, err
	}
	id := p.pk.LastID()
	if f.LongTerm {
		p.longTerm = append(p.longTerm, id)
		if len(p.longTerm) > maxLongTerm {
			p.longTerm = p.longTerm[1:]
		}
	}
	if o.Ref >= 0 {
		p.recRef = -1
	}
	for _, b := range pkts {
		c.snd.Send(b, now)
	}
	return id, nil
}

func (c *Client) queueReference(p *published, data []byte) error {
	if len(data) == 0 || len(data) > maxRefBytes {
		return ErrBadFrame
	}
	p.refID++
	n := (len(data) + refPartBytes - 1) / refPartBytes
	p.refParts = p.refParts[:0]
	for i := range n {
		part := make([]byte, refPrefix, refPrefix+refPartBytes)
		binary.BigEndian.PutUint16(part, p.refID)
		part[2], part[3] = byte(i), byte(n)
		part = append(part, data[i*refPartBytes:min((i+1)*refPartBytes, len(data))]...)
		p.refParts = append(p.refParts, part)
	}
	return nil
}

func (c *Client) sendReferenceParts(id uint8, p *published, dt int64, now int64) {
	if len(p.refParts) == 0 {
		p.refBytes = 0
		return
	}
	spare := max(c.faceBudget(p)-faceBps, faceBps/4)
	p.refBytes = min(p.refBytes+refShare*spare*float64(dt)/8e6, refPartBytes+refPrefix)
	for len(p.refParts) > 0 && p.refBytes >= float64(len(p.refParts[0])) {
		part := p.refParts[0]
		p.refParts = p.refParts[1:]
		p.refBytes -= float64(len(part))
		pkts, err := p.pk.PacketizeFrame(part, frag.Opts{Keyframe: true, Face: true, Ref: -1}, uint32(now))
		if err != nil {
			continue
		}
		for _, b := range pkts {
			c.snd.Send(b, now)
		}
	}
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
		switch r.kind {
		case KindAudio:
			r.audio = jitter.NewAudio(jitter.AudioConfig{NoLookAhead: c.cfg.NoLookAhead})
		case KindAuth:
			r.authOn = true
		default:
			r.video = jitter.NewVideo(jitter.VideoConfig{NoRecovery: c.cfg.NoRecovery})
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
	r.authOn, r.auth = false, nil
	r.acks, r.ackSent, r.rep, r.building = nil, nil, RepOff, nil
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
	if k, err := wire.Kind(p); err == nil && k == wire.KindFeedback {
		msgs, err := wire.ParseFeedback(p)
		if err != nil {
			return
		}
		for _, m := range msgs {
			c.dispatch(m, now)
		}
		return
	}
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
			r.audio.PushDepth(f.ID, f.Timestamp, f.Data, int64(f.Aux)*wire.DepthUnitUs, now)
		} else if r.video != nil {
			r.video.Push(f, now)
		} else if r.authOn && len(r.auth) < maxAuthQueue {
			r.auth = append(r.auth, f.Data)
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

func (c *Client) unwrapSent(p *published, id uint16) int64 {
	return seqnum.Near16(id, max(p.pk.LastID(), 0))
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
			if p.sending == RepFace {
				p.track.refAt = 0
			} else {
				c.event(&Event{Kind: EventKeyframeRequest, Stream: int(v.Stream)})
			}
		}
	case wire.Ack:
		if p := c.pubs[v.Stream]; p != nil && p.kind == KindVideo {
			for _, x := range v.Frames {
				id := c.unwrapSent(p, x)
				if !p.hasAck || id > p.acked {
					p.acked, p.hasAck = id, true
				}
			}
		}
	case wire.Recovery:
		if p := c.pubs[v.Stream]; p != nil && p.kind == KindVideo && p.sending == RepVideo {
			c.recover(v.Stream, p, c.unwrapSent(p, v.Frame), now)
		}
	case wire.BitrateHint:
		if p := c.pubs[v.Stream]; p != nil {
			p.hint, p.hinted = int(v.Bitrate), true
			c.event(&Event{Kind: EventBitrateHint, Stream: int(v.Stream), Bitrate: int(v.Bitrate)})
		}
	case wire.Error:
		c.event(&Event{Kind: EventError, Code: int(v.Code), Text: v.Text})
	}
}

func (c *Client) recover(id uint8, p *published, ref int64, now int64) {
	if p.recRef == ref && now-p.recAt < recoverRepeatUs {
		return
	}
	if !slices.Contains(p.longTerm, ref) || p.pk.NextID()-ref > 255 {
		c.event(&Event{Kind: EventKeyframeRequest, Stream: int(id)})
		return
	}
	p.recRef, p.recAt = ref, now
	c.recs++
	c.event(&Event{Kind: EventRecover, Stream: int(id), FrameID: ref})
}

func (c *Client) Tick(now int64) int64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	dt := int64(0)
	if c.last > 0 {
		dt = now - c.last
	}
	c.last = now
	for _, id := range slices.Sorted(maps.Keys(c.tracks)) {
		c.evaluate(c.tracks[id], now)
	}
	for _, id := range slices.Sorted(maps.Keys(c.pubs)) {
		if p := c.pubs[id]; p.kind == KindVideo {
			c.sendReferenceParts(id, p, dt, now)
		}
	}
	c.snd.Tick(now)
	rtt := c.snd.Controller().RTT()
	c.rcv.SetRTT(rtt)
	var fb [][]byte
	if rep := c.rcv.Report(now); rep != nil {
		fb = append(fb, rep)
	}
	fb = append(fb, c.rcv.Nacks(now)...)
	for _, id := range slices.Sorted(maps.Keys(c.remotes)) {
		r := c.remotes[id]
		if r.video == nil {
			continue
		}
		r.video.SetTiming(2 * rtt)
		for _, l := range r.ra.Expire(now) {
			r.video.Lost(l, now)
		}
		if b := c.ackBytes(id, r); b != nil {
			fb = append(fb, b)
		}
		if ok, stuck := r.video.RecoveryNeeded(now); ok {
			if b, err := wire.Append(nil, wire.Recovery{Stream: id, Frame: uint16(stuck)}); err == nil {
				fb = append(fb, b)
			}
		}
		if r.video.KeyframeNeeded(now) {
			c.send(wire.KeyframeRequest{Stream: id})
		}
	}
	c.sendFeedback(fb)
	return now + TickIntervalUs
}

func (c *Client) ackBytes(id uint8, r *remote) []byte {
	for _, a := range r.video.TakeAcks() {
		r.acks = append(r.acks, a)
		r.ackSent = append(r.ackSent, 0)
	}
	repeat := 1
	if !c.cfg.FeedbackOnControl {
		repeat = ackRepeat
	}
	var frames []uint16
	keep := 0
	for i, a := range r.acks {
		if r.ackSent[i] >= repeat {
			continue
		}
		r.ackSent[i]++
		if len(frames) < wire.MaxAck {
			frames = append(frames, uint16(a))
		}
		r.acks[keep], r.ackSent[keep] = a, r.ackSent[i]
		keep++
	}
	r.acks, r.ackSent = r.acks[:keep], r.ackSent[:keep]
	if len(frames) == 0 {
		return nil
	}
	b, err := wire.Append(nil, wire.Ack{Stream: id, Frames: frames})
	if err != nil {
		return nil
	}
	return b
}

func (c *Client) sendFeedback(msgs [][]byte) {
	if len(msgs) == 0 {
		return
	}
	if c.cfg.FeedbackOnControl {
		for _, m := range msgs {
			c.t.SendControl(m)
		}
		return
	}
	dgs, ctl := hop.PackFeedback(msgs, min(c.t.MaxDatagramSize(), hop.MaxPacketSize))
	for _, d := range dgs {
		c.t.SendDatagram(d)
	}
	for _, m := range ctl {
		c.t.SendControl(m)
	}
}

func (c *Client) evaluate(vt *vtrack, now int64) {
	if now-vt.lastEval < trackEveryUs {
		return
	}
	vt.lastEval = now
	if vt.face != 0 {
		budget := c.trackBudget(vt)
		switch {
		case vt.rep == RepVideo && budget < float64(c.cfg.FaceDownBitrate):
			vt.highSince = 0
			if vt.lowSince == 0 {
				vt.lowSince = now
			} else if now-vt.lowSince >= faceHoldUs {
				vt.rep, vt.lowSince = RepFace, 0
			}
		case vt.rep == RepFace && budget > float64(c.cfg.FaceUpBitrate):
			c.snd.Controller().RequestProbe(now)
			vt.lowSince = 0
			if vt.highSince == 0 {
				vt.highSince = now
			} else if now-vt.highSince >= faceHoldUs {
				vt.rep, vt.highSince = RepVideo, 0
			}
		default:
			vt.lowSince, vt.highSince = 0, 0
			if vt.rep == RepFace {
				c.snd.Controller().RequestProbe(now)
			}
		}
	}
	for _, id := range vt.encs {
		p := c.pubs[id]
		want := RepVideo
		switch {
		case vt.rep == RepFace && id == vt.id:
			want = RepFace
		case vt.rep == RepFace:
			want = RepOff
		case len(vt.encs) > 1 && p.hinted && p.hint == 0:
			want = RepOff
		}
		c.setSending(id, p, want, now)
	}
	if vt.face != 0 {
		p := c.pubs[vt.face]
		want := RepOff
		if vt.rep == RepVideo && p.hinted && p.hint > 0 {
			want = RepFace
		}
		c.setSending(vt.face, p, want, now)
	}
	for _, id := range append(slices.Clone(vt.encs), vt.face) {
		p := c.pubs[id]
		if p == nil || p.sending != RepFace {
			continue
		}
		if vt.refAt == 0 || now-vt.refAt >= refEveryUs {
			spare := c.faceBudget(p) - faceBps
			if vt.refAt != 0 && spare < faceBps/4 {
				continue
			}
			vt.refAt = now
			c.event(&Event{Kind: EventReferencePicture, Stream: int(id), MaxBytes: min(max(int(spare*refEveryUs/8e6*refShare), minRefBytes), maxRefBytes)})
		}
	}
}

func (c *Client) setSending(id uint8, p *published, rep int, now int64) {
	if p == nil || p.sending == rep {
		return
	}
	p.sending = rep
	if rep == RepFace {
		p.track.refAt = 0
	} else {
		p.refParts = p.refParts[:0]
	}
	c.event(&Event{Kind: EventSendRepresentation, Stream: int(id), Rep: rep})
}

func (c *Client) PullAudio(stream int, now int64) *AudioFrame {
	c.mu.Lock()
	defer c.mu.Unlock()
	r := c.remotes[uint8(stream)]
	if r == nil || r.audio == nil {
		return &AudioFrame{Status: AudioWaiting}
	}
	o := r.audio.PullOut(now)
	return &AudioFrame{Status: o.Status, Data: o.Data, Offset: o.Offset}
}

func (c *Client) PollVideo(stream int, now int64) *VideoFrame {
	c.mu.Lock()
	defer c.mu.Unlock()
	r := c.remotes[uint8(stream)]
	if r == nil || r.video == nil {
		return nil
	}
	for {
		f := r.video.Pop(now)
		if f == nil {
			return nil
		}
		rep := RepVideo
		if f.Face {
			rep = RepFace
		}
		if rep != r.rep {
			r.rep = rep
			c.event(&Event{Kind: EventRepresentation, Stream: stream, Rep: rep})
		}
		if f.Face && f.Keyframe {
			if pic := r.addPart(f.Data); pic != nil {
				r.faceRef = pic
				c.event(&Event{Kind: EventFaceReference, Stream: stream, MaxBytes: len(pic)})
				return &VideoFrame{Stream: stream, FrameID: f.ID, Keyframe: true, Face: true, Timestamp: f.Timestamp, Data: pic}
			}
			continue
		}
		return &VideoFrame{Stream: stream, FrameID: f.ID, Keyframe: f.Keyframe, Layer: f.Layer, Timestamp: f.Timestamp,
			Face: f.Face, Recovery: f.Recovery, LongTerm: f.LongTerm, Data: f.Data}
	}
}

func (r *remote) addPart(b []byte) []byte {
	if len(b) < refPrefix || b[3] == 0 || b[2] >= b[3] {
		return nil
	}
	id := binary.BigEndian.Uint16(b)
	if r.building == nil || r.building.id != id || len(r.building.parts) != int(b[3]) {
		if r.building != nil && int16(id-r.building.id) < 0 {
			return nil
		}
		r.building = &refPicture{id: id, parts: make([][]byte, b[3])}
	}
	pic := r.building
	if pic.parts[b[2]] != nil {
		return nil
	}
	pic.parts[b[2]] = b[refPrefix:]
	pic.have++
	if pic.have < len(pic.parts) {
		return nil
	}
	var out []byte
	for _, p := range pic.parts {
		out = append(out, p...)
	}
	r.building = nil
	return out
}

func (c *Client) FaceReference(stream int) []byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	if r := c.remotes[uint8(stream)]; r != nil {
		return r.faceRef
	}
	return nil
}

func (c *Client) Representation(stream int) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	if r := c.remotes[uint8(stream)]; r != nil {
		return r.rep
	}
	return RepOff
}

func (c *Client) SendingRepresentation(stream int) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	if p := c.pubs[uint8(stream)]; p != nil && p.kind == KindVideo {
		return p.sending
	}
	return RepOff
}

func (c *Client) AckedFrame(stream int) int64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	if p := c.pubs[uint8(stream)]; p != nil && p.hasAck {
		return p.acked
	}
	return -1
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

func (c *Client) faceBudget(p *published) float64 {
	b := c.trackBudget(p.track)
	if p.hinted && p.hint > 0 {
		b = min(b, float64(p.hint))
	}
	return b
}

func (c *Client) trackBudget(vt *vtrack) float64 {
	avail := float64(c.snd.Target())
	videos := 0
	for sid, q := range c.pubs {
		if q.kind != KindVideo {
			nominal := float64(nominalAudio)
			if q.kind == KindAuth {
				nominal = nominalAuth
			}
			media, repair := c.snd.StreamRate(sid)
			if media == 0 {
				media, repair = nominal, nominal*c.snd.FECRatio(sid)
			}
			avail -= media + repair + c.snd.StreamOverheadBps(sid)
			continue
		}
		avail -= c.snd.StreamOverheadBps(sid)
		if q.track == nil || q.track.id == sid {
			videos++
		}
	}
	fec := 0.0
	if vt != nil {
		fec = c.snd.FECRatio(vt.id)
	}
	return avail / float64(max(videos, 1)) / (1 + fec) * headerFactor
}

func (c *Client) videoBitrate(id uint8) int {
	p := c.pubs[id]
	if p == nil || p.kind != KindVideo {
		return 0
	}
	if p.sending == RepOff {
		return 0
	}
	vt := p.track
	share := c.trackBudget(vt)
	if vt != nil && p.sending == RepFace {
		return max(int(min(share, faceBps)), minVideoBps)
	}
	if vt != nil && len(vt.encs) > 1 {
		if f := c.pubs[vt.face]; f != nil && f.sending == RepFace {
			share -= faceBps
		}
		w := simulcastWeights[len(vt.encs)-1]
		total, mine := 0.0, 0.0
		for i, e := range vt.encs {
			if q := c.pubs[e]; q != nil && q.sending == RepVideo {
				total += w[i]
				if e == id {
					mine = w[i]
				}
			}
		}
		if total > 0 {
			share = share * mine / total
		}
	} else if vt != nil {
		if f := c.pubs[vt.face]; f != nil && f.sending == RepFace {
			share -= faceBps
		}
	}
	if p.hint > 0 {
		share = min(share, float64(p.hint))
	}
	if share < minVideoBps/2 {
		return 0
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
		PacketsQueued: c.snd.Pacer().QueuedBytes(), Recoveries: c.recs,
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
			s.AudioFEC += r.audio.Stats.FEC
			s.AudioDRED += r.audio.Stats.DRED
			s.AudioInserted += r.audio.Stats.Inserted
		}
		if r.video != nil {
			s.VideoReleased += r.video.Stats.Released
			s.VideoUndecodable += r.video.Stats.Undecodable
			s.KeyframeRequests += r.video.Stats.KeyframeRequests
			s.RecoveryRequests += r.video.Stats.RecoveryRequests
			s.RecoveryFrames += r.video.Stats.Recovered
			s.Escalations += r.video.Stats.Escalated
		}
	}
	return s
}
