package hosts

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	actionsMaxOutput      = 64 * 1024
	actionsResultPrefix   = "FORKMESH_ACTIONS_RESULT="
	actionsRemoteCommand  = "if command -v forkmesh >/dev/null 2>&1; then exec forkmesh --configure-mirror-actions-stdin; elif [ -x \"$HOME/.local/bin/forkmesh\" ]; then exec \"$HOME/.local/bin/forkmesh\" --configure-mirror-actions-stdin; else printf 'ForkMesh Actions helper is not installed.\\n' >&2; exit 127; fi"
	actionsTimeoutMessage = "[Actions] SSH configuration timed out after 45 seconds.\n"
)

var (
	actionsTimeout          = 45 * time.Second
	actionsRequestIDPattern = regexp.MustCompile(`^[a-f0-9]{32}\n?$`)
	actionsNodePattern      = regexp.MustCompile(`^[a-z][a-z0-9-]{0,62}$`)
	actionsVariablePattern  = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]{0,63}\n?$`)
	actionsEncodedPattern   = regexp.MustCompile(`^[A-Za-z0-9_-]+$`)
	actionsErrorCodePattern = regexp.MustCompile(`^[a-z][a-z0-9_]{0,63}$`)
	actionsResultKeys       = map[string]bool{"type": true, "schemaVersion": true, "ok": true, "requestId": true, "node": true,
		"actionsEnabled": true, "variablesReplaced": true, "variableCount": true, "errorCode": true}
)

type mirrorActionsRequest struct {
	RequestID        string            `json:"requestId"`
	Host             string            `json:"host"`
	SSHUser          string            `json:"sshUser"`
	NodeName         string            `json:"nodeName"`
	ActionsEnabled   bool              `json:"actionsEnabled"`
	ReplaceVariables bool              `json:"replaceVariables"`
	Variables        map[string]string `json:"variables"`
}

func sortedVariableNames(variables map[string]string) []string {
	names := make([]string, 0, len(variables))
	for name := range variables {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

func validateMirrorActionsConfigurationRequest(req mirrorActionsRequest) string {
	if !actionsRequestIDPattern.MatchString(req.RequestID) {
		return "The Actions request ID is invalid."
	}
	host := strings.TrimSpace(req.Host)
	if utf16Len(host) > 253 || strings.Contains(host, "..") || !sshHostPattern.MatchString(host) {
		return "The saved host address is invalid."
	}
	if !sshUserPattern.MatchString(strings.TrimSpace(req.SSHUser)) {
		return "The saved SSH username is invalid."
	}
	if !actionsNodePattern.MatchString(strings.TrimSpace(req.NodeName)) {
		return "The mirror node name is invalid."
	}
	if !req.ReplaceVariables && len(req.Variables) > 0 {
		return "Variable values require explicit replace-variables approval."
	}
	if len(req.Variables) > 64 {
		return "At most 64 Actions variables can be sent at once."
	}
	total := 0
	for _, name := range sortedVariableNames(req.Variables) {
		value := req.Variables[name]
		if !actionsVariablePattern.MatchString(name) {
			return "Actions variable names must use letters, digits, and underscores, and cannot start with a digit."
		}
		if strings.Contains(value, "\x00") {
			return "Actions variable values cannot contain NUL characters."
		}
		if len(value) > 16*1024 {
			return "Each Actions variable must be 16 KiB or smaller."
		}
		total += len(value)
		if total > 48*1024 {
			return "The combined Actions variable values must be 48 KiB or smaller."
		}
	}
	return ""
}

func qtJSONString(out *bytes.Buffer, text string) {
	const hexDigits = "0123456789abcdef"
	out.WriteByte('"')
	for _, r := range text {
		switch r {
		case '"':
			out.WriteString(`\"`)
		case '\\':
			out.WriteString(`\\`)
		case '\b':
			out.WriteString(`\b`)
		case '\f':
			out.WriteString(`\f`)
		case '\n':
			out.WriteString(`\n`)
		case '\r':
			out.WriteString(`\r`)
		case '\t':
			out.WriteString(`\t`)
		default:
			if r < 0x20 {
				out.WriteString(`\u00`)
				out.WriteByte(hexDigits[r>>4])
				out.WriteByte(hexDigits[r&0xF])
				continue
			}
			var encoded [utf8.UTFMax]byte
			n := utf8.EncodeRune(encoded[:], r)
			out.Write(encoded[:n])
		}
	}
	out.WriteByte('"')
}

