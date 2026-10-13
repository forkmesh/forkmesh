package hosts

import (
	"encoding/base64"
	"encoding/json"
	"strings"
	"testing"
	"time"
)

const actionsTestRequestID = "0123456789abcdef0123456789abcdef"

func actionsResult(fields map[string]any) string {
	result := map[string]any{"type": "forkmesh.mirror-actions-configuration-result", "schemaVersion": 1, "ok": true,
		"requestId": actionsTestRequestID, "node": "mirror1", "actionsEnabled": true, "variablesReplaced": true, "variableCount": 2}
	for key, value := range fields {
		if value == nil {
			delete(result, key)
			continue
		}
		result[key] = value
	}
	raw, _ := json.Marshal(result)
	return base64.RawURLEncoding.EncodeToString(raw)
}

func actionsParams(fields map[string]any) map[string]any {
	params := map[string]any{"requestId": actionsTestRequestID, "host": "203.0.113.5", "sshUser": "root", "nodeName": "mirror1",
		"actionsEnabled": true, "replaceVariables": true, "variables": map[string]any{"B_TOKEN": "two", "A_NAME": "o\"ne\n\x01/é"},
		"password": ""}
	for key, value := range fields {
		params[key] = value
	}
	return params
}

func TestActionsRemoteCommandParity(t *testing.T) {
	if len(actionsRemoteCommand) != 281 || cliSHA(actionsRemoteCommand) != "ca345c5506bf89cb0f8ea76ccf30aeae85c423ac2ea78d088905ba9a411ab4f9" {
		t.Fatalf("remote command differs from the C++: %s", actionsRemoteCommand)
	}
}

func TestMirrorActionsPayloadBytes(t *testing.T) {
	req := mirrorActionsRequest{RequestID: actionsTestRequestID, Host: " 203.0.113.5 ", SSHUser: "root", NodeName: " mirror1 ",
		ActionsEnabled: false, ReplaceVariables: true, Variables: map[string]string{"Z": "\x7f\t\\", "A_NAME": "o\"ne\n\x01/é"}}
	payload, err := buildMirrorActionsConfigurationPayload(req)
	if err != nil {
		t.Fatal(err)
	}
	want := `{"actionsEnabled":false,"node":"mirror1","requestId":"` + actionsTestRequestID +
		`","schemaVersion":1,"type":"forkmesh.mirror-actions-configuration","variables":{"mode":"replace","values":{"A_NAME":"o\"ne\n\u0001/é","Z":"` + "\x7f" + `\t\\"}}}` + "\n"
	if string(payload) != want {
		t.Fatalf("payload\n got %q\nwant %q", payload, want)
	}
	req.ReplaceVariables = false
	req.Variables = nil
	req.ActionsEnabled = true
	payload, _ = buildMirrorActionsConfigurationPayload(req)
	if string(payload) != `{"actionsEnabled":true,"node":"mirror1","requestId":"`+actionsTestRequestID+`","schemaVersion":1,"type":"forkmesh.mirror-actions-configuration"}`+"\n" {
		t.Fatalf("no variables: %s", payload)
	}
	big := strings.Repeat("\x01", 16*1024)
	req = mirrorActionsRequest{RequestID: actionsTestRequestID, Host: "h", SSHUser: "root", NodeName: "mirror1", ReplaceVariables: true,
		Variables: map[string]string{"A": big, "B": big, "C": big}}
	if _, err := buildMirrorActionsConfigurationPayload(req); err == nil || err.Error() != "The Actions configuration payload exceeds 64 KiB." {
		t.Fatalf("escaped payload over 64 KiB: %v", err)
	}
}

