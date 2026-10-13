package mirrornode

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestDashboardReplicaRefreshUsesGatewaySharedContractCommand(t *testing.T) {
	directory := t.TempDir()
	arguments := filepath.Join(directory, "arguments")
	program := filepath.Join(directory, "python")
	script := "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$FORKMESH_TEST_ARGUMENTS\"\nprintf '{\"ok\":true}\\n'\n"
	if err := os.WriteFile(program, []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	dashboard, err := json.Marshal(map[string]any{
		"enabled":          true,
		"assetsRepository": "forkmesh/forkmesh",
		"canonicalOrigin":  "https://app.forkmesh.com",
		"replicaPath":      filepath.Join(directory, "replica.json"),
	})
	if err != nil {
		t.Fatal(err)
	}
	daemon := &Daemon{
		config: Config{
			Python:        program,
			GatewayScript: "/opt/forkmesh/mirror_gateway.py",
			GatewayConfig: "/var/lib/forkmesh/gateway.json",
		},
		gateway: GatewayConfig{Dashboard: dashboard},
	}
	t.Setenv("FORKMESH_TEST_ARGUMENTS", arguments)
	if err := daemon.refreshDashboardReplica(context.Background()); err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(arguments)
	if err != nil {
		t.Fatal(err)
	}
	got := strings.Fields(string(raw))
	want := []string{
		"/opt/forkmesh/mirror_gateway.py",
		"--config",
		"/var/lib/forkmesh/gateway.json",
		"--sync-dashboard-replica",
	}
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Fatalf("unexpected helper arguments: %#v", got)
	}
}

func TestDashboardReplicaRefreshIsDisabledWithoutReplicaPath(t *testing.T) {
	dashboard := json.RawMessage(
		`{"enabled":true,"assetsRepository":"forkmesh/forkmesh"}`)
	daemon := &Daemon{gateway: GatewayConfig{Dashboard: dashboard}}
	if daemon.dashboardReplicaConfigured() {
		t.Fatal("dashboard without replicaPath enabled synchronization")
	}
	if err := daemon.refreshDashboardReplica(context.Background()); err != nil {
		t.Fatal(err)
	}
}

func TestDashboardReplicaRefreshAttemptsAreRateLimited(t *testing.T) {
	dashboard := json.RawMessage(
		`{"enabled":true,"assetsRepository":"forkmesh/forkmesh","replicaPath":"/var/lib/forkmesh/replica.json"}`)
	daemon := &Daemon{gateway: GatewayConfig{Dashboard: dashboard}}
	now := time.Unix(2_000_000_000, 0)
	if !daemon.dashboardReplicaRefreshDue(now) {
		t.Fatal("first configured refresh was not due")
	}
	if daemon.dashboardReplicaRefreshDue(now.Add(29 * time.Minute)) {
		t.Fatal("refresh repeated before the bounded interval")
	}
	if !daemon.dashboardReplicaRefreshDue(now.Add(30 * time.Minute)) {
		t.Fatal("refresh did not become due after the interval")
	}
}

func TestDashboardReplicaWebSocketInvalidationRefreshesImmediately(t *testing.T) {
	dashboard := json.RawMessage(
		`{"enabled":true,"assetsRepository":"forkmesh/forkmesh","replicaPath":"/var/lib/forkmesh/replica.json"}`)
	daemon := &Daemon{
		gateway:      GatewayConfig{Dashboard: dashboard},
		syncRequests: make(chan struct{}, 1),
	}
	now := time.Unix(2_000_000_000, 0)
	if !daemon.dashboardReplicaRefreshDue(now) {
		t.Fatal("first configured refresh was not due")
	}
	daemon.handleNodeEvent(Event{Topic: "replica", Repo: "forkmesh/forkmesh"})
	if !daemon.dashboardReplicaRefreshDue(now.Add(time.Second)) {
		t.Fatal("WebSocket invalidation did not bypass the safety interval")
	}
	if daemon.dashboardReplicaRefreshDue(now.Add(2 * time.Second)) {
		t.Fatal("one WebSocket invalidation caused more than one refresh")
	}
}
