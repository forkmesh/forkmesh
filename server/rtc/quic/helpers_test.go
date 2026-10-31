package quic

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"math/big"
	"net"
	"net/netip"
	"sync"
	"testing"
	"time"
)

var (
	certOnce sync.Once
	certVal  tls.Certificate
	certPool *x509.CertPool
)

func testCertificate(t testing.TB) (tls.Certificate, *x509.CertPool) {
	certOnce.Do(func() {
		key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			panic(err)
		}
		tmpl := &x509.Certificate{
			SerialNumber: big.NewInt(1),
			Subject:      pkix.Name{CommonName: "localhost"},
			NotBefore:    time.Now().Add(-time.Hour),
			NotAfter:     time.Now().Add(10 * 24 * time.Hour),
			DNSNames:     []string{"localhost"},
			IPAddresses:  []net.IP{net.ParseIP("127.0.0.1"), net.ParseIP("::1")},
			KeyUsage:     x509.KeyUsageDigitalSignature,
			ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		}
		der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
		if err != nil {
			panic(err)
		}
		leaf, _ := x509.ParseCertificate(der)
		certVal = tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key, Leaf: leaf}
		certPool = x509.NewCertPool()
		certPool.AddCert(leaf)
	})
	return certVal, certPool
}

func testServerConfig(t testing.TB) *Config {
	cert, _ := testCertificate(t)
	return &Config{
		TLSConfig:            &tls.Config{Certificates: []tls.Certificate{cert}, NextProtos: []string{"fmtest"}},
		MaxDatagramFrameSize: 1500,
		MaxIdleTimeout:       5 * time.Second,
	}
}

func testClientConfig(t testing.TB) *Config {
	_, pool := testCertificate(t)
	return &Config{
		TLSConfig:            &tls.Config{RootCAs: pool, NextProtos: []string{"fmtest"}, ServerName: "localhost"},
		MaxDatagramFrameSize: 1500,
		MaxIdleTimeout:       5 * time.Second,
	}
}

type pair struct {
	server  *Endpoint
	client  *Endpoint
	srvConn *Conn
	cliConn *Conn
	srvAddr netip.AddrPort
	cliSock net.PacketConn
}

func newPair(t testing.TB, sconf, cconf *Config, wrapClient func(net.PacketConn) net.PacketConn) *pair {
	t.Helper()
	srv, err := Listen("udp4", "127.0.0.1:0", sconf)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { srv.Close() })
	raw, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	var pc net.PacketConn = raw
	if wrapClient != nil {
		pc = wrapClient(raw)
	}
	cli, err := NewEndpoint(pc, cconf, false)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { cli.Close() })
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	addr := srv.LocalAddr().(*net.UDPAddr).AddrPort()
	type res struct {
		c   *Conn
		err error
	}
	accepted := make(chan res, 1)
	go func() {
		c, err := srv.Accept(ctx)
		accepted <- res{c, err}
	}()
	cc, err := cli.Dial(ctx, addr, cconf)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	r := <-accepted
	if r.err != nil {
		t.Fatalf("accept: %v", r.err)
	}
	return &pair{server: srv, client: cli, srvConn: r.c, cliConn: cc, srvAddr: addr, cliSock: pc}
}

type lossyConn struct {
	net.PacketConn
	mu       sync.Mutex
	dropOut  func(n int, b []byte) bool
	dropIn   func(n int, b []byte) bool
	outCount int
	inCount  int
}

func (l *lossyConn) WriteTo(b []byte, addr net.Addr) (int, error) {
	l.mu.Lock()
	l.outCount++
	drop := l.dropOut != nil && l.dropOut(l.outCount, b)
	l.mu.Unlock()
	if drop {
		return len(b), nil
	}
	return l.PacketConn.WriteTo(b, addr)
}

func (l *lossyConn) ReadFrom(b []byte) (int, net.Addr, error) {
	for {
		n, addr, err := l.PacketConn.ReadFrom(b)
		if err != nil {
			return n, addr, err
		}
		l.mu.Lock()
		l.inCount++
		drop := l.dropIn != nil && l.dropIn(l.inCount, b[:n])
		l.mu.Unlock()
		if !drop {
			return n, addr, err
		}
	}
}
