package fmm

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/link"
	"forkmesh.com/mirror-node/rtc/fmm/sfu"
)

func jsRLE(b []byte) string {
	var sb strings.Builder
	for i := 0; i < len(b); {
		j := i
		for j < len(b) && b[j] == 0 {
			j++
		}
		if j-i >= 6 {
			sb.WriteString("z" + strconv.Itoa(j-i) + ",")
			i = j
			continue
		}
		sb.WriteString(hex.EncodeToString(b[i : i+1]))
		i++
	}
	return sb.String()
}

func jsDigest(parts ...[]byte) string {
	h := sha256.New()
	for _, b := range parts {
		var n [4]byte
		binary.BigEndian.PutUint32(n[:], uint32(len(b)))
		h.Write(n[:])
		h.Write(b)
	}
	return hex.EncodeToString(h.Sum(nil)[:8])
}

func jsErr(err error) string {
	if err == nil {
		return ""
	}
	return "!" + err.Error()
}

type jsTx struct {
	ep   *link.Endpoint
	out  [][]byte
	kind []byte
}

func (t *jsTx) SendDatagram(p []byte) error {
	t.out = append(t.out, bytes.Clone(p))
	t.kind = append(t.kind, 'd')
	return t.ep.SendDatagram(p)
}

func (t *jsTx) MaxDatagramSize() int { return t.ep.MaxDatagramSize() }

func (t *jsTx) SendControl(p []byte) error {
	t.out = append(t.out, bytes.Clone(p))
	t.kind = append(t.kind, 'c')
	return t.ep.SendControl(p)
}

func (t *jsTx) take() string {
	if len(t.out) == 0 {
		return ""
	}
	parts := make([][]byte, 0, 2*len(t.out))
	for i, b := range t.out {
		parts = append(parts, []byte{t.kind[i]}, b)
	}
	s := strconv.Itoa(len(t.out)) + ":" + jsDigest(parts...)
	t.out, t.kind = nil, nil
	return s
}

type jsRec struct {
	c   *Client
	tx  *jsTx
	ops [][]any
}

func (r *jsRec) op(code string, now int64, args ...any) {
	r.ops = append(r.ops, append(append([]any{code, now}, args...), r.tx.take()))
}

func (r *jsRec) HandleDatagram(p []byte, now int64) {
	r.c.HandleDatagram(p, now)
	r.op("D", now, jsRLE(p))
}

func (r *jsRec) HandleControl(p []byte, now int64) {
	r.c.HandleControl(p, now)
	r.op("C", now, jsRLE(p))
}

func jsRet(v int, err error) any {
	if err != nil {
		return jsErr(err)
	}
	return v
}

func jsBool(b bool) int {
	if b {
		return 1
	}
	return 0
}

func jsStats(s *Stats) []any {
	return []any{s.TargetBitrate, s.VideoBitrate, s.RTTUs, s.Loss, s.SentPackets, s.ReceivedPackets, s.RecoveredPackets,
		s.AudioPlayed, s.AudioConcealed, s.AudioFEC, s.AudioDRED, s.AudioInserted, s.VideoReleased, s.VideoUndecodable,
		s.KeyframeRequests, s.RecoveryRequests, s.RecoveryFrames, s.Escalations, s.PacketsQueued, s.Recoveries}
}

type jsVid struct {
	id       int
	frame    int
	balance  float64
	forceKey bool
	lastKey  int64
	rep      int
	recover  int64
	lastID   int64
	refReq   int
}

type jsNode struct {
	idx     int
	c       *Client
	rec     *jsRec
	audioID int
	authID  int
	videoID int
	vids    map[int]*jsVid
	video   map[int]bool
	audio   map[int]bool
	auth    map[int]bool
	up      *link.Link
	down    *link.Link
}

type jsCallOpts struct {
	clients   int
	secs      int64
	face      bool
	simulcast int
	control   bool
	auth      bool
	cfg       Config
	up, down  link.Params
	upAt      int64
	upLater   *link.Params
	weakDown  *link.Params
	seed      uint64
	churn     bool
}

type jsCall struct {
	sim   *link.Sim
	room  *sfu.Room
	nodes []*jsNode
	o     jsCallOpts
}

