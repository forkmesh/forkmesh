package quic

import (
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/asn1"
	"encoding/binary"
	"errors"
	"io"
	"math/big"
	"net"
	"net/netip"
	"testing"
	"time"
)

func TestRetryValidatesAddress(t *testing.T) {
	sconf := testServerConfig(t)
	sconf.RequireAddressValidation = true
	p := newPair(t, sconf, testClientConfig(t), nil)
	p.cliConn.mu.Lock()
	retried := p.cliConn.retried
	p.cliConn.mu.Unlock()
	p.srvConn.mu.Lock()
	srvRetried := p.srvConn.retried
	p.srvConn.mu.Unlock()
	if !retried || !srvRetried {
		t.Fatalf("retry not used: client %v server %v", retried, srvRetried)
	}
	echoStreams(t, p.srvConn)
	s, _ := p.cliConn.OpenStream()
	s.Write([]byte("validated"))
	s.Close()
	s.SetReadDeadline(time.Now().Add(5 * time.Second))
	got, err := io.ReadAll(s)
	if err != nil || string(got) != "validated" {
		t.Fatalf("echo %q %v", got, err)
	}
}

func TestRetryTokenBoundToAddressAndTime(t *testing.T) {
	ts := newTokenSealer([]byte("key"))
	a := netip.MustParseAddrPort("192.0.2.1:4433")
	b := netip.MustParseAddrPort("192.0.2.2:4433")
	now := time.Now()
	tok := ts.retryToken(a, []byte("odcid123"), []byte("rscid123"), now)
	if o, r, ok := ts.openRetryToken(tok, a, now); !ok || string(o) != "odcid123" || string(r) != "rscid123" {
		t.Fatal("valid token rejected")
	}
	if _, _, ok := ts.openRetryToken(tok, b, now); ok {
		t.Fatal("token accepted from another address")
	}
	if _, _, ok := ts.openRetryToken(tok, a, now.Add(retryTokenLifetime+time.Second)); ok {
		t.Fatal("expired token accepted")
	}
	tok[len(tok)-1] ^= 1
	if _, _, ok := ts.openRetryToken(tok, a, now); ok {
		t.Fatal("tampered token accepted")
	}
	if _, _, ok := newTokenSealer([]byte("other")).openRetryToken(ts.retryToken(a, nil, nil, now), a, now); ok {
		t.Fatal("token from another key accepted")
	}
}

func TestVersionNegotiation(t *testing.T) {
	srv, err := Listen("udp4", "127.0.0.1:0", testServerConfig(t))
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	c := mustUDP(t)
	defer c.Close()
	pkt := []byte{0xc0}
	pkt = binary.BigEndian.AppendUint32(pkt, 0x1a2a3a4a)
	dcid := []byte{1, 2, 3, 4, 5, 6, 7, 8}
	scid := []byte{9, 9, 9, 9}
	pkt = append(pkt, byte(len(dcid)))
	pkt = append(pkt, dcid...)
	pkt = append(pkt, byte(len(scid)))
	pkt = append(pkt, scid...)
	pkt = pad(1200, pkt)
	c.WriteToUDPAddrPort(pkt, srv.LocalAddr().(*net.UDPAddr).AddrPort())
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, _, err := c.ReadFromUDPAddrPort(buf)
	if err != nil {
		t.Fatal(err)
	}
	gd, gs, versions, ok := parseVersionNegotiation(buf[:n])
	if !ok || !bytes.Equal(gd, scid) || !bytes.Equal(gs, dcid) {
		t.Fatalf("bad version negotiation %x", buf[:n])
	}
	found := false
	for _, v := range versions {
		if v == Version1 {
			found = true
		}
	}
	if !found {
		t.Fatalf("versions %x", versions)
	}
	c.WriteToUDPAddrPort(pkt[:600], srv.LocalAddr().(*net.UDPAddr).AddrPort())
	c.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	if _, _, err := c.ReadFromUDPAddrPort(buf); err == nil {
		t.Fatal("version negotiation sent for a short datagram")
	}
}

