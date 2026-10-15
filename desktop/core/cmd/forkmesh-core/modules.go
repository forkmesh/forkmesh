package main

import (
	"forkmesh.com/desktop-core/internal/actions"
	"forkmesh.com/desktop-core/internal/agentmodel"
	"forkmesh.com/desktop-core/internal/agentproc"
	"forkmesh.com/desktop-core/internal/agentrun"
	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/backup"
	"forkmesh.com/desktop-core/internal/cleanup"
	"forkmesh.com/desktop-core/internal/control"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/diag"
	"forkmesh.com/desktop-core/internal/files"
	"forkmesh.com/desktop-core/internal/hosts"
	"forkmesh.com/desktop-core/internal/insights"
	"forkmesh.com/desktop-core/internal/mergequeue"
	"forkmesh.com/desktop-core/internal/mesh"
	"forkmesh.com/desktop-core/internal/netlog"
	"forkmesh.com/desktop-core/internal/plugins"
	"forkmesh.com/desktop-core/internal/pulls"
	"forkmesh.com/desktop-core/internal/quality"
	"forkmesh.com/desktop-core/internal/relay"
	"forkmesh.com/desktop-core/internal/repos"
	"forkmesh.com/desktop-core/internal/secrets"
	"forkmesh.com/desktop-core/internal/stats"
	"forkmesh.com/desktop-core/internal/teammates"
	"forkmesh.com/desktop-core/internal/terminal"
	"forkmesh.com/desktop-core/internal/update"
	"forkmesh.com/desktop-core/internal/vault"
)

func modules() []daemon.Module {
	return []daemon.Module{
		control.New(),
		relay.New(),
		mesh.New(),
		agents.New(),
		agentproc.New(),
		agentrun.New(),
		agentmodel.New(),
		terminal.New(),
		repos.NewWithUILocate(),
		diag.New(),
		files.New(),
		hosts.New(),
		stats.New(),
		insights.New(),
		netlog.New(),
		quality.New(),
		vault.New(),
		secrets.New(),
		teammates.New(),
		update.New(),
		backup.New(),
		cleanup.New(),
		actions.New(),
		mergequeue.New(),
		pulls.New(),
		plugins.New(),
	}
}
