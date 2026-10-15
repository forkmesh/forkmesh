package qtjson

import (
	"encoding/base64"
	"encoding/json"
	"os"
	"strconv"
	"testing"
)

type golden struct {
	Qt      string `json:"qt"`
	Numbers []struct {
		In  string `json:"in"`
		Out string `json:"out"`
		Int bool   `json:"int"`
		Neg bool   `json:"neg"`
	} `json:"numbers"`
	Parse []struct {
		In       string `json:"in"`
		OK       bool   `json:"ok"`
		Out      string `json:"out"`
		Indented string `json:"indented"`
	} `json:"parse"`
	Sessions []struct {
		Compact  string `json:"compact"`
		Indented string `json:"indented"`
	} `json:"sessions"`
}

func loadGolden(t *testing.T) golden {
	t.Helper()
	data, err := os.ReadFile("testdata/golden.json")
	if err != nil {
		t.Fatal(err)
	}
	var g golden
	if err := json.Unmarshal(data, &g); err != nil {
		t.Fatal(err)
	}
	return g
}

func unb64(t *testing.T, s string) []byte {
	t.Helper()
	out, err := base64.StdEncoding.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func TestNumbersMatchQt(t *testing.T) {
	g := loadGolden(t)
	if len(g.Numbers) < 1000 {
		t.Fatalf("only %d number fixtures", len(g.Numbers))
	}
	for _, n := range g.Numbers {
		var v Value
		if n.Int {
			i, err := strconv.ParseInt(n.In, 10, 64)
			if err != nil {
				t.Fatal(err)
			}
			v = NewInt(i)
		} else {
			d, err := strconv.ParseFloat(n.In, 64)
			if err != nil {
				t.Fatal(err)
			}
			if n.Neg {
				d = -d
			}
			v = NewDouble(d)
		}
		got := ObjectOf("v", v).String()
		if got != n.Out {
			t.Errorf("%s (neg %v): got %s, Qt wrote %s", n.In, n.Neg, got, n.Out)
		}
	}
}

func TestParseAndCompactMatchQt(t *testing.T) {
	g := loadGolden(t)
	for _, c := range g.Parse {
		in := unb64(t, c.In)
		value, err := Parse(in)
		ok := err == nil && value.IsObject()
		if ok != c.OK {
			t.Errorf("%q: parsed %v (err %v), Qt %v", in, ok, err, c.OK)
			continue
		}
		if !ok {
			continue
		}
		if got, want := string(value.Compact()), string(unb64(t, c.Out)); got != want {
			t.Errorf("%q compact:\n got %q\nwant %q", in, got, want)
		}
		if got, want := string(value.Indented()), string(unb64(t, c.Indented)); got != want {
			t.Errorf("%q indented:\n got %q\nwant %q", in, got, want)
		}
	}
}

func TestReindentMatchesQtIndented(t *testing.T) {
	g := loadGolden(t)
	if len(g.Sessions) == 0 {
		t.Fatal("no session fixtures")
	}
	for _, c := range g.Sessions {
		got, err := Reindent(unb64(t, c.Compact))
		if err != nil {
			t.Fatal(err)
		}
		if want := string(unb64(t, c.Indented)); string(got) != want {
			t.Errorf("reindent:\n got %s\nwant %s", got, want)
		}
	}
}

func TestObjectEditingKeepsQtOrder(t *testing.T) {
	v := NewObject()
	v.Set("zeta", NewInt(1))
	v.Set("alpha", NewString("a"))
	v.Set("mid", NewDouble(1e-4))
	v.Set("alpha", NewString("b"))
	if got := v.String(); got != `{"alpha":"b","mid":1e-04,"zeta":1}` {
		t.Fatalf("got %s", got)
	}
	v.Remove("mid")
	if got := v.String(); got != `{"alpha":"b","zeta":1}` {
		t.Fatalf("got %s", got)
	}
	if v.Get("zeta").Int() != 1 || !v.Get("missing").IsUndefined() {
		t.Fatal("lookup")
	}
}

func TestLoneSurrogatesRoundTrip(t *testing.T) {
	v, err := Parse([]byte(`{"s":"\ud800x\udfff"}`))
	if err != nil {
		t.Fatal(err)
	}
	if got := v.String(); got != `{"s":"\ud800x\udfff"}` {
		t.Fatalf("got %s", got)
	}
}

func TestDeepNestingRefused(t *testing.T) {
	deep := make([]byte, 0, 4200)
	for i := 0; i < 1100; i++ {
		deep = append(deep, '[')
	}
	for i := 0; i < 1100; i++ {
		deep = append(deep, ']')
	}
	if _, err := Parse(deep); err != ErrNesting {
		t.Fatalf("err %v", err)
	}
}
