package quic

import (
	"context"
	"io"
	"testing"
	"time"
)

func BenchmarkStreamThroughput(b *testing.B) {
	p := newPair(b, testServerConfig(b), testClientConfig(b), nil)
	go func() {
		s, err := p.srvConn.AcceptStream(context.Background())
		if err != nil {
			return
		}
		io.Copy(io.Discard, s)
		s.Close()
	}()
	s, _ := p.cliConn.OpenStream()
	chunk := make([]byte, 64<<10)
	b.SetBytes(int64(len(chunk)))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, err := s.Write(chunk); err != nil {
			b.Fatal(err)
		}
	}
	s.Close()
	s.SetReadDeadline(time.Now().Add(30 * time.Second))
	io.ReadAll(s)
}

func BenchmarkDatagramRoundTrip(b *testing.B) {
	p := newPair(b, testServerConfig(b), testClientConfig(b), nil)
	ctx := context.Background()
	go func() {
		for {
			d, err := p.srvConn.ReceiveDatagram(ctx)
			if err != nil {
				return
			}
			p.srvConn.SendDatagram(d)
		}
	}()
	payload := make([]byte, 1000)
	b.SetBytes(int64(len(payload)))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		p.cliConn.SendDatagram(payload)
		c, cancel := context.WithTimeout(ctx, 200*time.Millisecond)
		p.cliConn.ReceiveDatagram(c)
		cancel()
	}
}
