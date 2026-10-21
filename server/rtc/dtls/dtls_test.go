package dtls

import (
	"bytes"
	"crypto"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/hex"
	"errors"
	mrand "math/rand"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"
)

const testRemote = "198.51.100.7:5000"

var (
	serverCertOnce sync.Once
	serverCertVal  *Certificate
	clientKeyVal   *ecdsa.PrivateKey
	rsaKeyOnce     sync.Once
	rsaKeyVal      *rsa.PrivateKey
)

func testServerCert(t testing.TB) *Certificate {
	serverCertOnce.Do(func() {
		var err error
		serverCertVal, err = GenerateCertificate()
		if err != nil {
			panic(err)
		}
		clientKeyVal, _ = ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	})
	return serverCertVal
}

func testRSAKey() *rsa.PrivateKey {
	rsaKeyOnce.Do(func() {
		k, err := rsa.GenerateKey(rand.Reader, 2048)
		if err != nil {
			panic(err)
		}
		rsaKeyVal = k
	})
	return rsaKeyVal
}

type harness struct {
	t         testing.TB
	cfg       *Config
	s         *Server
	c         *testClient
	now       time.Time
	toClient  [][]byte
	peerDER   [][]byte
	verifyErr error
}

func newHarness(t testing.TB, opts clientOptions, mutate func(*Config)) *harness {
	t.Helper()
	cert := testServerCert(t)
	if opts.key == nil {
		opts.key = clientKeyVal
	}
	if opts.certDER == nil {
		opts.certDER = selfSigned(t, opts.key)
	}
	if opts.sigAlg == 0 {
		opts.sigAlg = sigECDSASecp256r1SHA256
	}
	h := &harness{t: t, now: time.Unix(1700000000, 0)}
	h.cfg = &Config{
		Certificate:  cert,
		SRTPProfiles: []uint16{0x0007, 0x0001},
		CookieSecret: bytes.Repeat([]byte{0x5a}, 32),
		VerifyPeer: func(der []byte) error {
			h.peerDER = append(h.peerDER, der)
			return h.verifyErr
		},
	}
	if mutate != nil {
		mutate(h.cfg)
	}
	h.c = newTestClient(opts)
	h.s = NewServer(h.cfg, testRemote, func(b []byte) error {
		h.toClient = append(h.toClient, append([]byte(nil), b...))
		return nil
	})
	return h
}

func (h *harness) toServer(ds [][]byte) error {
	var last error
	for _, d := range ds {
		if err := h.s.HandleDatagram(d, h.now); err != nil {
			last = err
		}
	}
	return last
}

func (h *harness) take() [][]byte {
	out := h.toClient
	h.toClient = nil
	return out
}

func (h *harness) toClientAll(ds [][]byte) [][]byte {
	h.t.Helper()
	var out [][]byte
	for _, d := range ds {
		more, err := h.c.handle(d)
		if err != nil {
			h.t.Fatalf("client: %v", err)
		}
		out = append(out, more...)
	}
	return out
}

func (h *harness) run(out [][]byte) {
	h.t.Helper()
	for round := 0; round < 20; round++ {
		h.toServer(out)
		in := h.take()
		if len(in) == 0 {
			return
		}
		out = h.toClientAll(in)
	}
	h.t.Fatal("handshake did not settle")
}

func (h *harness) handshake() {
	h.t.Helper()
	h.run(h.c.start())
	h.checkDone()
}

func (h *harness) checkDone() {
	h.t.Helper()
	if err := h.s.Failed(); err != nil {
		h.t.Fatalf("server failed: %v", err)
	}
	if !h.s.Done() || !h.c.done {
		h.t.Fatalf("handshake incomplete: server %v client %v", h.s.Done(), h.c.done)
	}
	ours, err := h.s.ExportKeyingMaterial("EXTRACTOR-dtls_srtp", 60)
	if err != nil {
		h.t.Fatal(err)
	}
	if !bytes.Equal(ours, h.c.export("EXTRACTOR-dtls_srtp", 60)) {
		h.t.Fatal("exported keying material differs")
	}
	if h.s.SRTPProfile() != h.c.profile {
		h.t.Fatalf("profile server %#x client %#x", h.s.SRTPProfile(), h.c.profile)
	}
	if !bytes.Equal(h.s.PeerCertificate(), h.c.opts.certDER) {
		h.t.Fatal("peer certificate mismatch")
	}
	if FingerprintOf(h.c.serverCert) != h.cfg.Certificate.Fingerprint {
		h.t.Fatal("server certificate fingerprint mismatch")
	}
}