func bigCertificate(t *testing.T) tls.Certificate {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	filler := make([]byte, 12000)
	rand.Read(filler)
	tmpl := &x509.Certificate{
		SerialNumber:    big.NewInt(2),
		Subject:         pkix.Name{CommonName: "localhost"},
		NotBefore:       time.Now().Add(-time.Hour),
		NotAfter:        time.Now().Add(24 * time.Hour),
		DNSNames:        []string{"localhost"},
		ExtraExtensions: []pkix.Extension{{Id: asn1.ObjectIdentifier{1, 3, 6, 1, 4, 1, 99999, 1}, Value: filler}},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}
}

func TestAntiAmplificationLimit(t *testing.T) {
	sconf := testServerConfig(t)
	sconf.TLSConfig.Certificates = []tls.Certificate{bigCertificate(t)}
	srv, err := Listen("udp4", "127.0.0.1:0", sconf)
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	var sent, received, datagrams int
	wrap := &lossyConn{PacketConn: mustUDP(t)}
	wrap.dropOut = func(n int, b []byte) bool {
		if received == 0 {
			sent += len(b)
			return false
		}
		return true
	}
	wrap.dropIn = func(n int, b []byte) bool {
		received += len(b)
		datagrams++
		return true
	}
	cli, err := NewEndpoint(wrap, testClientConfig(t), false)
	if err != nil {
		t.Fatal(err)
	}
	defer cli.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 4*time.Second)
	defer cancel()
	if _, err := cli.Dial(ctx, srv.LocalAddr().(*net.UDPAddr).AddrPort(), testClientConfig(t)); err == nil {
		t.Fatal("handshake completed without client packets")
	}
	wrap.mu.Lock()
	defer wrap.mu.Unlock()
	if sent < 1200 {
		t.Fatalf("client initial was %d bytes", sent)
	}
	if received > 3*sent {
		t.Fatalf("server sent %d bytes for %d received before validation", received, sent)
	}
	if received < 3*sent-1200 {
		t.Fatalf("server sent only %d of its %d byte allowance", received, 3*sent)
	}
	t.Logf("server sent %d bytes in %d datagrams in reply to %d", received, datagrams, sent)
}

func TestStatelessReset(t *testing.T) {
	sconf := testServerConfig(t)
	sconf.StatelessResetKey = []byte("fixed reset key for the test....")
	p := newPair(t, sconf, testClientConfig(t), nil)
	p.server.mu.Lock()
	for k, c := range p.server.conns {
		if c == p.srvConn {
			delete(p.server.conns, k)
		}
	}
	p.server.mu.Unlock()
	s, _ := p.cliConn.OpenStream()
	s.Write(make([]byte, 200))
	select {
	case <-p.cliConn.Done():
	case <-time.After(5 * time.Second):
		t.Fatal("client did not see the stateless reset")
	}
	if err := p.cliConn.Err(); !errors.Is(err, ErrStatelessReset) {
		t.Fatalf("client closed with %v", err)
	}
}

func TestKeyUpdates(t *testing.T) {
	sconf, cconf := testServerConfig(t), testClientConfig(t)
	sconf.KeyUpdateInterval = 40
	cconf.KeyUpdateInterval = 25
	p := newPair(t, sconf, cconf, nil)
	echoStreams(t, p.srvConn)
	msg := make([]byte, 500<<10)
	rand.Read(msg)
	s, _ := p.cliConn.OpenStream()
	go func() {
		s.Write(msg)
		s.Close()
	}()
	s.SetReadDeadline(time.Now().Add(20 * time.Second))
	got, err := io.ReadAll(s)
	if err != nil || !bytes.Equal(got, msg) {
		t.Fatalf("echo across key updates: %d bytes, %v", len(got), err)
	}
	cs, ss := p.cliConn.Stats(), p.srvConn.Stats()
	if cs.KeyUpdates < 4 || ss.KeyUpdates < 4 {
		t.Fatalf("key updates: client %d server %d", cs.KeyUpdates, ss.KeyUpdates)
	}
}

