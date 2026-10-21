package sim

import (
	"os"
	"testing"

	"forkmesh.com/mirror-node/rtc/fmm/link"
)

func baseLink() link.Params { return link.Params{DelayUs: 20_000, OverheadB: 40} }

func with(f func(*link.Params)) link.Params {
	p := baseLink()
	f(&p)
	return p
}

func profiles(secs int) []profile {
	capped := func(rate int, queueUs int64) link.Params {
		return with(func(p *link.Params) { p.RateBps, p.QueueUs = rate, queueUs })
	}
	drop := secs / 3
	return []profile{
		{name: "clean", steps: []step{{0, baseLink()}}},
		{name: "loss-5", steps: []step{{0, with(func(p *link.Params) { p.Loss = 0.05 })}}},
		{name: "loss-10", steps: []step{{0, with(func(p *link.Params) { p.Loss = 0.10 })}}},
		{name: "loss-20", steps: []step{{0, with(func(p *link.Params) { p.Loss = 0.20 })}}},
		{name: "burst", steps: []step{{0, with(func(p *link.Params) { p.BurstEnter, p.BurstLeave, p.BurstLoss = 0.02, 0.25, 0.9 })}}},
		{name: "jitter-100", steps: []step{{0, with(func(p *link.Params) { p.DelayUs, p.JitterUs, p.InOrder = 100_000, 30_000, true })}}},
		{name: "cap-300k", steps: []step{{0, capped(300_000, 400_000)}}},
		{name: "drop-recover", steps: []step{
			{0, capped(2_000_000, 250_000)},
			{int64(drop) * 1_000_000, capped(300_000, 400_000)},
			{int64(2*drop) * 1_000_000, capped(2_000_000, 250_000)},
		}},
	}
}

func long() bool { return os.Getenv("FMM_SIM") != "" }

func pick(secs int, names ...string) []profile {
	var out []profile
	for _, p := range profiles(secs) {
		for _, n := range names {
			if p.name == n {
				p.secs = secs
				p.warmUs = 5_000_000
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
	for _, p := range pick(secs, names...) {
		k := newCall(p, 2, 7)
		k.run()
		r := k.result()
		rs = append(rs, r)
		limits := map[string]struct {
			conceal float64
			fps     float64
			freezes int
			delay   int64
		}{
			"clean":      {0.005, 29, 0, 100_000},
			"loss-10":    {0.03, 25, 3, 150_000},
			"jitter-100": {0.03, 25, 1, 320_000},
			"cap-300k":   {0.06, 15, 2, 300_000},
		}
		l := limits[p.name]
		if r.concealment > l.conceal {
			t.Errorf("%s: audio concealment %.3f above %.3f", p.name, r.concealment, l.conceal)
		}
		if r.fps < l.fps {
			t.Errorf("%s: %.1f fps below %.1f", p.name, r.fps, l.fps)
		}
		if r.freezes > l.freezes {
			t.Errorf("%s: %d freezes above %d", p.name, r.freezes, l.freezes)
		}
		if r.audioDelay > l.delay {
			t.Errorf("%s: audio delay %d ms above %d ms", p.name, r.audioDelay/1000, l.delay/1000)
		}
	}
	t.Logf("two clients through the SFU core, %d s each, first 5 s excluded\n%s", secs, table(rs))
}

func TestFullSimulationTable(t *testing.T) {
	if !long() {
		t.Skip("set FMM_SIM=1 for the full three-client table")
	}
	secs := 30
	var rs []result
	var dropCall *call
	for _, p := range pick(secs, "clean", "loss-5", "loss-10", "loss-20", "burst", "jitter-100", "cap-300k", "drop-recover") {
		k := newCall(p, 3, 11)
		k.run()
		rs = append(rs, k.result())
		if p.name == "drop-recover" {
			dropCall = k
		}
	}
	t.Logf("three clients through the SFU core, %d s each, first 5 s excluded\n%s", secs, table(rs))
	if dropCall != nil {
		t.Logf("drop-recover, client p0 (2 Mbps, 300 kbps from 10 s, 2 Mbps from 20 s, both directions)\n%s", dropCall.rateTable(dropCall.nodes[0]))
	}
}