func (h *harness) expectFailure(desc uint8) {
	h.t.Helper()
	if h.s.Failed() == nil {
		h.t.Fatal("server did not fail")
	}
	var ae *alertError
	if !errors.As(h.s.Failed(), &ae) || ae.desc != desc {
		h.t.Fatalf("failure %v, want alert %d", h.s.Failed(), desc)
	}
	in := h.take()
	found := false
	for _, d := range in {
		h.c.handle(d)
	}
	for _, a := range h.c.alerts {
		if a == desc {
			found = true
		}
	}
	if !found {
		h.t.Fatalf("client saw alerts %v, want %d", h.c.alerts, desc)
	}
}

func TestHandshake(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	h.handshake()
	if h.s.SRTPProfile() != 0x0007 {
		t.Fatalf("profile %#x", h.s.SRTPProfile())
	}
	if len(h.peerDER) != 1 || !bytes.Equal(h.peerDER[0], h.c.opts.certDER) {
		t.Fatal("VerifyPeer did not get the client certificate")
	}
	if h.c.hvrs != 1 || h.c.serverHellos != 1 {
		t.Fatalf("hvrs %d server hellos %d", h.c.hvrs, h.c.serverHellos)
	}
	if !h.c.serverCCS {
		t.Fatal("no change_cipher_spec from server")
	}
}

func TestProfilePreference(t *testing.T) {
	h := newHarness(t, clientOptions{srtpProfiles: []uint16{0x0007, 0x0001}}, func(c *Config) {
		c.SRTPProfiles = []uint16{0x0001, 0x0007}
	})
	h.handshake()
	if h.s.SRTPProfile() != 0x0001 {
		t.Fatalf("profile %#x", h.s.SRTPProfile())
	}
	h = newHarness(t, clientOptions{srtpProfiles: []uint16{0x0002, 0x0001}}, nil)
	h.handshake()
	if h.s.SRTPProfile() != 0x0001 {
		t.Fatalf("profile %#x", h.s.SRTPProfile())
	}
}

func TestClientKeyTypes(t *testing.T) {
	p384, _ := ecdsa.GenerateKey(elliptic.P384(), rand.Reader)
	cases := []struct {
		name string
		key  crypto.Signer
		alg  uint16
	}{
		{"p256", clientKeyVal, sigECDSASecp256r1SHA256},
		{"p384-sha384", p384, sigECDSASecp384r1SHA384},
		{"p384-sha256", p384, sigECDSASecp256r1SHA256},
		{"rsa", testRSAKey(), sigRSAPKCS1SHA256},
	}
	testServerCert(t)
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			h := newHarness(t, clientOptions{key: tc.key, sigAlg: tc.alg}, nil)
			h.handshake()
		})
	}
}

func TestFragmentation(t *testing.T) {
	h := newHarness(t, clientOptions{mtu: 150}, nil)
	h.s.mtu = 150
	out := h.c.start()
	maxLen, count := 0, 0
	fragmented := false
	for round := 0; round < 20; round++ {
		h.toServer(out)
		in := h.take()
		if len(in) == 0 {
			break
		}
		for _, d := range in {
			count++
			if len(d) > maxLen {
				maxLen = len(d)
			}
			for rest := d; len(rest) > 0; {
				rec, r, ok := nextRecord(rest)
				if !ok {
					t.Fatal("bad record from server")
				}
				rest = r
				if rec.typ == contentHandshake && rec.epoch == 0 {
					frags, _ := parseFragments(rec.payload)
					for _, f := range frags {
						if len(f.data) < f.length {
							fragmented = true
						}
					}
				}
			}
		}
		out = h.toClientAll(in)
	}
	h.checkDone()
	if maxLen > 150 {
		t.Fatalf("datagram of %d bytes exceeds mtu", maxLen)
	}
	if !fragmented || count < 5 {
		t.Fatalf("expected fragmented flight, got %d datagrams", count)
	}
}

