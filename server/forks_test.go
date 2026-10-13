package mirrornode

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestMaterializeWebsiteForkBorrowsObjectsAndPersistsProvenance(t *testing.T) {
	_, source := makeBareRepo(t)
	root := filepath.Dir(source)
	identity, _ := testIdentity(t)
	gatewayPath := filepath.Join(root, "gateway.json")
	var gateway GatewayConfig
	gateway.SchemaVersion = 1
	gateway.Node.Name = "mirror9"
	gateway.Node.PublicKey = identity.PublicKey()
	gateway.RouterPublicKey = identity.PublicKey()
	operations := []string{"blob", "git-info-refs", "git-upload-pack", "tree"}
	for _, repository := range []Repository{
		{Owner: "alice", Name: "proj", Visibility: "public", Enabled: true,
			GitDir: source, Operations: operations},
		{Owner: "mirror9", Name: "seed", Visibility: "public", Enabled: true,
			GitDir: source, Operations: operations},
	} {
		digest, err := refsSHA256(context.Background(), repository.GitDir)
		if err != nil {
			t.Fatal(err)
		}
		repository.Integrity.ExpectedRefsSHA256 = digest
		gateway.Repositories = append(gateway.Repositories, repository)
	}
	if _, err := writeGatewayConfig(gatewayPath, gateway); err != nil {
		t.Fatal(err)
	}
	before, err := refsSHA256(context.Background(), source)
	if err != nil {
		t.Fatal(err)
	}
	daemon := &Daemon{
		config: Config{
			GatewayConfig: gatewayPath,
			CatalogURL:    "http://localhost:1/api/repositories",
			PublishOwner:  "mirror9",
			Version:       "test",
		},
		gateway:      gateway,
		identity:     identity,
		supervisor:   NewSupervisor(),
		syncRequests: make(chan struct{}, 1),
	}
	job := forkJob{
		RequestID:   strings.Repeat("a", 36),
		SourceOwner: "alice", SourceName: "proj",
		SourceURL: "http://localhost:1/alice/proj",
		Owner:     "mirror9", Name: "proj", ForkedFrom: "alice/proj",
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := daemon.materializeFork(ctx, job); err != nil {
		t.Fatal(err)
	}

	destination := filepath.Join(root, "mirror9-proj.git")
	if !validBareRepository(destination) {
		t.Fatal("fork was not materialized as a bare repository")
	}
	alternates, err := os.ReadFile(filepath.Join(
		destination, "objects", "info", "alternates"))
	if err != nil {
		t.Fatalf("fork did not retain its shared object store: %v", err)
	}
	wantObjects := filepath.Join(source, "objects")
	if !strings.Contains(strings.TrimSpace(string(alternates)), wantObjects) {
		t.Fatalf("alternates = %q, want %q", alternates, wantObjects)
	}
	if remote := gitValue(context.Background(), destination, "remote"); remote != "" {
		t.Fatalf("independent fork retained a force-sync remote: %q", remote)
	}
	after, err := refsSHA256(context.Background(), source)
	if err != nil || after != before {
		t.Fatalf("hidden fork pins changed advertised source refs: %s -> %s (%v)",
			before, after, err)
	}
	pins := gitValue(context.Background(), source, "for-each-ref",
		"--format=%(refname)", "refs/forkmesh/fork-bases/"+job.RequestID)
	if pins == "" {
		t.Fatal("source objects were borrowed without GC-safe hidden pins")
	}

	loaded, _, err := loadGatewayConfig(gatewayPath)
	if err != nil {
		t.Fatal(err)
	}
	found := 0
	for _, repository := range loaded.Repositories {
		if repository.Owner == "mirror9" && repository.Name == "proj" {
			found++
			if repository.ForkedFrom != "alice/proj" ||
				repository.GitDir != destination ||
				repository.Integrity.ExpectedRefsSHA256 == "" {
				t.Fatalf("persisted fork is incomplete: %+v", repository)
			}
		}
	}
	if found != 1 {
		t.Fatalf("persisted fork count = %d", found)
	}
	// A lost HTTP acknowledgement retries the same job without cloning or
	// adding a second gateway row.
	if err := daemon.materializeFork(ctx, job); err != nil {
		t.Fatal(err)
	}
	if len(daemon.gateway.Repositories) != 3 {
		t.Fatalf("idempotent retry added repositories: %d",
			len(daemon.gateway.Repositories))
	}
}

func TestForkEventAndSafetyCheckAreBounded(t *testing.T) {
	daemon := &Daemon{
		forkJobsEnabled: true,
		forkJobsPending: true,
		syncRequests:    make(chan struct{}, 1),
	}
	if !daemon.shouldCheckForkJobs() || daemon.shouldCheckForkJobs() {
		t.Fatal("initial fork safety check was not consumed exactly once")
	}
	daemon.handleNodeEvent(Event{Topic: "forks", Repo: "mirror9/proj"})
	select {
	case <-daemon.syncRequests:
	default:
		t.Fatal("fork event did not queue a sync")
	}
	if !daemon.shouldCheckForkJobs() || daemon.shouldCheckForkJobs() {
		t.Fatal("fork event did not arm exactly one immediate drain")
	}
	daemon.mu.Lock()
	daemon.forkJobsLastCheck = time.Now().Add(-forkJobSafetyCheck - time.Second)
	daemon.mu.Unlock()
	if !daemon.shouldCheckForkJobs() {
		t.Fatal("lost-event safety interval did not arm a drain")
	}
}
