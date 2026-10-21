package quic

import (
	"bytes"
	"context"
	"crypto/rand"
	"io"
	mrand "math/rand"
	"net"
	"sync"
	"testing"
	"time"
)

func lossy(seed int64, rate float64, handshakeDrops map[int]bool) func(net.PacketConn) net.PacketConn {
	return func(pc net.PacketConn) net.PacketConn {
		var mu sync.Mutex
		rng := mrand.New(mrand.NewSource(seed))
		drop := func(n int, b []byte) bool {
			if handshakeDrops[n] {
				return true
			}
			mu.Lock()
			defer mu.Unlock()
			return rng.Float64() < rate
		}
		return &lossyConn{PacketConn: pc, dropOut: drop, dropIn: func(n int, b []byte) bool { return drop(n+1000, b) }}
	}
}

func TestHandshakeSurvivesLostFlights(t *testing.T) {
	drops := map[int]bool{1: true, 1001: true, 1002: true}
	p := newPair(t, testServerConfig(t), testClientConfig(t), lossy(1, 0, drops))
	echoStreams(t, p.srvConn)
	s, err := p.cliConn.OpenStream()
	if err != nil {
		t.Fatal(err)
	}
	s.Write([]byte("after loss"))
	s.Close()
	s.SetReadDeadline(time.Now().Add(10 * time.Second))
	got, err := io.ReadAll(s)
	if err != nil || string(got) != "after loss" {
		t.Fatalf("echo %q %v", got, err)
	}
}

func TestStreamTransferUnderLoss(t *testing.T) {
	p := newPair(t, testServerConfig(t), testClientConfig(t), lossy(7, 0.08, nil))
	echoStreams(t, p.srvConn)
	msg := make([]byte, 1<<20)
	rand.Read(msg)
	var wg sync.WaitGroup
	for i := 0; i < 3; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			s, err := p.cliConn.OpenStreamSync(context.Background())
			if err != nil {
				t.Error(err)
				return
			}
			go func() {
				s.Write(msg)
				s.Close()
			}()
			s.SetReadDeadline(time.Now().Add(60 * time.Second))
			got, err := io.ReadAll(s)
			if err != nil {
				t.Errorf("read: %v", err)
				return
			}
			if !bytes.Equal(got, msg) {
				t.Errorf("corrupted echo: %d bytes", len(got))
			}
		}()
	}
	wg.Wait()
	st := p.cliConn.Stats()
	t.Logf("client stats: sent %d lost %d rtt %v cwnd %d", st.PacketsSent, st.PacketsLost, st.SmoothedRTT, st.CongestionWindow)
	if st.PacketsLost == 0 {
		t.Error("loss injection had no effect")
	}
}

func TestDatagramsUnderLoss(t *testing.T) {
	p := newPair(t, testServerConfig(t), testClientConfig(t), lossy(3, 0.2, nil))
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	got := make(chan int, 1)
	go func() {
		n := 0
		for {
			d, err := p.srvConn.ReceiveDatagram(ctx)
			if err != nil || len(d) != 900 {
				got <- n
				return
			}
			n++
			if n == 140 {
				got <- n
				return
			}
		}
	}()
	payload := make([]byte, 900)
	for i := 0; i < 200; i++ {
		for p.cliConn.SendDatagram(payload) == ErrDatagramQueueFull {
			time.Sleep(time.Millisecond)
		}
	}
	n := <-got
	if n < 100 || n > 200 {
		t.Fatalf("received %d of 200 datagrams at 20%% loss", n)
	}
	time.Sleep(200 * time.Millisecond)
	st := p.cliConn.Stats()
	if st.DatagramsSent != 200 {
		t.Errorf("datagrams sent %d", st.DatagramsSent)
	}
}