func TestIdleTimeoutAndKeepAlive(t *testing.T) {
	sconf, cconf := testServerConfig(t), testClientConfig(t)
	sconf.MaxIdleTimeout = 300 * time.Millisecond
	cconf.MaxIdleTimeout = 300 * time.Millisecond
	p := newPair(t, sconf, cconf, nil)
	select {
	case <-p.cliConn.Done():
	case <-time.After(3 * time.Second):
		t.Fatal("idle connection stayed open")
	}
	if err := p.cliConn.Err(); err != ErrIdleTimeout {
		t.Fatalf("client error %v", err)
	}
	select {
	case <-p.srvConn.Done():
	case <-time.After(3 * time.Second):
		t.Fatal("idle server connection stayed open")
	}
	cconf.KeepAlivePeriod = 80 * time.Millisecond
	q := newPair(t, sconf, cconf, nil)
	select {
	case <-q.cliConn.Done():
		t.Fatalf("keep-alive connection closed: %v", q.cliConn.Err())
	case <-time.After(1200 * time.Millisecond):
	}
}

func TestApplicationClosePropagates(t *testing.T) {
	p := newPair(t, testServerConfig(t), testClientConfig(t), nil)
	p.cliConn.CloseWithError(42, "bye")
	select {
	case <-p.srvConn.Done():
	case <-time.After(3 * time.Second):
		t.Fatal("server did not see the close")
	}
	var ae *ApplicationError
	if err := p.srvConn.Err(); !errors.As(err, &ae) || ae.Code != 42 || ae.Reason != "bye" || !ae.Remote {
		t.Fatalf("server error %v", err)
	}
	if _, err := p.cliConn.OpenStream(); err == nil {
		t.Fatal("open stream on closed conn")
	}
}

