package pulls

import (
	"path/filepath"
	"strings"
	"testing"
)

func TestRemoteCloseEventClosesThePullAndKeepsItsRecord(t *testing.T) {
	f := newFixture(t)
	f.signing(t)
	f.addPull(t, &Pull{Number: 1, Title: "One", Base: "main", Head: "feature", Status: "open", Author: "BOBKEY123456", Sig: "s", Patch: "p"})
	remote := Event{Type: "status", ID: "c1", Author: "BOBKEY123456", AuthorName: "bob", State: "closed", Ts: 3, Sig: "sig"}
	if err := f.store.ApplyRemoteEvent(1, remote); err != nil {
		t.Fatal(err)
	}
	pr, ok := f.store.ReadPull(1, false)
	if !ok || pr.Status != "closed" {
		t.Fatalf("pull after close: ok=%v %+v", ok, pr)
	}
	if msg := lastCommitMessage(t, f.store.MetaWorkTree()); msg != "pull #1: closed (from bob)" {
		t.Fatalf("commit message %q", msg)
	}
	text := readText(t, filepath.Join(f.store.pullDir(1), "0002-status.md"))
	if !strings.Contains(text, "\nstate: closed\n") {
		t.Fatalf("status event lacks its state:\n%s", text)
	}
}

func TestRemoteDeleteEventRemovesThePull(t *testing.T) {
	f := newFixture(t)
	f.signing(t)
	f.addPull(t, &Pull{Number: 1, Title: "One", Base: "main", Head: "feature", Status: "closed", Author: "BOBKEY123456", Sig: "s", Patch: "p"})
	if err := f.store.ApplyRemoteEvent(1, Event{Type: "delete", ID: "d1", Author: "BOBKEY123456", Ts: 3, Sig: "sig"}); err != nil {
		t.Fatal(err)
	}
	if _, ok := f.store.ReadPull(1, false); ok {
		t.Fatal("deleted pull is still in the ledger")
	}
	if err := f.store.ApplyRemoteEvent(1, Event{Type: "delete", ID: "d2", Author: "BOBKEY123456", Ts: 4, Sig: "sig"}); err != nil {
		t.Fatalf("a repeated delete must drain quietly: %v", err)
	}
}

func TestRemoteLifecycleEventsLeaveAMergedPullAlone(t *testing.T) {
	for _, ev := range []Event{
		{Type: "status", ID: "c1", Author: "BOBKEY123456", State: "closed", Ts: 3, Sig: "sig"},
		{Type: "delete", ID: "d1", Author: "BOBKEY123456", Ts: 3, Sig: "sig"},
	} {
		f := newFixture(t)
		f.signing(t)
		f.addPull(t, &Pull{Number: 1, Title: "One", Base: "main", Head: "feature", Status: "merged", Author: "BOBKEY123456", Sig: "s", Patch: "p"})
		if err := f.store.ApplyRemoteEvent(1, ev); err != nil {
			t.Fatal(err)
		}
		pr, ok := f.store.ReadPull(1, false)
		if !ok || pr.Status != "merged" {
			t.Fatalf("%s changed a merged pull: ok=%v %+v", ev.Type, ok, pr)
		}
	}
}

func TestLifecycleEventsSignTheirState(t *testing.T) {
	closeEvent := &Event{Type: "status", State: "closed", Body: "done"}
	if got := ContentForSigning(closeEvent); got != "closed\x00done" {
		t.Fatalf("status content %q", got)
	}
	if got := ContentForSigning(&Event{Type: "delete", Body: "bye"}); got != "bye" {
		t.Fatalf("delete content %q", got)
	}
}
