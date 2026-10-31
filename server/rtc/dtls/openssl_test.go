package dtls

import (
	"bytes"
	"context"
	"crypto"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/hex"
	"encoding/pem"
	"errors"
	"math/big"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

func selfSigned(t testing.TB, key crypto.Signer) []byte {
	t.Helper()
	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(42),
		Subject:      pkix.Name{CommonName: "test-client"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(time.Hour),
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, key.Public(), key)
	if err != nil {
		t.Fatal(err)
	}
	return der
}

func writePEM(t *testing.T, dir string, der []byte, key crypto.Signer) (string, string) {
	t.Helper()
	pk, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	certPath := filepath.Join(dir, "client.pem")
	keyPath := filepath.Join(dir, "client.key")
	if err := os.WriteFile(certPath, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyPath, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: pk}), 0o600); err != nil {
		t.Fatal(err)
	}
	return certPath, keyPath
}

type udpServer struct {
	conn    *net.UDPConn
	cfg     *Config
	mtu     int
	mu      sync.Mutex
	server  *Server
	maxSent int
	sent    int
	done    chan struct{}
}

func startUDPServer(t *testing.T, cfg *Config, mtu int) *udpServer {
	t.Helper()
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	u := &udpServer{conn: conn, cfg: cfg, mtu: mtu, done: make(chan struct{})}
	go u.readLoop()
	go u.tickLoop()
	t.Cleanup(func() {
		conn.Close()
		<-u.done
	})
	return u
}

func (u *udpServer) get() *Server {
	u.mu.Lock()
	defer u.mu.Unlock()
	return u.server
}

func (u *udpServer) readLoop() {
	defer close(u.done)
	buf := make([]byte, 65536)
	for {
		n, addr, err := u.conn.ReadFromUDP(buf)
		if err != nil {
			return
		}
		u.mu.Lock()
		if u.server == nil {
			peer := addr
			u.server = NewServer(u.cfg, addr.String(), func(b []byte) error {
				u.mu.Lock()
				if len(b) > u.maxSent {
					u.maxSent = len(b)
				}
				u.sent++
				u.mu.Unlock()
				_, err := u.conn.WriteToUDP(b, peer)
				return err
			})
			if u.mtu > 0 {
				u.server.mtu = u.mtu
			}
		}
		s := u.server
		u.mu.Unlock()
		s.HandleDatagram(append([]byte(nil), buf[:n]...), time.Now())
	}
}

func (u *udpServer) tickLoop() {
	t := time.NewTicker(50 * time.Millisecond)
	defer t.Stop()
	for {
		select {
		case <-u.done:
			return
		case now := <-t.C:
			if s := u.get(); s != nil {
				s.Tick(now)
			}
		}
	}
}

var keyingMaterialLine = regexp.MustCompile(`Keying material: ([0-9A-Fa-f]+)`)
var srtpLine = regexp.MustCompile(`SRTP Extension negotiated, profile=(\S+)`)

