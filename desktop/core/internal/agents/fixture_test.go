package agents

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func cppMeta(overrides map[string]any) map[string]any {
	obj := map[string]any{
		"schemaVersion": 2, "id": 1, "priority": 50, "owner": "alice", "name": "repo",
		"issueNumber": 0, "issueTitle": "", "prompt": "Fix the thing", "pendingSteerMessage": "",
		"inFlightSteerMessage": "", "provider": "claude-code", "runtimeId": "claude-stream-json",
		"runtimeProtocolVersion": 1, "handoffFromProvider": "", "model": "opus", "mode": "Auto",
		"createPr": false, "associationOnly": false, "yolo": false, "genie": false, "strength": "",
		"orgTask": false, "orgTaskId": "", "startedByBot": "", "finishedByBot": "", "teammateId": "",
		"teammateIds": []any{}, "teammateName": "", "teammateAvatar": "", "chatConversation": "",
		"chatThreadRootId": "", "originInstanceId": "", "originInstanceUrl": "", "skillsUsed": []any{},
		"taskGrade": 0, "orgAgentJob": map[string]any{}, "webSessionId": "", "prNumber": 0,
		"status": "success", "branchName": "agent/1-fix", "worktreeName": "", "baseRef": "",
		"baseBranch": "main", "mergeCandidateHead": "", "merged": false, "mergeAttested": false,
		"mergedAtMs": 0, "createdAtMs": 1000, "startedAtMs": 1100, "finishedAtMs": 1200,
		"promptTokens": 0, "completionTokens": 0, "totalTokens": 0, "contextTokens": 0,
		"contextWindow": 0, "maxOutputTokens": 0, "estimatedCredits": 0, "costUsd": 0,
		"spendBeforeUsd": 0, "spendAfterUsd": 0, "numTurns": 0, "durationMs": 0, "runningMs": 0,
		"runningSinceMs": 0, "lastError": "",
	}
	for key, value := range overrides {
		if value == nil {
			delete(obj, key)
			continue
		}
		obj[key] = value
	}
	return obj
}

func writeFile(t *testing.T, path string, data []byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0o600); err != nil {
		t.Fatal(err)
	}
}

func writeMeta(t *testing.T, tree string, obj map[string]any) string {
	t.Helper()
	raw, err := json.MarshalIndent(obj, "", "    ")
	if err != nil {
		t.Fatal(err)
	}
	owner, _ := obj["owner"].(string)
	name, _ := obj["name"].(string)
	id := 0
	switch v := obj["id"].(type) {
	case int:
		id = v
	case float64:
		id = int(v)
	}
	dir := filepath.Join(tree, sanitizeRepoKey(owner, name), strconv.Itoa(id))
	writeFile(t, filepath.Join(dir, "meta.json"), raw)
	return dir
}

func tempRoot(t *testing.T) string {
	t.Helper()
	root := filepath.Join(t.TempDir(), "agents")
	if !strings.HasPrefix(root, os.TempDir()) {
		t.Fatalf("fixture root %s is outside the temp dir", root)
	}
	return root
}

func parseSession(t *testing.T, text string) Session {
	t.Helper()
	var s Session
	if err := json.Unmarshal([]byte(text), &s); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	return s
}
