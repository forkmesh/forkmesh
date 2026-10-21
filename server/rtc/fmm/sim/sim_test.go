package sim

import (
	"fmt"
	"os"
	"strings"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/link"
)

func baseLink() link.Params { return link.Params{DelayUs: 20_000, OverheadB: 40} }

func with(f func(*link.Params)) link.Params {
	p := baseLink()
	f(&p)
	return p
}

func capped(rate int, queueUs int64) link.Params {
	return with(func(p *link.Params) { p.RateBps, p.QueueUs = rate, queueUs })
}

func dropRecover(secs, low int) []step {
	drop := secs / 3
	return []step{
		{0, capped(2_000_000, 250_000)},
		{int64(drop) * 1_000_000, capped(low, 400_000)},
		{int64(2*drop) * 1_000_000, capped(2_000_000, 250_000)},
	}
}

func profiles(secs int) []profile {
	return []profile{
		{name: "clean", steps: []step{{0, baseLink()}}},
		{name: "loss-5", steps: []step{{0, with(func(p *link.Params) { p.Loss = 0.05 })}}},
		{name: "loss-10", steps: []step{{0, with(func(p *link.Params) { p.Loss = 0.10 })}}},
		{name: "loss-20", steps: []step{{0, with(func(p *link.Params) { p.Loss = 0.20 })}}},
		{name: "burst", steps: []step{{0, with(func(p *link.Params) { p.BurstEnter, p.BurstLeave, p.BurstLoss = 0.02, 0.25, 0.9 })}}},
		{name: "jitter-100", steps: []step{{0, with(func(p *link.Params) { p.DelayUs, p.JitterUs, p.InOrder = 100_000, 30_000, true })}}},
		{name: "cap-300k", steps: []step{{0, capped(300_000, 400_000)}}},
		{name: "drop-recover", steps: dropRecover(secs, 300_000)},
	}
}

func long() bool { return os.Getenv("FMM_SIM") != "" }

func pick(secs int, o opts, names ...string) []profile {
	var out []profile
	for _, n := range names {
		for _, p := range profiles(secs) {
			if p.name == n {
				p.secs = secs
				p.warmUs = 5_000_000
				p.o = o
				out = append(out, p)
			}
		}
	}
	return out
}

func TestShortCallsHoldUpOnImpairedLinks(t *testing.T) {
	secs := 12
	names := []string{"clean", "loss-10", "jitter-100", "cap-300k"}
	if testing.Short() {
		names = []string{"clean", "loss-10"}
	}
	var rs []result
	for _, p := range pick(secs, defaults(), names...) {
		var runs []result
		for _, seed := range []uint64{7, 8, 9, 10, 11, 12} {
			k := newCall(p, 2, seed)
			k.run()
			runs = append(runs, k.result())
		}
		r := average(runs)
		rs = append(rs, r)
		limits := map[string]struct {
			conceal float64
			fps     float64
			freezes int
			delay   int64
		}{
			"clean":      {0.005, 29, 0, 100_000},
			"loss-10":    {0.01, 27, 1, 150_000},
			"jitter-100": {0.04, 25, 1, 320_000},
			"cap-300k":   {0.03, 15, 2, 300_000},
		}
		l := limits[p.name]
		if r.plain > l.conceal {
			t.Errorf("%s: audible audio concealment %.3f above %.3f", p.name, r.plain, l.conceal)
		}
		if r.fps < l.fps {
			t.Errorf("%s: %.1f fps below %.1f", p.name, r.fps, l.fps)
		}
		if r.freezesAvg > float64(l.freezes) {
			t.Errorf("%s: %.1f freezes above %d", p.name, r.freezesAvg, l.freezes)
		}
		if r.audioDelay > l.delay {
			t.Errorf("%s: audio delay %d ms above %d ms", p.name, r.audioDelay/1000, l.delay/1000)
		}
	}
	t.Logf("two clients through the SFU core, %d s each, first 5 s excluded, mean of seeds 7 to 12\n%s", secs, table(rs))
}

func faceCall(secs int, o opts, seed uint64) *call {
	o.face = true
	p := profile{name: "drop-recover-100k", steps: dropRecover(secs, 100_000), secs: secs, warmUs: 5_000_000, o: o}
	k := newCall(p, 2, seed)
	k.run()
	return k
}

type faceSummary struct {
	faceFPS, conceal, delay float64
	secs                    int
	refs                    int
	backAt                  int64
}

