package daemon

import (
	"io"
	"log"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/paths"
)

func TestRememberRecallForget(t *testing.T) {
	root := t.TempDir()
	core := NewCore(paths.Layout{Project: "p", State: filepath.Join(root, "state")}, "test", log.New(io.Discard, "", 0))
	var missing []string
	if found, err := core.Recall("owners.json", &missing); found || err != nil {
		t.Fatalf("nothing saved: %v %v", found, err)
	}
	if err := core.Remember("../escape.json", 1); err == nil {
		t.Fatal("a name with a folder was accepted")
	}
	if err := core.Remember("owners.json", []string{"ada"}); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(filepath.Join(root, "state", "core", "owners.json"))
	if err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("saved file %v %v", info, err)
	}
	again := NewCore(core.Layout, "test", log.New(io.Discard, "", 0))
	again.recallOwners()
	if got := again.Get("control.owners"); !reflect.DeepEqual(got, []string{"ada"}) {
		t.Fatalf("owners after restart %v", got)
	}
	if err := core.Forget("owners.json"); err != nil {
		t.Fatal(err)
	}
	if err := core.Forget("owners.json"); err != nil {
		t.Fatal(err)
	}
	if found, _ := core.Recall("owners.json", &missing); found {
		t.Fatal("forgotten file recalled")
	}
}

func TestPreviousStopReadsTheLastCleanStop(t *testing.T) {
	root := t.TempDir()
	core := NewCore(paths.Layout{Project: "p", State: filepath.Join(root, "state")}, "test", log.New(io.Discard, "", 0))
	if stopped, restart := core.PreviousStop(); !stopped.IsZero() || restart {
		t.Fatalf("no previous run: %v %v", stopped, restart)
	}
	dir := filepath.Join(root, "core")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	core.recordStart(dir)
	core.recordStop(dir)
	again := NewCore(core.Layout, "test", log.New(io.Discard, "", 0))
	again.recordStart(dir)
	stopped, restart := again.PreviousStop()
	if stopped.IsZero() || restart || time.Since(stopped) > time.Minute {
		t.Fatalf("clean stop %v %v", stopped, restart)
	}
	crashed := NewCore(core.Layout, "test", log.New(io.Discard, "", 0))
	crashed.recordStart(dir)
	if stopped, _ := crashed.PreviousStop(); !stopped.IsZero() {
		t.Fatalf("a crash has no stop time: %v", stopped)
	}
}