func TestLostServerFlightRetransmitted(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	h.toServer(h.c.start())
	ch2 := h.toClientAll(h.take())
	h.toServer(ch2)
	if lost := h.take(); len(lost) == 0 {
		t.Fatal("no server flight")
	}
	h.now = h.now.Add(500 * time.Millisecond)
	h.s.Tick(h.now)
	if len(h.toClient) != 0 {
		t.Fatal("retransmitted too early")
	}
	h.now = h.now.Add(600 * time.Millisecond)
	if err := h.s.Tick(h.now); err != nil {
		t.Fatal(err)
	}
	again := h.take()
	if len(again) == 0 {
		t.Fatal("no retransmission after 1s")
	}
	final := h.toClientAll(again)
	var dup [][]byte
	for _, d := range final {
		dup = append(dup, d, d)
	}
	h.run(dup)
	h.checkDone()
}

func TestDuplicateClientHelloResendsFlight(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	h.toServer(h.c.start())
	ch2 := h.toClientAll(h.take())
	h.toServer(ch2)
	first := h.take()
	resent := h.c.pack([]flightItem{{typ: hsClientHello, seq: 1, body: h.c.clientHello[handshakeHeaderLen:]}})
	h.toServer(resent)
	second := h.take()
	if len(second) != len(first) || len(second) == 0 {
		t.Fatalf("expected the flight again, got %d of %d datagrams", len(second), len(first))
	}
	h.toServer(ch2)
	if len(h.take()) != 0 {
		t.Fatal("replayed record was not ignored")
	}
	h.toServer(h.c.pack([]flightItem{{typ: hsClientHello, seq: 1, body: h.c.clientHello[handshakeHeaderLen:]}}))
	if len(h.take()) != 0 {
		t.Fatal("peer-triggered resend not rate limited")
	}
	h.now = h.now.Add(minPeerResendGap)
	h.toServer(h.c.pack([]flightItem{{typ: hsClientHello, seq: 1, body: h.c.clientHello[handshakeHeaderLen:]}}))
	if len(h.take()) != len(first) {
		t.Fatal("no resend after the rate-limit gap")
	}
	h.toClientAll(first)
	h.run(h.toClientAll(second))
	h.checkDone()
}

func TestLostServerFinished(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	h.toServer(h.c.start())
	h.toServer(h.toClientAll(h.take()))
	final := h.toClientAll(h.take())
	h.toServer(final)
	if !h.s.Done() {
		t.Fatalf("server not done: %v", h.s.Failed())
	}
	h.take()
	h.toServer(final)
	if len(h.take()) != 0 {
		t.Fatal("exact replay of the final flight must be ignored")
	}
	h.toServer(h.c.pack(h.c.final))
	resent := h.take()
	if len(resent) == 0 {
		t.Fatal("server did not resend its final flight")
	}
	h.toClientAll(resent)
	h.checkDone()
}

func TestReorderedFinalFlight(t *testing.T) {
	h := newHarness(t, clientOptions{mtu: 200}, nil)
	h.toServer(h.c.start())
	h.toServer(h.toClientAll(h.take()))
	final := h.toClientAll(h.take())
	if len(final) < 3 {
		t.Fatalf("expected a multi-datagram final flight, got %d", len(final))
	}
	for i, j := 0, len(final)-1; i < j; i, j = i+1, j-1 {
		final[i], final[j] = final[j], final[i]
	}
	h.toServer(final)
	if !h.s.Done() {
		t.Fatalf("server not done after reordered flight: %v", h.s.Failed())
	}
	h.toClientAll(h.take())
	h.checkDone()
}

func TestRetransmitScheduleThenFail(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	h.toServer(h.c.start())
	h.toServer(h.toClientAll(h.take()))
	flight := len(h.take())
	start := h.now
	for i, at := range []time.Duration{1, 3, 7, 15} {
		h.s.Tick(start.Add(at*time.Second - time.Millisecond))
		if len(h.toClient) != 0 {
			t.Fatalf("resend %d too early", i)
		}
		if err := h.s.Tick(start.Add(at * time.Second)); err != nil {
			t.Fatal(err)
		}
		if got := len(h.take()); got != flight {
			t.Fatalf("resend %d sent %d datagrams, want %d", i, got, flight)
		}
	}
	if err := h.s.Tick(start.Add(23*time.Second - time.Millisecond)); err != nil {
		t.Fatal(err)
	}
	if err := h.s.Tick(start.Add(23 * time.Second)); !errors.Is(err, errTimeout) {
		t.Fatalf("expected timeout, got %v", err)
	}
	if h.s.Failed() == nil {
		t.Fatal("not failed")
	}
}