func summarizeFace(k *call) faceSummary {
	rx := k.nodes[1]
	var s faceSummary
	for _, r := range rx.rateLog {
		at := r.at / 1_000_000
		if at >= 13 && at <= 20 {
			s.faceFPS += float64(r.faceFPS)
			s.conceal += float64(r.conceal)
			s.secs++
		}
		if at > 20 && s.backAt == 0 && r.rep == 1 && r.videoFPS >= 25 {
			s.backAt = at
		}
	}
	s.faceFPS /= float64(max(s.secs, 1))
	if m := rx.video[firstVideo(rx)]; m != nil {
		s.refs = m.refs
	}
	return s
}

func TestFaceStreamCarriesTheCallThroughA100kDrop(t *testing.T) {
	secs := 30
	var b strings.Builder
	b.WriteString("| seed | face fps 13-20 s | audible audio concealments 13-20 s | reference pictures received | real video >= 25 fps again at |\n| --- | --- | --- | --- | --- |\n")
	for i, seed := range seeds[:3] {
		k := faceCall(secs, defaults(), seed)
		s := summarizeFace(k)
		fmt.Fprintf(&b, "| %d | %.1f | %.0f of %d frames | %d | %d s |\n", seed, s.faceFPS, s.conceal, s.secs*50, s.refs, s.backAt)
		if i == 0 {
			rx := k.nodes[1]
			t.Logf("seed %d, two clients, 2 Mbps, 100 kbps from 10 s, 2 Mbps from 20 s, both directions; client p1\n%s\n%s", seed, k.rateTable(rx), table([]result{k.result()}))
		}
		if s.faceFPS < 25 {
			t.Errorf("seed %d: face frames at %.1f fps through the 100 kbps section", seed, s.faceFPS)
		}
		if s.conceal > 0.03*50*float64(s.secs) {
			t.Errorf("seed %d: %.0f audible audio concealments in %d s at 100 kbps", seed, s.conceal, s.secs)
		}
		if s.backAt == 0 {
			t.Errorf("seed %d: did not return to real video after the link recovered", seed)
		}
		if s.refs == 0 {
			t.Errorf("seed %d: no reference picture reached the receiver", seed)
		}
	}
	t.Logf("drop-recover-100k with the face stream\n%s", b.String())
}

func firstVideo(n *node) int {
	for id := range n.video {
		return id
	}
	return 0
}

func TestFullSimulationTable(t *testing.T) {
	if !long() {
		t.Skip("set FMM_SIM=1 for the full three-client table")
	}
	secs := 30
	var rs []result
	var dropCall *call
	for _, p := range pick(secs, defaults(), "clean", "loss-5", "loss-10", "loss-20", "burst", "jitter-100", "cap-300k", "drop-recover") {
		r, calls := runSeeds(p, 3)
		rs = append(rs, r)
		if p.name == "drop-recover" {
			dropCall = calls[0]
		}
	}
	t.Logf("three clients through the SFU core, %d s each, first 5 s excluded, mean of seeds %v\n%s", secs, seeds, table(rs))
	if dropCall != nil {
		t.Logf("drop-recover, client p0 (2 Mbps, 300 kbps from 10 s, 2 Mbps from 20 s, both directions)\n%s", dropCall.rateTable(dropCall.nodes[0]))
	}
}

func TestFeatureAblation(t *testing.T) {
	if !long() {
		t.Skip("set FMM_SIM=1 for the ablation table")
	}
	secs := 30
	variants := []struct {
		name string
		o    opts
	}{
		{"all on", defaults()},
		{"no long-term recovery", func() opts { o := defaults(); o.ltr = false; return o }()},
		{"no DRED", func() opts { o := defaults(); o.dred = false; return o }()},
		{"feedback on the control stream", func() opts { o := defaults(); o.datagrams = false; return o }()},
		{"fixed 32 kbps audio", func() opts { o := defaults(); o.adaptive = false; return o }()},
	}
	var b strings.Builder
	for _, v := range variants {
		var rs []result
		for _, p := range pick(secs, v.o, "loss-10", "loss-20", "burst", "cap-300k", "drop-recover") {
			r, _ := runSeeds(p, 3)
			rs = append(rs, r)
		}
		fmt.Fprintf(&b, "%s\n%s\n", v.name, table(rs))
	}
	t.Logf("three clients, %d s\n%s", secs, b.String())
}

func pathLoss(name string, l link.Params) profile {
	return profile{name: name, steps: []step{{0, baseLink()}}, uplinks: map[int][]step{0: {{0, l}}}}
}

