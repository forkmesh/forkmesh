package hop

import (
	"maps"
	"slices"

	"forkmesh.com/mirror-node/rtc/fmm/cc"
	"forkmesh.com/mirror-node/rtc/fmm/fec"
	"forkmesh.com/mirror-node/rtc/fmm/internal/seqnum"
	"forkmesh.com/mirror-node/rtc/fmm/wire"
)

type Datagrams interface {
	SendDatagram(p []byte) error
	MaxDatagramSize() int
}

const (
	AudioFECDelayUs = 60_000
	VideoFECDelayUs = 50_000
	fecUpdateUs     = 200_000
	rateWindowUs    = 1_000_000
	MaxPacketSize   = 1200
	PacketOverhead  = 48
	DREDCoverUs     = 100_000
	DREDFECShare    = 0.15
)

type meter struct {
	bytes, packets int
	start          int64
	bps, pps       float64
}

func (m *meter) add(n int) {
	m.bytes += n
	m.packets++
}

func (m *meter) roll(now int64) {
	if m.start == 0 {
		m.start = now
		return
	}
	dt := now - m.start
	if dt < rateWindowUs {
		return
	}
	bps := float64(m.bytes) * 8e6 / float64(dt)
	pps := float64(m.packets) * 1e6 / float64(dt)
	if m.bps == 0 {
		m.bps, m.pps = bps, pps
	} else {
		m.bps, m.pps = 0.5*m.bps+0.5*bps, 0.5*m.pps+0.5*pps
	}
	m.bytes, m.packets, m.start = 0, 0, now
}

const (
	historySize    = 512
	historyAgeUs   = 1_000_000
	rtxShare       = 0.3
	nackReorderUs  = 5_000
	nackMaxAgeUs   = 600_000
	nackTries      = 3
	nackMaxMissing = 256
	nackMaxJump    = 512
)

type sentSlot struct {
	seq    uint16
	valid  bool
	data   []byte
	sentAt int64
	rtxAt  int64
}

type sendStream struct {
	audio        bool
	depth        int
	fec          *fec.Encoder
	media        meter
	repair       meter
	lower, upper int
	history      [historySize]sentSlot
}

func (st *sendStream) importantShare() float64 {
	t := st.lower + st.upper
	if t < 20 {
		return 0.4
	}
	share := float64(st.lower) / float64(t)
	if t > 2000 {
		st.lower, st.upper = st.lower/2, st.upper/2
	}
	return min(max(share, 0.1), 1)
}

type Sender struct {
	t                Datagrams
	cc               *cc.Controller
	pacer            cc.Pacer
	tseq             uint16
	streams          map[uint8]*sendStream
	order            []uint8
	probeID          int
	lastFEC          int64
	now              int64
	FixedFEC         *fec.Params
	AudioFECWithDRED bool
	SendErrors       int
	Retransmits      int
	RtxSkipped       int
	rtxStart         int64
	rtxBytes         int
	sentMeter        meter
	Sent             int
	SentBytes        int64
	Padding          int
}

func NewSender(t Datagrams, cfg cc.Config) *Sender {
	return &Sender{t: t, cc: cc.NewController(cfg), streams: map[uint8]*sendStream{}}
}

func (s *Sender) Controller() *cc.Controller { return s.cc }

func (s *Sender) Pacer() *cc.Pacer { return &s.pacer }

func (s *Sender) MaxPayload() int {
	return min(s.t.MaxDatagramSize(), MaxPacketSize) - wire.MediaHeaderLen - wire.FECReserve
}

func (s *Sender) AddStream(id uint8, audio bool) {
	delay := int64(VideoFECDelayUs)
	if audio {
		delay = AudioFECDelayUs
	}
	if s.streams[id] == nil {
		s.order = append(s.order, id)
		slices.Sort(s.order)
	}
	enc := fec.NewEncoder(id, min(s.t.MaxDatagramSize(), MaxPacketSize), delay)
	enc.SetUnequal(!audio)
	s.streams[id] = &sendStream{audio: audio, fec: enc}
}

func (s *Sender) RemoveStream(id uint8) {
	delete(s.streams, id)
	s.order = slices.DeleteFunc(s.order, func(x uint8) bool { return x == id })
}

