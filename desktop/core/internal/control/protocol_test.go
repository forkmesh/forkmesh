package control

import (
	"bytes"
	"encoding/hex"
	"strings"
	"testing"
)

func TestPinnedVectors(t *testing.T) {
	transcript := Transcript("CK", "CE", "CN", "SK", "SE")
	if string(transcript) != "forkmesh-local-control-v1\nCK\nCE\nCN\nSK\nSE" {
		t.Fatalf("transcript layout %q", transcript)
	}
	keys := DeriveKeys(bytes.Repeat([]byte{7}, 32), transcript)
	if !keys.Valid() {
		t.Fatal("keys not derived")
	}
	if bytes.Equal(keys.ClientToServer, keys.ServerToClient) {
		t.Fatal("directions must differ")
	}
	if got := hex.EncodeToString(keys.ClientToServer); got != "6e7a4a8c0002d3118fbc2d04ddbc268e61b1b45af566060d38e3235b2bd1e239" {
		t.Fatalf("c2s vector %s", got)
	}
	if got := hex.EncodeToString(keys.ServerToClient); got != "b76be7e0ae92fe18b561280e896315649f905d4ebed4685676c4dc4b1158596f" {
		t.Fatalf("s2c vector %s", got)
	}
	if got := PairingCode(transcript); got != "796568" {
		t.Fatalf("pairing code vector %s", got)
	}
	sealed := SealFrame(keys.ClientToServer, 3, []byte(`{"id":1}`))
	if got := hex.EncodeToString(sealed); got != "4fb9fc7fc067c05ecc5591ec3f59b2f37b2727354152adc0" {
		t.Fatalf("sealed frame vector %s", got)
	}
	if got := OpenFrame(keys.ClientToServer, 3, sealed); string(got) != `{"id":1}` {
		t.Fatalf("seal round trip %q", got)
	}
	if OpenFrame(keys.ClientToServer, 4, sealed) != nil {
		t.Fatal("wrong counter must be refused")
	}
	if OpenFrame(keys.ServerToClient, 3, sealed) != nil {
		t.Fatal("wrong key must be refused")
	}
	if OpenFrame(keys.ClientToServer, 3, SealFrame(keys.ClientToServer, 3, nil)) != nil {
		t.Fatal("an empty body never opens")
	}
	if len(PairingCode(transcript)) != 6 {
		t.Fatal("six digit code")
	}
	if DeriveKeys(make([]byte, 31), transcript).Valid() || DeriveKeys(make([]byte, 32), nil).Valid() {
		t.Fatal("bad inputs derive nothing")
	}
	if SealFrame(make([]byte, 16), 0, []byte("x")) != nil {
		t.Fatal("only 32-byte keys seal")
	}
}

func TestX25519AndBase64Url(t *testing.T) {
	a := NewEphemeral()
	b := NewEphemeral()
	ab := X25519(a.PrivateKey, b.PublicKey)
	ba := X25519(b.PrivateKey, a.PublicKey)
	if ab == nil || !bytes.Equal(ab, ba) {
		t.Fatal("x25519 must agree")
	}
	if X25519(a.PrivateKey, make([]byte, 32)) != nil {
		t.Fatal("an all-zero shared secret is refused")
	}
	key := ToBase64Url(bytes.Repeat([]byte{9}, 32))
	if !bytes.Equal(FromBase64Url(key, 32), bytes.Repeat([]byte{9}, 32)) {
		t.Fatal("base64url round trip")
	}
	if FromBase64Url(key, 31) != nil || FromBase64Url(key+"=", 32) != nil || FromBase64Url("", 0) != nil ||
		FromBase64Url(strings.Repeat("A", 201), 150) != nil || FromBase64Url("ab+/", 3) != nil {
		t.Fatal("base64url must be exact")
	}
}

func TestSafeFileName(t *testing.T) {
	cases := map[string]string{
		"../../etc/passwd":                      "passwd",
		"..\\evil.txt":                          "evil.txt",
		".hidden":                               "hidden",
		"":                                      "file",
		"a/b:c?.png":                            "b_c_.png",
		"../report.txt":                         "report.txt",
		"..\\..\\etc\\passwd":                   "passwd",
		"photo (1).jpg":                         "photo (1).jpg",
		"naïve$.txt":                            "naïve_.txt",
		"  spaced  ":                            "spaced",
		"dir/":                                  "file",
		"...":                                   "file",
		"tab\tname":                             "tab_name",
		"emoji\U0001F600.png":                   "emoji__.png",
		" lead":                                 "_lead",
		strings.Repeat("a", 130):                strings.Repeat("a", 120),
		"x" + strings.Repeat("b", 125) + ".jpg": strings.Repeat("b", 116) + ".jpg",
	}
	for input, want := range cases {
		if got := SafeFileName(input); got != want {
			t.Errorf("SafeFileName(%q) = %q, want %q", input, got, want)
		}
	}
}