func TestVerifyPeerRejects(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	h.verifyErr = errors.New("fingerprint mismatch")
	h.toServer(h.c.start())
	h.toServer(h.toClientAll(h.take()))
	h.toServer(h.toClientAll(h.take()))
	if len(h.peerDER) != 1 || !bytes.Equal(h.peerDER[0], h.c.opts.certDER) {
		t.Fatal("VerifyPeer not called with the client certificate")
	}
	h.expectFailure(alertBadCertificate)
	if h.s.PeerCertificate() != nil {
		t.Fatal("peer certificate exposed after rejection")
	}
	if _, err := h.s.ExportKeyingMaterial("EXTRACTOR-dtls_srtp", 60); err == nil {
		t.Fatal("export after failure")
	}
}

func TestNilVerifyPeerRejects(t *testing.T) {
	h := newHarness(t, clientOptions{}, func(c *Config) { c.VerifyPeer = nil })
	h.toServer(h.c.start())
	h.toServer(h.toClientAll(h.take()))
	h.toServer(h.toClientAll(h.take()))
	h.expectFailure(alertBadCertificate)
}

func TestNegotiationFailures(t *testing.T) {
	cases := []struct {
		name string
		opts clientOptions
		cfg  func(*Config)
		want uint8
	}{
		{"no-ems", clientOptions{noEMS: true}, nil, alertHandshakeFailure},
		{"no-srtp", clientOptions{noSRTP: true}, nil, alertHandshakeFailure},
		{"no-common-profile", clientOptions{srtpProfiles: []uint16{0x0002}}, nil, alertHandshakeFailure},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			h := newHarness(t, tc.opts, tc.cfg)
			h.toServer(h.c.start())
			hvr := h.take()
			if len(hvr) != 1 {
				t.Fatal("expected a hello verify request")
			}
			h.toServer(h.toClientAll(hvr))
			h.expectFailure(tc.want)
			if h.c.serverHellos != 0 || h.s.Done() {
				t.Fatal("server hello sent")
			}
		})
	}
}

func TestClientFlightFailures(t *testing.T) {
	cases := []struct {
		name string
		opts clientOptions
		want uint8
	}{
		{"tampered-finished", clientOptions{tamperFinished: true}, alertDecryptError},
		{"bad-signature", clientOptions{badSignature: true}, alertDecryptError},
		{"no-certificate", clientOptions{noCertificate: true}, alertHandshakeFailure},
		{"plaintext-finished", clientOptions{plainFinished: true}, alertUnexpectedMessage},
		{"rsa-alg-ecdsa-key", clientOptions{sigAlg: sigRSAPKCS1SHA256}, alertIllegalParameter},
		{"unoffered-alg", clientOptions{sigAlg: 0x0603}, alertIllegalParameter},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			h := newHarness(t, tc.opts, nil)
			h.toServer(h.c.start())
			h.toServer(h.toClientAll(h.take()))
			h.toServer(h.toClientAll(h.take()))
			h.expectFailure(tc.want)
			if h.c.done || h.s.Done() {
				t.Fatal("handshake finished")
			}
		})
	}
}

func TestBadCookieIgnored(t *testing.T) {
	h := newHarness(t, clientOptions{badCookie: true}, nil)
	h.toServer(h.c.start())
	hvr := h.take()
	ch2 := h.toClientAll(hvr)
	if len(ch2) != 1 {
		t.Fatal("no second client hello")
	}
	if VerifyCookie(h.cfg, testRemote, ch2[0]) {
		t.Fatal("tampered cookie verified")
	}
	h.toServer(ch2)
	h.toClientAll(h.take())
	if h.c.serverHellos != 0 || h.c.hvrs != 2 {
		t.Fatalf("server hellos %d hvrs %d", h.c.serverHellos, h.c.hvrs)
	}
	if h.s.Failed() != nil || h.s.Done() {
		t.Fatal("bad cookie changed server state")
	}
	good := newHarness(t, clientOptions{}, nil)
	good.toServer(good.c.start())
	gch2 := good.toClientAll(good.take())
	if !VerifyCookie(good.cfg, testRemote, gch2[0]) {
		t.Fatal("good cookie rejected")
	}
	if VerifyCookie(good.cfg, "198.51.100.8:5000", gch2[0]) {
		t.Fatal("cookie verified for another address")
	}
	rec, _, _ := nextRecord(gch2[0])
	if !VerifyCookie(good.cfg, testRemote, rec.payload) {
		t.Fatal("handshake-message form rejected")
	}
	other := *good.cfg
	other.CookieSecret = bytes.Repeat([]byte{1}, 32)
	if VerifyCookie(&other, testRemote, gch2[0]) {
		t.Fatal("cookie verified under another secret")
	}
	if VerifyCookie(good.cfg, testRemote, nil) || VerifyCookie(good.cfg, testRemote, []byte{22, 1}) {
		t.Fatal("garbage verified")
	}
}