func (s *Sender) HasStream(id uint8) bool { return s.streams[id] != nil }

func (s *Sender) Send(pkt []byte, now int64) {
	st := s.streams[wire.StreamOf(pkt)]
	if st == nil {
		return
	}
	class := cc.ClassVideo
	if st.audio {
		class = cc.ClassAudio
	}
	if st.audio {
		st.depth = int(wire.AuxOf(pkt))
	}
	repairs := st.fec.Add(pkt, now)
	st.media.add(len(pkt))
	if !st.audio {
		seq := wire.SeqOf(pkt)
		st.history[seq%historySize] = sentSlot{seq: seq, valid: true, data: pkt, sentAt: now}
	}
	if !st.audio && pkt[10]&(wire.FlagKeyframe|0x07) != 0x00 && pkt[10]&wire.FlagKeyframe == 0 {
		st.upper++
	} else {
		st.lower++
	}
	s.pacer.Push(class, pkt, now)
	for _, r := range repairs {
		st.repair.add(len(r))
		s.pacer.Push(class, r, now)
	}
}

func (s *Sender) OnReport(r *wire.Report, now int64) { s.cc.OnReport(r, now) }

func (s *Sender) OnNack(n wire.Nack, now int64) {
	st := s.streams[n.Stream]
	if st == nil || st.audio {
		return
	}
	s.rtxRoll(now)
	gap := max(s.cc.RTT(), 20_000)
	budget := rtxShare * float64(s.cc.Target()) / 8
	for _, q := range n.Seqs {
		h := &st.history[q%historySize]
		if !h.valid || h.seq != q || now-h.sentAt > historyAgeUs || (h.rtxAt != 0 && now-h.rtxAt < gap) {
			continue
		}
		if float64(s.rtxBytes+len(h.data)) > budget {
			s.RtxSkipped++
			continue
		}
		h.rtxAt = now
		s.rtxBytes += len(h.data)
		s.Retransmits++
		s.pacer.Push(cc.ClassRTX, slices.Clone(h.data), now)
	}
}

func (s *Sender) rtxRoll(now int64) {
	if now-s.rtxStart >= 1_000_000 {
		s.rtxStart, s.rtxBytes = now, 0
	}
}

func (s *Sender) Tick(now int64) {
	s.now = now
	for _, id := range s.order {
		st := s.streams[id]
		class := cc.ClassVideo
		if st.audio {
			class = cc.ClassAudio
		}
		for _, r := range st.fec.Flush(now) {
			st.repair.add(len(r))
			s.pacer.Push(class, r, now)
		}
		st.media.roll(now)
		st.repair.roll(now)
	}
	s.sentMeter.roll(now)
	if now-s.lastFEC >= fecUpdateUs {
		s.lastFEC = now
		s.updateFEC()
	}
	s.cc.CheckFeedback(now)
	factor := cc.PacingFactor
	if s.cc.Congested(now) {
		factor = cc.CongestedPacingFactor
		s.pacer.DropOlder(cc.CongestedVideoQueueUs, now)
	}
	s.pacer.SetRate(int(float64(s.cc.Target()) * factor))
	s.pacer.SetDrainCap(int(float64(s.cc.Target()) * factor))
	if s.probeID == 0 {
		if id, rate, bytes, ok := s.cc.StartProbe(now); ok {
			s.probeID = id
			s.pacer.SetProbe(rate, bytes)
		}
	}
	s.pacer.Tick(now, s.transmit, s.pad)
	if s.probeID != 0 && !s.pacer.Probing() {
		s.cc.EndProbe(s.probeID, now)
		s.probeID = 0
	}
}

func (s *Sender) updateFEC() {
	avail := float64(s.cc.Target()) - s.OverheadBps()
	for _, id := range s.order {
		avail -= s.streams[id].media.bps
	}
	for _, audio := range []bool{true, false} {
		want := 0.0
		for _, id := range s.order {
			st := s.streams[id]
			if st.audio != audio {
				continue
			}
			s.chooseFEC(st)
			want += st.media.bps * st.fec.Ratio(st.importantShare())
		}
		if want <= 0 {
			continue
		}
		scale := 1.0
		if want > avail && s.cc.Congested(s.now) {
			scale = max(avail, 0) / want
		}
		for _, id := range s.order {
			st := s.streams[id]
			if st.audio == audio && scale < 1 {
				st.fec.Scale(scale)
			}
		}
		avail -= want * scale
	}
}

