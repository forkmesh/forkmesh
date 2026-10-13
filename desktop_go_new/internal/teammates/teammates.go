package teammates

import (
	"context"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/rpc"
)

var knownTaskTypes = []string{"scheduled", "new-issues", "new-pulls", "assignments", "mentions", "ui-stalls", "error-logs"}

var taskTypeLabels = map[string]string{
	"on-demand":   "On-demand work",
	"scheduled":   "Scheduled work",
	"new-issues":  "New issues",
	"new-pulls":   "New pull requests",
	"assignments": "Assignments",
	"mentions":    "Mentions",
	"ui-stalls":   "UI stall repair",
	"error-logs":  "Error-log repair",
}

var scheduleModes = []string{"on-demand", "continuous", "hourly", "daily", "weekly", "weekdays", "custom", "events"}

var editableFields = []string{"name", "avatar", "specialty", "focus", "description", "instructions",
	"provider", "model", "mode", "skills", "backupModels", "taskAgents", "schedule", "relayChannel",
	"enabled", "selfImproving"}

type Module struct {
	Owners func() []string

	gate *phonefamily.Gate
}

func New() *Module { return &Module{} }

func (m *Module) Name() string { return "teammates" }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.gate = &phonefamily.Gate{Core: core, Owners: m.Owners}
	core.Registry.Handle("teammates.options", m.Name(), m.gate.Wrap(m.handleOptions))
	return nil
}

func options() map[string]any {
	taskTypes := make([]any, 0, len(knownTaskTypes))
	for _, id := range knownTaskTypes {
		taskTypes = append(taskTypes, map[string]any{"id": id, "label": taskTypeLabels[id]})
	}
	return map[string]any{
		"providers":     []string{"codex", "claude-code", "openai", "claude-api"},
		"modes":         []string{"", "Auto", "Plan", "Ask"},
		"scheduleModes": scheduleModes,
		"taskTypes":     taskTypes,
		"editable":      editableFields,
	}
}

func (m *Module) handleOptions(ctx context.Context, req *rpc.Request) (any, error) {
	return options(), nil
}