func (k *jsCall) rec(n *jsNode, code string, now int64, args ...any) {
	if n.rec != nil {
		n.rec.op(code, now, args...)
	}
}

func newJSCall(o jsCallOpts) *jsCall {
	s := link.NewSim(o.seed)
	room := sfu.NewRoom(sfu.Config{
		Admit:             func(ticket string) (string, error) { return ticket, nil },
		MaxBitrate:        3_000_000,
		FeedbackOnControl: o.control,
	})
	k := &jsCall{sim: s, room: room, o: o}
	for i := range o.clients {
		up, down := o.up, o.down
		if i != 0 {
			up, down = link.Params{DelayUs: 20_000, OverheadB: 40}, link.Params{DelayUs: 20_000, OverheadB: 40}
			if o.weakDown != nil && i == o.clients-1 {
				down = *o.weakDown
			}
		}
		a, b := s.Pipe(up, down, 1200)
		cfg := o.cfg
		n := &jsNode{idx: i, vids: map[int]*jsVid{}, video: map[int]bool{}, audio: map[int]bool{}, auth: map[int]bool{}, up: a.Out(), down: b.Out()}
		if i == 0 {
			tx := &jsTx{ep: a}
			n.c = NewClient(tx, &cfg)
			n.rec = &jsRec{c: n.c, tx: tx}
			a.SetHandler(n.rec)
		} else {
			n.c = NewClient(a, &cfg)
			a.SetHandler(n.c)
		}
		peer := room.Connect(b)
		b.SetHandler(peer)
		k.nodes = append(k.nodes, n)
		if i == 0 && o.upLater != nil {
			p := *o.upLater
			s.At(o.upAt, func() { n.up.SetParams(p) })
		}
	}
	return k
}

func (k *jsCall) start() {
	s := k.sim
	for _, n := range k.nodes {
		name := fmt.Sprintf("p%d", n.idx)
		err := n.c.Join(name, 0)
		k.rec(n, "J", 0, name, jsErr(err))
		id, err := n.c.PublishAudio("mic", 0)
		n.audioID = id
		k.rec(n, "PA", 0, "mic", jsRet(id, err))
		err = n.c.SetAudioRedundancy(n.audioID, 1000)
		k.rec(n, "RD", 0, n.audioID, 1000, jsErr(err))
		if k.o.auth {
			id, err := n.c.PublishAuth("auth", 0)
			n.authID = id
			k.rec(n, "PH", 0, "auth", jsRet(id, err))
		}
		enc := max(k.o.simulcast, 1)
		id, err = n.c.PublishVideoTrack("cam", 3, enc, k.o.face, 0)
		n.videoID = id
		k.rec(n, "PV", 0, "cam", 3, enc, jsBool(k.o.face), jsRet(id, err))
		for i := range enc {
			e := n.c.EncodingStream(n.videoID, i)
			k.rec(n, "ES", 0, n.videoID, i, e)
			n.vids[e] = &jsVid{id: e, rep: RepVideo, lastID: -1, recover: -1}
		}
		f := n.c.FaceStream(n.videoID)
		k.rec(n, "FS", 0, n.videoID, f)
		if f != 0 {
			n.vids[f] = &jsVid{id: f, rep: RepOff, lastID: -1, recover: -1}
		}
		offset := int64(n.idx) * 1_700
		s.Every(offset, 5_000, func() { k.tick(n) })
		s.Every(offset+700, 20_000, func() { k.sendAudio(n) })
		s.Every(offset+1_300, 33_333, func() { k.sendVideo(n) })
		s.Every(offset+11_000, 20_000, func() { k.pullAudio(n) })
		s.Every(1_000_000, 1_000_000, func() {
			now := s.Now()
			k.rec(n, "ST", now, jsStats(n.c.Stats()))
			k.rec(n, "TB", now, n.c.TargetBitrate())
			k.rec(n, "PI", now, n.c.PeerID())
			for _, id := range slices.Sorted(maps.Keys(n.vids)) {
				k.rec(n, "AK", now, id, n.c.AckedFrame(id))
				k.rec(n, "SR", now, id, n.c.SendingRepresentation(id))
			}
			for _, id := range slices.Sorted(maps.Keys(n.video)) {
				k.rec(n, "RP", now, id, n.c.Representation(id))
				ref := n.c.FaceReference(id)
				d := ""
				if ref != nil {
					d = jsDigest(ref)
				}
				k.rec(n, "FR", now, id, d)
			}
		})
		if k.o.auth {
			s.Every(offset+3_100, 200_000, func() {
				now := s.Now()
				b := make([]byte, 120)
				binary.BigEndian.PutUint64(b, uint64(now))
				err := n.c.SendAuth(n.authID, b, now)
				k.rec(n, "SH", now, n.authID, jsRLE(b), jsErr(err))
			})
		}
	}
	s.Every(2_500, 5_000, func() { k.room.Tick(s.Now()) })
	if k.o.churn {
		n := k.nodes[0]
		s.At(k.o.secs*1_000_000-700_000, func() {
			now := s.Now()
			err := n.c.SetReceiveLimit(400_000, now)
			k.rec(n, "RL", now, 400_000, jsErr(err))
			for _, id := range slices.Sorted(maps.Keys(n.audio)) {
				err := n.c.Unsubscribe(id, now)
				k.rec(n, "US", now, id, jsErr(err))
				delete(n.audio, id)
				break
			}
		})
		s.At(k.o.secs*1_000_000-400_000, func() {
			now := s.Now()
			err := n.c.Unpublish(n.videoID, now)
			k.rec(n, "UP", now, n.videoID, jsErr(err))
			n.vids = map[int]*jsVid{}
		})
		s.At(k.o.secs*1_000_000-100_000, func() {
			now := s.Now()
			err := n.c.Leave("bye", now)
			k.rec(n, "LV", now, "bye", jsErr(err))
			err = n.c.Leave("again", now)
			k.rec(n, "LV", now, "again", jsErr(err))
		})
	}
}

