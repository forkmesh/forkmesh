package sim

import (
	"encoding/binary"
	"fmt"
	"maps"
	"slices"
	"strings"

	"forkmesh.com/mirror-node/rtc/fmm"
	"forkmesh.com/mirror-node/rtc/fmm/link"
	"forkmesh.com/mirror-node/rtc/fmm/sfu"
)

const (
	tickUs       = 5_000
	audioFrameUs = 20_000
	videoFrameUs = 33_333
	audioBytes   = 80
	prefixBytes  = 16
	keyFactor    = 5.0
	minKeyGapUs  = 300_000
	faceBytes    = 64
	refBytes     = 3_000
	dredDepthMs  = 1000
	recBase      = 1.6
	recPerFrame  = 0.04
)

var layerPattern = [4]int{0, 2, 1, 2}
var layerWeight = [3]float64{1.6, 0.9, 0.75}

type step struct {
	at     int64
	params link.Params
}

type opts struct {
	ltr       bool
	dred      bool
	face      bool
	simulcast int
	datagrams bool
	adaptive  bool
}

func defaults() opts {
	return opts{ltr: true, dred: true, adaptive: true, datagrams: true, simulcast: 1}
}

type profile struct {
	name    string
	steps   []step
	marks   []int64
	secs    int
	warmUs  int64
	uplinks map[int][]step
	downs   map[int][]step
	o       opts
}

type audioMetrics struct {
	started   bool
	played    int
	concealed int
	fec       int
	dred      int
	inserted  int
	delays    []int64
	plainAt   []int64
}

type videoMetrics struct {
	releases []int64
	faces    []int64
	refs     int
	delays   []int64
	bytes    int
	layers   [3]int
	window   []int64
	reps     []int
}

type vstream struct {
	id       int
	frame    int
	balance  float64
	forceKey bool
	lastKey  int64
	keys     int
	keyBytes int
	recs     int
	recBytes int
	rep      int
	recover  int64
	lastID   int64
	refReq   int
	refsSent int
	faces    int
}

type node struct {
	idx     int
	name    string
	c       *fmm.Client
	up      *link.Link
	down    *link.Link
	peer    *sfu.Peer
	audioID int
	videoID int
	vids    map[int]*vstream
	kinds   map[int]int
	owners  map[int]string
	audio   map[int]*audioMetrics
	video   map[int]*videoMetrics
	rateLog []rateSample
	sentKB  int
}

type rateSample struct {
	at        int64
	target    int
	video     int
	down      int
	recvVideo int
	capacity  int
	queue     int64
	faceFPS   int
	videoFPS  int
	rep       int
	conceal   int
}

type call struct {
	sim   *link.Sim
	room  *sfu.Room
	nodes []*node
	p     profile
}

func newCall(p profile, clients int, seed uint64) *call {
	s := link.NewSim(seed)
	room := sfu.NewRoom(sfu.Config{
		Admit:             func(ticket string) (string, error) { return ticket, nil },
		MaxBitrate:        6_000_000,
		FeedbackOnControl: !p.o.datagrams,
		NoRecovery:        !p.o.ltr,
	})
	k := &call{sim: s, room: room, p: p}
	for i := range clients {
		upSteps, downSteps := p.steps, p.steps
		if st, ok := p.uplinks[i]; ok {
			upSteps = st
		}
		if st, ok := p.downs[i]; ok {
			downSteps = st
		}
		a, b := s.Pipe(upSteps[0].params, downSteps[0].params, 1200)
		c := fmm.NewClient(a, &fmm.Config{StartBitrate: 300_000, MinBitrate: 50_000, MaxBitrate: 2_500_000,
			FeedbackOnControl: !p.o.datagrams, NoRecovery: !p.o.ltr, NoLookAhead: !p.o.dred})
		peer := room.Connect(b)
		a.SetHandler(c)
		b.SetHandler(peer)
		n := &node{idx: i, name: fmt.Sprintf("p%d", i), c: c, up: a.Out(), down: b.Out(), peer: peer, vids: map[int]*vstream{},
			kinds: map[int]int{}, owners: map[int]string{}, audio: map[int]*audioMetrics{}, video: map[int]*videoMetrics{}}
		k.nodes = append(k.nodes, n)
		for _, st := range upSteps[1:] {
			params := st.params
			s.At(st.at, func() { n.up.SetParams(params) })
		}
		for _, st := range downSteps[1:] {
			params := st.params
			s.At(st.at, func() { n.down.SetParams(params) })
		}
	}
	return k
}

