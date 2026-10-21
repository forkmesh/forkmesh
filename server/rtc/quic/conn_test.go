package quic

import (
	"bytes"
	"context"
	"crypto/rand"
	"io"
	"testing"
	"time"
)

func echoStreams(t testing.TB, c *Conn) {
	go func() {
		for {
			s, err := c.AcceptStream(context.Background())
			if err != nil {
				return
			}
			go func() {
				io.Copy(s, s)
				s.Close()
			}()
		}
	}()
}

func TestHandshakeAndStreamEcho(t *testing.T) {
	p := newPair(t, testServerConfig(t), testClientConfig(t), nil)
	if got := p.cliConn.ConnectionState().NegotiatedProtocol; got != "fmtest" {
		t.Fatalf("alpn %q", got)
	}
	echoStreams(t, p.srvConn)
	s, err := p.cliConn.OpenStream()
	if err != nil {
		t.Fatal(err)
	}
	msg := make([]byte, 300000)
	rand.Read(msg)
	go func() {
		s.Write(msg)
		s.Close()
	}()
	s.SetReadDeadline(time.Now().Add(10 * time.Second))
	got, err := io.ReadAll(s)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, msg) {
		t.Fatalf("echo mismatch: got %d bytes", len(got))
	}
}

func TestDatagramsBothWays(t *testing.T) {
	p := newPair(t, testServerConfig(t), testClientConfig(t), nil)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	go func() {
		for {
			d, err := p.srvConn.ReceiveDatagram(ctx)
			if err != nil {
				return
			}
			p.srvConn.SendDatagram(append([]byte("echo:"), d...))
		}
	}()
	for i := 0; i < 20; i++ {
		if err := p.cliConn.SendDatagram([]byte{byte(i), 1, 2, 3}); err != nil {
			t.Fatal(err)
		}
	}
	seen := 0
	for seen < 15 {
		d, err := p.cliConn.ReceiveDatagram(ctx)
		if err != nil {
			t.Fatalf("after %d datagrams: %v", seen, err)
		}
		if !bytes.HasPrefix(d, []byte("echo:")) {
			t.Fatalf("bad datagram %q", d)
		}
		seen++
	}
	if max := p.cliConn.MaxDatagramSize(); max < 1000 {
		t.Fatalf("max datagram %d", max)
	}
	if err := p.cliConn.SendDatagram(make([]byte, 5000)); err != ErrDatagramTooLarge {
		t.Fatalf("oversized datagram: %v", err)
	}
}
