package agentrun

import (
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"forkmesh.com/desktop-core/internal/qtjson"
)

type golden struct {
	LogText []struct {
		Event     string `json:"event"`
		Parsed    bool   `json:"parsed"`
		Assistant string `json:"assistant"`
		User      string `json:"user"`
	} `json:"logtext"`
	Redact []struct {
		Credentials []string `json:"credentials"`
		Cases       []struct {
			In  string `json:"in"`
			Out string `json:"out"`
		} `json:"cases"`
	} `json:"redact"`
}

func loadInto(t *testing.T, into any) {
	t.Helper()
	data, err := os.ReadFile(filepath.Join("..", "qtjson", "testdata", "golden.json"))
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(data, into); err != nil {
		t.Fatal(err)
	}
}

func loadGolden(t *testing.T) golden {
	t.Helper()
	var g golden
	loadInto(t, &g)
	return g
}

func b64(t *testing.T, s string) []byte {
	t.Helper()
	out, err := base64.StdEncoding.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func TestLogTextMatchesCpp(t *testing.T) {
	g := loadGolden(t)
	if len(g.LogText) < 10 {
		t.Fatal("missing fixtures")
	}
	for i, c := range g.LogText {
		event, ok := qtjson.ParseObject(b64(t, c.Event))
		if ok != c.Parsed {
			t.Fatalf("case %d: parsed %v", i, ok)
		}
		if got, want := QtUTF8(AssistantContent(event.Get("message"))), string(b64(t, c.Assistant)); got != want {
			t.Errorf("case %d assistant:\n got %q\nwant %q", i, got, want)
		}
		if got, want := QtUTF8(UserEvent(event)), string(b64(t, c.User)); got != want {
			t.Errorf("case %d user:\n got %q\nwant %q", i, got, want)
		}
	}
}

func TestRedactionMatchesCpp(t *testing.T) {
	g := loadGolden(t)
	for _, set := range g.Redact {
		for i, c := range set.Cases {
			event, ok := qtjson.ParseObject(b64(t, c.In))
			if !ok {
				t.Fatal("fixture")
			}
			if got, want := string(Redact(event, set.Credentials).Compact()), string(b64(t, c.Out)); got != want {
				t.Errorf("case %d:\n got %s\nwant %s", i, got, want)
			}
		}
	}
}

func TestFramerMatchesCppRules(t *testing.T) {
	f := NewFramer(10)
	frames, errs := f.Append([]byte("ab\r\ncd\nlonglonglong"), false)
	if len(frames) != 2 || string(frames[0]) != "ab" || string(frames[1]) != "cd" || len(errs) != 1 {
		t.Fatalf("frames %q errs %v", frames, errs)
	}
	frames, errs = f.Append([]byte("still\nok\n"), false)
	if len(frames) != 1 || string(frames[0]) != "ok" || len(errs) != 0 {
		t.Fatalf("frames %q errs %v", frames, errs)
	}
	frames, _ = f.Append([]byte("tail"), true)
	if len(frames) != 1 || string(frames[0]) != "tail" {
		t.Fatalf("flush %q", frames)
	}
}
