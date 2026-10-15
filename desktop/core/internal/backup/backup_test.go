package backup

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/coretest"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/rpc"
)

func TestNamesListPruneAndDue(t *testing.T) {
	when := time.Date(2026, 10, 2, 13, 4, 5, 0, time.Local)
	name := FileName(when)
	if name != "forkmesh-backup-20261002-130405.tar.gz" {
		t.Fatal(name)
	}
	if parsed, ok := TimestampFromName(name); !ok || !parsed.Equal(when) {
		t.Fatalf("parsed %v %v", parsed, ok)
	}
	for _, bad := range []string{"forkmesh-backup-2026.tar.gz", "other-20261002-130405.tar.gz", "forkmesh-backup-20261002-130405.tar"} {
		if _, ok := TimestampFromName(bad); ok {
			t.Errorf("%s parsed", bad)
		}
	}
	root := t.TempDir()
	for i := 0; i < 5; i++ {
		path := filepath.Join(root, FileName(when.Add(time.Duration(i)*time.Hour)))
		if err := os.WriteFile(path, []byte(strings.Repeat("x", i+1)), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	_ = os.WriteFile(filepath.Join(root, "notes.txt"), []byte("x"), 0o600)
	snapshots := List(root)
	if len(snapshots) != 5 || !snapshots[0].Taken.After(snapshots[4].Taken) || snapshots[0].Bytes != 5 {
		t.Fatalf("list %+v", snapshots)
	}
	if prune := ToPrune(snapshots, 3); len(prune) != 2 || prune[0].Bytes != 2 {
		t.Fatalf("prune %+v", prune)
	}
	if prune := ToPrune(snapshots, 0); len(prune) != 4 {
		t.Fatalf("keep floor %d", len(prune))
	}
	now := when.Add(10 * time.Hour)
	if !IsDue(time.Time{}, now, IntervalMs) || !IsDue(when, now, IntervalMs) || IsDue(now, when, IntervalMs) ||
		IsDue(now.Add(-time.Minute), now, IntervalMs) {
		t.Fatal("due rules")
	}
}

func TestTarArgsAndLabels(t *testing.T) {
	root := t.TempDir()
	data := filepath.Join(root, "data")
	conf := filepath.Join(root, "config", "ForkMesh", "ForkMesh.conf")
	_ = os.MkdirAll(filepath.Join(data, "mirrors"), 0o700)
	_ = os.MkdirAll(filepath.Dir(conf), 0o700)
	_ = os.WriteFile(conf, []byte("[General]\n"), 0o600)
	args := ConfigArchiveTarArgs("/tmp/out.tar.gz", data, conf, []string{filepath.Join(data, "mirrors"), "/elsewhere", "", filepath.Join(data, "backups")})
	want := []string{"-czf", "/tmp/out.tar.gz", "--exclude=data/mirrors", "--exclude=data/backups", "-C", root, "data",
		"-C", filepath.Dir(conf), "ForkMesh.conf"}
	if strings.Join(args, "|") != strings.Join(want, "|") {
		t.Fatalf("args %v", args)
	}
	if ConfigArchiveTarArgs("/tmp/x", filepath.Join(root, "missing"), "", nil) != nil {
		t.Fatal("nothing to archive still produced args")
	}
	if got := LocationLabel(filepath.Join(data, "backups"), data); got != "ForkMesh data › backups" {
		t.Fatal(got)
	}
	if got := LocationLabel("/var/snapshots", data); got != "snapshots" {
		t.Fatal(got)
	}
	status := Status(StatusInput{Enabled: true, Keep: 24, Snapshots: []Snapshot{{Taken: time.UnixMilli(5000), Bytes: 10}, {Taken: time.UnixMilli(1000), Bytes: 3}},
		Failure: "weird", FailureAtMs: 6000, IntervalMs: IntervalMs, NowMs: 7000})
	if status["result"] != "failed" || status["error"] != "failed" || status["count"] != 2 || status["totalBytes"] != int64(13) ||
		status["lastBackupAtMs"] != int64(5000) || status["nextDueAtMs"] != int64(5000)+IntervalMs {
		t.Fatalf("status %v", status)
	}
	if off := Status(StatusInput{}); off["result"] != "off" || off["nextDueAtMs"] != int64(0) {
		t.Fatalf("off %v", off)
	}
}

func startBackups(t *testing.T) (*daemon.Core, *Module, *coretest.Peer, string) {
	t.Helper()
	core := coretest.New(t)
	core.Set("control.owners", []string{"alice"})
	module := New()
	coretest.Start(t, core, module)
	ui := coretest.NewPeer("ui", rpc.UI)
	core.Hub.Add(ui)
	root := t.TempDir()
	return core, module, ui, root
}

func nextEvent(t *testing.T, ui *coretest.Peer, name string, match func(map[string]any) bool) map[string]any {
	t.Helper()
	deadline := time.After(20 * time.Second)
	for {
		select {
		case event := <-ui.Events:
			if event.Name != name {
				continue
			}
			var data map[string]any
			_ = json.Unmarshal(event.Data, &data)
			if match == nil || match(data) {
				return data
			}
		case <-deadline:
			t.Fatalf("no %s event", name)
			return nil
		}
	}
}

func TestSnapshotRunsInTheCoreAndDiagIsNative(t *testing.T) {
	if _, err := exec.LookPath("tar"); err != nil {
		t.Skip("tar is not installed")
	}
	core, module, ui, root := startBackups(t)
	data := filepath.Join(root, "data")
	_ = os.MkdirAll(filepath.Join(data, "identity"), 0o700)
	_ = os.WriteFile(filepath.Join(data, "identity", "key"), []byte("secret"), 0o600)
	_ = os.MkdirAll(filepath.Join(data, "mirrors", "big"), 0o700)
	_ = os.WriteFile(filepath.Join(data, "mirrors", "big", "pack"), []byte("bulk"), 0o600)
	phone := coretest.NewPeer("phone", rpc.LAN)
	phone.Dev.User = "alice"
	if _, err := coretest.Call(t, core, phone, "diag.backups", nil); rpc.Code(err) != "unsupported" {
		t.Fatalf("unconfigured diag.backups: %v", err)
	}
	config := Config{Enabled: true, Keep: 2, Root: filepath.Join(data, "backups"), DataRoot: data,
		SettingsFile: filepath.Join(root, "ForkMesh.conf"), Excludes: []string{filepath.Join(data, "mirrors")}}
	coretest.MustCall(t, core, ui, "core.backups.config", config, nil)
	for i := 0; i < 3; i++ {
		module.Now = func() time.Time { return time.Date(2026, 10, 2, 10+i, 0, 0, 0, time.Local) }
		var started map[string]any
		coretest.MustCall(t, core, ui, "core.backups.now", map[string]any{"automatic": false}, &started)
		if started["started"] != true {
			t.Fatalf("not started: %v", started)
		}
		saved := nextEvent(t, ui, "core.backups.status", func(d map[string]any) bool {
			return strings.HasPrefix(d["text"].(string), "Backup saved: ") || d["error"] == true
		})
		if saved["error"] == true {
			t.Fatalf("backup failed: %v", saved)
		}
		for module.Busy() {
			time.Sleep(10 * time.Millisecond)
		}
	}
	snapshots := List(config.Root)
	if len(snapshots) != 2 {
		t.Fatalf("rotation kept %d", len(snapshots))
	}
	listing, err := exec.Command("tar", "-tzf", snapshots[0].Path).Output()
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(listing), "data/identity/key") || strings.Contains(string(listing), "mirrors/big") ||
		strings.Contains(string(listing), "backups/") {
		t.Fatalf("archive content:\n%s", listing)
	}
	stranger := coretest.NewPeer("stranger", rpc.Relay)
	stranger.Dev.User = "mallory"
	if _, err := coretest.Call(t, core, stranger, "diag.backups", nil); rpc.Code(err) != "not_owner" {
		t.Fatalf("stranger: %v", err)
	}
	var status map[string]any
	coretest.MustCall(t, core, phone, "diag.backups", nil, &status)
	if status["result"] != "ok" || status["count"] != float64(2) || status["enabled"] != true || status["keep"] != float64(2) ||
		status["location"] != "ForkMesh data › backups" {
		t.Fatalf("diag.backups %v", status)
	}
	if _, err := coretest.Call(t, core, phone, "core.backups.now", nil); rpc.Code(err) != "forbidden" {
		t.Fatalf("phone reached core.backups.now: %v", err)
	}
}

func TestFailedTarIsReportedAndPartIsRemoved(t *testing.T) {
	core, module, ui, root := startBackups(t)
	fake := filepath.Join(root, "tar")
	_ = os.WriteFile(fake, []byte("#!/bin/sh\necho 'tar: disk on fire' >&2\nexit 2\n"), 0o755)
	module.LookPath = func(string) (string, error) { return fake, nil }
	data := filepath.Join(root, "data")
	_ = os.MkdirAll(data, 0o700)
	coretest.MustCall(t, core, ui, "core.backups.config", Config{Root: filepath.Join(data, "backups"), DataRoot: data}, nil)
	coretest.MustCall(t, core, ui, "core.backups.now", nil, nil)
	failed := nextEvent(t, ui, "core.backups.status", func(d map[string]any) bool { return d["error"] == true })
	if failed["text"] != "Backup failed: tar: disk on fire" {
		t.Fatalf("status %v", failed)
	}
	changed := nextEvent(t, ui, "core.backups.changed", func(d map[string]any) bool { return d["running"] == false })
	if changed["failure"] != "tar_failed" {
		t.Fatalf("changed %v", changed)
	}
	entries, _ := os.ReadDir(filepath.Join(data, "backups"))
	if len(entries) != 0 {
		t.Fatalf("left %v", entries)
	}
	module.LookPath = func(string) (string, error) { return "", exec.ErrNotFound }
	coretest.MustCall(t, core, ui, "core.backups.now", nil, nil)
	missing := nextEvent(t, ui, "core.backups.status", nil)
	if missing["text"] != "Can't back up: the 'tar' tool was not found on this system." {
		t.Fatalf("status %v", missing)
	}
}

func TestUnpackCleansUpWhenNobodyCanFinishTheRestore(t *testing.T) {
	if _, err := exec.LookPath("tar"); err != nil {
		t.Skip("tar is not installed")
	}
	t.Setenv("FORKMESH_HOME", "")
	t.Setenv("FORKMESH_PROJECTS_DIR", "")
	core, _, ui, root := startBackups(t)
	src := filepath.Join(root, "src")
	_ = os.MkdirAll(filepath.Join(src, "data"), 0o700)
	_ = os.WriteFile(filepath.Join(src, "data", "f"), []byte("x"), 0o600)
	archive := filepath.Join(root, "a.tar.gz")
	if out, err := exec.Command("tar", "-czf", archive, "-C", src, "data").CombinedOutput(); err != nil {
		t.Fatalf("%v %s", err, out)
	}
	work := filepath.Join(root, "tmp-forkmesh-import-20261002-100000")
	if _, err := coretest.Call(t, core, ui, "core.backups.unpack", map[string]any{"archive": archive, "workRoot": filepath.Join(root, "elsewhere")}); rpc.Code(err) != "bad_params" {
		t.Fatalf("unchecked work root: %v", err)
	}
	coretest.MustCall(t, core, ui, "core.backups.unpack", map[string]any{"archive": archive, "workRoot": work, "stamp": "s"}, nil)
	done := nextEvent(t, ui, "core.backups.unpacked", nil)
	if done["ok"] != true || done["workRoot"] != work {
		t.Fatalf("unpacked %v", done)
	}
	if _, err := os.Stat(work); !os.IsNotExist(err) {
		t.Fatal("the work folder stayed without a desktop to finish the restore")
	}
	coretest.MustCall(t, core, ui, "core.backups.unpack", map[string]any{"archive": filepath.Join(root, "missing.tar.gz"), "workRoot": work}, nil)
	failed := nextEvent(t, ui, "core.backups.unpacked", nil)
	if failed["ok"] != false || failed["error"] == "" {
		t.Fatalf("failed unpack %v", failed)
	}
	if _, err := os.Stat(work); !os.IsNotExist(err) {
		t.Fatal("a failed unpack left its work folder")
	}
}

func TestCancelStopsARunningSnapshot(t *testing.T) {
	core, module, ui, root := startBackups(t)
	fake := filepath.Join(root, "tar")
	_ = os.WriteFile(fake, []byte("#!/bin/sh\nexec sleep 30\n"), 0o755)
	module.LookPath = func(string) (string, error) { return fake, nil }
	data := filepath.Join(root, "data")
	_ = os.MkdirAll(data, 0o700)
	config := Config{Root: filepath.Join(data, "backups"), DataRoot: data}
	coretest.MustCall(t, core, ui, "core.backups.now", map[string]any{"config": config}, nil)
	nextEvent(t, ui, "core.backups.changed", func(d map[string]any) bool { return d["running"] == true })
	for {
		module.mu.Lock()
		started := module.tarCmd != nil
		module.mu.Unlock()
		if started {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	var reply map[string]any
	coretest.MustCall(t, core, ui, "core.backups.cancel", nil, &reply)
	if reply["cancelled"] != true {
		t.Fatalf("cancel %v", reply)
	}
	changed := nextEvent(t, ui, "core.backups.changed", func(d map[string]any) bool { return d["running"] == false })
	if changed["failure"] != "" {
		t.Fatalf("a cancel was recorded as a failure: %v", changed)
	}
	entries, _ := os.ReadDir(config.Root)
	if len(entries) != 0 {
		t.Fatalf("left %v", entries)
	}
}