func (k *call) start() {
	s := k.sim
	for _, n := range k.nodes {
		n.c.Join(n.name, 0)
		n.audioID, _ = n.c.PublishAudio("mic", 0)
		if k.p.o.dred {
			n.c.SetAudioRedundancy(n.audioID, dredDepthMs)
		}
		n.videoID, _ = n.c.PublishVideoTrack("cam", 3, max(k.p.o.simulcast, 1), k.p.o.face, 0)
		for i := range max(k.p.o.simulcast, 1) {
			id := n.c.EncodingStream(n.videoID, i)
			n.vids[id] = &vstream{id: id, rep: fmm.RepVideo, lastID: -1, recover: -1}
		}
		if f := n.c.FaceStream(n.videoID); f != 0 {
			n.vids[f] = &vstream{id: f, rep: fmm.RepOff, lastID: -1, recover: -1}
		}
		offset := int64(n.idx) * 1_700
		s.Every(offset, tickUs, func() { k.tick(n) })
		s.Every(offset+700, audioFrameUs, func() { k.sendAudio(n) })
		s.Every(offset+1_300, videoFrameUs, func() { k.sendVideo(n) })
		s.Every(offset+11_000, audioFrameUs, func() { k.pullAudio(n) })
		s.Every(1_000_000, 1_000_000, func() { k.sampleRates(n) })
	}
	s.Every(2_500, tickUs, func() { k.room.Tick(s.Now()) })
}

func (k *call) run() {
	k.start()
	k.sim.RunUntil(int64(k.p.secs) * 1_000_000)
}

func (k *call) inWindow(t int64) bool { return t >= k.p.warmUs }

func (k *call) tick(n *node) {
	now := k.sim.Now()
	n.c.Tick(now)
	for e := n.c.PollEvent(); e != nil; e = n.c.PollEvent() {
		v := n.vids[e.Stream]
		switch e.Kind {
		case fmm.EventTrackAdded:
			n.kinds[e.Stream] = e.Track
			n.owners[e.Stream] = e.Peer
			n.c.Subscribe(e.Stream, 7, now)
			if e.Track == fmm.KindAudio {
				n.audio[e.Stream] = &audioMetrics{}
			} else {
				n.video[e.Stream] = &videoMetrics{}
			}
		case fmm.EventKeyframeRequest:
			if v != nil {
				v.forceKey = true
			}
		case fmm.EventRecover:
			if v != nil {
				v.recover = e.FrameID
			}
		case fmm.EventSendRepresentation:
			if v != nil {
				if e.Rep == fmm.RepVideo && v.rep != fmm.RepVideo {
					v.frame = 0
				}
				v.rep = e.Rep
			}
		case fmm.EventReferencePicture:
			if v != nil {
				v.refReq = e.MaxBytes
			}
		case fmm.EventRepresentation:
			if m := n.video[e.Stream]; m != nil {
				m.reps = append(m.reps, e.Rep)
			}
		}
	}
	for _, id := range slices.Sorted(maps.Keys(n.video)) {
		m := n.video[id]
		for f := n.c.PollVideo(id, now); f != nil; f = n.c.PollVideo(id, now) {
			if !k.inWindow(now) || len(f.Data) < 8 {
				continue
			}
			if f.Face && f.Keyframe {
				m.refs++
				continue
			}
			m.releases = append(m.releases, now)
			if f.Face {
				m.faces = append(m.faces, now)
			}
			m.delays = append(m.delays, now-int64(binary.BigEndian.Uint64(f.Data)))
			m.bytes += len(f.Data)
			m.layers[min(f.Layer, 2)]++
			m.window = append(m.window, int64(len(f.Data)))
		}
	}
}

func (k *call) sendAudio(n *node) {
	now := k.sim.Now()
	size := audioBytes
	if k.p.o.adaptive {
		size = n.c.AudioBitrate(n.audioID) / 8 / 50
	}
	if k.p.o.dred {
		ms := dredDepthMs
		if k.p.o.adaptive {
			ms = n.c.AudioRedundancyMs(n.audioID)
		}
		n.c.SetAudioRedundancy(n.audioID, ms)
		size += 6 + ms/40
	}
	b := make([]byte, size)
	binary.BigEndian.PutUint64(b, uint64(now))
	n.c.SendAudio(n.audioID, b, now)
}

