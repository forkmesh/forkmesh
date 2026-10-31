package webtransport

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"io"
	"net"
	"net/netip"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/h3"
	"forkmesh.com/mirror-node/rtc/quic"
)

type testServer struct {
	addr  netip.AddrPort
	pool  *x509.CertPool
	ended chan error
}

func startServer(t *testing.T, srv *Server) *testServer {
	cert, _ := shortLivedCert(t)
	ep, err := quic.Listen("udp4", "127.0.0.1:0", ServerQUICConfig(&quic.Config{
		TLSConfig: &tls.Config{Certificates: []tls.Certificate{cert}},
	}))
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(func() {
		cancel()
		ep.Close()
	})
	go srv.Serve(ctx, ep)
	leaf, err := x509.ParseCertificate(cert.Certificate[0])
	if err != nil {
		t.Fatal(err)
	}
	pool := x509.NewCertPool()
	pool.AddCert(leaf)
	return &testServer{addr: ep.LocalAddr().(*net.UDPAddr).AddrPort(), pool: pool}
}

func (ts *testServer) dial(t *testing.T, path, origin string) (*Session, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return Dial(ctx, ts.addr, "localhost", path, origin, &quic.Config{
		TLSConfig: &tls.Config{RootCAs: ts.pool, ServerName: "localhost"},
	})
}

func originCheck(r *h3.Request) int {
	if r.Path != "/call" {
		return 404
	}
	if r.Get("origin") != "https://forkmesh.com" {
		return 403
	}
	return 200
}

func TestGoClientSession(t *testing.T) {
	ended := make(chan error, 1)
	ts := startServer(t, &Server{
		Check: originCheck,
		Handler: func(sess *Session) {
			go func() {
				<-sess.Done()
				ended <- sess.Err()
			}()
			echoHandler(nil)(sess)
		},
	})
	sess, err := ts.dial(t, "/call", "https://forkmesh.com")
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if sess.MaxDatagramSize() < 1000 {
		t.Fatalf("max datagram %d", sess.MaxDatagramSize())
	}
	if err := sess.SendDatagram([]byte("frame")); err != nil {
		t.Fatal(err)
	}
	d, err := sess.ReceiveDatagram(ctx)
	if err != nil || string(d) != "echo:frame" {
		t.Fatalf("datagram %q %v", d, err)
	}
	st, err := sess.OpenStream(ctx)
	if err != nil {
		t.Fatal(err)
	}
	payload := bytes.Repeat([]byte("control "), 20000)
	go func() {
		st.Write(payload)
		st.Close()
	}()
	st.SetReadDeadline(time.Now().Add(10 * time.Second))
	got, err := io.ReadAll(st)
	if err != nil || !bytes.Equal(got, payload) {
		t.Fatalf("stream echo %d bytes %v", len(got), err)
	}
	u, err := sess.OpenUniStream(ctx)
	if err != nil {
		t.Fatal(err)
	}
	u.Write([]byte("ping"))
	u.Close()
	in, err := sess.AcceptUniStream(ctx)
	if err != nil {
		t.Fatal(err)
	}
	in.SetReadDeadline(time.Now().Add(5 * time.Second))
	if got, err := io.ReadAll(in); err != nil || string(got) != "server got: ping" {
		t.Fatalf("uni reply %q %v", got, err)
	}
	sb, err := sess.AcceptStream(ctx)
	if err != nil {
		t.Fatal(err)
	}
	sb.SetReadDeadline(time.Now().Add(5 * time.Second))
	if got, err := io.ReadAll(sb); err != nil || string(got) != "server bidi" {
		t.Fatalf("server stream %q %v", got, err)
	}
	sb.Write([]byte("go reply"))
	sb.Close()
	sess.CloseWithError(3, "bye")
	select {
	case err := <-ended:
		var se *SessionError
		if !errors.As(err, &se) || se.Code != 3 || se.Message != "bye" || !se.Remote {
			t.Fatalf("server saw %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("server did not see the session close")
	}
}

func TestSessionRejectedByCheck(t *testing.T) {
	ts := startServer(t, &Server{Check: originCheck, Handler: func(*Session) { t.Error("handler ran for a rejected session") }})
	_, err := ts.dial(t, "/call", "https://evil.example")
	var se *SessionError
	if !errors.As(err, &se) || se.Code != 403 {
		t.Fatalf("bad origin: %v", err)
	}
	_, err = ts.dial(t, "/other", "https://forkmesh.com")
	if !errors.As(err, &se) || se.Code != 404 {
		t.Fatalf("bad path: %v", err)
	}
}

func TestServerCloseReachesClient(t *testing.T) {
	ts := startServer(t, &Server{Handler: func(sess *Session) {
		time.Sleep(50 * time.Millisecond)
		sess.CloseWithError(9, "room ended")
	}})
	sess, err := ts.dial(t, "/", "")
	if err != nil {
		t.Fatal(err)
	}
	st, err := sess.OpenStream(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	select {
	case <-sess.Done():
	case <-time.After(5 * time.Second):
		t.Fatal("client did not see the close")
	}
	var se *SessionError
	if err := sess.Err(); !errors.As(err, &se) || se.Code != 9 || se.Message != "room ended" {
		t.Fatalf("client saw %v", err)
	}
	st.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := st.Read(make([]byte, 1)); err == nil {
		t.Fatal("stream survived its session")
	}
}

func TestErrorCodeMapping(t *testing.T) {
	for _, c := range []uint32{0, 1, 0x1d, 0x1e, 0x1f, 1000, 0xffffffff} {
		h := HTTP3ErrorCode(c)
		if (h-0x21)%0x1f == 0 {
			t.Fatalf("code %d mapped onto a reserved value", c)
		}
		back, ok := AppErrorCode(h)
		if !ok || back != c {
			t.Fatalf("code %d -> %x -> %d %v", c, h, back, ok)
		}
	}
	if HTTP3ErrorCode(0xffffffff) != lastAppCode {
		t.Fatal("last code")
	}
}
