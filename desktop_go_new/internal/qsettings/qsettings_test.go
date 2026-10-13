package qsettings

import "testing"

func TestParse(t *testing.T) {
	data := []byte("[General]\ntop=1\n\n[agents]\nmode=Auto\nlist=a, b, c\nquoted=\"x, y\"\nblob=\"@ByteArray({\\\"k\\\":\\\"v\\\"})\"\nnested\\deep\\key=true\nesc\\wss%253A%252F%252Fhost=yes\n[localControl]\nenabled=false\n")
	s := Parse(data)
	if s.Int("top", 0) != 1 {
		t.Fatalf("top: %q", s.String("top", ""))
	}
	if s.String("agents/mode", "") != "Auto" {
		t.Fatalf("mode")
	}
	if got := s.Strings("agents/list"); len(got) != 3 || got[2] != "c" {
		t.Fatalf("list %v", got)
	}
	if s.String("agents/quoted", "") != "x, y" {
		t.Fatalf("quoted %q", s.String("agents/quoted", ""))
	}
	if s.String("agents/blob", "") != `{"k":"v"}` {
		t.Fatalf("blob %q", s.String("agents/blob", ""))
	}
	if !s.Bool("agents/nested/deep/key", false) {
		t.Fatalf("nested")
	}
	if !s.Bool("agents/esc/wss%3A%2F%2Fhost", false) {
		t.Fatalf("escaped key: %v", s.Keys("agents/esc"))
	}
	if s.Bool("localControl/enabled", true) {
		t.Fatalf("bool")
	}
}

func TestByteArrayIsLatin1(t *testing.T) {
	escaped := Parse([]byte("[a]\nv=@ByteArray(caf\\xe9)\n"))
	if got := escaped.String("a/v", ""); got != "caf\xe9" {
		t.Fatalf("escaped byte: %q", got)
	}
	utf8 := Parse([]byte("[a]\nv=@ByteArray(\"caf\xc3\x83\xc2\xa9\")\n"))
	value, _ := utf8.Value("a/v")
	if string(value.Bytes) != "caf\xc3\xa9" {
		t.Fatalf("utf-8 bytes: %q", value.Bytes)
	}
}

func TestQuotedListItems(t *testing.T) {
	s := Parse([]byte("[a]\nv=\".a\", \".b\", c\n"))
	got := s.Strings("a/v")
	if len(got) != 3 || got[0] != ".a" || got[1] != ".b" || got[2] != "c" {
		t.Fatalf("got %q", got)
	}
}
