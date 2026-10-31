package bench

import (
	"slices"
	"strings"
	"time"

	"forkmesh.com/mirror-node/rtc/netsim"
)

type Profile struct {
	Name        string        `json:"name"`
	Description string        `json:"description"`
	Up          []netsim.Step `json:"up,omitempty"`
	Down        []netsim.Step `json:"down,omitempty"`
}

func both(name, description string, steps ...netsim.Step) Profile {
	return Profile{Name: name, Description: description, Up: steps, Down: steps}
}

func steady(profile netsim.Profile) netsim.Step {
	return netsim.Step{Profile: profile}
}

func queueFor(rate int64, hold time.Duration) int {
	return int(float64(rate) / 8 * hold.Seconds())
}

func Profiles() []Profile {
	fast := netsim.Profile{Rate: 2_000_000, QueueBytes: queueFor(2_000_000, 250*time.Millisecond)}
	slow := netsim.Profile{Rate: 300_000, QueueBytes: queueFor(300_000, 400*time.Millisecond)}
	wifi := netsim.Profile{Delay: 10 * time.Millisecond, Jitter: 3 * time.Millisecond}
	cellular := netsim.Profile{
		Delay: 60 * time.Millisecond, Jitter: 15 * time.Millisecond, Loss: 0.01,
		Rate: 4_000_000, QueueBytes: queueFor(4_000_000, 150*time.Millisecond),
	}
	return []Profile{
		both("clean", "no impairment"),
		both("loss-5", "5% random loss each way", steady(netsim.Profile{Loss: 0.05})),
		both("loss-10", "10% random loss each way", steady(netsim.Profile{Loss: 0.10})),
		both("loss-20", "20% random loss each way", steady(netsim.Profile{Loss: 0.20})),
		both("burst", "Gilbert-Elliott burst loss, about 6.7% in bursts of about 4 packets",
			steady(netsim.Profile{Burst: netsim.Burst{Enter: 0.02, Leave: 0.25, LossBad: 0.9}})),
		both("jitter-100", "100 ms one-way delay with ±30 ms jitter, in order",
			steady(netsim.Profile{Delay: 100 * time.Millisecond, Jitter: 30 * time.Millisecond})),
		both("cap-1m", "1 Mbps each way, 250 ms drop-tail queue",
			steady(netsim.Profile{Rate: 1_000_000, QueueBytes: queueFor(1_000_000, 250*time.Millisecond)})),
		both("cap-300k", "300 kbps each way, 400 ms drop-tail queue", steady(slow)),
		both("drop-recover", "2 Mbps, 300 kbps from 10 s, 2 Mbps again from 20 s",
			steady(fast), netsim.Step{At: 10 * time.Second, Profile: slow}, netsim.Step{At: 20 * time.Second, Profile: fast}),
		both("path-change", "Wi-Fi (10±3 ms) then at 12 s cellular (60±15 ms, 1% loss, 4 Mbps) on a new source port",
			steady(wifi), netsim.Step{At: 12 * time.Second, Profile: cellular, Rebind: true}),
	}
}

func Select(profiles []Profile, names string) []Profile {
	if strings.TrimSpace(names) == "" {
		return profiles
	}
	wanted := strings.Split(names, ",")
	for i := range wanted {
		wanted[i] = strings.TrimSpace(wanted[i])
	}
	var out []Profile
	for _, profile := range profiles {
		if slices.Contains(wanted, profile.Name) {
			out = append(out, profile)
		}
	}
	return out
}

func (p Profile) Marks() []time.Duration {
	var marks []time.Duration
	for _, step := range append(append([]netsim.Step(nil), p.Up...), p.Down...) {
		if step.At > 0 && !slices.Contains(marks, step.At) {
			marks = append(marks, step.At)
		}
	}
	slices.Sort(marks)
	return marks
}
