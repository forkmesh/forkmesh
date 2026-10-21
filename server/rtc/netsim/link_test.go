package netsim

import (
	"math"
	"testing"
	"time"
)

var epoch = time.Unix(1_700_000_000, 0)

func TestACleanLinkDeliversEveryPacketAtOnce(t *testing.T) {
	link := NewLink(1, epoch)
	for i := range 1000 {
		now := epoch.Add(time.Duration(i) * time.Millisecond)
		arrivals := link.Plan(now, 1200)
		if len(arrivals) != 1 || !arrivals[0].Equal(now) {
			t.Fatalf("packet %d: %v", i, arrivals)
		}
	}
	if stats := link.Stats(); stats.Delivered != 1000 || stats.Lost != 0 || stats.Bytes != 1_200_000 {
		t.Fatalf("stats %+v", stats)
	}
}

func TestRandomLossDropsTheConfiguredShare(t *testing.T) {
	for _, loss := range []float64{0.05, 0.1, 0.2} {
		link := NewLink(7, epoch, Step{Profile: Profile{Loss: loss}})
		const total = 200_000
		lost := 0
		for i := range total {
			if len(link.Plan(epoch.Add(time.Duration(i)*time.Microsecond), 100)) == 0 {
				lost++
			}
		}
		got := float64(lost) / total
		if math.Abs(got-loss) > 0.005 {
			t.Errorf("loss %.2f: dropped %.4f", loss, got)
		}
	}
}

func TestGilbertElliottLossComesInBursts(t *testing.T) {
	burst := Burst{Enter: 0.02, Leave: 0.25, LossBad: 1}
	link := NewLink(3, epoch, Step{Profile: Profile{Burst: burst}})
	const total = 400_000
	lost, runs, run := 0, 0, 0
	longest := 0
	for i := range total {
		if len(link.Plan(epoch.Add(time.Duration(i)*time.Microsecond), 100)) == 0 {
			lost++
			run++
			continue
		}
		if run > 0 {
			runs++
			longest = max(longest, run)
		}
		run = 0
	}
	stationary := burst.Enter / (burst.Enter + burst.Leave)
	share := float64(lost) / total
	if math.Abs(share-stationary) > 0.01 {
		t.Errorf("burst loss %.4f, want about %.4f", share, stationary)
	}
	mean := float64(lost) / float64(runs)
	if math.Abs(mean-1/burst.Leave) > 0.3 {
		t.Errorf("mean burst %.2f packets, want about %.2f", mean, 1/burst.Leave)
	}
	if longest < 10 {
		t.Errorf("longest burst only %d packets", longest)
	}
	random := NewLink(3, epoch, Step{Profile: Profile{Loss: share}})
	lost, runs, run = 0, 0, 0
	for i := range total {
		if len(random.Plan(epoch.Add(time.Duration(i)*time.Microsecond), 100)) == 0 {
			lost++
			run++
			continue
		}
		if run > 0 {
			runs++
		}
		run = 0
	}
	if float64(lost)/float64(runs) > 1.3 {
		t.Errorf("random loss should not come in bursts: mean run %.2f", float64(lost)/float64(runs))
	}
}

func TestDelayAndJitterStayInBoundsAndKeepOrderUnlessAllowed(t *testing.T) {
	profile := Profile{Delay: 100 * time.Millisecond, Jitter: 30 * time.Millisecond}
	ordered := NewLink(5, epoch, Step{Profile: profile})
	profile.Reorder = true
	shuffled := NewLink(5, epoch, Step{Profile: profile})
	var lastOrdered, lastShuffled time.Time
	reordered := 0
	low, high := time.Hour, time.Duration(0)
	for i := range 5000 {
		now := epoch.Add(time.Duration(i) * 2 * time.Millisecond)
		a := ordered.Plan(now, 500)[0]
		b := shuffled.Plan(now, 500)[0]
		if a.Before(lastOrdered) {
			t.Fatalf("packet %d overtook the one before it", i)
		}
		if b.Before(lastShuffled) {
			reordered++
		}
		lastOrdered, lastShuffled = a, b
		delay := b.Sub(now)
		low, high = min(low, delay), max(high, delay)
		if a.Sub(now) < 70*time.Millisecond {
			t.Fatalf("packet %d arrived after %v", i, a.Sub(now))
		}
	}
	if low < 70*time.Millisecond || high > 130*time.Millisecond || high-low < 50*time.Millisecond {
		t.Errorf("delay spread %v..%v", low, high)
	}
	if reordered < 500 {
		t.Errorf("only %d of 5000 packets reordered", reordered)
	}
}

