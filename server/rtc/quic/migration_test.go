package quic

import (
	"bytes"
	"context"
	"crypto/rand"
	"io"
	"net"
	"net/netip"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func writeSlowly(s *Stream, msg []byte, chunk int, pause time.Duration, progress *atomic.Int64) {
	for off := 0; off < len(msg); off += chunk {
		end := off + chunk
		if end > len(msg) {
			end = len(msg)
		}
		if _, err := s.Write(msg[off:end]); err != nil {
			return
		}
		progress.Store(int64(end))
		time.Sleep(pause)
	}
	s.Close()
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestMigrationToNewLocalPortMidTransfer(t *testing.T) {
	p := newPair(t, testServerConfig(t), testClientConfig(t), nil)
	echoStreams(t, p.srvConn)
	waitFor(t, "spare connection ids", func() bool {
		p.cliConn.mu.Lock()
		defer p.cliConn.mu.Unlock()
		return p.cliConn.unusedPeerCID() != nil
	})
	oldDCID := append([]byte(nil), p.cliConn.dcid...)
	s, err := p.cliConn.OpenStream()
	if err != nil {
		t.Fatal(err)
	}
	msg := make([]byte, 600<<10)
	rand.Read(msg)
	var progress atomic.Int64
	go writeSlowly(s, msg, 8<<10, 2*time.Millisecond, &progress)
	done := make(chan []byte, 1)
	go func() {
		s.SetReadDeadline(time.Now().Add(30 * time.Second))
		got, _ := io.ReadAll(s)
		done <- got
	}()
	waitFor(t, "a third of the transfer", func() bool { return progress.Load() > int64(len(msg)/3) })
	fresh, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	if err := p.client.Rebind(fresh); err != nil {
		t.Fatal(err)
	}
	newAddr := fresh.LocalAddr().(*net.UDPAddr).AddrPort()
	if err := p.cliConn.SendDatagram([]byte("after move")); err != nil {
		t.Fatal(err)
	}
	got := <-done
	if !bytes.Equal(got, msg) {
		t.Fatalf("echo after migration: %d of %d bytes", len(got), len(msg))
	}
	waitFor(t, "server to adopt the new path", func() bool {
		return p.srvConn.RemoteAddr().(*net.UDPAddr).AddrPort() == newAddr
	})
	d, err := p.srvConn.ReceiveDatagram(ctxTimeout(t, 5*time.Second))
	if err != nil || string(d) != "after move" {
		t.Fatalf("datagram after migration %q %v", d, err)
	}
	st := p.srvConn.Stats()
	if st.Migrations < 1 || st.PathChallenges < 1 {
		t.Fatalf("server stats %+v", st)
	}
	p.cliConn.mu.Lock()
	newDCID := append([]byte(nil), p.cliConn.dcid...)
	p.cliConn.mu.Unlock()
	if bytes.Equal(oldDCID, newDCID) {
		t.Fatal("client kept its connection id across the move")
	}
}

func ctxTimeout(t *testing.T, d time.Duration) context.Context {
	ctx, cancel := context.WithTimeout(context.Background(), d)
	t.Cleanup(cancel)
	return ctx
}

type relay struct {
	front    *net.UDPConn
	server   netip.AddrPort
	mu       sync.Mutex
	back     *net.UDPConn
	client   netip.AddrPort
	spoof    *net.UDPConn
	spoofOne atomic.Bool
	closed   atomic.Bool
}

func newRelay(t *testing.T, server netip.AddrPort) *relay {
	front, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	r := &relay{front: front, server: server}
	r.back = r.listenBack(t)
	t.Cleanup(r.close)
	go func() {
		buf := make([]byte, 2048)
		for {
			n, from, err := front.ReadFromUDPAddrPort(buf)
			if err != nil {
				return
			}
			r.mu.Lock()
			r.client = from
			back := r.back
			spoof := r.spoof
			r.mu.Unlock()
			if spoof != nil && n > 100 && buf[0]&0x80 == 0 && r.spoofOne.CompareAndSwap(true, false) {
				spoof.WriteToUDPAddrPort(buf[:n], r.server)
				time.Sleep(2 * time.Millisecond)
			}
			back.WriteToUDPAddrPort(buf[:n], r.server)
		}
	}()
	return r
}

func (r *relay) listenBack(t *testing.T) *net.UDPConn {
	back, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	go func() {
		buf := make([]byte, 2048)
		for {
			n, _, err := back.ReadFromUDPAddrPort(buf)
			if err != nil {
				return
			}
			r.mu.Lock()
			client := r.client
			current := r.back == back
			r.mu.Unlock()
			if current {
				r.front.WriteToUDPAddrPort(buf[:n], client)
			}
		}
	}()
	return back
}

func (r *relay) rebindBack(t *testing.T) netip.AddrPort {
	nb := r.listenBack(t)
	r.mu.Lock()
	old := r.back
	r.back = nb
	r.mu.Unlock()
	time.AfterFunc(time.Second, func() { old.Close() })
	return nb.LocalAddr().(*net.UDPAddr).AddrPort()
}

func (r *relay) close() {
	r.front.Close()
	r.mu.Lock()
	r.back.Close()
	if r.spoof != nil {
		r.spoof.Close()
	}
	r.mu.Unlock()
}

func dialThrough(t *testing.T, srv *Endpoint, r *relay) (*Conn, *Conn) {
	cli, err := NewEndpoint(mustUDP(t), testClientConfig(t), false)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { cli.Close() })
	ctx := ctxTimeout(t, 10*time.Second)
	accepted := make(chan *Conn, 1)
	go func() {
		c, _ := srv.Accept(ctx)
		accepted <- c
	}()
	cc, err := cli.Dial(ctx, r.front.LocalAddr().(*net.UDPAddr).AddrPort(), testClientConfig(t))
	if err != nil {
		t.Fatal(err)
	}
	sc := <-accepted
	if sc == nil {
		t.Fatal("no server conn")
	}
	return cc, sc
}

func mustUDP(t *testing.T) *net.UDPConn {
	c, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func TestNATRebindingIsValidatedBeforeUse(t *testing.T) {
	srv, err := Listen("udp4", "127.0.0.1:0", testServerConfig(t))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { srv.Close() })
	r := newRelay(t, srv.LocalAddr().(*net.UDPAddr).AddrPort())
	cc, sc := dialThrough(t, srv, r)
	echoStreams(t, sc)
	s, err := cc.OpenStream()
	if err != nil {
		t.Fatal(err)
	}
	msg := make([]byte, 400<<10)
	rand.Read(msg)
	var progress atomic.Int64
	go writeSlowly(s, msg, 8<<10, 2*time.Millisecond, &progress)
	done := make(chan []byte, 1)
	go func() {
		s.SetReadDeadline(time.Now().Add(30 * time.Second))
		got, _ := io.ReadAll(s)
		done <- got
	}()
	waitFor(t, "a third of the transfer", func() bool { return progress.Load() > int64(len(msg)/3) })
	newAddr := r.rebindBack(t)
	got := <-done
	if !bytes.Equal(got, msg) {
		t.Fatalf("echo across rebinding: %d of %d bytes", len(got), len(msg))
	}
	waitFor(t, "server to adopt the rebound path", func() bool {
		return sc.RemoteAddr().(*net.UDPAddr).AddrPort() == newAddr
	})
	if st := sc.Stats(); st.PathChallenges < 1 {
		t.Fatalf("no path challenge before migrating: %+v", st)
	}
}

func TestSpoofedAddressDoesNotStealPath(t *testing.T) {
	srv, err := Listen("udp4", "127.0.0.1:0", testServerConfig(t))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { srv.Close() })
	r := newRelay(t, srv.LocalAddr().(*net.UDPAddr).AddrPort())
	cc, sc := dialThrough(t, srv, r)
	echoStreams(t, sc)
	original := sc.RemoteAddr().(*net.UDPAddr).AddrPort()
	spoof := mustUDP(t)
	go func() {
		buf := make([]byte, 2048)
		for {
			if _, _, err := spoof.ReadFromUDPAddrPort(buf); err != nil {
				return
			}
		}
	}()
	r.mu.Lock()
	r.spoof = spoof
	r.mu.Unlock()
	s, err := cc.OpenStream()
	if err != nil {
		t.Fatal(err)
	}
	msg := make([]byte, 300<<10)
	rand.Read(msg)
	var progress atomic.Int64
	go writeSlowly(s, msg, 8<<10, 3*time.Millisecond, &progress)
	done := make(chan []byte, 1)
	go func() {
		s.SetReadDeadline(time.Now().Add(30 * time.Second))
		got, _ := io.ReadAll(s)
		done <- got
	}()
	waitFor(t, "some progress", func() bool { return progress.Load() > int64(len(msg)/4) })
	r.spoofOne.Store(true)
	got := <-done
	if !bytes.Equal(got, msg) {
		t.Fatalf("echo with spoofed copy: %d of %d bytes", len(got), len(msg))
	}
	if r.spoofOne.Load() {
		t.Fatal("spoofed packet was never sent")
	}
	time.Sleep(100 * time.Millisecond)
	if now := sc.RemoteAddr().(*net.UDPAddr).AddrPort(); now != original {
		t.Fatalf("server moved to %v after a spoofed packet", now)
	}
	if st := sc.Stats(); st.PathChallenges < 1 {
		t.Fatalf("spoofed address was not challenged: %+v", st)
	}
}
