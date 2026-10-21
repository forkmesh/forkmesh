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
	Admit             func(ticket string) (peer string, err error)
	MaxPeers          int
	MaxTracks         int
	StartBitrate      int
	MinBitrate        int
	MaxBitrate        int
	FeedbackOnControl bool
	NoRecovery        bool
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
	cheapHeadroom    = 1.15
	cheapBps         = 60_000
	probeShare       = 0.9
	faceModeUs       = 500_000
	activeGapUs      = 500_000
	encodingUpHoldUs = 3_000_000
	encodingHeadroom = 0.8
	lowestHintBps    = 30_000
	faceOverheadBps  = 30 * 48 * 8
	videoToFaceBps   = 100_000
	faceToVideoBps   = 160_000
	overloadFactor   = 1.3
	overloadHoldUs   = 1_000_000
	deliveredLoss    = 0.05
	minHintBps       = 60_000
	idleHintBps      = 150_000
	defaultMaxPeers  = 16
	defaultMaxTracks = 4
	ackEveryUs       = 100_000
	encodingHoldUs   = 1_000_000
	recoveryMarginUs = 150_000
	maxLongTerm      = 32
	maxAcked         = 32
	maxRefSpan       = 240
	lowNominalBps    = 32_000
	videoNominalBps  = 300_000
	knownFloorBps    = 5_000
)

var ErrRejected = errors.New("sfu: join rejected")

type layerMeter struct {
	bytes   [wire.MaxLayer + 1]int
	packets [wire.MaxLayer + 1]int
	bps     [wire.MaxLayer + 1]float64
	ohBps   [wire.MaxLayer + 1]float64
	known   [wire.MaxLayer + 1]float64
	start   int64
	seen    bool
}

func (m *layerMeter) add(layer int, n int, now int64) {
	if m.start == 0 {
		m.start = now
	}
	m.bytes[layer] += n
	m.packets[layer]++
}