func TestReplayedRecordsIgnored(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	h.handshake()
	closeRec := h.c.encrypted(contentAlert, []byte{alertLevelWarning, alertCloseNotify})
	warn := h.c.encrypted(contentAlert, []byte{alertLevelWarning, 90})
	h.toServer([][]byte{warn})
	h.toServer([][]byte{warn})
	if h.s.Failed() != nil {
		t.Fatal(h.s.Failed())
	}
	forged := h.c.encrypted(contentAlert, []byte{alertLevelFatal, alertHandshakeFailure})
	forged[len(forged)-1] ^= 1
	h.toServer([][]byte{forged})
	plain := plainRecord(nil, contentAlert, 0, 99, []byte{alertLevelFatal, alertHandshakeFailure})
	h.toServer([][]byte{plain})
	if h.s.Failed() != nil {
		t.Fatalf("forged or plaintext alert accepted: %v", h.s.Failed())
	}
	h.toServer([][]byte{closeRec})
	if !errors.Is(h.s.Failed(), errClosed) {
		t.Fatalf("close_notify not handled: %v", h.s.Failed())
	}
	h.toClientAll(h.take())
	if !h.c.closeNotify {
		t.Fatal("server did not answer close_notify")
	}
	if !h.s.Done() {
		t.Fatal("Done should stay true after close")
	}
}

func TestReplayWindow(t *testing.T) {
	var w replayWindow
	for _, s := range []uint64{5, 6, 3, 70} {
		if !w.fresh(s) {
			t.Fatalf("%d not fresh", s)
		}
		w.mark(s)
	}
	for _, s := range []uint64{5, 6, 3, 70, 6} {
		if w.fresh(s) {
			t.Fatalf("%d replayed", s)
		}
	}
	if !w.fresh(7) || !w.fresh(69) || w.fresh(6) {
		t.Fatal("window bounds")
	}
	w.mark(200)
	if w.fresh(136) || !w.fresh(137) || w.fresh(70) {
		t.Fatal("window slide")
	}
}

func TestApplicationDataRefused(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	h.handshake()
	h.toServer([][]byte{h.c.encrypted(contentApplicationData, []byte("hello"))})
	h.expectFailure(alertUnexpectedMessage)
	if !h.s.Done() {
		t.Fatal("Done should stay true after a post-handshake failure")
	}
	ae := &alertError{}
	errors.As(h.s.Failed(), &ae)
	if !strings.Contains(ae.Error(), "application data") {
		t.Fatal(ae)
	}
}

func TestRenegotiationRefused(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	h.handshake()
	body := h.c.helloBody()
	msg := marshalHandshake(hsClientHello, h.c.msgSeq, body)
	h.toServer([][]byte{h.c.encrypted(contentHandshake, msg)})
	h.expectFailure(alertUnexpectedMessage)
}

func TestPlaintextAfterHandshakeIgnored(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	h.handshake()
	body := h.c.helloBody()
	h.toServer([][]byte{
		plainRecord(nil, contentApplicationData, 0, 90, []byte("x")),
		plainRecord(nil, contentHandshake, 0, 91, marshalHandshake(hsClientHello, h.c.msgSeq, body)),
		plainRecord(nil, contentHandshake, 0, 92, []byte{1, 2, 3}),
		plainRecord(nil, contentChangeCipherSpec, 0, 93, []byte{9, 9}),
		plainRecord(nil, 60, 0, 94, []byte{1}),
	})
	if h.s.Failed() != nil || len(h.take()) != 0 {
		t.Fatalf("plaintext record after handshake had an effect: %v", h.s.Failed())
	}
}

func TestHandshakeOnlyEpochZero(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	h.toServer(h.c.start())
	h.toServer(h.toClientAll(h.take()))
	h.toServer([][]byte{plainRecord(nil, contentApplicationData, 0, 50, []byte("x"))})
	h.expectFailure(alertUnexpectedMessage)
}

