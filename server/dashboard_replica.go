package mirrornode

// Public dashboard projection refresh.
//
// The Python gateway owns the shared Worker/mirror schema and atomic disk
// format. The daemon only schedules that bounded helper alongside repository
// synchronization, avoiding a second Go copy of the public API allowlist.

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os/exec"
	"strings"
	"time"
)

// The node-event WebSocket is the normal invalidation path. This interval is
// only a dropped-frame/controller-restart safety read; keeping it long avoids
// turning an idle independent mirror into a constant controller poller.
const dashboardReplicaRefreshInterval = 30 * time.Minute

type dashboardReplicaConfig struct {
	Enabled     bool   `json:"enabled"`
	ReplicaPath string `json:"replicaPath"`
}

func (d *Daemon) dashboardReplicaConfigured() bool {
	if len(d.gateway.Dashboard) == 0 {
		return false
	}
	var config dashboardReplicaConfig
	if err := json.Unmarshal(d.gateway.Dashboard, &config); err != nil {
		return false
	}
	return config.Enabled && strings.TrimSpace(config.ReplicaPath) != ""
}

func (d *Daemon) dashboardReplicaRefreshDue(now time.Time) bool {
	if !d.dashboardReplicaConfigured() {
		return false
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.dashboardReplicaPending {
		d.dashboardReplicaPending = false
		d.dashboardReplicaLastAttempt = now
		return true
	}
	if !d.dashboardReplicaLastAttempt.IsZero() {
		elapsed := now.Sub(d.dashboardReplicaLastAttempt)
		if elapsed >= 0 && elapsed < dashboardReplicaRefreshInterval {
			return false
		}
	}
	d.dashboardReplicaLastAttempt = now
	return true
}

func (d *Daemon) refreshDashboardReplica(ctx context.Context) error {
	if !d.dashboardReplicaConfigured() {
		return nil
	}
	command := exec.CommandContext(
		ctx,
		d.config.Python,
		d.config.GatewayScript,
		"--config",
		d.config.GatewayConfig,
		"--sync-dashboard-replica",
	)
	output, err := command.CombinedOutput()
	if err != nil {
		message := boundedText(output, 240)
		if message == "" {
			message = err.Error()
		}
		return fmt.Errorf("refresh public projections: %s", message)
	}
	if len(output) == 0 {
		return errors.New("refresh public projections returned no result")
	}
	var summary struct {
		OK        bool `json:"ok"`
		Refreshed int  `json:"refreshed"`
	}
	if err := json.Unmarshal(output, &summary); err != nil || !summary.OK {
		return errors.New("refresh public projections returned an invalid result")
	}
	// The helper atomically replaced the durable bundle. Tell the long-lived
	// gateway to preload that revision once; every request after this signal is
	// served from memory without a per-request stat/read of the replica file.
	if summary.Refreshed > 0 && d.supervisor != nil {
		d.supervisor.Invalidate("gateway")
	}
	return nil
}
