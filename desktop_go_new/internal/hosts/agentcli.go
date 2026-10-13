package hosts

import (
	"context"
	"fmt"
	"strings"
	"sync"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	agentCliTranscriptCap = 8192
	agentCliBusyMessage   = "A mirror agent-CLI install is already running."
)

var (
	agentCliScriptHead = []string{
		"set -eu",
		"umask 077",
		"home=${HOME:-/root}",
	}
	agentCliScriptReadPayload = []string{
		"tmp=$(mktemp)",
		"cat > \"$tmp\"",
		"sec() { sed -n \"s/^$1 //p\" \"$tmp\" | base64 -d; }",
		"has() { grep -q \"^$1 \" \"$tmp\"; }",
	}
	agentCliScriptInstall = []string{
		"echo \"Installing Claude Code from claude.ai...\"",
		"curl -fsSL https://claude.ai/install.sh | bash",
		"echo \"Installing Codex from chatgpt.com...\"",
		"curl -fsSL https://chatgpt.com/codex/install.sh | sh",
		"export PATH=\"$home/.local/bin:$home/.claude/bin:$PATH\"",
		"agent_home=\"$home\"",
		"agent_owner=\"\"",
	}
	agentCliScriptServiceTemplate = "if test \"$(id -u)\" = 0 && id forkmesh-node >/dev/null 2>&1; then agent_home=$(getent passwd forkmesh-node | cut -d: -f6); agent_owner=forkmesh-node; %1; echo \"Installed agent CLIs for the forkmesh-node service.\"; fi"
	agentCliScriptCredentials     = []string{
		"if has claude; then mkdir -p \"$agent_home/.claude\"; sec claude > \"$agent_home/.claude/.credentials.json\"; chmod 600 \"$agent_home/.claude/.credentials.json\"; echo \"Copied the controller Claude Code login.\"; fi",
		"if has codex; then mkdir -p \"$agent_home/.codex\"; sec codex > \"$agent_home/.codex/auth.json\"; chmod 600 \"$agent_home/.codex/auth.json\"; echo \"Copied the controller Codex login.\"; fi",
		"if has env; then mkdir -p \"$agent_home/.forkmesh\"; sec env > \"$agent_home/.forkmesh/agent-env\"; chmod 600 \"$agent_home/.forkmesh/agent-env\"; for rc in \"$agent_home/.profile\" \"$agent_home/.bashrc\"; do touch \"$rc\"; grep -q .forkmesh/agent-env \"$rc\" || printf \"%s\\n\" \". \\\"$agent_home/.forkmesh/agent-env\\\"\" >> \"$rc\"; done; echo \"Installed the agent API keys in ~/.forkmesh/agent-env.\"; fi",
		"if test -n \"$agent_owner\"; then chown -R \"$agent_owner:$agent_owner\" \"$agent_home/.claude\" \"$agent_home/.codex\" \"$agent_home/.forkmesh\" \"$agent_home/.profile\" \"$agent_home/.bashrc\" 2>/dev/null || true; fi",
		"rm -f \"$tmp\"",
	}
	agentCliScriptVerify = []string{
		"echo \"Claude Code:\"",
		"command -v claude; claude --version",
		"echo \"Codex:\"",
		"command -v codex; codex --version",
	}
	agentCliScriptDoneSignedIn = "echo \"Installation complete. This mirror is signed in with the controller agent access.\""
	agentCliScriptDoneLogin    = "echo \"Installation complete. Provider login is still required on this mirror.\""
)

func agentCliBootstrapRemoteCommand(withCredentials bool) string {
	script := append([]string{}, agentCliScriptHead...)
	if withCredentials {
		script = append(script, agentCliScriptReadPayload...)
	}
	script = append(script, agentCliScriptInstall...)
	script = append(script, qtArg(agentCliScriptServiceTemplate, agentCliServicePublishScript))
	if withCredentials {
		script = append(script, agentCliScriptCredentials...)
	}
	script = append(script, agentCliScriptVerify...)
	if withCredentials {
		script = append(script, agentCliScriptDoneSignedIn)
	} else {
		script = append(script, agentCliScriptDoneLogin)
	}
	return "sh -lc '" + strings.Join(script, "; ") + "'"
}

type agentCredentials struct {
	Payload []byte `json:"payload"`
	Summary string `json:"summary"`
	Empty   bool   `json:"empty"`
	Error   string `json:"error"`
}

func wipeBytes(data []byte) {
	for i := range data {
		data[i] = 0
	}
}