func TestAudioThroughPathLoss(t *testing.T) {
	if !long() {
		t.Skip("set FMM_SIM=1 for the audio loss table")
	}
	secs := 30
	ps := []profile{
		pathLoss("random 10%", with(func(p *link.Params) { p.Loss = 0.10 })),
		pathLoss("random 20%", with(func(p *link.Params) { p.Loss = 0.20 })),
		pathLoss("random 30%", with(func(p *link.Params) { p.Loss = 0.30 })),
		pathLoss("burst (GE 0.02/0.25, 90%)", with(func(p *link.Params) { p.BurstEnter, p.BurstLeave, p.BurstLoss = 0.02, 0.25, 0.9 })),
		pathLoss("burst (GE 0.05/0.2, 90%)", with(func(p *link.Params) { p.BurstEnter, p.BurstLeave, p.BurstLoss = 0.05, 0.2, 0.9 })),
	}
	var b strings.Builder
	b.WriteString("| path loss (p0 uplink only) | DRED | audible conceal | FEC recovered | DRED recovered | audio delay mean / p95 ms |\n| --- | --- | --- | --- | --- | --- |\n")
	for _, p := range ps {
		for _, dred := range []bool{false, true} {
			p.secs, p.warmUs = secs, 5_000_000
			p.o = defaults()
			p.o.dred = dred
			played, plain, fec, dr := 0, 0, 0, 0
			var delays []int64
			for _, seed := range seeds {
				k := newCall(p, 2, seed)
				k.run()
				for _, m := range k.nodes[1].audio {
					played += m.played
					plain += m.concealed - m.fec + m.inserted
					fec += m.fec
					dr += m.dred
					delays = append(delays, m.delays...)
				}
			}
			total := float64(played + plain + fec + dr)
			fmt.Fprintf(&b, "| %s | %v | %.2f%% | %.2f%% | %.2f%% | %d / %d |\n", p.name, dred, 100*float64(plain)/total, 100*float64(fec)/total,
				100*float64(dr)/total, mean(delays)/1000, pct(delays, 0.95)/1000)
		}
	}
	t.Logf("two clients, %d s, loss on p0's uplink only, audio p0 -> p1, seeds %v\n%s", secs, seeds, b.String())
}

func TestSimulcastKeepsAWeakSubscriberFromDraggingOthers(t *testing.T) {
	if !long() {
		t.Skip("set FMM_SIM=1 for the simulcast table")
	}
	secs := 30
	var b strings.Builder
	b.WriteString("| setup | p0 <- p1 video kbps | p1 <- p0 video kbps | p2 <- p0 video kbps | p2 <- p1 video kbps | p2 fps | p0/p1 freezes | p2 audible conceal |\n| --- | --- | --- | --- | --- | --- | --- | --- |\n")
	for _, v := range []struct {
		name      string
		simulcast int
		face      bool
	}{{"one encoding", 1, false}, {"simulcast x3", 3, false}, {"simulcast x3 + face", 3, true}, {"one encoding + face", 1, true}} {
		o := defaults()
		o.simulcast, o.face = v.simulcast, v.face
		p := profile{name: "weak-subscriber", steps: []step{{0, capped(2_500_000, 250_000)}}, secs: secs, warmUs: 8_000_000, o: o,
			downs: map[int][]step{2: {{0, capped(250_000, 400_000)}}}}
		k := newCall(p, 3, 23)
		k.run()
		window := float64(int64(secs)*1_000_000-p.warmUs) / 1e6
		kbps := func(n *node, from string) float64 {
			for id, m := range n.video {
				if n.owners[id] == from {
					return float64(m.bytes) * 8 / window / 1000
				}
			}
			return 0
		}
		fps, frz := 0.0, 0
		for _, i := range []int{0, 1} {
			for _, m := range k.nodes[i].video {
				f, _ := freezes(m.releases)
				frz += f
			}
		}
		for _, m := range k.nodes[2].video {
			fps += float64(len(m.releases)) / window / 2
		}
		played, plain := 0, 0
		for _, m := range k.nodes[2].audio {
			played += m.played + m.concealed + m.dred + m.inserted
			plain += m.concealed - m.fec + m.inserted
		}
		fmt.Fprintf(&b, "| %s | %.0f | %.0f | %.0f | %.0f | %.1f | %d | %.2f%% |\n", v.name, kbps(k.nodes[0], "p1"), kbps(k.nodes[1], "p0"),
			kbps(k.nodes[2], "p0"), kbps(k.nodes[2], "p1"), fps, frz, 100*float64(plain)/float64(max(played, 1)))
	}
	t.Logf("three clients at 2.5 Mbps, p2's downlink 250 kbps, %d s, first 8 s excluded\n%s", secs, b.String())
}