func (k *jsCall) tick(n *jsNode) {
	now := k.sim.Now()
	next := n.c.Tick(now)
	k.rec(n, "T", now, next)
	for {
		e := n.c.PollEvent()
		if e == nil {
			k.rec(n, "EV", now, nil)
			break
		}
		k.rec(n, "EV", now, []any{e.Kind, e.Stream, e.Track, e.Layers, e.Peer, e.Name, e.Bitrate, e.Code, e.Text, e.FrameID, e.Rep, e.MaxBytes})
		v := n.vids[e.Stream]
		switch e.Kind {
		case EventTrackAdded:
			if e.Track == KindAuth && !k.o.auth {
				continue
			}
			err := n.c.Subscribe(e.Stream, 7, now)
			k.rec(n, "SB", now, e.Stream, 7, jsErr(err))
			switch e.Track {
			case KindAudio:
				n.audio[e.Stream] = true
			case KindAuth:
				n.auth[e.Stream] = true
			default:
				n.video[e.Stream] = true
			}
		case EventTrackRemoved:
			delete(n.audio, e.Stream)
			delete(n.video, e.Stream)
			delete(n.auth, e.Stream)
		case EventKeyframeRequest:
			if v != nil {
				v.forceKey = true
			}
		case EventRecover:
			if v != nil {
				v.recover = e.FrameID
			}
		case EventSendRepresentation:
			if v != nil {
				if e.Rep == RepVideo && v.rep != RepVideo {
					v.frame = 0
				}
				v.rep = e.Rep
			}
		case EventReferencePicture:
			if v != nil {
				v.refReq = e.MaxBytes
			}
		}
	}
	for _, id := range slices.Sorted(maps.Keys(n.video)) {
		for {
			f := n.c.PollVideo(id, now)
			if f == nil {
				k.rec(n, "PO", now, id, nil)
				break
			}
			fl := jsBool(f.Keyframe) | jsBool(f.Face)<<1 | jsBool(f.Recovery)<<2 | jsBool(f.LongTerm)<<3
			k.rec(n, "PO", now, id, []any{f.Stream, f.FrameID, fl, f.Layer, f.Timestamp, jsDigest(f.Data)})
		}
	}
	for _, id := range slices.Sorted(maps.Keys(n.auth)) {
		for {
			b := n.c.PollAuth(id)
			if b == nil {
				k.rec(n, "PL", now, id, nil)
				break
			}
			k.rec(n, "PL", now, id, jsDigest(b))
		}
	}
}