func (m *layerMeter) roll(now int64, active bool) {
	if m.start == 0 || now-m.start < meterWindowUs {
		return
	}
	if !active {
		m.seen = false
	}
	dt := float64(now - m.start)
	total := 0
	for l := range m.bytes {
		total += m.bytes[l]
	}
	for l := range m.bytes {
		r := float64(m.bytes[l]) * 8e6 / dt
		o := float64(m.packets[l]*hop.PacketOverhead) * 8e6 / dt
		if m.seen {
			m.bps[l] = 0.5*m.bps[l] + 0.5*r
			m.ohBps[l] = 0.5*m.ohBps[l] + 0.5*o
		} else {
			m.bps[l], m.ohBps[l] = r, o
		}
		m.bytes[l], m.packets[l] = 0, 0
	}
	if active && total > 0 && m.upTo(wire.MaxLayer) > knownFloorBps {
		m.known = m.bps
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

func (m *layerMeter) overheadUpTo(layer int) float64 {
	t := 0.0
	for l := 0; l <= layer; l++ {
		t += m.ohBps[l]
	}
	return t
}

func (m *layerMeter) knownUpTo(layer int) float64 {
	t := 0.0
	for l := 0; l <= layer; l++ {
		t += m.known[l]
	}
	return t
}

type encoding struct {
	t          *track
	id         uint8
	rank       int
	layers     int
	meter      layerMeter
	seqs       seqnum.Unwrapper16
	frames     seqnum.Unwrapper16
	lastKeyReq int64
	hint       int
	hintAt     int64
	hinted     bool
	highest    int64
	face       bool
	lastAt     int64
	activeFrom int64
	longTerm   []int64
	lastRec    int64
	recHigh    int64
	acked      int64
	ackSent    bool
	Recoveries int
	Suppressed int
	Keyframes  int
}

func (e *encoding) requestKeyframe(now int64) {
	if e.t.audio() || (e.lastKeyReq != 0 && now-e.lastKeyReq < keyframeGapUs) {
		return
	}
	e.lastKeyReq = now
	e.Keyframes++
	e.t.owner.send(wire.KeyframeRequest{Stream: e.id})
}

func (e *encoding) activeOver(from, now int64) bool {
	return e.lastAt > 0 && now-e.lastAt <= activeGapUs && e.activeFrom <= from
}

func (e *encoding) active(now int64) bool {
	return e.lastAt > 0 && now-e.lastAt <= activeGapUs
}

func (e *encoding) rate(layer int) float64 {
	layer = min(layer, e.layers-1)
	if e.meter.seen {
		if r := e.meter.upTo(layer); r > knownFloorBps {
			return r
		}
	}
	if r := e.meter.knownUpTo(layer); r > 0 {
		return r
	}
	nominal := float64(videoNominalBps)
	if e.face || (len(e.t.encs) > 1 && e == e.t.encs[0]) {
		nominal = lowNominalBps
	}
	return nominal * float64(layer+1) / float64(e.layers)
}

func (e *encoding) wireRate(layer int) float64 {
	r := e.rate(layer)
	if e.meter.seen {
		r += e.meter.overheadUpTo(min(layer, e.layers-1))
	} else {
		r += float64(faceOverheadBps)
	}
	return r
}

func (e *encoding) noteFrame(m *wire.Media, frame int64, now int64) {
	if frame >= e.highest {
		if m.Face != e.face {
			e.meter = layerMeter{}
		}
		if e.lastAt == 0 || now-e.lastAt > activeGapUs {
			e.activeFrom = now
		}
		e.face, e.lastAt = m.Face, now
		if m.Keyframe && frame > e.highest {
			e.longTerm = slices.DeleteFunc(e.longTerm, func(x int64) bool { return x < frame })
		}
	}
	e.highest = max(e.highest, frame)
	if !m.LongTerm {
		return
	}
	i, found := slices.BinarySearch(e.longTerm, frame)
	if found {
		return
	}
	e.longTerm = slices.Insert(e.longTerm, i, frame)
	if len(e.longTerm) > maxLongTerm {
		e.longTerm = e.longTerm[1:]
	}
}

func (e *encoding) viewers() []*forwarder {
	var out []*forwarder
	for _, f := range e.t.fwds {
		if f.subscribed && f.cur == e && f.started && !f.needKey && !f.paused {
			out = append(out, f)
		}
	}
	return out
}

func (e *encoding) common() (int64, bool) {
	subs := e.viewers()
	if len(subs) == 0 {
		return 0, false
	}
	for i := len(e.longTerm) - 1; i >= 0; i-- {
		id := e.longTerm[i]
		all := true
		for _, f := range subs {
			if !slices.Contains(f.acked, id) {
				all = false
				break
			}
		}
		if all {
			return id, true
		}
	}
	return 0, false
}

type track struct {
	owner  *Peer
	id     uint8
	kind   int
	layers int
	name   string
	encs   []*encoding
	fwds   []*forwarder
}

func (t *track) audio() bool { return t.kind == wire.TrackAudio }

func (t *track) startEncoding(now int64) *encoding {
	for _, e := range t.encs {
		if e.faceMode(now) {
			return e
		}
	}
	for _, e := range t.encs {
		if e.active(now) && !e.face {
			return e
		}
	}
	return t.primary()
}

func (t *track) faceEncoding() *encoding {
	if len(t.encs) > 1 && t.encs[0].rank == 0 {
		return t.encs[0]
	}
	return nil
}

func (t *track) lowestVideo() *encoding {
	for _, e := range t.encs {
		if e != t.faceEncoding() {
			return e
		}
	}
	return t.encs[0]
}

func (t *track) primary() *encoding {
	for _, e := range t.encs {
		if e.id == t.id {
			return e
		}
	}
	return t.encs[len(t.encs)-1]
}

type decision struct {
	enc     *encoding
	frame   int64
	valid   bool
	forward bool
	offset  int64
}

type forwarder struct {
	sub           *Peer
	src           *track
	down          uint8
	subscribed    bool
	maxLayer      int
	layer         int
	want          int
	cur           *encoding
	target        *encoding
	mapped        *encoding
	switchedAt    int64
	needKey       bool
	started       bool
	rebase        bool
	offset        int64
	fOffset       int64
	highestIn     int64
	lastDown      int64
	lastDownFrame int64
	mapFloor      int64
	decisions     [decisionRing]decision
	acked         []int64
	share         float64
	paused        bool
	pausedAt      int64
	Forwarded     int
	Dropped       int
	Pauses        int
	Switches      int
}

func switchPoint(m *wire.Media) bool {
	return m.Keyframe || m.RefDelta == 0
}

func (f *forwarder) decide(e *encoding, m *wire.Media, frame, seq int64) decision {
	slot := &f.decisions[frame%decisionRing]
	if slot.valid && slot.enc == e && slot.frame == frame {
		return *slot
	}
	first := seq - int64(m.FragIndex)
	fwd := true
	if !f.src.audio() {
		switch {
		case f.paused:
			fwd = false
		case f.needKey:
			fwd = switchPoint(m)
			if fwd {
				f.needKey = false
				f.layer = f.want
				f.rebase = true
			}
		default:
			if m.Keyframe || m.Layer == 0 {
				f.layer = f.want
			}
			f.layer = min(f.layer, f.want)
			fwd = int(m.Layer) <= f.layer
		}
	}
	d := decision{enc: e, frame: frame, valid: true, forward: fwd}
	switch {
	case fwd && (f.rebase || !f.started):
		if f.mapped != e {
			if f.started {
				f.fOffset = f.lastDownFrame + 1 - frame
			}
			f.mapped = e
			f.mapFloor = frame + f.fOffset
		}
		f.started, f.rebase = true, false
		f.offset = first - (f.lastDown + 1)
		f.highestIn = first - 1
		d.offset = f.offset
	case fwd:
		d.offset = f.offset
	case f.started && !f.rebase && first > f.highestIn:
		f.offset += int64(m.FragCount)
		f.highestIn = first + int64(m.FragCount) - 1
	}
	if fwd {
		f.lastDown = max(f.lastDown, first-d.offset+int64(m.FragCount)-1)
		f.lastDownFrame = max(f.lastDownFrame, frame+f.fOffset)
	}
	*slot = d
	return d
}

func (f *forwarder) forward(e *encoding, m *wire.Media, raw []byte, frame, seq int64, now int64) {
	if !f.subscribed {
		return
	}
	if f.target == e && e != f.cur && switchPoint(m) && !f.paused {
		f.cur, f.target = e, nil
		f.needKey = true
		f.switchedAt = now
		f.acked = f.acked[:0]
		f.Switches++
	}
	if e != f.cur {
		return
	}
	d := f.decide(e, m, frame, seq)
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
	if !f.src.audio() {
		wire.SetFrameID(out, uint16(frame+f.fOffset))
	}
	wire.SetTransportSeq(out, 0)
	f.Forwarded++
	f.sub.down.Send(out, now)
}

func (f *forwarder) upFrame(down uint16) (int64, bool) {
	if !f.started || f.cur == nil || f.mapped != f.cur {
		return 0, false
	}
	d := seqnum.Near16(down, f.lastDownFrame)
	if d < f.mapFloor || d > f.lastDownFrame {
		return 0, false
	}
	return d - f.fOffset, true
}

func (f *forwarder) ack(frames []uint16) {
	for _, x := range frames {
		up, ok := f.upFrame(x)
		if !ok || slices.Contains(f.acked, up) {
			continue
		}
		f.acked = append(f.acked, up)
		if len(f.acked) > maxAcked {
			f.acked = f.acked[1:]
		}
	}
}

func (f *forwarder) choice() *encoding {
	if f.target != nil {
		return f.target
	}
	return f.cur
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
	encs      map[uint8]*encoding
	fwds      map[uint8]*forwarder
	nextDown  uint8
	recvCap   int
	lastAlloc int64
	lastHint  int64
	lastAck   int64
	overSince int64
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
	p := &Peer{
		room: r, t: t, up: hop.NewReceiver(),
		down:   hop.NewSender(t, cc.Config{StartBps: r.cfg.StartBitrate, MinBps: r.cfg.MinBitrate, MaxBps: r.cfg.MaxBitrate}),
		tracks: map[uint8]*track{}, encs: map[uint8]*encoding{}, fwds: map[uint8]*forwarder{}, nextDown: 1,
	}
	if !r.cfg.FeedbackOnControl {
		p.up.SetRepeat(1)
	}
	return p
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

func (p *Peer) Closed() bool {
	p.room.mu.Lock()
	defer p.room.mu.Unlock()
	return p.closed
}

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
	if k, err := wire.Kind(b); err == nil && k == wire.KindFeedback {
		msgs, err := wire.ParseFeedback(b)
		if err != nil {
			return
		}
		for _, m := range msgs {
			r.dispatch(p, m, now)
		}
		return
	}
	for _, raw := range p.up.Handle(b, now) {
		m, err := wire.ParseMedia(raw)
		if err != nil {
			continue
		}
		e := p.encs[m.Stream]
		if e == nil {
			continue
		}
		t := e.t
		if !t.audio() && int(m.Layer) >= e.layers {
			continue
		}
		seq := e.seqs.Unwrap(m.Seq)
		frame := e.frames.Unwrap(m.FrameID)
		e.meter.add(int(m.Layer), len(raw), now)
		if !t.audio() {
			e.noteFrame(&m, frame, now)
		}
		for _, f := range t.fwds {
			f.forward(e, &m, raw, frame, seq, now)
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
		if e := p.encs[v.Stream]; e != nil {
			r.unpublishEncoding(e, now)
		}
	case wire.Subscribe:
		if f := p.fwds[v.Stream]; f != nil {
			f.maxLayer = int(v.MaxLayer)
			f.want = min(f.want, f.maxLayer)
			if !f.subscribed {
				f.subscribed = true
				f.cur = f.src.startEncoding(now)
				f.want = min(f.cur.layers-1, f.maxLayer)
				f.layer = f.want
				p.down.AddStream(f.down, f.src.audio())
				if !f.src.audio() {
					f.needKey = true
					f.cur.requestKeyframe(now)
				}
			}
		}
	case wire.Unsubscribe:
		if f := p.fwds[v.Stream]; f != nil && f.subscribed {
			f.subscribed, f.started, f.target, f.mapped = false, false, nil, nil
			f.lastDown, f.lastDownFrame, f.fOffset = -1, -1, 0
			p.down.RemoveStream(f.down)
		}
	case wire.KeyframeRequest:
		if f := p.fwds[v.Stream]; f != nil && f.subscribed && f.cur != nil && !f.paused {
			f.cur.requestKeyframe(now)
			if f.target != nil {
				f.target.requestKeyframe(now)
			}
		}
	case wire.Ack:
		if f := p.fwds[v.Stream]; f != nil && f.subscribed && !f.src.audio() {
			f.ack(v.Frames)
		}
	case wire.Recovery:
		if f := p.fwds[v.Stream]; f != nil && f.subscribed && !f.src.audio() {
			r.recover(f, v.Frame, now)
		}
	case wire.BitrateHint:
		if v.Stream == wire.AllStreams {
			p.recvCap = int(v.Bitrate)
		}
	case wire.Leave:
		r.closeLocked(p)
	}
}

func (r *Room) recover(f *forwarder, frame uint16, now int64) {
	e := f.cur
	if e == nil {
		return
	}
	if r.cfg.NoRecovery || !f.started || f.needKey || f.paused {
		e.requestKeyframe(now)
		return
	}
	stuckDown := seqnum.Near16(frame, f.lastDownFrame)
	if stuckDown < f.mapFloor {
		return
	}
	stuck := stuckDown - f.fOffset
	if e.lastRec != 0 && stuck <= e.recHigh {
		life := e.t.owner.down.Controller().RTT() + f.sub.down.Controller().RTT() + recoveryMarginUs
		if now-e.lastRec < life {
			e.Suppressed++
			return
		}
		e.requestKeyframe(now)
		return
	}
	ref, ok := e.common()
	if !ok || e.highest-ref > maxRefSpan {
		e.requestKeyframe(now)
		return
	}
	e.lastRec, e.recHigh = now, e.highest
	e.Recoveries++
	e.t.owner.send(wire.Recovery{Stream: e.id, Frame: uint16(ref)})
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
	refuse := func() { p.send(wire.Error{Code: ErrCodeTrack, Text: "track refused"}) }
	if p.encs[v.Stream] != nil {
		refuse()
		return
	}
	if v.Track != 0 && v.Track != v.Stream {
		t := p.tracks[v.Track]
		if t == nil || t.audio() || len(t.encs) >= wire.MaxEncodings || slices.ContainsFunc(t.encs, func(e *encoding) bool { return e.rank == int(v.Rank) }) {
			refuse()
			return
		}
		e := &encoding{t: t, id: v.Stream, rank: int(v.Rank), layers: int(v.Layers)}
		r.addEncoding(p, t, e)
		return
	}
	if len(p.tracks) >= r.cfg.MaxTracks {
		refuse()
		return
	}
	t := &track{owner: p, id: v.Stream, kind: int(v.Kind), layers: int(v.Layers), name: v.Name}
	p.tracks[v.Stream] = t
	r.addEncoding(p, t, &encoding{t: t, id: v.Stream, rank: int(v.Rank), layers: int(v.Layers)})
	for _, o := range r.peers {
		if o != p {
			r.attach(t, o)
		}
	}
}

func (r *Room) addEncoding(p *Peer, t *track, e *encoding) {
	t.encs = append(t.encs, e)
	slices.SortFunc(t.encs, func(a, b *encoding) int { return a.rank - b.rank })
	p.encs[e.id] = e
	p.up.RemoveStream(e.id)
	if !t.audio() {
		p.up.EnableNack(e.id)
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
	f := &forwarder{sub: sub, src: t, down: id, maxLayer: wire.MaxLayer, want: t.layers - 1, layer: t.layers - 1, lastDown: -1, lastDownFrame: -1}
	sub.fwds[id] = f
	t.fwds = append(t.fwds, f)
	sub.send(wire.TrackAdded{Stream: id, Kind: uint8(t.kind), Layers: uint8(t.layers), Peer: t.owner.id, Name: t.name})
}

func (r *Room) unpublishEncoding(e *encoding, now int64) {
	t := e.t
	if e.id == t.id || len(t.encs) == 1 {
		r.unpublish(t)
		return
	}
	t.encs = slices.DeleteFunc(t.encs, func(x *encoding) bool { return x == e })
	delete(t.owner.encs, e.id)
	t.owner.up.RemoveStream(e.id)
	for _, f := range t.fwds {
		if f.target == e {
			f.target = nil
		}
		if f.cur == e {
			f.cur = t.primary()
			if f.subscribed {
				f.needKey = true
				f.cur.requestKeyframe(now)
			}
		}
	}
}

func (r *Room) unpublish(t *track) {
	for _, f := range t.fwds {
		delete(f.sub.fwds, f.down)
		f.sub.down.RemoveStream(f.down)
		f.sub.send(wire.TrackRemoved{Stream: f.down})
	}
	t.fwds = nil
	for _, e := range t.encs {
		delete(t.owner.encs, e.id)
		t.owner.up.RemoveStream(e.id)
	}
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
			for _, e := range t.encs {
				e.meter.roll(now, t.audio() || e.activeOver(e.meter.start, now))
			}
		}
	}
	for _, p := range r.peers {
		p.down.Tick(now)
		p.up.SetRTT(p.down.Controller().RTT())
		p.sendFeedback(now)
		if now-p.lastAlloc >= allocateEveryUs {
			p.lastAlloc = now
			p.allocate(now)
		}
	}
	for _, p := range r.peers {
		if now-p.lastHint >= hintEveryUs {
			p.lastHint = now
			for _, t := range sortedTracks(p) {
				for _, e := range t.encs {
					e.updateHint(now, false)
				}
			}
		}
		if now-p.lastAck >= ackEveryUs {
			p.lastAck = now
			for _, t := range sortedTracks(p) {
				for _, e := range t.encs {
					e.sendAck()
				}
			}
		}
	}
}

func (p *Peer) sendFeedback(now int64) {
	var msgs [][]byte
	if rep := p.up.Report(now); rep != nil {
		msgs = append(msgs, rep)
	}
	msgs = append(msgs, p.up.Nacks(now)...)
	if len(msgs) == 0 {
		return
	}
	if p.room.cfg.FeedbackOnControl {
		for _, m := range msgs {
			p.t.SendControl(m)
		}
		return
	}
	dgs, ctl := hop.PackFeedback(msgs, min(p.t.MaxDatagramSize(), hop.MaxPacketSize))
	for _, d := range dgs {
		p.t.SendDatagram(d)
	}
	for _, m := range ctl {
		p.t.SendControl(m)
	}
}

func (e *encoding) sendAck() {
	if e.t.audio() {
		return
	}
	id, ok := e.common()
	if !ok || (e.ackSent && id == e.acked) {
		return
	}
	e.acked, e.ackSent = id, true
	e.t.owner.send(wire.Ack{Stream: e.id, Frames: []uint16{uint16(id)}})
}

func (p *Peer) budget() float64 {
	b := float64(p.down.Target()) - p.down.OverheadBps()
	if p.recvCap > 0 {
		b = min(b, float64(p.recvCap))
	}
	return b
}

type option struct {
	e     *encoding
	layer int
}

func (e *encoding) faceMode(now int64) bool {
	return e.face && now-e.lastAt < faceModeUs
}

func (f *forwarder) options(now int64) []option {
	var out []option
	fe := f.src.faceEncoding()
	for _, e := range f.src.encs {
		if len(f.src.encs) > 1 && e.faceMode(now) && e != fe {
			return []option{{e, 0}}
		}
	}
	if fe != nil {
		limit := float64(faceToVideoBps)
		if f.choice() != fe {
			limit = videoToFaceBps
		}
		if f.share < limit {
			return []option{{fe, 0}}
		}
	}
	for _, e := range f.src.encs {
		if e == fe {
			continue
		}
		for l := 0; l < e.layers && l <= f.maxLayer; l++ {
			out = append(out, option{e, l})
		}
	}
	return out
}

func (f *forwarder) position(opts []option) int {
	c := f.choice()
	best := 0
	for i, o := range opts {
		if o.e == c && o.layer <= f.layer {
			best = i
		}
	}
	return best
}

func (p *Peer) allocate(now int64) {
	budget := p.budget()
	var videos []*forwarder
	for _, f := range p.fwds {
		if !f.subscribed {
			continue
		}
		if f.src.audio() {
			budget -= f.src.encs[0].meter.upTo(0) * (1 + p.down.FECRatio(f.down))
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
	known := true
	for _, f := range videos {
		if len(f.src.encs) == 1 && !f.src.encs[0].meter.seen {
			known = false
		}
	}
	if !known {
		for _, f := range videos {
			f.want = min(f.choice().layers-1, f.maxLayer)
		}
		return
	}
	opts := make([][]option, len(videos))
	pos := make([]int, len(videos))
	cur := make([]int, len(videos))
	remaining := max(budget, 0)
	cost := func(i, j int) float64 {
		return opts[i][j].e.rate(opts[i][j].layer) * (1 + p.down.FECRatio(videos[i].down))
	}
	for i, f := range videos {
		opts[i] = f.options(now)
		cur[i] = f.position(opts[i])
		remaining -= cost(i, 0)
	}
	for changed := true; changed; {
		changed = false
		for i := range videos {
			next := pos[i] + 1
			if next >= len(opts[i]) {
				continue
			}
			delta := cost(i, next) - cost(i, pos[i])
			limit := remaining
			if next > cur[i] {
				limit *= upgradeHeadroom
				if opts[i][next].e != opts[i][cur[i]].e {
					limit = remaining * encodingHeadroom
				}
			}
			if delta <= limit {
				pos[i] = next
				remaining -= delta
				changed = true
			}
		}
	}
	for i, f := range videos {
		f.apply(opts[i][pos[i]], now)
	}
	ctl := p.down.Controller()
	delivered := budget
	if ctl.Loss() < deliveredLoss {
		delivered = budget + float64(ctl.AckedRate()-ctl.Target())
	}
	proven := max(budget+probeShare*float64(ctl.ProbeEstimate(now))-float64(ctl.Target()), delivered)
	p.shed(videos, opts, max(budget, delivered, 0), max(proven, 0), now)
}

func (f *forwarder) apply(o option, now int64) {
	f.want = o.layer
	c := f.choice()
	if o.e == c {
		return
	}
	if o.e == f.cur {
		f.target = nil
		return
	}
	up := o.e.rank > c.rank && !o.e.faceMode(now) && c.active(now)
	if up && now-f.switchedAt < encodingUpHoldUs {
		f.want = min(f.want, c.layers-1)
		return
	}
	f.target = o.e
	f.switchedAt = now
	o.e.requestKeyframe(now)
	o.e.updateHint(now, true)
}

func (p *Peer) shed(videos []*forwarder, opts [][]option, budget, proven float64, now int64) {
	low := func(i int) float64 {
		return opts[i][0].e.wireRate(0) * (1 + p.down.FECRatio(videos[i].down))
	}
	oh := 0.0
	for _, f := range videos {
		oh += p.down.StreamOverheadBps(f.down)
	}
	budget += oh
	proven += oh
	need := 0.0
	for i, f := range videos {
		if !f.paused {
			need += low(i)
		}
	}
	over := need > budget
	if !over {
		p.overSince = 0
	} else if p.overSince == 0 {
		p.overSince = now
	}
	urgent := need > overloadFactor*budget || (over && now-p.overSince >= overloadHoldUs)
	for i := len(videos) - 1; i >= 0 && need > budget && urgent; i-- {
		f := videos[i]
		if f.paused || now-f.pausedAt < pauseHoldUs {
			continue
		}
		f.paused, f.pausedAt = true, now
		f.Pauses++
		need -= low(i)
	}
	for i, f := range videos {
		if f.paused {
			p.down.Controller().RequestProbe(now)
		}
		if !f.paused || now-f.pausedAt < pauseHoldUs {
			continue
		}
		cost := low(i)
		h := resumeHeadroom
		if cost < cheapBps {
			h = cheapHeadroom
		}
		if need+h*cost <= max(budget, proven) {
			f.paused, f.pausedAt = false, now
			f.needKey = true
			if f.target != nil {
				f.cur, f.target = f.target, nil
			}
			f.cur.requestKeyframe(now)
			need += cost
		}
	}
}

func (e *encoding) updateHint(now int64, force bool) {
	t := e.t
	if t.audio() {
		return
	}
	total := e.meter.upTo(wire.MaxLayer)
	tl0 := defaultTL0Share
	if total > 0 && e.meter.seen {
		tl0 = min(max(e.meter.upTo(0)/total, 0.3), 1)
	}
	hint := float64(idleHintBps)
	if len(t.encs) > 1 && e != t.primary() {
		hint = 0
	}
	subs := 0
	need, best := 0.0, 0.0
	lowest := len(t.encs) > 1 && e == t.lowestVideo()
	for _, f := range t.fwds {
		if !f.subscribed || f.share <= 0 || (f.paused && !lowest) || f.choice() != e {
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
		floor := float64(minHintBps)
		if lowest {
			floor = lowestHintBps
		}
		hint = max(min(need, best)*upgradeHeadroom, floor)
	} else if len(t.encs) > 1 && e == t.primary() && t.anyViewer() {
		hint = 0
	}
	h := int(hint)
	if !e.hinted || force || (h == 0) != (e.hint == 0) || absDiff(h, e.hint)*10 > e.hint || now-e.hintAt >= hintRefreshUs {
		if force && e.hinted && h == e.hint {
			return
		}
		e.hint, e.hintAt, e.hinted = h, now, true
		t.owner.send(wire.BitrateHint{Stream: e.id, Bitrate: uint32(h)})
	}
}

func (t *track) anyViewer() bool {
	for _, f := range t.fwds {
		if f.subscribed && !f.paused {
			return true
		}
	}
	return false
}

func absDiff(a, b int) int {
	if a > b {
		return a - b
	}
	return b - a
}
