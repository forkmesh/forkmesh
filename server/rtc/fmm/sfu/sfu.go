package sfu

import (
	"errors"
	"slices"
	"sync"

	"forkmesh.com/mirror-node/rtc/fmm/cc"
	"forkmesh.com/mirror-node/rtc/fmm/hop"
	"forkmesh.com/mirror-node/rtc/fmm/internal/seqnum"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

type Transport interface {
	SendDatagram(p []byte) error
	MaxDatagramSize() int
	SendControl(p []byte) error
}

type Config struct {
	Admit        func(ticket string) (peer string, err error)
	MaxPeers     int
	MaxTracks    int
	StartBitrate int
	MinBitrate   int
	MaxBitrate   int
}

const (
	ErrCodeAdmission = 401
	ErrCodeFull      = 503
	ErrCodeTrack     = 400

	allocateEveryUs  = 200_000
	hintEveryUs      = 500_000
	hintRefreshUs    = 2_000_000
	keyframeGapUs    = 300_000
	meterWindowUs    = 1_000_000
	decisionRing     = 256
	defaultTL0Share  = 0.5
	upgradeHeadroom  = 0.9
	pauseHoldUs      = 2_000_000
	resumeHeadroom   = 1.5
	minHintBps       = 60_000
	idleHintBps      = 150_000
	defaultMaxPeers  = 16
	defaultMaxTracks = 4
)

var ErrRejected = errors.New("sfu: join rejected")

type layerMeter struct {
	bytes [wire.MaxLayer + 1]int
	bps   [wire.MaxLayer + 1]float64
	start int64
	seen  bool
}

func (m *layerMeter) add(layer int, n int, now int64) {
	if m.start == 0 {
		m.start = now
	}
	m.bytes[layer] += n
}

func (m *layerMeter) roll(now int64) {
	if m.start == 0 || now-m.start < meterWindowUs {
		return
	}
	dt := float64(now - m.start)
	for l := range m.bytes {
		r := float64(m.bytes[l]) * 8e6 / dt
		if m.seen {
			m.bps[l] = 0.5*m.bps[l] + 0.5*r
		} else {
			m.bps[l] = r
		}
		m.bytes[l] = 0
	}
	m.seen = true
	m.start = now
}

func (m *layerMeter) upTo(layer int) float64 {
	t := 0.0
	for l := 0; l <= layer; l++ {
		t += m.bps[l]
	}
	return t
}

type track struct {
	owner      *Peer
	id         uint8
	kind       int
	layers     int
	name       string
	meter      layerMeter
	seqs       seqnum.Unwrapper16
	frames     seqnum.Unwrapper16
	lastKeyReq int64
	fwds       []*forwarder
	hint       int
	hintAt     int64
}

func (t *track) audio() bool { return t.kind == wire.TrackAudio }

func (t *track) requestKeyframe(now int64) {
	if t.audio() || (t.lastKeyReq != 0 && now-t.lastKeyReq < keyframeGapUs) {
		return
	}
	t.lastKeyReq = now
	t.owner.send(wire.KeyframeRequest{Stream: t.id})
}

type decision struct {
	frame   int64
	valid   bool
	forward bool
	offset  int64
}

type forwarder struct {
	sub        *Peer
	src        *track
	down       uint8
	subscribed bool
	maxLayer   int
	layer      int
	want       int
	needKey    bool
	started    bool
	offset     int64
	highestIn  int64
	decisions  [decisionRing]decision
	share      float64
	paused     bool
	pausedAt   int64
	Forwarded  int
	Dropped    int
	Pauses     int
}

func (f *forwarder) decide(m *wire.Media, frame, seq int64) decision {
	slot := &f.decisions[frame%decisionRing]
	if slot.valid && slot.frame == frame {
		return *slot
	}
	first := seq - int64(m.FragIndex)
	fwd := true
	if !f.src.audio() {
		if f.paused {
			fwd = false
		} else if f.needKey {
			fwd = m.Keyframe
			if fwd {
				f.needKey = false
				f.layer = f.want
			}
		} else {
			if m.Keyframe || m.Layer == 0 {
				f.layer = f.want
			}
			f.layer = min(f.layer, f.want)
			fwd = int(m.Layer) <= f.layer
		}
	}
	d := decision{frame: frame, valid: true, forward: fwd}
	switch {
	case fwd && !f.started:
		f.started = true
		f.offset = first
		f.highestIn = first - 1
		d.offset = f.offset
	case fwd:
		d.offset = f.offset
	case f.started && first > f.highestIn:
		f.offset += int64(m.FragCount)
		f.highestIn = first + int64(m.FragCount) - 1
	}
	*slot = d
	return d
}

func (f *forwarder) forward(m *wire.Media, raw []byte, frame, seq int64, now int64) {
	if !f.subscribed {
		return
	}
	d := f.decide(m, frame, seq)
	if !d.forward {
		f.Dropped++
		return
	}
	if len(raw) > f.sub.t.MaxDatagramSize() {
		f.Dropped++
		return
	}
	f.highestIn = max(f.highestIn, seq)
	out := slices.Clone(raw)
	wire.SetStream(out, f.down)
	wire.SetSeq(out, uint16(seq-d.offset))
	wire.SetTransportSeq(out, 0)
	f.Forwarded++
	f.sub.down.Send(out, now)
}

type Peer struct {
	room      *Room
	t         Transport
	id        string
	joined    bool
	closed    bool
	ctl       wire.Reader
	up        *hop.Receiver
	down      *hop.Sender
	tracks    map[uint8]*track
	fwds      map[uint8]*forwarder
	nextDown  uint8
	recvCap   int
	lastAlloc int64
	lastHint  int64
}

type Room struct {
	mu    sync.Mutex
	cfg   Config
	peers []*Peer
}

func NewRoom(cfg Config) *Room {
	if cfg.MaxPeers <= 0 {
		cfg.MaxPeers = defaultMaxPeers
	}
	if cfg.MaxTracks <= 0 {
		cfg.MaxTracks = defaultMaxTracks
	}
	return &Room{cfg: cfg}
}

func (r *Room) Connect(t Transport) *Peer {
	r.mu.Lock()
	defer r.mu.Unlock()
	return &Peer{
		room: r, t: t, up: hop.NewReceiver(),
		down:   hop.NewSender(t, cc.Config{StartBps: r.cfg.StartBitrate, MinBps: r.cfg.MinBitrate, MaxBps: r.cfg.MaxBitrate}),
		tracks: map[uint8]*track{}, fwds: map[uint8]*forwarder{}, nextDown: 1,
	}
}

func (r *Room) Peers() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	var out []string
	for _, p := range r.peers {
		out = append(out, p.id)
	}
	return out
}

