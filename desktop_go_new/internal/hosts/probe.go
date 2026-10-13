package hosts

import (
	"context"
	"strings"
	"time"
)

const (
	probeTimeout   = 35 * time.Second
	probeMaxOutput = 4096
)

const probeRemoteCommand = `sh -lc 'probe_home="$HOME"; if test "$(id -u)" = 0 && id forkmesh-node >/dev/null 2>&1; then probe_home=$(getent passwd forkmesh-node | cut -d: -f6); fi; export PATH="$probe_home/.local/bin:$probe_home/.claude/bin:$PATH"; mirror=0; mirror_service=0; if command -v forkmesh-mirror-node >/dev/null 2>&1 || test -x /usr/local/bin/forkmesh-mirror-node; then mirror=1; fi; if systemctl is-active --quiet forkmesh-mirror-node.service >/dev/null 2>&1; then mirror_service=1; fi; printf "FORKMESH=%s MIRROR=%s MIRRORSVC=%s CLAUDE=%s CODEX=%s\\n" "$(command -v forkmesh >/dev/null 2>&1 && echo 1 || echo 0)" "$mirror" "$mirror_service" "$(command -v claude >/dev/null 2>&1 && echo 1 || echo 0)" "$(command -v codex >/dev/null 2>&1 && echo 1 || echo 0)"'`

type probeOutcome struct {
	Key         string `json:"key"`
	Name        string `json:"name"`
	IP          string `json:"ip"`
	User        string `json:"user"`
	Status      string `json:"status"`
	Claude      string `json:"claude"`
	Codex       string `json:"codex"`
	Color       string `json:"color"`
	ClaudeColor string `json:"claudeColor,omitempty"`
	CodexColor  string `json:"codexColor,omitempty"`
	Tooltip     string `json:"tooltip,omitempty"`
	TooltipKind string `json:"tooltipKind,omitempty"`
	Online      bool   `json:"online"`
	At          int64  `json:"at"`
}

const (
	dash       = "—"
	colorGreen = "#2da44e"
	colorAmber = "#d29922"
	colorRed   = "#cf222e"
	colorGrey  = "#8b949e"
)

func probeBuildFailure(err error) probeOutcome {
	return probeOutcome{Status: "● Attention", Claude: dash, Codex: dash, Color: colorAmber, Tooltip: err.Error(), TooltipKind: "raw"}
}

func interpretProbe(result runResult, savedStatus string) probeOutcome {
	if !result.Started {
		return probeOutcome{Status: "● Attention · SSH unavailable", Claude: dash, Codex: dash, Color: colorRed}
	}
	output := strings.ToValidUTF8(string(result.Output), "�")
	if result.OK && strings.Contains(output, "FORKMESH=") {
		forkmesh := strings.Contains(output, "FORKMESH=1")
		mirrorNode := strings.Contains(output, "MIRROR=1")
		mirrorRunning := strings.Contains(output, "MIRRORSVC=1")
		claude := strings.Contains(output, "CLAUDE=1")
		codex := strings.Contains(output, "CODEX=1")
		healthy := forkmesh || (mirrorNode && mirrorRunning)
		status := "● Online · ForkMesh missing"
		switch {
		case forkmesh:
			status = "● Online"
		case mirrorNode && mirrorRunning:
			status = "● Online · Mirror node"
		case mirrorNode:
			status = "● Attention · Mirror node stopped"
		}
		outcome := probeOutcome{Status: status, Claude: "Not installed", Codex: "Not installed", Color: colorAmber,
			ClaudeColor: colorGrey, CodexColor: colorGrey, Online: true}
		if healthy {
			outcome.Color = colorGreen
		}
		if claude {
			outcome.Claude = "✓ Installed"
			outcome.ClaudeColor = colorGreen
		}
		if codex {
			outcome.Codex = "✓ Installed"
			outcome.CodexColor = colorGreen
		}
		return outcome
	}
	rejected := containsFold(output, "Permission denied")
	timedOut := result.TimedOut || containsFold(output, "Connection timed out")
	provisioning := containsFold(savedStatus, "install") || containsFold(savedStatus, "vultr") || savedStatus == "added"
	outcome := probeOutcome{Claude: dash, Codex: dash, Color: colorGrey, Tooltip: output, TooltipKind: "preview"}
	switch {
	case rejected:
		outcome.Status = "● Attention · SSH key rejected"
		outcome.Color = colorRed
	case timedOut && provisioning:
		outcome.Status = "◌ Provisioning · waiting for SSH"
	default:
		outcome.Status = "● Offline · unreachable"
	}
	return outcome
}

func (m *Module) probeHost(ctx context.Context, name, ip, user, savedStatus, password, identity string) probeOutcome {
	command, err := buildHostSSHCommand(m.sshDir(), ip, user, password, probeRemoteCommand, identity)
	var outcome probeOutcome
	if err != nil {
		outcome = probeBuildFailure(err)
	} else {
		outcome = interpretProbe(runSSH(ctx, command, probeTimeout, probeMaxOutput, true), savedStatus)
	}
	outcome.Key = credentialKey(name, ip, user)
	outcome.Name, outcome.IP, outcome.User = name, ip, user
	outcome.At = m.now().UnixMilli()
	return outcome
}