func TestStreamLimitsAndCredit(t *testing.T) {
	sconf := testServerConfig(t)
	sconf.MaxIncomingStreams = 2
	p := newPair(t, sconf, testClientConfig(t), nil)
	echoStreams(t, p.srvConn)
	var streams []*Stream
	for i := 0; i < 2; i++ {
		s, err := p.cliConn.OpenStream()
		if err != nil {
			t.Fatal(err)
		}
		streams = append(streams, s)
	}
	if _, err := p.cliConn.OpenStream(); err != ErrStreamLimit {
		t.Fatalf("third stream: %v", err)
	}
	opened := make(chan *Stream, 1)
	go func() {
		s, err := p.cliConn.OpenStreamSync(ctxTimeout(t, 5*time.Second))
		if err != nil {
			t.Error(err)
		}
		opened <- s
	}()
	streams[0].Write([]byte("one"))
	streams[0].Close()
	streams[0].SetReadDeadline(time.Now().Add(5 * time.Second))
	if got, err := io.ReadAll(streams[0]); err != nil || string(got) != "one" {
		t.Fatalf("first stream %q %v", got, err)
	}
	select {
	case s := <-opened:
		if s == nil {
			t.Fatal("no stream")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("stream credit not returned")
	}
}

func TestSmallFlowControlWindows(t *testing.T) {
	sconf, cconf := testServerConfig(t), testClientConfig(t)
	sconf.StreamReceiveWindow, sconf.ConnectionReceiveWindow = 4096, 8192
	cconf.StreamReceiveWindow, cconf.ConnectionReceiveWindow = 4096, 8192
	p := newPair(t, sconf, cconf, nil)
	echoStreams(t, p.srvConn)
	msg := make([]byte, 200<<10)
	rand.Read(msg)
	s, _ := p.cliConn.OpenStream()
	go func() {
		s.Write(msg)
		s.Close()
	}()
	s.SetReadDeadline(time.Now().Add(20 * time.Second))
	got, err := io.ReadAll(s)
	if err != nil || !bytes.Equal(got, msg) {
		t.Fatalf("echo with small windows: %d bytes %v", len(got), err)
	}
}

func TestResetAndStopSending(t *testing.T) {
	p := newPair(t, testServerConfig(t), testClientConfig(t), nil)
	ctx := ctxTimeout(t, 5*time.Second)
	s, _ := p.cliConn.OpenStream()
	s.Write([]byte("partial"))
	ss, err := p.srvConn.AcceptStream(ctx)
	if err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 7)
	ss.SetReadDeadline(time.Now().Add(5 * time.Second))
	if _, err := io.ReadFull(ss, buf); err != nil {
		t.Fatal(err)
	}
	s.CancelWrite(5)
	var se *StreamError
	_, err = ss.Read(buf)
	if !errors.As(err, &se) || se.Code != 5 || !se.Remote {
		t.Fatalf("server read after reset: %v", err)
	}
	ss.CancelRead(9)
	s2, _ := p.cliConn.OpenStream()
	s2.Write([]byte("x"))
	ss2, err := p.srvConn.AcceptStream(ctx)
	if err != nil {
		t.Fatal(err)
	}
	ss2.CancelRead(6)
	deadline := time.Now().Add(5 * time.Second)
	for {
		_, err := s2.Write([]byte("more"))
		if errors.As(err, &se) && se.Code == 6 && se.Remote {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("writer never saw STOP_SENDING: %v", err)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestUnidirectionalStreams(t *testing.T) {
	p := newPair(t, testServerConfig(t), testClientConfig(t), nil)
	ctx := ctxTimeout(t, 5*time.Second)
	u, err := p.cliConn.OpenUniStream()
	if err != nil {
		t.Fatal(err)
	}
	u.Write([]byte("one way"))
	u.Close()
	if _, err := u.Read(make([]byte, 1)); err != ErrNotSupported {
		t.Fatalf("read on send-only stream: %v", err)
	}
	r, err := p.srvConn.AcceptUniStream(ctx)
	if err != nil {
		t.Fatal(err)
	}
	r.SetReadDeadline(time.Now().Add(5 * time.Second))
	got, err := io.ReadAll(r)
	if err != nil || string(got) != "one way" {
		t.Fatalf("uni %q %v", got, err)
	}
	if _, err := r.Write([]byte("x")); err != ErrNotSupported {
		t.Fatalf("write on receive-only stream: %v", err)
	}
	back, err := p.srvConn.OpenUniStream()
	if err != nil {
		t.Fatal(err)
	}
	back.Write([]byte("reply"))
	back.Close()
	cr, err := p.cliConn.AcceptUniStream(ctx)
	if err != nil {
		t.Fatal(err)
	}
	cr.SetReadDeadline(time.Now().Add(5 * time.Second))
	if got, err := io.ReadAll(cr); err != nil || string(got) != "reply" {
		t.Fatalf("server uni %q %v", got, err)
	}
}

func TestEndpointCaps(t *testing.T) {
	sconf := testServerConfig(t)
	sconf.MaxConnectionsPerIP = 2
	srv, err := Listen("udp4", "127.0.0.1:0", sconf)
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	go func() {
		for {
			if _, err := srv.Accept(context.Background()); err != nil {
				return
			}
		}
	}()
	addr := srv.LocalAddr().(*net.UDPAddr).AddrPort()
	ok := 0
	for i := 0; i < 3; i++ {
		ctx, cancel := context.WithTimeout(context.Background(), 1500*time.Millisecond)
		c, err := Dial(ctx, "udp4", addr.String(), testClientConfig(t))
		cancel()
		if err == nil {
			ok++
			defer c.Close()
		}
	}
	if ok != 2 {
		t.Fatalf("%d connections from one address, cap is 2", ok)
	}
}