func (m *Module) runAgentCliInstall(ctx context.Context, node, ip, user, sshPassword, identityFile string, copyCredentials bool, origin string, done func(ok bool, message string)) (*Job, string) {
	report := func(ok bool, message string) {
		if done != nil {
			done(ok, message)
		}
	}
	if m.jobRunning("agentClis") {
		report(false, agentCliBusyMessage)
		return nil, agentCliBusyMessage
	}
	var payload []byte
	summary := ""
	if copyCredentials {
		credentials := m.agentCredentials()
		if credentials.Empty {
			wipeBytes(credentials.Payload)
			message := fmt.Sprintf("This device has no Claude Code or Codex login to copy to %s. Sign in here first, then use Install Claude + Codex on that host.", node)
			report(false, message)
			return nil, message
		}
		if len(credentials.Payload) == 0 {
			wipeBytes(credentials.Payload)
			message := credentials.Error
			if message == "" {
				message = "This device has no Claude Code or Codex login to copy."
			}
			report(false, message)
			return nil, message
		}
		payload = credentials.Payload
		summary = credentials.Summary
	}
	command, err := buildHostSSHCommand(m.sshDir(), ip, user, sshPassword, agentCliBootstrapRemoteCommand(copyCredentials), identityFile)
	if err != nil {
		wipeBytes(payload)
		report(false, err.Error())
		return nil, err.Error()
	}
	job := m.startJob("agentClis", node, "", origin, installChannel)
	m.appendOutput(installChannel, fmt.Sprintf("Installing Claude Code and Codex on %s (%s@%s)...\n", node, user, ip))
	if identityFile == "" {
		m.appendOutput(installChannel, "The shared ForkMesh SSH key has not been generated on this device; using the current session credential or the system SSH agent.\n")
	} else {
		m.appendOutput(installChannel, fmt.Sprintf("Using the shared ForkMesh SSH identity (%s).\n", identityFile))
	}
	if copyCredentials {
		m.appendOutput(installChannel, fmt.Sprintf("Copying this device's agent access (%s) over the SSH session's stdin.\n", summary))
	}
	go func() {
		var outputMu sync.Mutex
		transcript := []byte{}
		reader := func() func([]byte) {
			carry := &utf8Carry{}
			return func(chunk []byte) {
				outputMu.Lock()
				defer outputMu.Unlock()
				transcript = append(transcript, chunk...)
				if len(transcript) > agentCliTranscriptCap {
					transcript = append([]byte(nil), transcript[len(transcript)-agentCliTranscriptCap:]...)
				}
				m.appendOutput(installChannel, carry.decode(chunk))
			}
		}
		var stdin []byte
		if len(payload) > 0 {
			stdin = payload
		}
		result := runProcess(job.Context(), procSpec{Program: command.Program, Args: command.Args, Env: command.Env, Stdin: stdin,
			OnOutput: reader(), Stderr: reader()})
		wipeBytes(payload)
		if !result.Started {
			message := "Could not start the pinned SSH installer."
			m.finishJob(job, false, message, map[string]any{"started": false})
			report(false, message)
			return
		}
		ok := result.OK()
		if ok {
			m.appendOutput(installChannel, "\nAgent CLI installation finished.\n")
		} else {
			m.appendOutput(installChannel, fmt.Sprintf("\nAgent CLI installation failed (exit %d).\n", result.ExitCode))
		}
		outputMu.Lock()
		output := strings.ToValidUTF8(string(transcript), "�")
		outputMu.Unlock()
		timedOut := containsFold(output, "Connection timed out")
		keyRejected := containsFold(output, "Permission denied")
		var message string
		switch {
		case ok && copyCredentials:
			message = fmt.Sprintf("Claude Code and Codex are installed on %s and signed in with this device's access.", node)
		case ok:
			message = fmt.Sprintf("Claude Code and Codex are installed on %s. Finish the provider sign-ins in the terminal that just opened.", node)
		case timedOut:
			message = fmt.Sprintf("SSH could not reach %s on port 22. The saved key was not reached; use the host's reachable public/stable address or connect this device to the private network, then retry.", ip)
		case keyRejected && identityFile != "":
			message = fmt.Sprintf("The mirror rejected the shared ForkMesh SSH key. Add that key to %s's authorized_keys, then retry.", node)
		default:
			message = fmt.Sprintf("Agent CLI installation failed on %s; see Live output.", node)
		}
		m.finishJob(job, ok, message, map[string]any{"exitCode": result.ExitCode})
		report(ok, message)
		m.broadcastUI("core.hosts.probeRequested", map[string]any{})
		if ok && !copyCredentials {
			m.broadcastUI("core.hosts.loginTerminal", map[string]any{"node": node, "ip": ip, "user": user})
		}
	}()
	return job, ""
}

func (m *Module) registerAgentMethods(desktop func(string, rpc.Handler)) {
	desktop("core.hosts.agentClis", m.handleAgentClis)
	desktop("core.hosts.actionsConfig", m.handleActionsConfig)
}

func (m *Module) handleAgentClis(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Node            string `json:"node"`
		IP              string `json:"ip"`
		User            string `json:"user"`
		Password        string `json:"password"`
		IdentityFile    string `json:"identityFile"`
		CopyCredentials bool   `json:"copyCredentials"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	identity := params.IdentityFile
	if identity == "" {
		identity = savedHostIdentityFile(m.savedHosts(), m.sshDir(), params.Node, params.IP, params.User)
	}
	m.clearOutput(installChannel)
	m.setStatus("install", fmt.Sprintf("Installing agent CLIs on %s...", params.Node))
	job, why := m.runAgentCliInstall(m.ctx, params.Node, params.IP, params.User, params.Password, identity, params.CopyCredentials, "desktop", func(ok bool, message string) {
		m.setStatus("install", message)
	})
	if job == nil {
		return map[string]any{"started": false, "job": "", "message": why}, nil
	}
	return map[string]any{"started": true, "job": job.ID, "message": ""}, nil
}
