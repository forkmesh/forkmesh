package vault

import (
	"context"
	"testing"
)

func finalizeOn() *bool {
	on := true
	return &on
}

func TestCopyModeKeepsPlaintextAndSealsValues(t *testing.T) {
	h := newHarness(t)
	off := false
	h.module.Finalize = &off
	writeConf(t, h.core.Layout.SettingsFile(), map[string]any{"API_TOKEN": "s3cr3t", "PLAIN": "value"}, "")
	result, err := h.module.Migrate(context.Background())
	if err != nil {
		t.Fatalf("migrate: %v", err)
	}
	if result["state"] != "copied" {
		t.Fatalf("state %v", result["state"])
	}
	if h.writes != 0 {
		t.Fatalf("copy mode wrote the settings %d times", h.writes)
	}
	values, err := h.module.ActionVariables()
	if err != nil || values["API_TOKEN"] != "s3cr3t" || values["PLAIN"] != "value" {
		t.Fatalf("vault values %v %v", len(values), err)
	}
	if pending := PendingPlaintext(h.module.settings()); len(pending) != 2 {
		t.Fatalf("plaintext should stay, pending %d", len(pending))
	}
	t.Setenv("FORKMESH_VAULT_FINALIZE", "1")
	if !h.module.finalized() {
		t.Fatal("the environment switch should finalize")
	}
}
