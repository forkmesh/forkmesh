package agents

import (
	"encoding/base64"
	"encoding/json"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/qtjson"
)

type goldenOp struct {
	Op      string `json:"op"`
	ID      int    `json:"id"`
	Session string `json:"session"`
	Text    string `json:"text"`
	Event   string `json:"event"`
	Patch   string `json:"patch"`
}

type goldenFile struct {
	Sessions []struct {
		Raw      string `json:"raw"`
		Compact  string `json:"compact"`
		Indented string `json:"indented"`
		RepoKey  string `json:"repoKey"`
	} `json:"sessions"`
	Store struct {
		Ops   []goldenOp        `json:"ops"`
		Files map[string]string `json:"files"`
		Modes map[string]string `json:"modes"`
		Dirs  []string          `json:"dirs"`
	} `json:"store"`
}

func loadGoldenFile(t *testing.T) goldenFile {
	t.Helper()
	data, err := os.ReadFile(filepath.Join("..", "qtjson", "testdata", "golden.json"))
	if err != nil {
		t.Fatal(err)
	}
	var g goldenFile
	if err := json.Unmarshal(data, &g); err != nil {
		t.Fatal(err)
	}
	return g
}

func decode64(t *testing.T, s string) []byte {
	t.Helper()
	out, err := base64.StdEncoding.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func qtMode(t *testing.T, hex string) fs.FileMode {
	t.Helper()
	q, err := strconv.ParseInt(hex, 16, 32)
	if err != nil {
		t.Fatal(err)
	}
	return fs.FileMode(((q>>8)&7)<<6 | ((q>>4)&7)<<3 | (q & 7))
}

func TestSessionRecordsMatchCpp(t *testing.T) {
	g := loadGoldenFile(t)
	for _, c := range g.Sessions {
		compact := decode64(t, c.Compact)
		if c.Raw == "" {
			obj, ok := qtjson.ParseObject(compact)
			if !ok {
				t.Fatal("compact fixture does not parse")
			}
			if got, err := qtjson.Reindent(compact); err != nil || string(got) != string(decode64(t, c.Indented)) {
				t.Errorf("meta.json bytes:\n got %s\nwant %s", got, decode64(t, c.Indented))
			}
			if c.RepoKey != "" && SessionFromQt(obj).RepoKey() != c.RepoKey {
				t.Errorf("repo key %q, want %q", SessionFromQt(obj).RepoKey(), c.RepoKey)
			}
			continue
		}
		obj, ok := qtjson.ParseObject(decode64(t, c.Raw))
		if !ok {
			t.Fatal("raw fixture does not parse")
		}
		got := SessionFromQt(obj).ToQt()
		if string(got.Compact()) != string(compact) {
			t.Errorf("fromJson/toJson:\n got %s\nwant %s", got.Compact(), compact)
		}
		if string(got.Indented()) != string(decode64(t, c.Indented)) {
			t.Errorf("indented:\n got %s\nwant %s", got.Indented(), decode64(t, c.Indented))
		}
	}
}

func TestWriterReplaysCppStoreScenario(t *testing.T) {
	g := loadGoldenFile(t)
	root := filepath.Join(t.TempDir(), "agents")
	w := NewWriter(NewStore(root))
	w.SetClock(func() time.Time { return time.UnixMilli(1759500000000) })
	records := map[int]qtjson.Value{}
	dirOf := func(id int) string {
		dir, ok := w.SessionDirOf(records[id])
		if !ok {
			t.Fatalf("no session %d", id)
		}
		return dir
	}
	for _, op := range g.Store.Ops {
		switch op.Op {
		case "create":
			input, ok := qtjson.ParseObject([]byte(op.Session))
			if !ok {
				t.Fatal("bad create input")
			}
			created, err := w.CreateRecord(input)
			if err != nil {
				t.Fatal(err)
			}
			if id := created.Get("id").Int(); id != op.ID {
				t.Fatalf("created id %d, C++ %d", id, op.ID)
			}
			records[op.ID] = created
		case "save":
			record, err := qtjson.ParseKeepNumbers([]byte(op.Session))
			if err != nil {
				t.Fatal(err)
			}
			records[record.Get("id").Int()] = record
			_, _ = w.SaveRecord(record)
		case "appendLog":
			w.AppendLog(dirOf(op.ID), op.ID, op.Text)
		case "appendEvent":
			w.AppendEventLine(dirOf(op.ID), op.ID, []byte(op.Event))
		case "writePatch":
			w.WritePatch(dirOf(op.ID), op.Patch)
		case "clearEvents":
			w.ClearEvents(dirOf(op.ID))
		case "recycle":
			w.Flush()
			if ok, err := w.RecycleRecord(records[op.ID]); !ok {
				t.Fatalf("recycle %d: %v", op.ID, err)
			}
		case "restore":
			if ok, err := w.RestoreRecycled(op.ID); !ok {
				t.Fatalf("restore %d: %v", op.ID, err)
			}
		case "delete":
			w.Flush()
			if ok, err := w.DeleteRecord(records[op.ID]); !ok {
				t.Fatalf("delete %d: %v", op.ID, err)
			}
		default:
			t.Fatalf("unknown op %q", op.Op)
		}
	}
	w.Flush()
	got := map[string][]byte{}
	gotDirs := []string{}
	_ = filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil || path == root {
			return err
		}
		rel, _ := filepath.Rel(root, path)
		rel = filepath.ToSlash(rel)
		if d.IsDir() {
			gotDirs = append(gotDirs, rel)
			return nil
		}
		data, _ := os.ReadFile(path)
		if filepath.Base(path) == "deleted-at.json" {
			if obj, ok := qtjson.ParseObject(data); !ok || obj.Get("deletedAtMs").Int64() <= 0 {
				t.Errorf("%s: bad stamp %s", rel, data)
			}
			data = []byte("STAMP")
		}
		got[rel] = data
		info, _ := d.Info()
		if want := qtMode(t, g.Store.Modes[rel]); info.Mode().Perm() != want {
			t.Errorf("%s: mode %v, C++ %v", rel, info.Mode().Perm(), want)
		}
		return nil
	})
	for rel, encoded := range g.Store.Files {
		want := decode64(t, encoded)
		if string(got[rel]) != string(want) {
			t.Errorf("%s:\n got %q\nwant %q", rel, got[rel], want)
		}
		delete(got, rel)
	}
	for rel := range got {
		t.Errorf("unexpected file %s", rel)
	}
	sort.Strings(gotDirs)
	wantDirs := append([]string(nil), g.Store.Dirs...)
	sort.Strings(wantDirs)
	if len(gotDirs) != len(wantDirs) {
		t.Fatalf("dirs %v, C++ %v", gotDirs, wantDirs)
	}
	for i := range gotDirs {
		if gotDirs[i] != wantDirs[i] {
			t.Fatalf("dirs %v, C++ %v", gotDirs, wantDirs)
		}
	}
}