func buildMirrorActionsConfigurationPayload(req mirrorActionsRequest) ([]byte, error) {
	if validation := validateMirrorActionsConfigurationRequest(req); validation != "" {
		return nil, errors.New(validation)
	}
	var out bytes.Buffer
	out.WriteString(`{"actionsEnabled":`)
	out.WriteString(strconv.FormatBool(req.ActionsEnabled))
	out.WriteString(`,"node":`)
	qtJSONString(&out, strings.TrimSpace(req.NodeName))
	out.WriteString(`,"requestId":`)
	qtJSONString(&out, req.RequestID)
	out.WriteString(`,"schemaVersion":1,"type":"forkmesh.mirror-actions-configuration"`)
	if req.ReplaceVariables {
		out.WriteString(`,"variables":{"mode":"replace","values":{`)
		for i, name := range sortedVariableNames(req.Variables) {
			if i > 0 {
				out.WriteByte(',')
			}
			qtJSONString(&out, name)
			out.WriteByte(':')
			qtJSONString(&out, req.Variables[name])
		}
		out.WriteString(`}}`)
	}
	out.WriteString("}\n")
	payload := out.Bytes()
	if len(payload) > 64*1024 {
		wipeBytes(payload)
		return nil, errors.New("The Actions configuration payload exceeds 64 KiB.")
	}
	return payload, nil
}

func parseMirrorActionsConfigurationResult(output []byte, expectedRequestID, expectedNode string) (map[string]any, string) {
	if len(output) > actionsMaxOutput {
		return nil, "The mirror Actions helper returned too much output."
	}
	var encoded []byte
	sentinels := 0
	for _, line := range bytes.Split(output, []byte{'\n'}) {
		line = bytes.TrimSuffix(line, []byte{'\r'})
		if !bytes.HasPrefix(line, []byte(actionsResultPrefix)) {
			continue
		}
		sentinels++
		encoded = line[len(actionsResultPrefix):]
	}
	if sentinels != 1 || len(encoded) == 0 || len(encoded) > 4096 {
		return nil, "The mirror Actions helper did not return one valid result."
	}
	if !actionsEncodedPattern.Match(encoded) {
		return nil, "The mirror Actions helper result encoding is invalid."
	}
	text := string(encoded)
	if len(text)%4 == 1 {
		text = text[:len(text)-1]
	}
	decoded, _ := base64.RawURLEncoding.DecodeString(text)
	failed := "The mirror Actions helper result failed validation."
	var result map[string]any
	if json.Unmarshal(decoded, &result) != nil || result == nil {
		return nil, failed
	}
	for key := range result {
		if !actionsResultKeys[key] {
			return nil, failed
		}
	}
	count, isNumber := result["variableCount"].(float64)
	validCount := isNumber && count >= 0 && count <= 64 && math.Floor(count) == count
	errorCode, _ := result["errorCode"].(string)
	validErrorCode := errorCode == "" || actionsErrorCodePattern.MatchString(errorCode)
	typeName, _ := result["type"].(string)
	schema, _ := result["schemaVersion"].(float64)
	_, okIsBool := result["ok"].(bool)
	requestID, _ := result["requestId"].(string)
	node, _ := result["node"].(string)
	_, enabledIsBool := result["actionsEnabled"].(bool)
	_, replacedIsBool := result["variablesReplaced"].(bool)
	if typeName != "forkmesh.mirror-actions-configuration-result" || schema != 1 || !okIsBool ||
		requestID != expectedRequestID || node != expectedNode || !enabledIsBool || !replacedIsBool || !validCount || !validErrorCode {
		return nil, failed
	}
	return result, ""
}

type actionsOutput struct {
	mu       sync.Mutex
	data     []byte
	overflow bool
}

func (o *actionsOutput) add(chunk []byte) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if len(o.data)+len(chunk) <= actionsMaxOutput {
		o.data = append(o.data, chunk...)
	} else {
		o.overflow = true
		remaining := actionsMaxOutput - len(o.data)
		if remaining < 0 {
			remaining = 0
		}
		o.data = append(o.data, chunk[:remaining]...)
	}
	wipeBytes(chunk)
}

