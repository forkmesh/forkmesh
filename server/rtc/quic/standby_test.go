package quic

import (
	"context"
	"encoding/binary"
	"net"
	"net/netip"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/netsim"
)

func serveMedia(t *testing.T, ep *Endpoint, push bool) {
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	go func() {
		for {
			c, err := ep.Accept(ctx)
			if err != nil {
				return
			}
			go func() {
				for {
					d, err := c.ReceiveDatagram(ctx)
					if err != nil {
						return
					}
					c.SendDatagram(d)
				}
			}()
			if push {
				go func() {
					tick := time.NewTicker(20 * time.Millisecond)
					defer tick.Stop()
					b := make([]byte, 100)
					for {
						select {
						case <-ctx.Done():
							return
						case <-c.Done():
							return
						case <-tick.C:
							c.SendDatagram(b)
						}
					}
				}()
			}
		}
	}()
}

func TestStandbyDetectsDeadPrimary(t *testing.T) {
	cases := []struct {
		name   string
		up     netsim.Profile
		down   netsim.Profile
		before time.Duration
	}{
		{"blackhole", netsim.Profile{Delay: 20 * time.Millisecond, Loss: 1}, netsim.Profile{Delay: 20 * time.Millisecond, Loss: 1}, time.Second},
		{"uplink-only", netsim.Profile{Delay: 20 * time.Millisecond, Loss: 1}, netsim.Profile{Delay: 20 * time.Millisecond}, time.Second},
		{"lossy-not-dead", netsim.Profile{Delay: 20 * time.Millisecond, Loss: 0.1}, netsim.Profile{Delay: 20 * time.Millisecond, Loss: 0.1}, time.Second},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			srvA, err := Listen("udp4", "127.0.0.1:0", testServerConfig(t))
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { srvA.Close() })
			srvB, err := Listen("udp4", "127.0.0.1:0", testServerConfig(t))
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { srvB.Close() })
			serveMedia(t, srvA, true)
			serveMedia(t, srvB, false)
			relay, err := netsim.Listen("127.0.0.1:0", srvA.LocalAddr().(*net.UDPAddr).AddrPort(), netsim.Config{Seed: 3})
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { relay.Close() })
			sock := mustUDP(t)
			key := flowKey(sock)
			clean := []netsim.Step{{Profile: netsim.Profile{Delay: 20 * time.Millisecond}}}
			relay.Shape(key, time.Now(), clean, clean)
			cli, err := NewEndpoint(sock, testClientConfig(t), false)
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { cli.Close() })
			ctx := ctxTimeout(t, 20*time.Second)
			primary, err := cli.Dial(ctx, relay.Addr(), testClientConfig(t))
			if err != nil {
				t.Fatal(err)
			}
			relayB, err := netsim.Listen("127.0.0.1:0", srvB.LocalAddr().(*net.UDPAddr).AddrPort(), netsim.Config{Seed: 4})
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { relayB.Close() })
			coreB := []netsim.Step{{Profile: netsim.Profile{Delay: 25 * time.Millisecond}}}
			relayB.Shape(key, time.Now(), coreB, coreB)
			sb := NewStandby(primary, func(ctx context.Context) (*Conn, error) {
				return cli.Dial(ctx, relayB.Addr(), testClientConfig(t))
			}, StandbyOptions{})
			t.Cleanup(func() { sb.Close() })
			if _, err := sb.Conn(ctx); err != nil {
				t.Fatal(err)
			}
			go func() {
				for {
					if _, err := primary.ReceiveDatagram(ctx); err != nil {
						return
					}
				}
			}()
			stop := make(chan struct{})
			sent := make(chan struct{})
			go func() {
				defer close(sent)
				tick := time.NewTicker(20 * time.Millisecond)
				defer tick.Stop()
				b := make([]byte, 120)
				for i := uint64(0); ; i++ {
					select {
					case <-stop:
						return
					case <-tick.C:
					}
					binary.BigEndian.PutUint64(b, i)
					primary.SendDatagram(b)
				}
			}()
			time.Sleep(tc.before)
			select {
			case <-sb.PrimaryDead():
				t.Fatalf("primary declared dead on a clean link: %v", primary.Health())
			default:
			}
			kill := time.Now()
			relay.Shape(key, kill, []netsim.Step{{Profile: tc.up}}, []netsim.Step{{Profile: tc.down}})
			if tc.name == "lossy-not-dead" {
				time.Sleep(2 * time.Second)
				close(stop)
				<-sent
				if at, reason := sb.DeadSince(); !at.IsZero() {
					t.Fatalf("lossy primary declared dead after %v: %v", at.Sub(kill), reason)
				}
				return
			}
			select {
			case <-sb.PrimaryDead():
			case <-time.After(3 * time.Second):
				t.Fatalf("dead primary not detected: %+v", primary.Health())
			}
			close(stop)
			<-sent
			at, reason := sb.DeadSince()
			detect := at.Sub(kill)
			next, err := sb.Promote(ctx)
			if err != nil {
				t.Fatal(err)
			}
			moved := time.Now()
			if err := next.SendDatagram([]byte("join")); err != nil {
				t.Fatal(err)
			}
			got, err := next.ReceiveDatagram(ctx)
			if err != nil || string(got) != "join" {
				t.Fatalf("standby echo %q %v", got, err)
			}
			move := time.Since(moved)
			rtt := next.Stats().SmoothedRTT
			t.Logf("%s: dead primary detected after %v (%s); call moved to the standby in %v (standby srtt %v)", tc.name, detect, reason, move, rtt)
			if detect > 300*time.Millisecond {
				t.Errorf("detection took %v", detect)
			}
			if _, err := sb.Conn(ctx); err != nil {
				t.Fatalf("no fresh standby after promotion: %v", err)
			}
		})
	}
}

func TestStandbyDetectsClosedPrimary(t *testing.T) {
	p := newPair(t, testServerConfig(t), testClientConfig(t), nil)
	addr := netip.AddrPortFrom(p.srvAddr.Addr(), p.srvAddr.Port())
	sb := NewStandby(p.cliConn, func(ctx context.Context) (*Conn, error) {
		return p.client.Dial(ctx, addr, testClientConfig(t))
	}, StandbyOptions{})
	t.Cleanup(func() { sb.Close() })
	go func() {
		for {
			if _, err := p.server.Accept(context.Background()); err != nil {
				return
			}
		}
	}()
	ctx := ctxTimeout(t, 10*time.Second)
	if _, err := sb.Conn(ctx); err != nil {
		t.Fatal(err)
	}
	start := time.Now()
	p.srvConn.CloseWithError(9, "core going away")
	select {
	case <-sb.PrimaryDead():
	case <-ctx.Done():
		t.Fatal("closed primary not reported")
	}
	if _, reason := sb.DeadSince(); reason != DeadClosed {
		t.Fatalf("reason %v", reason)
	}
	t.Logf("closed primary reported after %v", time.Since(start))
}