func (s *Sender) chooseFEC(st *sendStream) {
	if s.FixedFEC != nil {
		st.fec.SetParams(*s.FixedFEC)
		return
	}
	delay := float64(VideoFECDelayUs)
	if st.audio {
		delay = AudioFECDelayUs
	}
	maxK := max(1, int(st.media.pps*delay/1e6))
	if st.audio {
		p := fec.Choose(true, s.cc.Loss(), s.cc.Burst(), maxK)
		if st.depth*wire.DepthUnitUs >= DREDCoverUs && !s.AudioFECWithDRED && st.media.bps*p.Ratio() > DREDFECShare*float64(s.cc.Target()) {
			p = fec.Params{}
		}
		st.fec.SetParams(p)
		return
	}
	share := st.importantShare()
	imp, oth := fec.ChooseVideo(s.cc.Loss(), s.cc.Burst(), max(1, int(float64(maxK)*share)), max(1, int(float64(maxK)*(1-share))))
	st.fec.SetClassParams(imp, oth)
}

func (s *Sender) pad(size int) []byte {
	s.Padding++
	return wire.AppendPadding(make([]byte, 0, size), 0, min(size, s.t.MaxDatagramSize()))
}

func (s *Sender) transmit(b []byte, probe bool) {
	s.tseq++
	wire.SetTransportSeq(b, s.tseq)
	pid := 0
	if probe {
		pid = s.probeID
	}
	s.cc.OnSent(s.tseq, len(b)+PacketOverhead, s.now, pid)
	s.sentMeter.add(len(b))
	s.Sent++
	s.SentBytes += int64(len(b))
	if s.t.SendDatagram(b) != nil {
		s.SendErrors++
	}
}

func (s *Sender) Target() int { return s.cc.Target() }

func (s *Sender) OverheadBps() float64 { return s.sentMeter.pps * PacketOverhead * 8 }

func (s *Sender) StreamOverheadBps(id uint8) float64 {
	if st := s.streams[id]; st != nil {
		return (st.media.pps + st.repair.pps) * PacketOverhead * 8
	}
	return 0
}

func (s *Sender) FECRatio(id uint8) float64 {
	if st := s.streams[id]; st != nil {
		return st.fec.Ratio(st.importantShare())
	}
	return 0
}

func (s *Sender) StreamRate(id uint8) (mediaBps, repairBps float64) {
	if st := s.streams[id]; st != nil {
		return st.media.bps, st.repair.bps
	}
	return 0, 0
}

type miss struct {
	since    int64
	lastSent int64
	tries    int
}

type nackState struct {
	seqs    seqnum.Unwrapper16
	highest int64
	init    bool
	missing map[int64]*miss
}

func (n *nackState) onMedia(seq uint16, now int64) {
	s := n.seqs.Unwrap(seq)
	if !n.init {
		n.init, n.highest = true, s
		return
	}
	if s > n.highest {
		if s-n.highest > nackMaxJump {
			clear(n.missing)
		} else {
			for q := n.highest + 1; q < s && len(n.missing) < nackMaxMissing; q++ {
				n.missing[q] = &miss{since: now}
			}
		}
		n.highest = s
		return
	}
	delete(n.missing, s)
}

func (n *nackState) collect(now, rtt int64) []uint16 {
	var out []uint16
	gap := max(rtt*3/2, 20_000)
	for _, q := range slices.Sorted(maps.Keys(n.missing)) {
		m := n.missing[q]
		if now-m.since > nackMaxAgeUs || m.tries >= nackTries {
			delete(n.missing, q)
			continue
		}
		if (m.tries == 0 && now-m.since >= nackReorderUs) || (m.tries > 0 && now-m.lastSent >= gap) {
			m.tries++
			m.lastSent = now
			out = append(out, uint16(q))
			if len(out) == wire.MaxNack {
				break
			}
		}
	}
	return out
}