func TestPreCookieGarbageIsSilent(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	h.toServer([][]byte{
		plainRecord(nil, contentApplicationData, 0, 1, []byte("x")),
		plainRecord(nil, contentAlert, 0, 2, []byte{2, 40}),
		plainRecord(nil, contentHandshake, 0, 3, []byte{1, 2, 3}),
		plainRecord(nil, contentChangeCipherSpec, 0, 4, []byte{1}),
	})
	if len(h.take()) != 0 || h.s.Failed() != nil {
		t.Fatal("pre-cookie garbage had an effect")
	}
	h.handshake()
}

func TestFragmentedClientHello(t *testing.T) {
	h := newHarness(t, clientOptions{mtu: 80 + recordHeaderLen + handshakeHeaderLen}, nil)
	first := h.c.start()
	if len(first) < 2 {
		t.Fatalf("client hello not fragmented: %d", len(first))
	}
	h.run(first)
	h.checkDone()
}

func TestInvalidConfig(t *testing.T) {
	cert := testServerCert(t)
	p384, _ := ecdsa.GenerateKey(elliptic.P384(), rand.Reader)
	send := func([]byte) error { return nil }
	for i, cfg := range []*Config{
		nil,
		{SRTPProfiles: []uint16{1}, CookieSecret: make([]byte, 32)},
		{Certificate: cert, CookieSecret: make([]byte, 32)},
		{Certificate: cert, SRTPProfiles: []uint16{1}, CookieSecret: make([]byte, 8)},
		{Certificate: &Certificate{DER: cert.DER, Key: p384}, SRTPProfiles: []uint16{1}, CookieSecret: make([]byte, 32)},
	} {
		s := NewServer(cfg, testRemote, send)
		if s.Failed() == nil {
			t.Fatalf("config %d accepted", i)
		}
		if s.HandleDatagram([]byte{22}, time.Now()) == nil || s.Tick(time.Now()) == nil {
			t.Fatalf("config %d server usable", i)
		}
	}
	if NewServer(&Config{Certificate: cert, SRTPProfiles: []uint16{1}, CookieSecret: make([]byte, 32)}, testRemote, nil).Failed() == nil {
		t.Fatal("nil send accepted")
	}
}

func TestExportErrors(t *testing.T) {
	h := newHarness(t, clientOptions{}, nil)
	if _, err := h.s.ExportKeyingMaterial("EXTRACTOR-dtls_srtp", 60); err == nil {
		t.Fatal("export before handshake")
	}
	if h.s.SRTPProfile() != 0 || h.s.PeerCertificate() != nil || h.s.Done() {
		t.Fatal("state exposed before handshake")
	}
	h.handshake()
	for _, label := range []string{"master secret", "key expansion", "client finished", "server finished", "extended master secret"} {
		if _, err := h.s.ExportKeyingMaterial(label, 16); err == nil {
			t.Fatalf("reserved label %q exported", label)
		}
	}
	for _, n := range []int{0, -1, 1 << 16} {
		if _, err := h.s.ExportKeyingMaterial("EXTRACTOR-dtls_srtp", n); err == nil {
			t.Fatalf("length %d exported", n)
		}
	}
	a, _ := h.s.ExportKeyingMaterial("EXTRACTOR-dtls_srtp", 60)
	b, _ := h.s.ExportKeyingMaterial("EXTRACTOR-dtls_srtp", 30)
	if !bytes.Equal(a[:30], b) {
		t.Fatal("exporter not a prefix")
	}
}

func TestPRFVector(t *testing.T) {
	secret, _ := hex.DecodeString("9bbe436ba940f017b17652849a71db35")
	seed, _ := hex.DecodeString("a0ba9f936cda311827a6f796ffd5198c")
	want, _ := hex.DecodeString("e3f229ba727be17b8d122620557cd453c2aab21d07c3d495329b52d4e61edb5a6b301791e90d35c9c9a46b4e14baf9af0fa022f7077def17abfd3797c0564bab4fbc91666e9def9b97fce34f796789baa48082d122ee42c5a72e5a5110fff70187347b66")
	if got := prf(secret, "test label", seed, len(want)); !bytes.Equal(got, want) {
		t.Fatalf("prf\n got %x\nwant %x", got, want)
	}
}