func (p *Peer) ID() string { return p.id }

func (p *Peer) Downstream() *hop.Sender { return p.down }

func (p *Peer) send(m wire.Message) {
	if b, err := wire.Append(nil, m); err == nil {
		p.t.SendControl(b)
	}
}

func (p *Peer) HandleDatagram(b []byte, now int64) {
	r := p.room
	r.mu.Lock()
	defer r.mu.Unlock()
	if !p.joined || p.closed {
		return
	}
	for _, raw := range p.up.Handle(b, now) {
		m, err := wire.ParseMedia(raw)
		if err != nil {
			continue
		}
		t := p.tracks[m.Stream]
		if t == nil {
			continue
		}
		if !t.audio() && int(m.Layer) >= t.layers {
			continue
		}
		seq := t.seqs.Unwrap(m.Seq)
		frame := t.frames.Unwrap(m.FrameID)
		t.meter.add(int(m.Layer), len(raw), now)
		for _, f := range t.fwds {
			f.forward(&m, raw, frame, seq, now)
		}
	}
}

func (p *Peer) HandleControl(b []byte, now int64) {
	r := p.room
	r.mu.Lock()
	defer r.mu.Unlock()
	if p.closed {
		return
	}
	if p.ctl.Write(b) != nil {
		r.closeLocked(p)
		return
	}
	for {
		m, err := p.ctl.Next()
		if err != nil {
			r.closeLocked(p)
			return
		}
		if m == nil || p.closed {
			return
		}
		r.dispatch(p, m, now)
	}
}

func (r *Room) dispatch(p *Peer, m wire.Message, now int64) {
	if j, ok := m.(wire.Join); ok {
		r.join(p, j.Ticket, now)
		return
	}
	if !p.joined {
		p.send(wire.Error{Code: ErrCodeAdmission, Text: "join first"})
		r.closeLocked(p)
		return
	}
	switch v := m.(type) {
	case *wire.Report:
		p.down.OnReport(v, now)
	case wire.Nack:
		p.down.OnNack(v, now)
	case wire.Publish:
		r.publish(p, v, now)
	case wire.Unpublish:
		if t := p.tracks[v.Stream]; t != nil {
			r.unpublish(t)
		}
	case wire.Subscribe:
		if f := p.fwds[v.Stream]; f != nil {
			f.maxLayer = int(v.MaxLayer)
			f.want = min(f.want, f.maxLayer)
			if !f.subscribed {
				f.subscribed = true
				f.want = min(f.src.layers-1, f.maxLayer)
				f.layer = f.want
				p.down.AddStream(f.down, f.src.audio())
				if !f.src.audio() {
					f.needKey = true
					f.src.requestKeyframe(now)
				}
			}
		}
	case wire.Unsubscribe:
		if f := p.fwds[v.Stream]; f != nil && f.subscribed {
			f.subscribed, f.started = false, false
			p.down.RemoveStream(f.down)
		}
	case wire.KeyframeRequest:
		if f := p.fwds[v.Stream]; f != nil && f.subscribed {
			f.src.requestKeyframe(now)
		}
	case wire.BitrateHint:
		if v.Stream == wire.AllStreams {
			p.recvCap = int(v.Bitrate)
		}
	case wire.Leave:
		r.closeLocked(p)
	}
}

