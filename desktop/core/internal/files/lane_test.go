package files

import (
	"context"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/git"
)

func TestPullWaitsForTheRepositoryLane(t *testing.T) {
	f := newFixture(t)
	f.module.Git = fakeGit(t, `echo "ran $1"`)
	runner := &git.Runner{}
	f.core.Set(git.SharedRunnerKey, runner)
	if f.module.laneRunner() != runner {
		t.Fatalf("the files module does not use the core's shared git runner")
	}
	_, lease, err := runner.Hold(context.Background(), f.repo)
	if err != nil {
		t.Fatal(err)
	}
	if got := runAndWait(t, f, "alice/proj", "fetch"); got != `{"action":"fetch","message":"ran fetch","ok":true,"root":"alice/proj"}` {
		t.Errorf("fetch is a read and runs beside a held lane: %s", got)
	}
	token := prepare(t, f, f.phone, "alice/proj", "pull")["token"]
	if _, err := call(t, f, f.phone, "files.browse.run", map[string]any{"root": "alice/proj", "action": "pull", "token": token}); err != "" {
		t.Fatalf("run pull: %s", err)
	}
	select {
	case e := <-f.phone.Events:
		t.Fatalf("pull finished while another index writer held the lane: %s %s", e.Name, e.Data)
	case <-time.After(400 * time.Millisecond):
	}
	if !f.module.Busy() {
		t.Errorf("a pull waiting for the lane is not busy")
	}
	lease.Release()
	if got := string(f.phone.Next(t, "files.browse.action", 5*time.Second)); got != `{"action":"pull","message":"ran pull","ok":true,"root":"alice/proj"}` {
		t.Errorf("pull after the lane was released: %s", got)
	}
}

func TestStopEndsAPullWaitingForTheLane(t *testing.T) {
	f := newFixture(t)
	f.module.Git = fakeGit(t, `echo "ran $1"`)
	f.module.Lanes = &git.Runner{}
	_, lease, err := f.module.Lanes.Hold(context.Background(), f.repo)
	if err != nil {
		t.Fatal(err)
	}
	defer lease.Release()
	token := prepare(t, f, f.phone, "alice/proj", "pull")["token"]
	if _, err := call(t, f, f.phone, "files.browse.run", map[string]any{"root": "alice/proj", "action": "pull", "token": token}); err != "" {
		t.Fatalf("run pull: %s", err)
	}
	f.module.Stop()
	deadline := time.Now().Add(5 * time.Second)
	for f.module.Busy() && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if f.module.Busy() {
		t.Errorf("Stop left a pull waiting for the lane")
	}
}