func (k *call) sendVideo(n *node) {
	now := k.sim.Now()
	for _, id := range slices.Sorted(maps.Keys(n.vids)) {
		v := n.vids[id]
		switch v.rep {
		case fmm.RepFace:
			k.sendFace(n, v, now)
		case fmm.RepVideo:
			k.sendFrame(n, v, now)
		}
	}
}

func (k *call) sendFace(n *node, v *vstream, now int64) {
	if v.refReq > 0 {
		b := make([]byte, min(refBytes, v.refReq))
		binary.BigEndian.PutUint64(b, uint64(now))
		if _, err := n.c.SendVideoFrame(v.id, &fmm.OutFrame{Data: b, Keyframe: true, Face: true}, now); err == nil {
			v.refsSent++
		}
		v.refReq = 0
	}
	b := make([]byte, faceBytes)
	binary.BigEndian.PutUint64(b, uint64(now))
	n.c.SendVideoFrame(v.id, &fmm.OutFrame{Data: b, Face: true}, now)
	v.faces++
	n.sentKB += faceBytes
}

func (k *call) sendFrame(n *node, v *vstream, now int64) {
	rate := float64(n.c.VideoBitrate(v.id))
	if rate <= 0 {
		return
	}
	avg := rate / 8 / 30
	key := v.frame == 0 || (v.forceKey && now-v.lastKey >= minKeyGapUs)
	recover := !key && v.recover >= 0 && k.p.o.ltr && v.lastID >= 0
	if key {
		v.frame = 0
		v.forceKey = false
		v.lastKey = now
		v.keys++
		v.recover = -1
	}
	layer := layerPattern[v.frame%4]
	if recover {
		layer = 0
	}
	v.balance = min(max(v.balance+avg, -30*avg), 2*avg)
	want := layerWeight[layer] * avg
	if recover {
		want = min(recBase+recPerFrame*float64(v.lastID+1-v.recover), keyFactor) * avg
	}
	if key {
		want = keyFactor * avg
	}
	size := max(prefixBytes+8, int(want+0.25*min(v.balance, 0)))
	if key {
		size = max(size, int(2*avg))
	}
	if recover {
		size = max(size, int(recBase*avg))
	}
	v.balance -= float64(size)
	n.sentKB += size
	b := make([]byte, size)
	binary.BigEndian.PutUint64(b, uint64(now))
	binary.BigEndian.PutUint32(b[8:], uint32(v.frame))
	f := &fmm.OutFrame{Data: b, Keyframe: key, Layer: layer, LongTerm: k.p.o.ltr && layer == 0, Recover: recover}
	id, err := n.c.SendVideoFrame(v.id, f, now)
	if err != nil {
		if recover {
			v.recover = -1
			v.forceKey = true
		} else {
			v.forceKey = true
		}
		v.frame++
		return
	}
	if key {
		v.keyBytes += size
	}
	if recover {
		v.recs++
		v.recBytes += size
		v.recover = -1
	}
	v.lastID = id
	v.frame++
}

func (k *call) pullAudio(n *node) {
	now := k.sim.Now()
	for _, id := range slices.Sorted(maps.Keys(n.audio)) {
		m := n.audio[id]
		f := n.c.PullAudio(id, now)
		if f.Status == fmm.AudioWaiting {
			continue
		}
		m.started = true
		if !k.inWindow(now) {
			continue
		}
		switch f.Status {
		case fmm.AudioPlay:
			m.played++
			m.delays = append(m.delays, now-int64(binary.BigEndian.Uint64(f.Data)))
		case fmm.AudioConceal:
			m.concealed++
			if f.Data != nil {
				m.fec++
			} else {
				m.plainAt = append(m.plainAt, now)
			}
		case fmm.AudioDRED:
			m.dred++
		case fmm.AudioInsert:
			m.inserted++
			m.plainAt = append(m.plainAt, now)
		}
	}
}