func TestMirrorActionsValidation(t *testing.T) {
	base := func() mirrorActionsRequest {
		return mirrorActionsRequest{RequestID: actionsTestRequestID, Host: "203.0.113.5", SSHUser: "root", NodeName: "mirror1", ReplaceVariables: true}
	}
	many := map[string]string{}
	for i := 0; i < 65; i++ {
		many["V"+strings.Repeat("X", i)] = "v"
	}
	cases := []struct {
		mutate func(*mirrorActionsRequest)
		want   string
	}{
		{func(r *mirrorActionsRequest) { r.RequestID = "ABC" }, "The Actions request ID is invalid."},
		{func(r *mirrorActionsRequest) { r.Host = "a..b" }, "The saved host address is invalid."},
		{func(r *mirrorActionsRequest) { r.Host = strings.Repeat("a", 254) }, "The saved host address is invalid."},
		{func(r *mirrorActionsRequest) { r.SSHUser = "1root" }, "The saved SSH username is invalid."},
		{func(r *mirrorActionsRequest) { r.NodeName = "Mirror1" }, "The mirror node name is invalid."},
		{func(r *mirrorActionsRequest) { r.ReplaceVariables = false; r.Variables = map[string]string{"A": "1"} }, "Variable values require explicit replace-variables approval."},
		{func(r *mirrorActionsRequest) { r.Variables = many }, "At most 64 Actions variables can be sent at once."},
		{func(r *mirrorActionsRequest) { r.Variables = map[string]string{"1A": "x"} }, "Actions variable names must use letters, digits, and underscores, and cannot start with a digit."},
		{func(r *mirrorActionsRequest) { r.Variables = map[string]string{"A": "x\x00"} }, "Actions variable values cannot contain NUL characters."},
		{func(r *mirrorActionsRequest) { r.Variables = map[string]string{"A": strings.Repeat("é", 8193)} }, "Each Actions variable must be 16 KiB or smaller."},
		{func(r *mirrorActionsRequest) {
			r.Variables = map[string]string{"A": strings.Repeat("x", 16384), "B": strings.Repeat("x", 16384), "C": strings.Repeat("x", 16384), "D": "x"}
		}, "The combined Actions variable values must be 48 KiB or smaller."},
		{func(r *mirrorActionsRequest) { r.RequestID += "\n"; r.Variables = map[string]string{"TRAILING\n": "x"} }, ""},
	}
	for i, c := range cases {
		req := base()
		c.mutate(&req)
		if got := validateMirrorActionsConfigurationRequest(req); got != c.want {
			t.Fatalf("case %d: got %q want %q", i, got, c.want)
		}
	}
}

func TestParseMirrorActionsResult(t *testing.T) {
	good := actionsResult(nil)
	line := "FORKMESH_ACTIONS_RESULT="
	parsed, why := parseMirrorActionsConfigurationResult([]byte("noise\r\n"+line+good+"\r\nmore\n"), actionsTestRequestID, "mirror1")
	if why != "" || parsed["ok"] != true || parsed["variableCount"] != 2.0 {
		t.Fatalf("good: %v %q", parsed, why)
	}
	document, _ := base64.RawURLEncoding.DecodeString(good)
	aligned := base64.RawURLEncoding.EncodeToString(document)
	for len(aligned)%4 != 0 {
		document = append(document, ' ')
		aligned = base64.RawURLEncoding.EncodeToString(document)
	}
	if _, why := parseMirrorActionsConfigurationResult([]byte(line+aligned+"A"), actionsTestRequestID, "mirror1"); why != "" {
		t.Fatalf("a dangling sextet is dropped like Qt: %q", why)
	}
	failures := []struct {
		output string
		want   string
	}{
		{strings.Repeat("x", 64*1024+1), "The mirror Actions helper returned too much output."},
		{"nothing\n", "The mirror Actions helper did not return one valid result."},
		{line + good + "\n" + line + good + "\n", "The mirror Actions helper did not return one valid result."},
		{line + "\n", "The mirror Actions helper did not return one valid result."},
		{line + good + "==", "The mirror Actions helper result encoding is invalid."},
		{line + actionsResult(map[string]any{"extra": 1}), "The mirror Actions helper result failed validation."},
		{line + actionsResult(map[string]any{"node": "other"}), "The mirror Actions helper result failed validation."},
		{line + actionsResult(map[string]any{"variableCount": 1.5}), "The mirror Actions helper result failed validation."},
		{line + actionsResult(map[string]any{"variableCount": 65}), "The mirror Actions helper result failed validation."},
		{line + actionsResult(map[string]any{"errorCode": "Bad"}), "The mirror Actions helper result failed validation."},
		{line + actionsResult(map[string]any{"ok": "yes"}), "The mirror Actions helper result failed validation."},
		{line + actionsResult(map[string]any{"schemaVersion": 2}), "The mirror Actions helper result failed validation."},
		{line + base64.RawURLEncoding.EncodeToString([]byte("[1]")), "The mirror Actions helper result failed validation."},
	}
	for i, c := range failures {
		if parsed, why := parseMirrorActionsConfigurationResult([]byte(c.output), actionsTestRequestID, "mirror1"); parsed != nil || why != c.want {
			t.Fatalf("case %d: %v %q", i, parsed, why)
		}
	}
	if _, why := parseMirrorActionsConfigurationResult([]byte(line+actionsResult(map[string]any{"errorCode": 7})), actionsTestRequestID, "mirror1"); why != "" {
		t.Fatalf("a non-string errorCode reads as empty in Qt: %q", why)
	}
}