func TestOpenSSLInterop(t *testing.T) {
	bin, err := exec.LookPath("openssl")
	if err != nil {
		t.Skip("openssl not installed")
	}
	p256, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	p384, _ := ecdsa.GenerateKey(elliptic.P384(), rand.Reader)
	rsaKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name        string
		key         crypto.Signer
		profiles    []uint16
		wantProfile uint16
		wantName    string
		serverMTU   int
		clientMTU   int
	}{
		{"ecdsa-p256-gcm", p256, []uint16{0x0007, 0x0001}, 0x0007, "SRTP_AEAD_AES_128_GCM", 0, 0},
		{"ecdsa-p256-cm", p256, []uint16{0x0001, 0x0007}, 0x0001, "SRTP_AES128_CM_SHA1_80", 0, 0},
		{"ecdsa-p384", p384, []uint16{0x0007, 0x0001}, 0x0007, "SRTP_AEAD_AES_128_GCM", 0, 0},
		{"rsa-2048", rsaKey, []uint16{0x0007, 0x0001}, 0x0007, "SRTP_AEAD_AES_128_GCM", 0, 0},
		{"fragmented", p256, []uint16{0x0007, 0x0001}, 0x0007, "SRTP_AEAD_AES_128_GCM", 200, 300},
	}
	serverCert, err := GenerateCertificate()
	if err != nil {
		t.Fatal(err)
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			der := selfSigned(t, tc.key)
			certPath, keyPath := writePEM(t, dir, der, tc.key)
			var gotPeer []byte
			var peerMu sync.Mutex
			cfg := &Config{
				Certificate:  serverCert,
				SRTPProfiles: tc.profiles,
				CookieSecret: bytes.Repeat([]byte{7}, 32),
				VerifyPeer: func(d []byte) error {
					peerMu.Lock()
					defer peerMu.Unlock()
					gotPeer = d
					if !bytes.Equal(d, der) {
						return errors.New("unexpected client certificate")
					}
					return nil
				},
			}
			u := startUDPServer(t, cfg, tc.serverMTU)
			args := []string{"s_client", "-dtls1_2",
				"-connect", u.conn.LocalAddr().String(),
				"-cipher", "ECDHE-ECDSA-AES128-GCM-SHA256",
				"-use_srtp", "SRTP_AEAD_AES_128_GCM:SRTP_AES128_CM_SHA1_80",
				"-cert", certPath, "-key", keyPath,
				"-keymatexport", "EXTRACTOR-dtls_srtp", "-keymatexportlen", "60",
			}
			if tc.clientMTU > 0 {
				args = append(args, "-mtu", strconv.Itoa(tc.clientMTU))
			}
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()
			cmd := exec.CommandContext(ctx, bin, args...)
			cmd.Dir = dir
			cmd.Stdin = strings.NewReader("")
			cmd.Env = append(os.Environ(), "HOME="+dir)
			out, err := cmd.CombinedOutput()
			if err != nil && ctx.Err() != nil {
				t.Fatalf("openssl timed out: %s", out)
			}
			s := u.get()
			if s == nil {
				t.Fatalf("server never saw a datagram: %s", out)
			}
			if !s.Done() {
				t.Fatalf("handshake not done (failed: %v): %s", s.Failed(), out)
			}
			m := keyingMaterialLine.FindSubmatch(out)
			if m == nil {
				t.Fatalf("no keying material in output: %s", out)
			}
			theirs, err := hex.DecodeString(string(m[1]))
			if err != nil {
				t.Fatal(err)
			}
			ours, err := s.ExportKeyingMaterial("EXTRACTOR-dtls_srtp", 60)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(ours, theirs) {
				t.Fatalf("keying material mismatch\nours   %x\ntheirs %x", ours, theirs)
			}
			pm := srtpLine.FindSubmatch(out)
			if pm == nil || string(pm[1]) != tc.wantName {
				t.Fatalf("openssl srtp profile %q, want %s", pm, tc.wantName)
			}
			if s.SRTPProfile() != tc.wantProfile {
				t.Fatalf("server profile %#x, want %#x", s.SRTPProfile(), tc.wantProfile)
			}
			peerMu.Lock()
			if !bytes.Equal(gotPeer, der) || !bytes.Equal(s.PeerCertificate(), der) {
				t.Fatal("peer certificate not passed through")
			}
			peerMu.Unlock()
			if tc.serverMTU > 0 {
				u.mu.Lock()
				maxSent, sent := u.maxSent, u.sent
				u.mu.Unlock()
				if maxSent > tc.serverMTU || sent < 6 {
					t.Fatalf("server sent %d datagrams, largest %d, mtu %d", sent, maxSent, tc.serverMTU)
				}
			}
			deadline := time.Now().Add(2 * time.Second)
			for s.Failed() == nil && time.Now().Before(deadline) {
				time.Sleep(10 * time.Millisecond)
			}
			if !errors.Is(s.Failed(), errClosed) {
				t.Fatalf("expected close_notify from openssl, got %v", s.Failed())
			}
		})
	}
}