func (k *jsCall) sendAudio(n *jsNode) {
	now := k.sim.Now()
	size := n.c.AudioBitrate(n.audioID) / 8 / 50
	k.rec(n, "AB", now, n.audioID, n.c.AudioBitrate(n.audioID))
	ms := n.c.AudioRedundancyMs(n.audioID)
	k.rec(n, "AM", now, n.audioID, ms)
	err := n.c.SetAudioRedundancy(n.audioID, ms)
	k.rec(n, "RD", now, n.audioID, ms, jsErr(err))
	size += 6 + ms/40
	b := make([]byte, size)
	binary.BigEndian.PutUint64(b, uint64(now))
	err = n.c.SendAudio(n.audioID, b, now)
	k.rec(n, "SA", now, n.audioID, jsRLE(b), jsErr(err))
}

func (k *jsCall) sendVideo(n *jsNode) {
	now := k.sim.Now()
	for _, id := range slices.Sorted(maps.Keys(n.vids)) {
		v := n.vids[id]
		switch v.rep {
		case RepFace:
			if v.refReq > 0 {
				b := make([]byte, min(1_400, v.refReq))
				binary.BigEndian.PutUint64(b, uint64(now))
				k.sendFrame(n, v.id, &OutFrame{Data: b, Keyframe: true, Face: true}, now)
				v.refReq = 0
			}
			b := make([]byte, 40)
			binary.BigEndian.PutUint64(b, uint64(now))
			k.sendFrame(n, v.id, &OutFrame{Data: b, Face: true}, now)
		case RepVideo:
			k.sendCamera(n, v, now)
		}
	}
}

func (k *jsCall) sendFrame(n *jsNode, stream int, f *OutFrame, now int64) (int64, error) {
	id, err := n.c.SendVideoFrame(stream, f, now)
	fl := jsBool(f.Keyframe) | jsBool(f.LongTerm)<<1 | jsBool(f.Face)<<2 | jsBool(f.Recover)<<3
	out := any(id)
	if err != nil {
		out = jsErr(err)
	}
	k.rec(n, "SV", now, stream, jsRLE(f.Data), fl, f.Layer, f.RefFrame, out)
	return id, err
}

func (k *jsCall) sendCamera(n *jsNode, v *jsVid, now int64) {
	rate := float64(n.c.VideoBitrate(v.id))
	k.rec(n, "VB", now, v.id, int(rate))
	if rate <= 0 {
		return
	}
	avg := rate / 8 / 30
	key := v.frame == 0 || (v.forceKey && now-v.lastKey >= 300_000)
	recover := !key && v.recover >= 0 && v.lastID >= 0
	if key {
		v.frame, v.forceKey, v.lastKey, v.recover = 0, false, now, -1
	}
	layer := [4]int{0, 2, 1, 2}[v.frame%4]
	if recover {
		layer = 0
	}
	v.balance = min(max(v.balance+avg, -30*avg), 2*avg)
	want := [3]float64{1.6, 0.9, 0.75}[layer] * avg
	if recover {
		want = min(1.6+0.04*float64(v.lastID+1-v.recover), 5) * avg
	}
	if key {
		want = 5 * avg
	}
	size := max(24, int(want+0.25*min(v.balance, 0)))
	v.balance -= float64(size)
	b := make([]byte, size)
	binary.BigEndian.PutUint64(b, uint64(now))
	binary.BigEndian.PutUint32(b[8:], uint32(v.frame))
	var ref int64
	if recover && v.frame%3 == 0 {
		ref = v.recover
	}
	id, err := k.sendFrame(n, v.id, &OutFrame{Data: b, Keyframe: key, Layer: layer, LongTerm: layer == 0, Recover: recover, RefFrame: ref}, now)
	if err != nil {
		v.recover = -1
		v.forceKey = true
		v.frame++
		return
	}
	if recover {
		v.recover = -1
	}
	v.lastID = id
	v.frame++
}