func (k *call) sampleRates(n *node) {
	now := k.sim.Now()
	st := n.c.Stats()
	recv := 0
	faces, videos := 0, 0
	rep := 0
	for _, id := range slices.Sorted(maps.Keys(n.video)) {
		m := n.video[id]
		for _, b := range m.window {
			recv += int(b)
		}
		m.window = m.window[:0]
		for _, t := range m.releases {
			if t > now-1_000_000 {
				videos++
			}
		}
		for _, t := range m.faces {
			if t > now-1_000_000 {
				faces++
			}
		}
		if len(m.reps) > 0 {
			rep = m.reps[len(m.reps)-1]
		}
	}
	conceal := 0
	for _, m := range n.audio {
		for _, t := range m.plainAt {
			if t > now-1_000_000 {
				conceal++
			}
		}
	}
	n.rateLog = append(n.rateLog, rateSample{at: now, target: st.TargetBitrate, video: n.c.VideoBitrate(n.videoID),
		down: n.peer.Downstream().Target(), recvVideo: recv * 8, capacity: n.down.Params().RateBps,
		queue: max(n.up.QueueDelay(now), n.down.QueueDelay(now)), faceFPS: faces, videoFPS: videos - faces, rep: rep, conceal: conceal})
}

type result struct {
	profile     string
	concealment float64
	plain       float64
	fecShare    float64
	dredShare   float64
	audioDelay  int64
	audioP95    int64
	fps         float64
	freezes     int
	freezeMs    int64
	videoDelay  int64
	videoP95    int64
	videoKbps   float64
	targetKbps  float64
	keyframes   int
	keyKB       float64
	recoveries  int
	recKB       float64
	escalations int
	recovered   int
	undecodable int
	queueP95    int64
	averaged    bool
	freezesAvg  float64
	freezeMsAvg float64
	keysAvg     float64
	recsAvg     float64
	escAvg      float64
}

func (r result) counts() (fr, frMs, keys, recs, esc float64) {
	if r.averaged {
		return r.freezesAvg, r.freezeMsAvg, r.keysAvg, r.recsAvg, r.escAvg
	}
	return float64(r.freezes), float64(r.freezeMs), float64(r.keyframes), float64(r.recoveries), float64(r.escalations)
}

func pct(v []int64, p float64) int64 {
	if len(v) == 0 {
		return 0
	}
	s := slices.Clone(v)
	slices.Sort(s)
	return s[min(len(s)-1, int(p*float64(len(s))))]
}

func mean(v []int64) int64 {
	if len(v) == 0 {
		return 0
	}
	t := int64(0)
	for _, x := range v {
		t += x
	}
	return t / int64(len(v))
}

func freezes(rel []int64) (int, int64) {
	n, total := 0, int64(0)
	avg := 0.0
	for i := 1; i < len(rel); i++ {
		d := float64(rel[i] - rel[i-1])
		if i > 1 && d > max(3*avg, avg+150_000) {
			n++
			total += int64(d)
		}
		if i == 1 {
			avg = d
		} else {
			avg = 0.95*avg + 0.05*d
		}
	}
	return n, total
}

func (k *call) result() result {
	r := result{profile: k.p.name}
	window := float64(int64(k.p.secs)*1_000_000-k.p.warmUs) / 1e6
	var aDelays, vDelays, queues []int64
	played, conc, plain, fec, dred, streams := 0, 0, 0, 0, 0, 0
	fpsSum, kbpsSum, targetSum := 0.0, 0.0, 0.0
	targets := 0
	for _, n := range k.nodes {
		for _, m := range n.audio {
			played += m.played
			conc += m.concealed + m.inserted
			plain += m.concealed - m.fec + m.inserted
			fec += m.fec
			dred += m.dred
			aDelays = append(aDelays, m.delays...)
		}
		for _, m := range n.video {
			streams++
			fpsSum += float64(len(m.releases)) / window
			kbpsSum += float64(m.bytes) * 8 / window / 1000
			f, ms := freezes(m.releases)
			r.freezes += f
			r.freezeMs += ms / 1000
			vDelays = append(vDelays, m.delays...)
		}
		for _, s := range n.rateLog {
			if k.inWindow(s.at) {
				targetSum += float64(s.video)
				targets++
				queues = append(queues, s.queue)
			}
		}
		st := n.c.Stats()
		r.recovered += st.RecoveredPackets
		r.undecodable += st.VideoUndecodable
		r.escalations += st.Escalations
		for _, v := range n.vids {
			r.keyframes += v.keys
			r.keyKB += float64(v.keyBytes) / 1000
			r.recoveries += v.recs
			r.recKB += float64(v.recBytes) / 1000
		}
	}
	if total := played + conc + dred; total > 0 {
		r.concealment = float64(conc) / float64(total)
		r.plain = float64(plain) / float64(total)
		r.fecShare = float64(fec) / float64(total)
		r.dredShare = float64(dred) / float64(total)
	}
	r.audioDelay, r.audioP95 = mean(aDelays), pct(aDelays, 0.95)
	r.videoDelay, r.videoP95 = mean(vDelays), pct(vDelays, 0.95)
	if streams > 0 {
		r.fps = fpsSum / float64(streams)
		r.videoKbps = kbpsSum / float64(streams)
	}
	if targets > 0 {
		r.targetKbps = targetSum / float64(targets) / 1000
	}
	r.queueP95 = pct(queues, 0.95)
	return r
}

