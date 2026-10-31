package quic

import (
	"context"
	"net"
	"testing"
	"time"
)

func datagramWindowAfterLoss(t *testing.T, exempt bool) (int, int, int) {
	sconf, cconf := testServerConfig(t), testClientConfig(t)
	cconf.DatagramLossExempt = exempt
	p := newPair(t, sconf, cconf, func(pc net.PacketConn) net.PacketConn {
		return &lossyConn{PacketConn: pc, dropOut: func(n int, _ []byte) bool { return n > 20 && n%10 == 0 }}
	})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	got := 0
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			if _, err := p.srvConn.ReceiveDatagram(ctx); err != nil {
				return
			}
			got++
		}
	}()
	start, _ := p.cliConn.PathInfo(0)
	b := make([]byte, 1000)
	tick := time.NewTicker(2 * time.Millisecond)
	defer tick.Stop()
	for end := time.Now().Add(1500 * time.Millisecond); time.Now().Before(end); {
		<-tick.C
		p.cliConn.SendDatagram(b)
	}
	time.Sleep(50 * time.Millisecond)
	cancel()
	<-done
	info, _ := p.cliConn.PathInfo(0)
	return start.CongestionWindow, info.CongestionWindow, got
}

func TestDatagramLossExemptKeepsTheWindowOpen(t *testing.T) {
	plainStart, plain, plainGot := datagramWindowAfterLoss(t, false)
	exemptStart, exempt, exemptGot := datagramWindowAfterLoss(t, true)
	t.Logf("10%% datagram loss: window %d -> %d B (%d delivered) without the exemption, %d -> %d B (%d delivered) with it", plainStart, plain, plainGot, exemptStart, exempt, exemptGot)
	if plain > plainStart/2 {
		t.Fatalf("window without the exemption %d, started at %d: datagram loss did not back it off", plain, plainStart)
	}
	if exempt < exemptStart {
		t.Fatalf("window with the exemption %d, started at %d: datagram loss shrank it", exempt, exemptStart)
	}
}

func TestDatagramLossExemptStillBacksOffForStreams(t *testing.T) {
	sconf, cconf := testServerConfig(t), testClientConfig(t)
	cconf.DatagramLossExempt = true
	p := newPair(t, sconf, cconf, func(pc net.PacketConn) net.PacketConn {
		return &lossyConn{PacketConn: pc, dropOut: func(n int, _ []byte) bool { return n > 20 && n%10 == 0 }}
	})
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	go func() {
		st, err := p.srvConn.AcceptStream(ctx)
		if err != nil {
			return
		}
		buf := make([]byte, 64<<10)
		for {
			if _, err := st.Read(buf); err != nil {
				return
			}
		}
	}()
	st, err := p.cliConn.OpenStreamSync(ctx)
	if err != nil {
		t.Fatal(err)
	}
	before := p.cliConn.Stats()
	if _, err := st.Write(make([]byte, 2<<20)); err != nil {
		t.Fatal(err)
	}
	st.Close()
	time.Sleep(300 * time.Millisecond)
	info, _ := p.cliConn.PathInfo(0)
	t.Logf("stream under 10%% loss: window %d B, lost %d packets", info.CongestionWindow, p.cliConn.Stats().PacketsLost-before.PacketsLost)
	if info.CongestionWindow >= 1<<20 {
		t.Fatalf("stream losses did not shrink the window: %d", info.CongestionWindow)
	}
}