func (k *jsCall) pullAudio(n *jsNode) {
	now := k.sim.Now()
	for _, id := range slices.Sorted(maps.Keys(n.audio)) {
		f := n.c.PullAudio(id, now)
		d := ""
		if f.Data != nil {
			d = jsDigest(f.Data)
		}
		k.rec(n, "PU", now, id, []any{f.Status, d, f.Offset})
	}
}

type jsClientScenario struct {
	Name   string `json:"name"`
	Config Config `json:"config"`
	Max    int    `json:"maxDatagram"`
	Ops    [][]any `json:"ops"`
}

func runJSClientScenario(name string, o jsCallOpts) jsClientScenario {
	k := newJSCall(o)
	k.start()
	k.sim.RunUntil(o.secs * 1_000_000)
	n := k.nodes[0]
	n.rec.op("ST", o.secs*1_000_000, jsStats(n.c.Stats()))
	return jsClientScenario{Name: name, Config: o.cfg, Max: 1200, Ops: n.rec.ops}
}

func buildClientFixture() []jsClientScenario {
	base := Config{StartBitrate: 300_000, MinBitrate: 50_000, MaxBitrate: 500_000}
	clean := link.Params{DelayUs: 20_000, OverheadB: 40}
	var out []jsClientScenario
	out = append(out, runJSClientScenario("clean-three", jsCallOpts{clients: 3, secs: 3, cfg: base, up: clean, down: clean, seed: 21, auth: true}))
	out = append(out, runJSClientScenario("loss-10", jsCallOpts{clients: 2, secs: 4, cfg: base, seed: 22,
		up: link.Params{DelayUs: 25_000, Loss: 0.10, OverheadB: 40}, down: link.Params{DelayUs: 25_000, Loss: 0.10, OverheadB: 40}}))
	faceCfg := base
	faceCfg.MaxBitrate = 600_000
	out = append(out, runJSClientScenario("cap-face", jsCallOpts{clients: 2, secs: 5, cfg: faceCfg, face: true, seed: 23,
		up: link.Params{DelayUs: 20_000, RateBps: 1_000_000, QueueUs: 300_000, OverheadB: 40}, down: clean,
		upAt: 1_500_000, upLater: &link.Params{DelayUs: 20_000, RateBps: 100_000, QueueUs: 300_000, OverheadB: 40}}))
	out = append(out, runJSClientScenario("simulcast-weak", jsCallOpts{clients: 3, secs: 3, cfg: faceCfg, simulcast: 3, face: true, seed: 24,
		up: clean, down: link.Params{DelayUs: 20_000, Loss: 0.03, OverheadB: 40},
		weakDown: &link.Params{DelayUs: 20_000, RateBps: 250_000, QueueUs: 300_000, OverheadB: 40}}))
	ctl := base
	ctl.FeedbackOnControl = true
	ctl.NoLookAhead = true
	out = append(out, runJSClientScenario("control-feedback", jsCallOpts{clients: 2, secs: 3, cfg: ctl, control: true, seed: 25, churn: true,
		up: link.Params{DelayUs: 30_000, Loss: 0.05, JitterUs: 6_000, OverheadB: 40}, down: link.Params{DelayUs: 30_000, Loss: 0.05, OverheadB: 40}}))
	return out
}

func TestJSParity(t *testing.T) {
	b, err := json.Marshal(map[string]any{"scenarios": buildClientFixture()})
	if err != nil {
		t.Fatal(err)
	}
	b = append(b, '\n')
	path := filepath.Join("testdata", "js-parity.json")
	if os.Getenv("FMM_JS_PARITY_WRITE") == "1" {
		if err := os.MkdirAll("testdata", 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, b, 0o644); err != nil {
			t.Fatal(err)
		}
		t.Logf("wrote %s (%d bytes)", path, len(b))
		return
	}
	stored, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("%v (run with FMM_JS_PARITY_WRITE=1)", err)
	}
	if !bytes.Equal(stored, b) {
		t.Fatalf("%s is stale: run FMM_JS_PARITY_WRITE=1 go test -run JSParity ./rtc/fmm", path)
	}
}