var seeds = []uint64{11, 12, 13, 14, 15, 16, 17, 18}

func runSeeds(p profile, clients int) (result, []*call) {
	var rs []result
	var calls []*call
	for _, seed := range seeds {
		k := newCall(p, clients, seed)
		k.run()
		rs = append(rs, k.result())
		calls = append(calls, k)
	}
	return average(rs), calls
}

func average(rs []result) result {
	n := float64(len(rs))
	out := result{profile: rs[0].profile}
	var fr, frMs, keys, recs, esc float64
	for _, r := range rs {
		out.concealment += r.concealment / n
		out.plain += r.plain / n
		out.fecShare += r.fecShare / n
		out.dredShare += r.dredShare / n
		out.audioDelay += r.audioDelay / int64(n)
		out.audioP95 += r.audioP95 / int64(n)
		out.fps += r.fps / n
		out.videoDelay += r.videoDelay / int64(n)
		out.videoP95 += r.videoP95 / int64(n)
		out.videoKbps += r.videoKbps / n
		out.targetKbps += r.targetKbps / n
		out.keyKB += r.keyKB / n
		out.recKB += r.recKB / n
		out.queueP95 += r.queueP95 / int64(n)
		fr += float64(r.freezes) / n
		frMs += float64(r.freezeMs) / n
		keys += float64(r.keyframes) / n
		recs += float64(r.recoveries) / n
		esc += float64(r.escalations) / n
	}
	out.freezesAvg, out.freezeMsAvg, out.keysAvg, out.recsAvg, out.escAvg = fr, frMs, keys, recs, esc
	out.averaged = true
	return out
}

func table(rs []result) string {
	var b strings.Builder
	b.WriteString("| profile | audible conceal | FEC / DRED recovered | audio delay mean/p95 ms | video fps | freezes (total ms) | video delay mean/p95 ms | video recv kbps | encoder target kbps | keyframes (kB) | recovery frames (kB) | escalations | queue p95 ms |\n")
	b.WriteString("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n")
	for _, r := range rs {
		fr, frMs, keys, recs, esc := r.counts()
		fmt.Fprintf(&b, "| %s | %.2f%% | %.2f%% / %.2f%% | %d / %d | %.1f | %.1f (%.1f s) | %d / %d | %.0f | %.0f | %.1f (%.0f) | %.1f (%.0f) | %.1f | %d |\n",
			r.profile, 100*r.plain, 100*r.fecShare, 100*r.dredShare, r.audioDelay/1000, r.audioP95/1000, r.fps, fr, frMs/1000,
			r.videoDelay/1000, r.videoP95/1000, r.videoKbps, r.targetKbps, keys, r.keyKB, recs, r.recKB, esc, r.queueP95/1000)
	}
	return b.String()
}

func (k *call) rateTable(n *node) string {
	var b strings.Builder
	b.WriteString("| t s | link kbps | uplink target kbps | encoder kbps | SFU->client target kbps | received video kbps | video fps | face fps | showing | audible conceal frames | queue ms |\n| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n")
	for _, s := range n.rateLog {
		capS := "none"
		if s.capacity > 0 {
			capS = fmt.Sprint(s.capacity / 1000)
		}
		rep := map[int]string{0: "-", fmm.RepVideo: "video", fmm.RepFace: "face"}[s.rep]
		fmt.Fprintf(&b, "| %d | %s | %d | %d | %d | %d | %d | %d | %s | %d | %d |\n", s.at/1_000_000, capS, s.target/1000, s.video/1000, s.down/1000,
			s.recvVideo/1000, s.videoFPS, s.faceFPS, rep, s.conceal, s.queue/1000)
	}
	return b.String()
}
