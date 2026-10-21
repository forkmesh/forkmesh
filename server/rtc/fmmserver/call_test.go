package fmmserver

import (
	"context"
	"sort"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/fmmcall"
	"forkmesh.com/mirror-node/rtc/netsim"
)

func TestThreePeopleHoldACallThroughImpairedLinks(t *testing.T) {
	c := startCore(t, "n1", 7)
	room := testRoom(4)
	profiles := []netsim.Profile{
		{Delay: 20 * time.Millisecond, Jitter: 3 * time.Millisecond},
		{Delay: 30 * time.Millisecond, Jitter: 5 * time.Millisecond, Loss: 0.03},
		{Delay: 45 * time.Millisecond, Jitter: 8 * time.Millisecond, Loss: 0.05, Rate: 3_000_000},
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	var parties []*party
	for i := range profiles {
		p := newParty(t, uint8(i), true)
		shape(c.relay, p.key(), profiles[i])
		if err := p.join(ctx, fmmcall.Access{Endpoint: endpointOf(c, ticketFor(room, p.peer, p.acct(), false))}); err != nil {
			t.Fatal(err)
		}
		parties = append(parties, p)
	}
	defer func() {
		for _, p := range parties {
			p.leave()
		}
	}()
	time.Sleep(time.Second)
	start := time.Now()
	time.Sleep(3 * time.Second)
	end := time.Now()
	if got := len(c.server.Peers(room)); got != 3 {
		t.Fatalf("%d peers in the room", got)
	}
	for _, p := range parties {
		for _, o := range parties {
			if o == p {
				continue
			}
			got := 0
			for _, pl := range p.playsFrom(o.idx, start) {
				if pl.at.Before(end) {
					got++
				}
			}
			want := int(end.Sub(start) / (20 * time.Millisecond))
			t.Logf("p%d hears p%d: %d of %d frames", p.idx, o.idx, got, want)
			if got < want*70/100 {
				t.Errorf("p%d heard only %d of %d audio frames from p%d", p.idx, got, want, o.idx)
			}
		}
		t.Logf("p%d decoded %d video frames, errors %v, stats %+v", p.idx, p.videoFrames(), p.errors, *p.call.Session().Client.Stats())
		if floor := []int{20, 20, 5}[p.idx]; p.videoFrames() < floor {
			t.Errorf("p%d received %d video frames", p.idx, p.videoFrames())
		}
	}
}

type gap struct {
	listener, speaker uint8
	silent            time.Duration
	back              time.Duration
}

func TestCoreDeathMovesTheCallToTheStandbyCore(t *testing.T) {
	for _, mode := range []string{"silent", "closed"} {
		t.Run(mode, func(t *testing.T) { runFailover(t, mode) })
	}
}

func runFailover(t *testing.T, mode string) {
	cores := []*core{startCore(t, "n2", 21), startCore(t, "n3", 22), startCore(t, "n4", 23)}
	room := testRoom(5)
	order := rendezvous(room, cores)
	primary, standby, third := order[0], order[1], order[2]
	link := netsim.Profile{Delay: 20 * time.Millisecond, Jitter: 2 * time.Millisecond}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	var parties []*party
	for i := 0; i < 3; i++ {
		p := newParty(t, uint8(i), false)
		for _, c := range cores {
			shape(c.relay, p.key(), link)
		}
		sb := endpointOf(standby, ticketFor(room, p.peer, p.acct(), true))
		access := fmmcall.Access{Endpoint: endpointOf(primary, ticketFor(room, p.peer, p.acct(), false)), Standby: &sb}
		if err := p.join(ctx, access); err != nil {
			t.Fatal(err)
		}
		parties = append(parties, p)
	}
	defer func() {
		for _, p := range parties {
			p.leave()
		}
	}()
	for _, p := range parties {
		if err := p.call.StandbyReady(ctx); err != nil {
			t.Fatal(err)
		}
	}
	time.Sleep(1500 * time.Millisecond)
	if standby.server.Parked() != 3 || len(primary.server.Peers(room)) != 3 {
		t.Fatalf("before: %d parked on the standby, %d peers on the primary", standby.server.Parked(), len(primary.server.Peers(room)))
	}
	killed := time.Now()
	if mode == "silent" {
		for _, p := range parties {
			shape(primary.relay, p.key(), netsim.Profile{Loss: 1})
		}
	}
	primary.server.Close()
	for _, p := range parties {
		select {
		case <-p.call.Moved():
		case <-time.After(5 * time.Second):
			t.Fatalf("p%d did not move", p.idx)
		}
	}
	time.Sleep(1500 * time.Millisecond)
	if got := len(standby.server.Peers(room)); got != 3 {
		t.Fatalf("%d peers on the standby core", got)
	}
	if third.server.Joins() != 0 || primary.server.Joins() != 3 || standby.server.Joins() != 3 {
		t.Fatalf("joins: primary %d, standby %d, third %d", primary.server.Joins(), standby.server.Joins(), third.server.Joins())
	}
	var gaps []gap
	worst := time.Duration(0)
	for _, p := range parties {
		deadAt, movedAt, reason := p.call.Failover()
		t.Logf("p%d: primary judged dead after %v (%v), moved after %v", p.idx, deadAt.Sub(killed).Round(time.Millisecond), reason, movedAt.Sub(killed).Round(time.Millisecond))
		for _, o := range parties {
			if o == p {
				continue
			}
			if len(p.playsFrom(o.idx, killed.Add(time.Second))) == 0 {
				t.Fatalf("p%d never heard p%d again", p.idx, o.idx)
			}
			g := measureGap(p.playsFrom(o.idx, killed.Add(-time.Second)), killed)
			g.listener, g.speaker = p.idx, o.idx
			gaps = append(gaps, g)
			if g.back > worst {
				worst = g.back
			}
		}
	}
	sort.Slice(gaps, func(i, j int) bool { return gaps[i].back < gaps[j].back })
	for _, g := range gaps {
		t.Logf("%s: p%d hears p%d again %v after the core died (longest silence %v)", mode, g.listener, g.speaker, g.back.Round(time.Millisecond), g.silent.Round(time.Millisecond))
	}
	if worst > 500*time.Millisecond {
		t.Errorf("%s: audio came back after %v, target 500 ms", mode, worst)
	}
}

func measureGap(plays []play, killed time.Time) gap {
	var g gap
	for i := 1; i < len(plays); i++ {
		if plays[i].at.Before(killed) {
			continue
		}
		if d := plays[i].at.Sub(plays[i-1].at); d > g.silent {
			g.silent = d
			g.back = plays[i].at.Sub(killed)
		}
	}
	return g
}