func TestGenerateCertificate(t *testing.T) {
	c, err := GenerateCertificate()
	if err != nil {
		t.Fatal(err)
	}
	cert, err := x509.ParseCertificate(c.DER)
	if err != nil {
		t.Fatal(err)
	}
	if cert.Subject.CommonName != "forkmesh-calls" || cert.Issuer.CommonName != "forkmesh-calls" {
		t.Fatal(cert.Subject)
	}
	if err := cert.CheckSignature(cert.SignatureAlgorithm, cert.RawTBSCertificate, cert.Signature); err != nil {
		t.Fatal(err)
	}
	pub, ok := cert.PublicKey.(*ecdsa.PublicKey)
	if !ok || pub.Curve != elliptic.P256() || !pub.Equal(&c.Key.PublicKey) {
		t.Fatal("key mismatch")
	}
	if d := cert.NotAfter.Sub(time.Now()); d < 29*24*time.Hour || d > 31*24*time.Hour {
		t.Fatalf("validity %v", d)
	}
	if !regexp.MustCompile(`^sha-256 [0-9A-F]{2}(:[0-9A-F]{2}){31}$`).MatchString(c.Fingerprint) {
		t.Fatal(c.Fingerprint)
	}
	sum := sha256.Sum256(c.DER)
	if strings.ReplaceAll(strings.TrimPrefix(c.Fingerprint, "sha-256 "), ":", "") != strings.ToUpper(hex.EncodeToString(sum[:])) {
		t.Fatal("fingerprint digest")
	}
	if FingerprintOf(c.DER) != c.Fingerprint {
		t.Fatal("FingerprintOf")
	}
	c2, _ := GenerateCertificate()
	cert2, _ := x509.ParseCertificate(c2.DER)
	if cert2.SerialNumber.Cmp(cert.SerialNumber) == 0 {
		t.Fatal("serial not random")
	}
}

func TestGarbageDoesNotPanic(t *testing.T) {
	rng := mrand.New(mrand.NewSource(1))
	h := newHarness(t, clientOptions{mtu: 300}, nil)
	h.s.mtu = 300
	var corpus [][]byte
	out := h.c.start()
	for round := 0; round < 20 && len(out) > 0; round++ {
		corpus = append(corpus, out...)
		h.toServer(out)
		in := h.take()
		corpus = append(corpus, in...)
		out = h.toClientAll(in)
	}
	h.checkDone()
	for stage := 0; stage < 4; stage++ {
		for i := 0; i < 3000; i++ {
			g := newHarness(t, clientOptions{}, nil)
			switch stage {
			case 1:
				g.toServer(g.c.start())
				g.toServer(g.toClientAll(g.take()))
				g.take()
			case 2:
				g.toServer(g.c.start())
				g.toServer(g.toClientAll(g.take()))
				final := g.toClientAll(g.take())
				g.toServer(final[:1])
				g.take()
			case 3:
				g.handshake()
			}
			for k := 0; k < 4; k++ {
				var d []byte
				if rng.Intn(2) == 0 {
					d = make([]byte, rng.Intn(200))
					rng.Read(d)
					if len(d) > 0 {
						d[0] = byte(20 + rng.Intn(4))
					}
				} else {
					d = append([]byte(nil), corpus[rng.Intn(len(corpus))]...)
					for m := 0; m < 1+rng.Intn(4) && len(d) > 0; m++ {
						d[rng.Intn(len(d))] = byte(rng.Intn(256))
					}
					if rng.Intn(4) == 0 && len(d) > 0 {
						d = d[:rng.Intn(len(d))]
					}
				}
				g.s.HandleDatagram(d, g.now)
				g.s.Tick(g.now.Add(time.Duration(rng.Intn(30)) * time.Second))
			}
			if i > 200 && stage != 0 {
				break
			}
		}
	}
}

func FuzzHandleDatagram(f *testing.F) {
	h := newHarness(f, clientOptions{}, nil)
	out := h.c.start()
	for round := 0; round < 20 && len(out) > 0; round++ {
		for _, d := range out {
			f.Add(d)
		}
		h.toServer(out)
		in := h.take()
		for _, d := range in {
			f.Add(d)
		}
		out = h.toClientAll(in)
	}
	cfg := h.cfg
	f.Fuzz(func(t *testing.T, d []byte) {
		s := NewServer(cfg, testRemote, func([]byte) error { return nil })
		s.HandleDatagram(d, time.Unix(0, 0))
		s.HandleDatagram(d, time.Unix(0, 0))
		s.Tick(time.Unix(100, 0))
	})
}