func TestARateCapDrainsAtTheRateAndDropsTheTail(t *testing.T) {
	profile := Profile{Rate: 300_000, QueueBytes: 15_000}
	link := NewLink(9, epoch, Step{Profile: profile})
	var last time.Time
	delivered := 0
	worst := time.Duration(0)
	for i := range 3000 {
		now := epoch.Add(time.Duration(i) * time.Millisecond)
		for _, at := range link.Plan(now, 1000) {
			delivered++
			last = at
			worst = max(worst, at.Sub(now))
		}
	}
	rate := float64(delivered*1000*8) / last.Sub(epoch).Seconds()
	if math.Abs(rate-300_000) > 10_000 {
		t.Errorf("delivered %.0f bps through a 300 kbps cap", rate)
	}
	if stats := link.Stats(); stats.Overflow == 0 || stats.Overflow+stats.Delivered != 3000 {
		t.Errorf("stats %+v", stats)
	}
	if limit := time.Duration(float64(profile.QueueBytes*8) / 300_000 * float64(time.Second)); worst > limit+30*time.Millisecond {
		t.Errorf("queueing delay %v exceeds the queue of %v", worst, limit)
	}
	idle := epoch.Add(10 * time.Second)
	if at := link.Plan(idle, 1000); len(at) != 1 || at[0].Sub(idle) != time.Duration(1000*8)*time.Second/300_000 {
		t.Errorf("an idle link should only add serialization: %v", at)
	}
}

func TestDuplicationSendsSomePacketsTwice(t *testing.T) {
	link := NewLink(11, epoch, Step{Profile: Profile{Duplicate: 0.1}})
	extra := 0
	for i := range 50_000 {
		extra += len(link.Plan(epoch.Add(time.Duration(i)*time.Microsecond), 100)) - 1
	}
	if share := float64(extra) / 50_000; math.Abs(share-0.1) > 0.01 {
		t.Errorf("duplicated %.4f", share)
	}
}

func TestScheduledStepsChangeTheLinkOverTime(t *testing.T) {
	fast := Profile{Rate: 2_000_000, QueueBytes: 64_000}
	slow := Profile{Rate: 300_000, QueueBytes: 15_000}
	link := NewLink(13, epoch,
		Step{At: 20 * time.Second, Profile: fast},
		Step{Profile: fast},
		Step{At: 10 * time.Second, Profile: slow},
	)
	for _, check := range []struct {
		at   time.Duration
		rate int64
	}{{0, 2_000_000}, {9 * time.Second, 2_000_000}, {10 * time.Second, 300_000}, {19 * time.Second, 300_000}, {20 * time.Second, 2_000_000}} {
		if got := link.ProfileAt(epoch.Add(check.at)).Rate; got != check.rate {
			t.Errorf("at %v: rate %d, want %d", check.at, got, check.rate)
		}
	}
	cellular := Profile{Delay: 80 * time.Millisecond, Loss: 0.01}
	wifi := NewLink(17, epoch, Step{Profile: Profile{Delay: 10 * time.Millisecond}}, Step{At: time.Second, Profile: cellular})
	before := wifi.Plan(epoch.Add(500*time.Millisecond), 100)
	after := wifi.Plan(epoch.Add(2*time.Second), 100)
	if before[0].Sub(epoch.Add(500*time.Millisecond)) != 10*time.Millisecond || after[0].Sub(epoch.Add(2*time.Second)) != 80*time.Millisecond {
		t.Errorf("wifi %v then cellular %v", before, after)
	}
}
