package quic

import (
	"bytes"
	"crypto/tls"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"os/exec"
	"testing"
	"time"
)

func TestRFC8439ChaChaBlock(t *testing.T) {
	key := make([]byte, 32)
	for i := range key {
		key[i] = byte(i)
	}
	nonce := nonceWords(unhex("000000090000004a00000000"))
	var out [64]byte
	chachaBlock(&out, newChachaKey(key), 1, &nonce)
	want := "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e"
	if hex.EncodeToString(out[:]) != want {
		t.Fatalf("block %x", out)
	}
}

func TestRFC8439Poly1305(t *testing.T) {
	var key [32]byte
	copy(key[:], unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"))
	var tag [16]byte
	poly1305Sum(tag[:], []byte("Cryptographic Forum Research Group"), &key)
	if hex.EncodeToString(tag[:]) != "a8061dc1305136c6c22b8baf0c0127a9" {
		t.Fatalf("tag %x", tag)
	}
}

func TestRFC8439AEAD(t *testing.T) {
	key := make([]byte, 32)
	for i := range key {
		key[i] = byte(0x80 + i)
	}
	nonce := unhex("070000004041424344454647")
	ad := unhex("50515253c0c1c2c3c4c5c6c7")
	pt := []byte("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.")
	want := unhex(`d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b
		1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de5
		76d26586cec64b61161ae10b594f09e26a7e902ecbd0600691`)
	a := newChaCha20Poly1305(key)
	got := a.Seal(nil, nonce, pt, ad)
	if !bytes.Equal(got, want) {
		t.Fatalf("seal %x", got)
	}
	back, err := a.Open(nil, nonce, got, ad)
	if err != nil || !bytes.Equal(back, pt) {
		t.Fatalf("open %q %v", back, err)
	}
	for i := range got {
		bad := append([]byte(nil), got...)
		bad[i] ^= 0x01
		if _, err := a.Open(nil, nonce, bad, ad); err == nil {
			t.Fatalf("forgery at byte %d accepted", i)
		}
	}
	if _, err := a.Open(nil, nonce, got, ad[:4]); err == nil {
		t.Fatal("wrong associated data accepted")
	}
	inPlace := append([]byte(nil), pt...)
	inPlace = append(inPlace, make([]byte, 16)...)[:len(pt)]
	sealed := a.Seal(inPlace[:0], nonce, inPlace, ad)
	if !bytes.Equal(sealed, want) {
		t.Fatal("in-place seal differs")
	}
}

func TestRFC9001ChaChaShortHeader(t *testing.T) {
	secret := unhex("9ac312a7f877468ebe69422748ad00a15443f18203a07d6060f688f30f21632b")
	k := newDirectionKeys(suiteChaCha20, secret)
	if got := hkdfExpandLabel(suiteChaCha20.hash, secret, "quic key", nil, 32); hex.EncodeToString(got) != "c6d98ff3441c3fe1b2182094f69caa2ed4b716b65488960a7a984979fb23e1c8" {
		t.Fatalf("key %x", got)
	}
	if got := hkdfExpandLabel(suiteChaCha20.hash, secret, "quic hp", nil, 32); hex.EncodeToString(got) != "25a282b9e82f06f21f488917a4fc8f1b73573685608597d0efcb076b0ab7a7a4" {
		t.Fatalf("hp %x", got)
	}
	if got := hkdfExpandLabel(suiteChaCha20.hash, secret, "quic ku", nil, 32); hex.EncodeToString(got) != "1223504755036d556342ee9361d253421a826c9ecdf3c7148684b36b714881f9" {
		t.Fatalf("ku %x", got)
	}
	want := unhex("4cfe4189655e5cd55c41f69080575d7999c25a5bfb")
	got := protectForTest(k, unhex("4200bff4"), []byte{0x01}, 654360564)
	if !bytes.Equal(got, want) {
		t.Fatalf("protected %x", got)
	}
	pkt := append([]byte(nil), want...)
	pnLen, ok := k.hp.unprotect(pkt, 1)
	if !ok || pnLen != 3 {
		t.Fatalf("unprotect pnLen %d", pnLen)
	}
	pn := decodePacketNumber(654360563, readTruncatedPN(pkt[1:], pnLen), pnLen)
	if pn != 654360564 {
		t.Fatalf("pn %d", pn)
	}
	payload, err := k.key.open(pkt, 1+pnLen, pn)
	if err != nil || !bytes.Equal(payload, []byte{1}) {
		t.Fatalf("payload %x %v", payload, err)
	}
	next := k.nextGeneration()
	sealed := protectForTest(next, unhex("4200bff4"), []byte{0x01}, 654360564)
	if bytes.Equal(sealed, want) {
		t.Fatal("updated key produced the same packet")
	}
}

func TestChaChaHandshakeEndToEnd(t *testing.T) {
	if os.Getenv("FM_QUIC_CHACHA_CHILD") != "1" {
		if testing.Short() {
			t.Skip("spawns a child test process")
		}
		cmd := exec.Command(os.Args[0], "-test.run=^TestChaChaHandshakeEndToEnd$", "-test.count=1")
		cmd.Env = append(os.Environ(), "FM_QUIC_CHACHA_CHILD=1", "GODEBUG=cpu.aes=off")
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("child: %v\n%s", err, out)
		}
		if !bytes.Contains(out, []byte("negotiated TLS_CHACHA20_POLY1305_SHA256")) {
			t.Fatalf("child did not negotiate chacha:\n%s", out)
		}
		return
	}
	p := newPair(t, testServerConfig(t), testClientConfig(t), nil)
	suite := p.cliConn.ConnectionState().CipherSuite
	fmt.Println("negotiated", tls.CipherSuiteName(suite))
	echoStreams(t, p.srvConn)
	s, _ := p.cliConn.OpenStream()
	msg := bytes.Repeat([]byte("chacha"), 50000)
	go func() {
		s.Write(msg)
		s.Close()
	}()
	s.SetReadDeadline(time.Now().Add(10 * time.Second))
	got, err := io.ReadAll(s)
	if err != nil || !bytes.Equal(got, msg) {
		t.Fatalf("echo %d %v", len(got), err)
	}
}
