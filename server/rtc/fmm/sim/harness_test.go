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
)

var layerPattern = [4]int{0, 2, 1, 2}
var layerWeight = [3]float64{1.6, 0.9, 0.75}

type step struct {
	at     int64
	params link.Params
}

type profile struct {
	name   string
	steps  []step
	marks  []int64
	secs   int
	warmUs int64
}

type audioMetrics struct {
	started   bool
	played    int
	concealed int
	inserted  int
	delays    []int64
}

type videoMetrics struct {
	releases []int64
	delays   []int64
	bytes    int
	layers   [3]int
	window   []int64
}

type encoder struct {
	frame    int
	balance  float64
	forceKey bool
	lastKey  int64
	keys     int
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
	enc     encoder
	kinds   map[int]int
	owners  map[int]string
	audio   map[int]*audioMetrics
	video   map[int]*videoMetrics
	rateLog []rateSample
	hint    int
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
		Admit:      func(ticket string) (string, error) { return ticket, nil },
		MaxBitrate: 6_000_000,
	})
	k := &call{sim: s, room: room, p: p}
	for i := range clients {
		a, b := s.Pipe(p.steps[0].params, p.steps[0].params, 1200)
		c := fmm.NewClient(a, &fmm.Config{StartBitrate: 300_000, MinBitrate: 50_000, MaxBitrate: 2_500_000})
		peer := room.Connect(b)
		a.SetHandler(c)
		b.SetHandler(peer)
		n := &node{idx: i, name: fmt.Sprintf("p%d", i), c: c, up: a.Out(), down: b.Out(), peer: peer,
			kinds: map[int]int{}, owners: map[int]string{}, audio: map[int]*audioMetrics{}, video: map[int]*videoMetrics{}}
		k.nodes = append(k.nodes, n)
		for _, st := range p.steps[1:] {
			params := st.params
			s.At(st.at, func() {
				n.up.SetParams(params)
				n.down.SetParams(params)
			})
		}
	}
	return k
}

func (k *call) start() {
	s := k.sim
	for _, n := range k.nodes {
		n.c.Join(n.name, 0)
		n.audioID, _ = n.c.PublishAudio("mic", 0)
		n.videoID, _ = n.c.PublishVideo("cam", 3, 0)
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
			n.enc.forceKey = true
		case fmm.EventBitrateHint:
			n.hint = e.Bitrate
		}
	}
	for _, id := range slices.Sorted(maps.Keys(n.video)) {
		m := n.video[id]
		for f := n.c.PollVideo(id, now); f != nil; f = n.c.PollVideo(id, now) {
			if !k.inWindow(now) || len(f.Data) < prefixBytes {
				continue
			}
			m.releases = append(m.releases, now)
			m.delays = append(m.delays, now-int64(binary.BigEndian.Uint64(f.Data)))
			m.bytes += len(f.Data)
			m.layers[min(f.Layer, 2)]++
			m.window = append(m.window, int64(len(f.Data)))
		}
	}
}

func (k *call) sendAudio(n *node) {
	now := k.sim.Now()
	b := make([]byte, audioBytes)
	binary.BigEndian.PutUint64(b, uint64(now))
	n.c.SendAudio(n.audioID, b, now)
}

func (k *call) sendVideo(n *node) {
	now := k.sim.Now()
	e := &n.enc
	rate := float64(n.c.VideoBitrate(n.videoID))
	avg := rate / 8 / 30
	key := e.frame == 0 || (e.forceKey && now-e.lastKey >= minKeyGapUs)
	if key {
		e.frame = 0
		e.forceKey = false
		e.lastKey = now
		e.keys++
	}
	layer := layerPattern[e.frame%4]
	e.balance = min(max(e.balance+avg, -30*avg), 2*avg)
	want := layerWeight[layer] * avg
	if key {
		want = keyFactor * avg
	}
	size := max(prefixBytes+8, int(want+0.25*min(e.balance, 0)))
	if key {
		size = max(size, int(2*avg))
	}
	e.balance -= float64(size)
	n.sentKB += size
	b := make([]byte, size)
	binary.BigEndian.PutUint64(b, uint64(now))
	binary.BigEndian.PutUint32(b[8:], uint32(e.frame))
	if n.c.SendVideo(n.videoID, b, key, layer, now) != nil {
		e.forceKey = true
	}
	e.frame++
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
		case fmm.AudioInsert:
			m.inserted++
		}
	}
}

func (k *call) sampleRates(n *node) {
	now := k.sim.Now()
	st := n.c.Stats()
	recv := 0
	for _, m := range n.video {
		for _, b := range m.window {
			recv += int(b)
		}
		m.window = m.window[:0]
	}
	n.rateLog = append(n.rateLog, rateSample{at: now, target: st.TargetBitrate, video: n.c.VideoBitrate(n.videoID),
		down: n.peer.Downstream().Target(), recvVideo: recv * 8, capacity: n.down.Params().RateBps, queue: max(n.up.QueueDelay(now), n.down.QueueDelay(now))})
}

type result struct {
	profile     string
	concealment float64
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
	recovered   int
	undecodable int
	queueP95    int64
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
	played, conc, streams := 0, 0, 0
	fpsSum, kbpsSum, targetSum := 0.0, 0.0, 0.0
	targets := 0
	for _, n := range k.nodes {
		for _, m := range n.audio {
			played += m.played
			conc += m.concealed + m.inserted
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
		r.keyframes += n.enc.keys
	}
	if played+conc > 0 {
		r.concealment = float64(conc) / float64(played+conc)
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

func table(rs []result) string {
	var b strings.Builder
	b.WriteString("| profile | audio conceal | audio delay mean/p95 ms | video fps | freezes (total ms) | video delay mean/p95 ms | video recv kbps | encoder target kbps | keyframes | FEC recovered | queue p95 ms |\n")
	b.WriteString("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n")
	for _, r := range rs {
		fmt.Fprintf(&b, "| %s | %.2f%% | %d / %d | %.1f | %d (%d) | %d / %d | %.0f | %.0f | %d | %d | %d |\n",
			r.profile, 100*r.concealment, r.audioDelay/1000, r.audioP95/1000, r.fps, r.freezes, r.freezeMs,
			r.videoDelay/1000, r.videoP95/1000, r.videoKbps, r.targetKbps, r.keyframes, r.recovered, r.queueP95/1000)
	}
	return b.String()
}

func (k *call) rateTable(n *node) string {
	var b strings.Builder
	b.WriteString("| t s | link kbps | uplink target kbps | encoder kbps | SFU->client target kbps | received video kbps | queue ms |\n| --- | --- | --- | --- | --- | --- | --- |\n")
	for _, s := range n.rateLog {
		capS := "none"
		if s.capacity > 0 {
			capS = fmt.Sprint(s.capacity / 1000)
		}
		fmt.Fprintf(&b, "| %d | %s | %d | %d | %d | %d | %d |\n", s.at/1_000_000, capS, s.target/1000, s.video/1000, s.down/1000, s.recvVideo/1000, s.queue/1000)
	}
	return b.String()
}