func (m *Module) runHostActionsConfiguration(ctx context.Context, req mirrorActionsRequest, sshPassword, origin string, done func(ok bool, message string)) (*Job, string) {
	finish := func(ok bool, message string) {
		if done != nil {
			done(ok, message)
		}
	}
	if req.RequestID == "" {
		req.RequestID = newStreamID()
	}
	identity := savedHostIdentityFile(m.savedHosts(), m.sshDir(), req.NodeName, req.Host, req.SSHUser)
	stdin, err := buildMirrorActionsConfigurationPayload(req)
	var command sshCommand
	if err == nil {
		command, err = buildHostSSHCommand(m.sshDir(), req.Host, req.SSHUser, sshPassword, actionsRemoteCommand, identity)
		if err != nil {
			wipeBytes(stdin)
		}
	}
	if err != nil {
		m.setStatus("install", err.Error())
		finish(false, err.Error())
		return nil, err.Error()
	}
	requestID := req.RequestID
	node := strings.TrimSpace(req.NodeName)
	host := strings.TrimSpace(req.Host)
	enabled := req.ActionsEnabled
	replaceVariables := req.ReplaceVariables
	variableCount := len(req.Variables)
	req.Variables = nil
	job := m.startJob("actions", node, "", origin, installChannel)
	m.setStatus("install", fmt.Sprintf("Sending Actions state to %s over authenticated SSH…", node))
	state := "disabled"
	if enabled {
		state = "enabled"
	}
	sent := 0
	if replaceVariables {
		sent = variableCount
	}
	m.appendOutput(installChannel, fmt.Sprintf("\n[Actions] Sending %s state and %d variable name(s) to %s@%s; values are stdin-only and redacted.\n",
		state, sent, strings.TrimSpace(req.SSHUser), host))
	go func() {
		output := &actionsOutput{}
		result := runProcess(job.Context(), procSpec{Program: command.Program, Args: command.Args, Env: command.Env, Stdin: stdin,
			Timeout: actionsTimeout, OnOutput: output.add})
		wipeBytes(stdin)
		if !result.Started {
			m.appendOutput(installChannel, "[Actions] Could not start SSH. Install OpenSSH (and sshpass for password login).\n")
			message := "Mirror Actions configuration failed: could not start SSH."
			m.setStatus("install", message)
			m.finishJob(job, false, message, map[string]any{"started": false})
			finish(false, message)
			return
		}
		if result.TimedOut {
			m.appendOutput(installChannel, actionsTimeoutMessage)
		}
		output.mu.Lock()
		defer output.mu.Unlock()
		var parsed map[string]any
		resultError := ""
		if !output.overflow {
			parsed, resultError = parseMirrorActionsConfigurationResult(output.data, requestID, node)
		}
		confirmed := !result.Crashed && result.ExitCode == 0 && parsed != nil &&
			parsed["ok"] == true && parsed["actionsEnabled"] == enabled && parsed["variablesReplaced"] == replaceVariables &&
			parsed["variableCount"] == float64(sent)
		wipeBytes(output.data)
		output.data = nil
		var message string
		if confirmed {
			replaced := ""
			if replaceVariables {
				replaced = fmt.Sprintf(" and replaced %d variable(s)", variableCount)
			}
			message = fmt.Sprintf("Mirror %s confirmed Actions %s%s.", node, state, replaced)
			m.appendOutput(installChannel, "[Actions] "+message+"\n")
		} else {
			if output.overflow {
				resultError = "The remote helper returned more than 64 KiB."
			} else if resultError == "" {
				resultError = "The host did not confirm the requested state."
			}
			m.appendOutput(installChannel, fmt.Sprintf("[Actions] Configuration was not confirmed (exit %d): %s\n", result.ExitCode, resultError))
			message = "Mirror Actions configuration failed: " + resultError
		}
		m.setStatus("install", message)
		m.finishJob(job, confirmed, message, map[string]any{"exitCode": result.ExitCode, "confirmed": confirmed})
		finish(confirmed, message)
	}()
	return job, ""
}

func (m *Module) handleActionsConfig(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		mirrorActionsRequest
		Password string `json:"password"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	job, why := m.runHostActionsConfiguration(m.ctx, params.mirrorActionsRequest, params.Password, "desktop", nil)
	if job == nil {
		return map[string]any{"started": false, "job": "", "message": why}, nil
	}
	return map[string]any{"started": true, "job": job.ID, "message": ""}, nil
}