func TestActionsConfigurationConfirmed(t *testing.T) {
	f := newCliFixture(t)
	t.Setenv("FAKE_SSH_MODE", "actions")
	t.Setenv("FAKE_ACTIONS_RESULT", actionsResult(nil))
	started := f.call(t, "core.hosts.actionsConfig", actionsParams(nil))
	if started["started"] != true {
		t.Fatalf("started %v", started)
	}
	job := f.waitJob(t, started["job"].(string))
	want := "Mirror mirror1 confirmed Actions enabled and replaced 2 variable(s)."
	if !job.OK || job.Kind != "actions" || job.Message != want || f.mod.status("install") != want {
		t.Fatalf("job %+v status %q", job, f.mod.status("install"))
	}
	stdin := string(f.stdinBytes())
	wantStdin := `{"actionsEnabled":true,"node":"mirror1","requestId":"` + actionsTestRequestID +
		`","schemaVersion":1,"type":"forkmesh.mirror-actions-configuration","variables":{"mode":"replace","values":{"A_NAME":"o\"ne\n\u0001/é","B_TOKEN":"two"}}}` + "\n"
	if stdin != wantStdin {
		t.Fatalf("stdin\n got %q\nwant %q", stdin, wantStdin)
	}
	transcript := f.mod.transcriptText(installChannel)
	if !strings.Contains(transcript, "\n[Actions] Sending enabled state and 2 variable name(s) to root@203.0.113.5; values are stdin-only and redacted.\n[Actions] "+want+"\n") {
		t.Fatalf("transcript:\n%s", transcript)
	}
	if strings.Contains(transcript, "helper output") || strings.Contains(transcript, "B_TOKEN") {
		t.Fatalf("remote output rendered:\n%s", transcript)
	}
	log := f.sshLog()
	if !strings.Contains(log, "root@203.0.113.5 "+actionsRemoteCommand) || strings.Contains(log, "two") {
		t.Fatalf("ssh log %s", log)
	}
	t.Setenv("FAKE_ACTIONS_RESULT", actionsResult(map[string]any{"actionsEnabled": false, "variablesReplaced": false, "variableCount": 0, "requestId": nil}))
	started = f.call(t, "core.hosts.actionsConfig", actionsParams(map[string]any{"requestId": "", "actionsEnabled": false, "replaceVariables": false, "variables": map[string]any{}}))
	job = f.waitJob(t, started["job"].(string))
	if job.OK || job.Message != "Mirror Actions configuration failed: The mirror Actions helper result failed validation." {
		t.Fatalf("generated request id must be checked against the result: %+v", job)
	}
	sent := decode(t, json.RawMessage(f.stdinBytes()))
	if id, _ := sent["requestId"].(string); !actionsRequestIDPattern.MatchString(id) || id == actionsTestRequestID {
		t.Fatalf("generated request id %v", sent["requestId"])
	}
}