type Receiver struct {
	nack      map[uint8]*nackState
	rtt       int64
	Nacked    int
	rec       cc.Recorder
	dec       map[uint8]*fec.Decoder
	Media     int
	Recovered int
	Repairs   int
	Padding   int
	Invalid   int
}

func NewReceiver() *Receiver { return &Receiver{dec: map[uint8]*fec.Decoder{}} }

func (r *Receiver) decoder(id uint8) *fec.Decoder {
	d := r.dec[id]
	if d == nil {
		d = fec.NewDecoder(id)
		r.dec[id] = d
	}
	return d
}

func (r *Receiver) Handle(p []byte, now int64) [][]byte {
	k, err := wire.Kind(p)
	if err != nil {
		r.Invalid++
		return nil
	}
	switch k {
	case wire.KindMedia:
		m, err := wire.ParseMedia(p)
		if err != nil {
			r.Invalid++
			return nil
		}
		r.rec.OnPacket(m.TransportSeq, now)
		r.Media++
		out := [][]byte{p}
		rec := r.decoder(m.Stream).AddMedia(p, m.FEC, now)
		r.Recovered += len(rec)
		if n := r.nack[m.Stream]; n != nil {
			n.onMedia(m.Seq, now)
			for _, x := range rec {
				n.onMedia(wire.SeqOf(x), now)
			}
		}
		return append(out, rec...)
	case wire.KindRepair:
		rp, err := wire.ParseRepair(p)
		if err != nil {
			r.Invalid++
			return nil
		}
		r.rec.OnPacket(rp.TransportSeq, now)
		r.Repairs++
		rec := r.decoder(rp.Stream).AddRepair(rp, now)
		r.Recovered += len(rec)
		if n := r.nack[rp.Stream]; n != nil {
			for _, x := range rec {
				n.onMedia(wire.SeqOf(x), now)
			}
		}
		return rec
	default:
		ts, err := wire.ParsePadding(p)
		if err != nil {
			r.Invalid++
			return nil
		}
		r.rec.OnPacket(ts, now)
		r.Padding++
		return nil
	}
}

func (r *Receiver) RemoveStream(id uint8) {
	delete(r.dec, id)
	delete(r.nack, id)
}

func (r *Receiver) EnableNack(id uint8) {
	if r.nack == nil {
		r.nack = map[uint8]*nackState{}
	}
	if r.nack[id] == nil {
		r.nack[id] = &nackState{missing: map[int64]*miss{}}
	}
}

func (r *Receiver) SetRTT(rtt int64) { r.rtt = rtt }

func (r *Receiver) Nacks(now int64) [][]byte {
	var out [][]byte
	for _, id := range slices.Sorted(maps.Keys(r.nack)) {
		seqs := r.nack[id].collect(now, r.rtt)
		if len(seqs) == 0 {
			continue
		}
		r.Nacked += len(seqs)
		if b, err := wire.Append(nil, wire.Nack{Stream: id, Seqs: seqs}); err == nil {
			out = append(out, b)
		}
	}
	return out
}

func (r *Receiver) Report(now int64) []byte {
	rep := r.rec.Report(now)
	if rep == nil {
		return nil
	}
	b, err := wire.Append(nil, rep)
	if err != nil {
		return nil
	}
	return b
}

func PackFeedback(msgs [][]byte, maxSize int) (datagrams, control [][]byte) {
	var cur []byte
	for _, m := range msgs {
		if wire.FeedbackHeaderLen+len(m) > maxSize {
			control = append(control, m)
			continue
		}
		if len(cur) > 0 && wire.FeedbackHeaderLen+len(cur)+len(m) > maxSize {
			datagrams = append(datagrams, wire.AppendFeedback(nil, cur))
			cur = nil
		}
		cur = append(cur, m...)
	}
	if len(cur) > 0 {
		datagrams = append(datagrams, wire.AppendFeedback(nil, cur))
	}
	return datagrams, control
}

func (r *Receiver) SetRepeat(n int) { r.rec.Repeat = n }
