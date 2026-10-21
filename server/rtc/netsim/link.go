package netsim

import (
	"math/rand/v2"
	"sort"
	"sync"
	"time"
)

const DefaultQueueBytes = 64 << 10

type Burst struct {
	Enter    float64 `json:"enter,omitempty"`
	Leave    float64 `json:"leave,omitempty"`
	LossGood float64 `json:"lossGood,omitempty"`
	LossBad  float64 `json:"lossBad,omitempty"`
}

func (b Burst) active() bool {
	return b.Enter > 0 || b.LossGood > 0
}

type Profile struct {
	Loss       float64       `json:"loss,omitempty"`
	Burst      Burst         `json:"burst,omitzero"`
	Delay      time.Duration `json:"delay,omitempty"`
	Jitter     time.Duration `json:"jitter,omitempty"`
	Reorder    bool          `json:"reorder,omitempty"`
	Rate       int64         `json:"rate,omitempty"`
	QueueBytes int           `json:"queueBytes,omitempty"`
	Duplicate  float64       `json:"duplicate,omitempty"`
}

type Step struct {
	At      time.Duration `json:"at"`
	Profile Profile       `json:"profile"`
	Rebind  bool          `json:"rebind,omitempty"`
}

type LinkStats struct {
	Packets    int64 `json:"packets"`
	Bytes      int64 `json:"bytes"`
	Delivered  int64 `json:"delivered"`
	Lost       int64 `json:"lost"`
	Overflow   int64 `json:"overflow"`
	Duplicated int64 `json:"duplicated"`
}

type Link struct {
	mu      sync.Mutex
	steps   []Step
	start   time.Time
	rng     *rand.Rand
	bad     bool
	drained time.Time
	last    time.Time
	stats   LinkStats
}

func NewLink(seed uint64, start time.Time, steps ...Step) *Link {
	link := &Link{rng: rand.New(rand.NewPCG(seed, seed^0x9e3779b97f4a7c15))}
	link.Schedule(start, steps...)
	return link
}

func (l *Link) Schedule(start time.Time, steps ...Step) {
	sorted := append([]Step(nil), steps...)
	sort.SliceStable(sorted, func(i, j int) bool { return sorted[i].At < sorted[j].At })
	l.mu.Lock()
	l.start = start
	l.steps = sorted
	l.mu.Unlock()
}

func (l *Link) ProfileAt(now time.Time) Profile {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.profileLocked(now)
}

func (l *Link) profileLocked(now time.Time) Profile {
	elapsed := now.Sub(l.start)
	current := Profile{}
	for _, step := range l.steps {
		if step.At > elapsed {
			break
		}
		current = step.Profile
	}
	return current
}

func (l *Link) Stats() LinkStats {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.stats
}

func (l *Link) Plan(now time.Time, size int) []time.Time {
	l.mu.Lock()
	defer l.mu.Unlock()
	profile := l.profileLocked(now)
	l.stats.Packets++
	l.stats.Bytes += int64(size)
	if l.loseLocked(profile) {
		l.stats.Lost++
		return nil
	}
	copies := 1
	if profile.Duplicate > 0 && l.rng.Float64() < profile.Duplicate {
		copies = 2
		l.stats.Duplicated++
	}
	var arrivals []time.Time
	for range copies {
		departure, ok := l.enqueueLocked(profile, now, size)
		if !ok {
			l.stats.Overflow++
			continue
		}
		arrival := departure.Add(profile.Delay + l.jitterLocked(profile))
		if arrival.Before(departure) {
			arrival = departure
		}
		if !profile.Reorder && arrival.Before(l.last) {
			arrival = l.last
		}
		if arrival.After(l.last) {
			l.last = arrival
		}
		arrivals = append(arrivals, arrival)
		l.stats.Delivered++
	}
	return arrivals
}

func (l *Link) loseLocked(profile Profile) bool {
	if profile.Burst.active() {
		loss := profile.Burst.LossGood
		if l.bad {
			loss = profile.Burst.LossBad
		}
		lost := loss > 0 && l.rng.Float64() < loss
		if l.bad {
			if l.rng.Float64() < profile.Burst.Leave {
				l.bad = false
			}
		} else if l.rng.Float64() < profile.Burst.Enter {
			l.bad = true
		}
		if lost {
			return true
		}
	}
	return profile.Loss > 0 && l.rng.Float64() < profile.Loss
}

func (l *Link) enqueueLocked(profile Profile, now time.Time, size int) (time.Time, bool) {
	if profile.Rate <= 0 {
		return now, true
	}
	limit := profile.QueueBytes
	if limit <= 0 {
		limit = DefaultQueueBytes
	}
	if l.drained.Before(now) {
		l.drained = now
	}
	backlog := l.drained.Sub(now).Seconds() * float64(profile.Rate) / 8
	if backlog+float64(size) > float64(limit) {
		return time.Time{}, false
	}
	l.drained = l.drained.Add(time.Duration(float64(size*8) / float64(profile.Rate) * float64(time.Second)))
	return l.drained, true
}

func (l *Link) jitterLocked(profile Profile) time.Duration {
	if profile.Jitter <= 0 {
		return 0
	}
	return time.Duration((l.rng.Float64()*2 - 1) * float64(profile.Jitter))
}