func TestActionsConfigurationNotConfirmed(t *testing.T) {
	f := newCliFixture(t)
	t.Setenv("FAKE_SSH_MODE", "actions")
	cases := []struct {
		result string
		exit   string
		line   string
		want   string
	}{
		{actionsResult(map[string]any{"ok": false, "errorCode": "helper_refused"}), "0", "(exit 0): The host did not confirm the requested state.", "Mirror Actions configuration failed: The host did not confirm the requested state."},
		{actionsResult(map[string]any{"variableCount": 1}), "0", "(exit 0): The host did not confirm the requested state.", "Mirror Actions configuration failed: The host did not confirm the requested state."},
		{actionsResult(nil), "3", "(exit 3): The host did not confirm the requested state.", "Mirror Actions configuration failed: The host did not confirm the requested state."},
		{"", "0", "(exit 0): The mirror Actions helper did not return one valid result.", "Mirror Actions configuration failed: The mirror Actions helper did not return one valid result."},
	}
	for i, c := range cases {
		t.Setenv("FAKE_ACTIONS_RESULT", c.result)
		t.Setenv("FAKE_ACTIONS_EXIT", c.exit)
		started := f.call(t, "core.hosts.actionsConfig", actionsParams(nil))
		job := f.waitJob(t, started["job"].(string))
		if job.OK || job.Message != c.want || f.mod.status("install") != c.want {
			t.Fatalf("case %d: %+v", i, job)
		}
		if !strings.Contains(f.mod.transcriptText(installChannel), "[Actions] Configuration was not confirmed "+c.line+"\n") {
			t.Fatalf("case %d transcript:\n%s", i, f.mod.transcriptText(installChannel))
		}
	}
}

func TestActionsConfigurationOverflowTimeoutAndRefusals(t *testing.T) {
	f := newCliFixture(t)
	t.Setenv("FAKE_SSH_MODE", "flood")
	started := f.call(t, "core.hosts.actionsConfig", actionsParams(nil))
	job := f.waitJob(t, started["job"].(string))
	if job.OK || job.Message != "Mirror Actions configuration failed: The remote helper returned more than 64 KiB." {
		t.Fatalf("flood %+v", job)
	}
	if !strings.Contains(f.mod.transcriptText(installChannel), "[Actions] Configuration was not confirmed (exit 0): The remote helper returned more than 64 KiB.\n") {
		t.Fatalf("flood transcript:\n%s", f.mod.transcriptText(installChannel))
	}

	previous := actionsTimeout
	actionsTimeout = 300 * time.Millisecond
	t.Setenv("FAKE_SSH_MODE", "hang")
	started = f.call(t, "core.hosts.actionsConfig", actionsParams(nil))
	second := f.call(t, "core.hosts.actionsConfig", actionsParams(nil))
	job = f.waitJob(t, started["job"].(string))
	other := f.waitJob(t, second["job"].(string))
	actionsTimeout = previous
	if job.OK || other.OK || job.Message != "Mirror Actions configuration failed: The mirror Actions helper did not return one valid result." {
		t.Fatalf("timeout %+v", job)
	}
	transcript := f.mod.transcriptText(installChannel)
	if !strings.Contains(transcript, actionsTimeoutMessage+"[Actions] Configuration was not confirmed (exit 9): The mirror Actions helper did not return one valid result.\n") {
		t.Fatalf("timeout transcript:\n%s", transcript)
	}

	result := f.call(t, "core.hosts.actionsConfig", actionsParams(map[string]any{"nodeName": "Mirror1"}))
	if result["started"] != false || result["message"] != "The mirror node name is invalid." || f.mod.status("install") != "The mirror node name is invalid." {
		t.Fatalf("invalid node %v", result)
	}
	result = f.call(t, "core.hosts.actionsConfig", actionsParams(map[string]any{"replaceVariables": false}))
	if result["started"] != false || result["message"] != "Variable values require explicit replace-variables approval." {
		t.Fatalf("unapproved variables %v", result)
	}
	t.Setenv("PATH", t.TempDir())
	started = f.call(t, "core.hosts.actionsConfig", actionsParams(nil))
	job = f.waitJob(t, started["job"].(string))
	if job.OK || job.Message != "Mirror Actions configuration failed: could not start SSH." ||
		!strings.Contains(f.mod.transcriptText(installChannel), "[Actions] Could not start SSH. Install OpenSSH (and sshpass for password login).\n") {
		t.Fatalf("unstartable %+v", job)
	}
}
