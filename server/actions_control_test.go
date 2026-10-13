package mirrornode

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func readEnvelope(t *testing.T, path string) []byte {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func TestActionsControlEnvelopeAcceptsEveryAction(t *testing.T) {
	f := newActionsFixture(t)
	now := time.Now()
	for index, body := range []map[string]any{
		{"action": "dispatch", "workflow": ".forkmesh/ci.yml", "ref": "refs/heads/release/1.2"},
		{"action": "rerun", "runId": 7},
		{"action": "cancel", "runId": 7},
		{"action": "approve", "runId": 7},
		{"action": "reject", "runId": 9007199254740991},
		{"action": "set-workflow-enabled", "workflow": ".forkmesh/ci.yaml", "enabled": false},
		{"action": "set-repository-enabled", "enabled": true},
	} {
		id := "accept_request_00000" + string(rune('a'+index))
		raw := readEnvelope(t, f.writeEnvelope(id, body, now.UnixMilli(), nil))
		request, verdict, err := parseActionsControlEnvelope(raw, "mirror9", f.routerID, now)
		if verdict != actionsControlAccepted || err != nil {
			t.Fatalf("%v: %v %v", body, verdict, err)
		}
		// Owner and repository come from the signed route, never the body.
		if request.RequestID != id || request.Action != body["action"] ||
			request.Owner != "alice" || request.Repository != "proj" || request.Actor != "alice" {
			t.Fatalf("request = %#v", request)
		}
	}
}

// A courier can drop a request. It must not be able to author, alter,
// redirect or replay one.
func TestActionsControlEnvelopeRefusesWhatTheRouterDidNotSign(t *testing.T) {
	f := newActionsFixture(t)
	now := time.Now()
	body := map[string]any{"action": "cancel", "runId": 7}
	_, otherKey, _ := ed25519.GenerateKey(nil)
	capability := func(envelope map[string]any) map[string]any {
		return envelope["capability"].(map[string]any)
	}
	for name, mutate := range map[string]func(map[string]any){
		"altered body": func(e map[string]any) {
			e["body"] = strings.Replace(e["body"].(string), `"runId":7`, `"runId":8`, 1)
		},
		"another repository": func(e map[string]any) { e["repository"] = "other" },
		"another owner":      func(e map[string]any) { e["owner"] = "mallory" },
		"another node":       func(e map[string]any) { e["node"] = "mirror10" },
		"route rewritten to match a forged repository": func(e map[string]any) {
			e["repository"] = "other"
			capability(e)["path"] = "/v1/repositories/alice/other/actions-control"
		},
		"another operation's route": func(e map[string]any) {
			capability(e)["path"] = "/v1/repositories/alice/proj/merge-pull"
		},
		"GET": func(e map[string]any) { capability(e)["method"] = "GET" },
		"re-dated capability": func(e map[string]any) {
			capability(e)["issuedAt"] = capability(e)["issuedAt"].(int64) + 1
		},
		"another capability id": func(e map[string]any) { capability(e)["requestId"] = "capability_other_id" },
		"signed by another key": func(e map[string]any) {
			capability(e)["signature"] = base64.RawURLEncoding.EncodeToString(
				ed25519.Sign(otherKey, []byte("anything")))
		},
		"empty signature":    func(e map[string]any) { capability(e)["signature"] = "" },
		"extra envelope key": func(e map[string]any) { e["extra"] = true },
		"extra capability key": func(e map[string]any) {
			capability(e)["extra"] = true
		},
		"unknown envelope type": func(e map[string]any) { e["type"] = "forkmesh.other" },
		"version as a string":   func(e map[string]any) { e["schemaVersion"] = "1" },
		"version as a boolean":  func(e map[string]any) { e["schemaVersion"] = true },
		"body as an object":     func(e map[string]any) { e["body"] = map[string]any{"action": "cancel"} },
		"path traversal owner":  func(e map[string]any) { e["owner"] = ".." },
	} {
		raw := readEnvelope(t, f.writeEnvelope("tamper_request_00001", body, now.UnixMilli(), mutate))
		request, verdict, err := parseActionsControlEnvelope(raw, "mirror9", f.routerID, now)
		if verdict != actionsControlInvalid || err == nil || request.RequestID != "" {
			t.Fatalf("%s: verdict=%v err=%v request=%#v", name, verdict, err, request)
		}
	}
	good := readEnvelope(t, f.writeEnvelope("tamper_request_00001", body, now.UnixMilli(), nil))
	if _, verdict, _ := parseActionsControlEnvelope(good, "mirror9", "", now); verdict != actionsControlInvalid {
		t.Fatal("no router key means nothing can be verified")
	}
	if _, verdict, _ := parseActionsControlEnvelope(append(good, make([]byte, maxActionsControlEnvelope)...),
		"mirror9", f.routerID, now); verdict != actionsControlInvalid {
		t.Fatal("an oversize envelope is refused")
	}
}

// A request the router really signed, but that is stale or malformed, is
// answerable: the browser is told it was refused.
func TestActionsControlEnvelopeRejectsSignedButUnusableRequests(t *testing.T) {
	f := newActionsFixture(t)
	now := time.Now()
	stale := readEnvelope(t, f.writeEnvelope("stale_request_000001",
		map[string]any{"action": "cancel", "runId": 7}, now.Add(-11*time.Minute).UnixMilli(), nil))
	request, verdict, _ := parseActionsControlEnvelope(stale, "mirror9", f.routerID, now)
	if verdict != actionsControlRejected || request.RequestID != "stale_request_000001" {
		t.Fatalf("stale: %v %#v", verdict, request)
	}
	future := readEnvelope(t, f.writeEnvelope("future_request_00001",
		map[string]any{"action": "cancel", "runId": 7}, now.Add(2*time.Minute).UnixMilli(), nil))
	if _, verdict, _ := parseActionsControlEnvelope(future, "mirror9", f.routerID, now); verdict != actionsControlRejected {
		t.Fatalf("future-dated: %v", verdict)
	}
	for name, body := range map[string]map[string]any{
		"missing field":      {"action": "cancel"},
		"extra field":        {"action": "cancel", "runId": 7, "ref": "refs/heads/main"},
		"run id as string":   {"action": "cancel", "runId": "7"},
		"run id as float":    {"action": "cancel", "runId": 7.5},
		"run id zero":        {"action": "cancel", "runId": 0},
		"enabled as integer": {"action": "set-repository-enabled", "enabled": 1},
		"workflow elsewhere": {"action": "dispatch", "workflow": "ci.yml", "ref": "refs/heads/main"},
		"workflow traversal": {"action": "dispatch", "workflow": ".forkmesh/../x.yml", "ref": "refs/heads/main"},
		"tag ref":            {"action": "dispatch", "workflow": ".forkmesh/ci.yml", "ref": "refs/tags/v1"},
		"ref with dots":      {"action": "dispatch", "workflow": ".forkmesh/ci.yml", "ref": "refs/heads/a..b"},
		"ref lock suffix":    {"action": "dispatch", "workflow": ".forkmesh/ci.yml", "ref": "refs/heads/a.lock"},
		"ref option":         {"action": "dispatch", "workflow": ".forkmesh/ci.yml", "ref": "--upload-pack=x"},
		"bad actor":          {"action": "cancel", "runId": 7, "actor": "Not An Account"},
	} {
		raw := readEnvelope(t, f.writeEnvelope("reject_request_00001", body, now.UnixMilli(), nil))
		request, verdict, err := parseActionsControlEnvelope(raw, "mirror9", f.routerID, now)
		if verdict != actionsControlRejected || err == nil || request.RequestID != "reject_request_00001" {
			t.Fatalf("%s: verdict=%v err=%v", name, verdict, err)
		}
	}
	// An unknown action names nothing that could be answered.
	raw := readEnvelope(t, f.writeEnvelope("unknown_request_0001",
		map[string]any{"action": "delete-everything"}, now.UnixMilli(), nil))
	if _, verdict, _ := parseActionsControlEnvelope(raw, "mirror9", f.routerID, now); verdict != actionsControlInvalid {
		t.Fatalf("unknown action: %v", verdict)
	}
}

func TestActionsControlDirectoryScanTrustsOnlyPlainRequestFiles(t *testing.T) {
	root := t.TempDir()
	directory := filepath.Join(root, actionsControlDirectoryName)
	if err := os.MkdirAll(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	write := func(name, content string) string {
		path := filepath.Join(directory, name)
		if err := os.WriteFile(path, []byte(content), 0o640); err != nil {
			t.Fatal(err)
		}
		return path
	}
	good := write("plain_request_000001.json", "{}")
	write(".plain_request_000001.json.tmp", "{}") // the gateway's in-flight file
	write("short.json", "{}")
	write("notes.txt", "{}")
	outside := filepath.Join(root, "outside.json")
	if err := os.WriteFile(outside, []byte(`{"stolen":true}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(directory, "linked_request_00001.json")); err != nil {
		t.Fatal(err)
	}
	if err := os.Link(good, filepath.Join(directory, "second_link_req_0001.json")); err != nil {
		t.Fatal(err)
	}
	write("oversize_request_001.json", strings.Repeat("x", maxActionsControlEnvelope+1))

	problems := map[string]string{}
	for _, file := range scanActionsControlDirectory(directory, maxActionsControlFiles) {
		problems[file.Name] = file.Problem
		if file.Problem != "" && file.Bytes != nil {
			t.Fatalf("%s: a refused file must not be read", file.Name)
		}
	}
	if len(problems) != 4 {
		t.Fatalf("only request-named files are considered: %v", problems)
	}
	for name, want := range map[string]string{
		"linked_request_00001.json": "following a link",
		"second_link_req_0001.json": "more than one link",
		"plain_request_000001.json": "more than one link",
		"oversize_request_001.json": "out of bounds",
	} {
		if !strings.Contains(problems[name], want) {
			t.Fatalf("%s: %q", name, problems[name])
		}
	}
	// A directory reached through a link, or with another name, is not scanned.
	alias := filepath.Join(root, "alias")
	if err := os.Symlink(directory, alias); err != nil {
		t.Fatal(err)
	}
	if scanActionsControlDirectory(alias, 8) != nil || scanActionsControlDirectory(root, 8) != nil {
		t.Fatal("only a real directory named actions-control is scanned")
	}
	// Removal never follows the link out of the directory.
	removeActionsControlFile(directory, "linked_request_00001.json")
	if _, err := os.Stat(outside); err != nil {
		t.Fatal("removing a linked entry must not touch its target")
	}
	removeActionsControlFile(directory, "../outside.json")
	if _, err := os.Stat(outside); err != nil {
		t.Fatal("a name outside the request pattern is never removed")
	}
}

func TestActionsControlSeenRingIsBounded(t *testing.T) {
	var ring []string
	for i := 0; i < maxActionsControlSeen+10; i++ {
		ring = rememberActionsControl(ring, "request_"+strings.Repeat("0", 8)+string(rune('A'+i%26))+string(rune('a'+i/26)))
	}
	if len(ring) != maxActionsControlSeen {
		t.Fatalf("ring = %d", len(ring))
	}
	if !actionsControlSeen(ring, ring[len(ring)-1]) || actionsControlSeen(ring, "never_seen_request_1") {
		t.Fatal("ring membership is wrong")
	}
}

// The capability message must stay byte-identical to the Worker's and the
// gateway's request_message, or nothing the router signs would verify here.
func TestActionsCapabilityMessageMatchesTheRoutersFormat(t *testing.T) {
	got := string(actionsCapabilityMessage("mirror9", "/v1/repositories/a/b/actions-control",
		strings.Repeat("ab", 32), "capability_id_0001", 1784920000000))
	want := "forkmesh-masked-proxy-v1\nmirror9\nPOST\n/v1/repositories/a/b/actions-control\n" +
		strings.Repeat("ab", 32) + "\ncapability_id_0001\n1784920000000"
	if got != want {
		t.Fatalf("message = %q", got)
	}
	var probe map[string]json.RawMessage
	_ = json.Unmarshal([]byte(`{"a":1.0,"b":1e3,"c":"1","d":true,"e":01}`), &probe)
	for key := range probe {
		if _, ok := jsonInteger(probe[key], 0, 10000); ok {
			t.Fatalf("%s must not read as an integer", key)
		}
	}
}
