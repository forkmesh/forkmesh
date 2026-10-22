package h3

import (
	"bytes"
	"encoding/hex"
	"strings"
	"testing"
)

func unhex(s string) []byte {
	b, err := hex.DecodeString(strings.Join(strings.Fields(s), ""))
	if err != nil {
		panic(err)
	}
	return b
}

func TestHuffmanRFC7541Examples(t *testing.T) {
	for _, tc := range []struct{ enc, want string }{
		{"f1e3 c2e5 f23a 6ba0 ab90 f4ff", "www.example.com"},
		{"a8eb 1064 9cbf", "no-cache"},
		{"25a8 49e9 5ba9 7d7f", "custom-key"},
		{"25a8 49e9 5bb8 e8b4 bf", "custom-value"},
		{"6402", "302"},
		{"aec3 771a 4b", "private"},
		{"d07a be94 1054 d444 a820 0595 040b 8166 e082 a62d 1bff", "Mon, 21 Oct 2013 20:13:21 GMT"},
	} {
		got, err := huffmanDecode(nil, unhex(tc.enc), 1000)
		if err != nil || string(got) != tc.want {
			t.Errorf("decode %s: %q %v", tc.enc, got, err)
		}
	}
	if _, err := huffmanDecode(nil, []byte{0xff, 0xff, 0xff, 0xff}, 100); err == nil {
		t.Error("EOS accepted")
	}
	if _, err := huffmanDecode(nil, unhex("f1e3 c2e5 f23a 6ba0 ab90 f4fe"), 100); err == nil {
		t.Error("bad padding accepted")
	}
	if _, err := huffmanDecode(nil, unhex("f1e3 c2e5 f23a 6ba0 ab90 f4ff"), 5); err == nil {
		t.Error("output cap ignored")
	}
}

func TestQPACKStaticDecoding(t *testing.T) {
	fields, err := DecodeFieldSection(unhex("0000 510b 2f69 6e64 6578 2e68 746d 6c"), 4096)
	if err != nil || len(fields) != 1 || fields[0] != (Field{":path", "/index.html"}) {
		t.Fatalf("RFC 9204 B.1: %v %v", fields, err)
	}
	if _, err := DecodeFieldSection(unhex("0300 80"), 4096); err == nil {
		t.Fatal("non-zero required insert count accepted")
	}
	if _, err := DecodeFieldSection(unhex("0000 80"), 4096); err == nil {
		t.Fatal("dynamic table reference accepted")
	}
	if _, err := DecodeFieldSection(unhex("0000 ff64"), 4096); err == nil {
		t.Fatal("out of range static index accepted")
	}
	in := []Field{{":method", "CONNECT"}, {":protocol", "webtransport"}, {":scheme", "https"}, {":authority", "example.com:4433"}, {":path", "/calls"}, {"origin", "https://example.com"}, {"x-custom", strings.Repeat("v", 300)}}
	out, err := DecodeFieldSection(AppendFieldSection(nil, in), 8192)
	if err != nil {
		t.Fatal(err)
	}
	if len(out) != len(in) {
		t.Fatalf("round trip %v", out)
	}
	for i := range in {
		if out[i] != in[i] {
			t.Fatalf("field %d: %v != %v", i, out[i], in[i])
		}
	}
	if _, err := DecodeFieldSection(AppendFieldSection(nil, in), 200); err == nil {
		t.Fatal("field section size cap ignored")
	}
	enc := AppendFieldSection(nil, []Field{{":status", "200"}})
	if !bytes.Equal(enc, []byte{0, 0, 0xc0 | 25}) {
		t.Fatalf(":status 200 encoded as %x", enc)
	}
}

func TestSettingsParsing(t *testing.T) {
	s := Settings{SettingEnableConnectProtocol: 1, SettingH3Datagram: 1, 0x21: 5}
	got, err := parseSettings(s.encode())
	if err != nil || got[SettingEnableConnectProtocol] != 1 || got[0x21] != 5 {
		t.Fatalf("settings %v %v", got, err)
	}
	dup := append(s.encode(), s.encode()...)
	if _, err := parseSettings(dup); err == nil {
		t.Fatal("duplicate settings accepted")
	}
	if _, err := parseSettings([]byte{0x02, 0x01}); err == nil {
		t.Fatal("HTTP/2 setting accepted")
	}
	if _, err := parseSettings([]byte{0x08}); err == nil {
		t.Fatal("truncated setting accepted")
	}
}

func TestRequestValidation(t *testing.T) {
	good := []Field{{":method", "CONNECT"}, {":protocol", "webtransport"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}}
	if r, ok := parseRequest(good); !ok || r.Protocol != "webtransport" {
		t.Fatal("extended CONNECT rejected")
	}
	bad := [][]Field{
		{{":method", "GET"}, {":path", "/"}},
		{{":method", "CONNECT"}, {":protocol", "webtransport"}, {":scheme", "https"}, {":path", "/"}},
		{{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {":path", "/x"}},
		{{":method", "GET"}, {"a", "b"}, {":scheme", "https"}, {":path", "/"}},
		{{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"Upper", "x"}},
		{{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"connection", "close"}},
		{{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"x", "a\nb"}},
		{{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {":bogus", "x"}},
		{{":method", "GET"}, {":protocol", "webtransport"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}},
	}
	for i, f := range bad {
		if _, ok := parseRequest(f); ok {
			t.Errorf("malformed request %d accepted: %v", i, f)
		}
	}
}

func FuzzDecodeFieldSection(f *testing.F) {
	f.Add(unhex("0000 510b 2f69 6e64 6578 2e68 746d 6c"))
	f.Add(AppendFieldSection(nil, []Field{{":method", "CONNECT"}, {"x", "y"}}))
	f.Add(unhex("0000 2a9f f1e3 c2e5 f23a 6ba0 ab90 f4ff"))
	f.Fuzz(func(t *testing.T, b []byte) {
		fields, err := DecodeFieldSection(b, 4096)
		if err != nil {
			return
		}
		size := 0
		for _, fl := range fields {
			size += len(fl.Name) + len(fl.Value) + 32
		}
		if size > 4096 {
			t.Fatalf("decoded %d bytes past the cap", size)
		}
		parseRequest(fields)
	})
}

func FuzzSettings(f *testing.F) {
	f.Add(Settings{1: 0, 6: 16384, 0x33: 1}.encode())
	f.Fuzz(func(t *testing.T, b []byte) {
		s, err := parseSettings(b)
		if err != nil {
			return
		}
		if len(s) > maxSettings {
			t.Fatal("settings cap ignored")
		}
	})
}