func (r *Room) join(p *Peer, ticket string, now int64) {
	if p.joined {
		return
	}
	id := ""
	var err error
	if r.cfg.Admit != nil {
		id, err = r.cfg.Admit(ticket)
	} else {
		err = ErrRejected
	}
	if err != nil || id == "" || len(id) > wire.MaxName {
		p.send(wire.Error{Code: ErrCodeAdmission, Text: "ticket refused"})
		r.closeLocked(p)
		return
	}
	for _, o := range r.peers {
		if o.id == id {
			p.send(wire.Error{Code: ErrCodeAdmission, Text: "peer id in use"})
			r.closeLocked(p)
			return
		}
	}
	if len(r.peers) >= r.cfg.MaxPeers {
		p.send(wire.Error{Code: ErrCodeFull, Text: "room full"})
		r.closeLocked(p)
		return
	}
	p.id, p.joined = id, true
	r.peers = append(r.peers, p)
	p.send(wire.Joined{Peer: id})
	for _, o := range r.peers {
		if o == p {
			continue
		}
		for _, t := range sortedTracks(o) {
			r.attach(t, p)
		}
	}
}

func sortedTracks(p *Peer) []*track {
	var out []*track
	for _, t := range p.tracks {
		out = append(out, t)
	}
	slices.SortFunc(out, func(a, b *track) int { return int(a.id) - int(b.id) })
	return out
}

func (r *Room) publish(p *Peer, v wire.Publish, now int64) {
	if p.tracks[v.Stream] != nil || len(p.tracks) >= r.cfg.MaxTracks {
		p.send(wire.Error{Code: ErrCodeTrack, Text: "track refused"})
		return
	}
	t := &track{owner: p, id: v.Stream, kind: int(v.Kind), layers: int(v.Layers), name: v.Name}
	p.tracks[v.Stream] = t
	p.up.RemoveStream(v.Stream)
	if !t.audio() {
		p.up.EnableNack(v.Stream)
	}
	for _, o := range r.peers {
		if o != p {
			r.attach(t, o)
		}
	}
}

func (r *Room) attach(t *track, sub *Peer) {
	var id uint8
	for range 255 {
		c := sub.nextDown
		sub.nextDown++
		if sub.nextDown == 0 || sub.nextDown == wire.AllStreams {
			sub.nextDown = 1
		}
		if c != 0 && c != wire.AllStreams && sub.fwds[c] == nil {
			id = c
			break
		}
	}
	if id == 0 {
		return
	}
	f := &forwarder{sub: sub, src: t, down: id, maxLayer: wire.MaxLayer, want: t.layers - 1, layer: t.layers - 1}
	sub.fwds[id] = f
	t.fwds = append(t.fwds, f)
	sub.send(wire.TrackAdded{Stream: id, Kind: uint8(t.kind), Layers: uint8(t.layers), Peer: t.owner.id, Name: t.name})
}

func (r *Room) unpublish(t *track) {
	for _, f := range t.fwds {
		delete(f.sub.fwds, f.down)
		f.sub.down.RemoveStream(f.down)
		f.sub.send(wire.TrackRemoved{Stream: f.down})
	}
	t.fwds = nil
	delete(t.owner.tracks, t.id)
}

func (p *Peer) Close(now int64) {
	p.room.mu.Lock()
	defer p.room.mu.Unlock()
	p.room.closeLocked(p)
}

func (r *Room) closeLocked(p *Peer) {
	if p.closed {
		return
	}
	p.closed = true
	for _, t := range sortedTracks(p) {
		r.unpublish(t)
	}
	for _, f := range p.fwds {
		f.src.fwds = slices.DeleteFunc(f.src.fwds, func(x *forwarder) bool { return x == f })
	}
	p.fwds = map[uint8]*forwarder{}
	r.peers = slices.DeleteFunc(r.peers, func(x *Peer) bool { return x == p })
}

