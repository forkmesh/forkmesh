package mirrornode

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

type syncStatusFixtureCase struct {
	ID          string          `json:"id"`
	ExpectState string          `json:"expectState"`
	Evidence    json.RawMessage `json:"evidence"`
	Expect      json.RawMessage `json:"expect"`
}

type syncStatusFixture struct {
	PrivateStrings []string                `json:"privateStrings"`
	Cases          []syncStatusFixtureCase `json:"cases"`
	Sequences      []struct {
		ID    string                  `json:"id"`
		Steps []syncStatusFixtureCase `json:"steps"`
	} `json:"sequences"`
}

func loadSyncStatusFixture(t *testing.T) syncStatusFixture {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join("..", "conformance", "sync-status", "v1", "fixture.json"))
	if err != nil {
		t.Fatal(err)
	}
	var fixture syncStatusFixture
	if err := json.Unmarshal(raw, &fixture); err != nil {
		t.Fatal(err)
	}
	return fixture
}

func checkSyncStatusCase(t *testing.T, id string, entry syncStatusFixtureCase, private []string) {
	t.Helper()
	var evidence SyncEvidence
	if err := json.Unmarshal(entry.Evidence, &evidence); err != nil {
		t.Fatalf("%s: evidence: %v", id, err)
	}
	encoded, err := json.Marshal(EvaluateSyncStatus(evidence))
	if err != nil {
		t.Fatal(err)
	}
	var got, want map[string]any
	if err := json.Unmarshal(encoded, &got); err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(entry.Expect, &want); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("%s:\n got  %s\n want %s", id, encoded, entry.Expect)
	}
	for _, secret := range private {
		if strings.Contains(string(encoded), secret) {
			t.Fatalf("%s leaks %q", id, secret)
		}
	}
}

func TestSyncStatusMatchesSharedFixture(t *testing.T) {
	fixture := loadSyncStatusFixture(t)
	if len(fixture.Cases) == 0 || len(fixture.Sequences) == 0 {
		t.Fatal("fixture has no cases")
	}
	for _, entry := range fixture.Cases {
		checkSyncStatusCase(t, entry.ID, entry, fixture.PrivateStrings)
	}
	for _, sequence := range fixture.Sequences {
		for i, step := range sequence.Steps {
			checkSyncStatusCase(t, sequence.ID+"#"+string(rune('0'+i)), step, fixture.PrivateStrings)
		}
	}
}

func TestDaemonSyncEvidenceNeverReportsFalseSuccess(t *testing.T) {
	started := time.UnixMilli(1_800_000_000_000)
	now := started.Add(2 * time.Minute)

	if got := EvaluateSyncStatus(daemonSyncEvidence(now, started, time.Time{}, false, "")); got.State != "queued" {
		t.Fatalf("never synced = %s, want queued", got.State)
	}
	failed := EvaluateSyncStatus(daemonSyncEvidence(now, started, now.Add(-time.Minute), false, "dial tcp 203.0.113.7:443: refused"))
	if failed.State != "failed" || failed.Current || strings.Contains(failed.Summary, "203.0.113.7") {
		t.Fatalf("failed sync = %+v", failed)
	}
	if got := EvaluateSyncStatus(daemonSyncEvidence(now, started, now.Add(-time.Minute), true, "")); got.State != "verified" {
		t.Fatalf("healthy sync = %s, want verified", got.State)
	}
	if got := EvaluateSyncStatus(daemonSyncEvidence(now.Add(2*time.Hour), started, now, true, "")); got.State != "stale" {
		t.Fatalf("old sync = %s, want stale", got.State)
	}
}