func TestIsControlHello(t *testing.T) {
	if !IsControlHello([]byte(`{"t":"fm-control-hello","v":1}`)) {
		t.Fatal("hello detected")
	}
	if IsControlHello([]byte(`{"t":"fm-control-hello","v":2}`)) {
		t.Fatal("other version")
	}
	if IsControlHello([]byte(`{"iv":"x","ct":"y"}`)) {
		t.Fatal("mesh envelope")
	}
	if IsControlHello([]byte(`{"t":"fm-control-hello","v":1.5}`)) || IsControlHello([]byte(`["fm-control-hello"]`)) {
		t.Fatal("malformed hellos")
	}
}

func TestFrameLengthPrefix(t *testing.T) {
	framed := Frame([]byte("abc"))
	if !bytes.Equal(framed, []byte{0, 0, 0, 3, 'a', 'b', 'c'}) {
		t.Fatalf("frame %v", framed)
	}
	buffer := append(append([]byte(nil), framed...), 0, 0)
	payload, taken := takeFrame(&buffer)
	if taken != 1 || string(payload) != "abc" || len(buffer) != 2 {
		t.Fatalf("take %d %q %v", taken, payload, buffer)
	}
	if _, taken := takeFrame(&buffer); taken != 0 {
		t.Fatal("partial header waits")
	}
	zero := []byte{0, 0, 0, 0}
	if _, taken := takeFrame(&zero); taken != -1 {
		t.Fatal("zero length refused")
	}
	huge := []byte{0, 0x40, 0, 1}
	if _, taken := takeFrame(&huge); taken != -1 {
		t.Fatal("oversized length refused")
	}
	if MaxFrameBytes != 4*1024*1024 || FileChunkBytes != 256*1024 || MaxRelayFrameBytes != 1024*1024 ||
		RelayChunkChars != 9000 {
		t.Fatal("constants changed")
	}
}

func TestSendBudget(t *testing.T) {
	budget := NewSendBudget()
	if budget.Limit() != 180 {
		t.Fatalf("default limit %d", budget.Limit())
	}
	for i := int64(0); i < 180; i++ {
		if budget.WaitMs(1000+i) != 0 {
			t.Fatal("frames inside the budget go at once")
		}
		budget.Spend(1000 + i)
	}
	if got := budget.WaitMs(1200); got != 9800 {
		t.Fatalf("full budget waits %d", got)
	}
	if budget.WaitMs(11000) != 0 {
		t.Fatal("the window slides")
	}
	budget.Configure(10000, 1000)
	if budget.Limit() != 900 {
		t.Fatalf("advertised budget %d", budget.Limit())
	}
	budget.Reset()
	if budget.WaitMs(0) != 0 {
		t.Fatal("reset empties the window")
	}
	budget.Configure(10, 1)
	if budget.WindowMs() != 1000 || budget.Limit() != 1 {
		t.Fatal("floors")
	}
	if FollowIntervalMs(false) >= FollowIntervalMs(true) {
		t.Fatal("the LAN streams more often than the relay")
	}
}

func TestUniquePath(t *testing.T) {
	dir := t.TempDir()
	first := uniquePath(dir, "photo.jpg")
	if !strings.HasSuffix(first, "photo.jpg") {
		t.Fatal(first)
	}
	writeFile(t, first, "x")
	if got := uniquePath(dir, "photo.jpg"); !strings.HasSuffix(got, "photo (2).jpg") {
		t.Fatal(got)
	}
	writeFile(t, uniquePath(dir, "photo.jpg")+".part", "x")
	if got := uniquePath(dir, "photo.jpg"); !strings.HasSuffix(got, "photo (3).jpg") {
		t.Fatal(got)
	}
	writeFile(t, uniquePath(dir, "archive.tar.gz"), "x")
	if got := uniquePath(dir, "archive.tar.gz"); !strings.HasSuffix(got, "archive.tar (2).gz") {
		t.Fatal(got)
	}
	writeFile(t, uniquePath(dir, "noext"), "x")
	if got := uniquePath(dir, "noext"); !strings.HasSuffix(got, "noext (2)") {
		t.Fatal(got)
	}
}