func (r *Room) Tick(now int64) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, p := range r.peers {
		for _, t := range p.tracks {
			t.meter.roll(now)
		}
	}
	for _, p := range r.peers {
		p.down.Tick(now)
		if rep := p.up.Report(now); rep != nil {
			p.t.SendControl(rep)
		}
		p.up.SetRTT(p.down.Controller().RTT())
		for _, n := range p.up.Nacks(now) {
			p.t.SendControl(n)
		}
		if now-p.lastAlloc >= allocateEveryUs {
			p.lastAlloc = now
			p.allocate(now)
		}
	}
	for _, p := range r.peers {
		if now-p.lastHint >= hintEveryUs {
			p.lastHint = now
			for _, t := range sortedTracks(p) {
				t.updateHint(now)
			}
		}
	}
}

func (p *Peer) budget() float64 {
	b := float64(p.down.Target()) - p.down.OverheadBps()
	if p.recvCap > 0 {
		b = min(b, float64(p.recvCap))
	}
	return b
}

func (p *Peer) allocate(now int64) {
	budget := p.budget()
	var videos []*forwarder
	for _, f := range p.fwds {
		if !f.subscribed {
			continue
		}
		if f.src.audio() {
			budget -= f.src.meter.upTo(0) * (1 + p.down.FECRatio(f.down))
			continue
		}
		videos = append(videos, f)
	}
	if len(videos) == 0 {
		return
	}
	slices.SortFunc(videos, func(a, b *forwarder) int { return int(a.down) - int(b.down) })
	share := max(budget, 0) / float64(len(videos))
	for _, f := range videos {
		f.share = share / (1 + p.down.FECRatio(f.down))
	}
	remaining := max(budget, 0)
	want := make([]int, len(videos))
	known := true
	for i, f := range videos {
		if !f.src.meter.seen {
			known = false
		}
		want[i] = 0
		remaining -= f.src.meter.upTo(0) * (1 + p.down.FECRatio(f.down))
	}
	if !known {
		for _, f := range videos {
			f.want = min(f.src.layers-1, f.maxLayer)
		}
		return
	}
	for changed := true; changed; {
		changed = false
		for i, f := range videos {
			next := want[i] + 1
			if next > min(f.src.layers-1, f.maxLayer) {
				continue
			}
			cost := (f.src.meter.upTo(next) - f.src.meter.upTo(want[i])) * (1 + p.down.FECRatio(f.down))
			limit := remaining
			if next > f.layer {
				limit *= upgradeHeadroom
			}
			if cost <= limit {
				want[i] = next
				remaining -= cost
				changed = true
			}
		}
	}
	for i, f := range videos {
		f.want = want[i]
	}
	p.shed(videos, max(budget, 0), now)
}

func (p *Peer) shed(videos []*forwarder, budget float64, now int64) {
	need := 0.0
	for _, f := range videos {
		if !f.paused {
			need += f.src.meter.upTo(0) * (1 + p.down.FECRatio(f.down))
		}
	}
	for i := len(videos) - 1; i >= 0 && need > budget; i-- {
		f := videos[i]
		if f.paused || now-f.pausedAt < pauseHoldUs {
			continue
		}
		f.paused, f.pausedAt = true, now
		f.Pauses++
		need -= f.src.meter.upTo(0) * (1 + p.down.FECRatio(f.down))
	}
	for _, f := range videos {
		if !f.paused || now-f.pausedAt < pauseHoldUs {
			continue
		}
		cost := f.src.meter.upTo(0) * (1 + p.down.FECRatio(f.down))
		if need+resumeHeadroom*cost <= budget {
			f.paused, f.pausedAt = false, now
			f.needKey = true
			f.src.requestKeyframe(now)
			need += cost
		}
	}
}

func (t *track) updateHint(now int64) {
	if t.audio() {
		return
	}
	total := t.meter.upTo(wire.MaxLayer)
	tl0 := defaultTL0Share
	if total > 0 && t.meter.seen {
		tl0 = min(max(t.meter.upTo(0)/total, 0.3), 1)
	}
	hint := float64(idleHintBps)
	subs := 0
	need, best := 0.0, 0.0
	for _, f := range t.fwds {
		if !f.subscribed || f.share <= 0 {
			continue
		}
		n := f.share / tl0
		if subs == 0 || n < need {
			need = n
		}
		best = max(best, f.share)
		subs++
	}
	if subs > 0 {
		hint = max(min(need, best)*upgradeHeadroom, minHintBps)
	}
	h := int(hint)
	if t.hint == 0 || absDiff(h, t.hint)*10 > t.hint || now-t.hintAt >= hintRefreshUs {
		t.hint, t.hintAt = h, now
		t.owner.send(wire.BitrateHint{Stream: t.id, Bitrate: uint32(h)})
	}
}

func absDiff(a, b int) int {
	if a > b {
		return a - b
	}
	return b - a
}
