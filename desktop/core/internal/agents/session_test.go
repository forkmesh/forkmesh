package agents

import (
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

func TestLegacyRecordMigrates(t *testing.T) {
	s := parseSession(t, `{
		"id": 7, "owner": "alice", "name": "repo", "provider": "codex",
		"priority": 0, "heldBy": "phone", "runtimeProtocolVersion": 0,
		"teammateIds": ["  a ", "a", "", 5, "b"],
		"skillsUsed": ["Build", "build", " test "],
		"orgAgentJob": {"sessionId": "  web-1 ", "lease": 3},
		"createdAtMs": 1.7e12, "durationMs": "42", "issueNumber": 3.5,
		"taskGrade": 9, "costUsd": 2, "merged": 1
	}`)
	if s.SchemaVersion != SchemaCurrent || !s.SchemaSupported() {
		t.Fatalf("schema %d", s.SchemaVersion)
	}
	if s.RuntimeID != RuntimeCodexAppServer {
		t.Fatalf("runtime %q", s.RuntimeID)
	}
	if s.Priority != 1 || s.RuntimeProtocolVersion != 1 || s.TaskGrade != 5 {
		t.Fatalf("bounds %d %d %d", s.Priority, s.RuntimeProtocolVersion, s.TaskGrade)
	}
	if s.Held || s.HeldBy != "" {
		t.Fatalf("heldBy kept without held")
	}
	if !reflect.DeepEqual(s.TeammateIDs, []string{"a", "b"}) {
		t.Fatalf("teammates %v", s.TeammateIDs)
	}
	if !reflect.DeepEqual(s.SkillsUsed, []string{"Build", "test"}) {
		t.Fatalf("skills %v", s.SkillsUsed)
	}
	if s.WebSessionID != "web-1" {
		t.Fatalf("web session %q", s.WebSessionID)
	}
	if s.Status != StatusQueued {
		t.Fatalf("status %q", s.Status)
	}
	if s.CreatedAtMs != 1700000000000 || s.DurationMs != 42 || s.IssueNumber != 0 {
		t.Fatalf("numbers %d %d %d", s.CreatedAtMs, s.DurationMs, s.IssueNumber)
	}
	if s.CostUsd != 2 || s.Merged {
		t.Fatalf("cost %v merged %v", s.CostUsd, s.Merged)
	}
}

func TestSchemaVersionRules(t *testing.T) {
	cases := map[string]int{
		`{"schemaVersion": 1}`:   2,
		`{"schemaVersion": 2}`:   2,
		`{"schemaVersion": 3}`:   3,
		`{"schemaVersion": "2"}`: 0,
		`{"schemaVersion": 2.5}`: 0,
		`{}`:                     2,
	}
	for text, want := range cases {
		if got := parseSession(t, text).SchemaVersion; got != want {
			t.Errorf("%s: schema %d, want %d", text, got, want)
		}
	}
	if parseSession(t, `{"schemaVersion": 3}`).SchemaSupported() {
		t.Fatal("schema 3 must be unsupported")
	}
}

func TestRuntimeForLegacyProvider(t *testing.T) {
	cases := map[string]string{
		"codex": RuntimeCodexAppServer, " OpenAI-Codex ": RuntimeCodexAppServer,
		"claude-code": RuntimeClaudeStreamJSON, "claude-api": RuntimeManagedRunner, "": RuntimeManagedRunner,
	}
	for provider, want := range cases {
		if got := RuntimeForLegacyProvider(provider); got != want {
			t.Errorf("%q -> %q, want %q", provider, got, want)
		}
	}
	if got := parseSession(t, `{"provider":"codex","runtimeId":"  custom  "}`).RuntimeID; got != "custom" {
		t.Fatalf("explicit runtime %q", got)
	}
}

func TestRoundTripKeepsUnknownFields(t *testing.T) {
	meta := cppMeta(map[string]any{
		"futureField": map[string]any{"a": []any{1, 2}}, "directCheckout": false, "held": true,
		"heldBy": "x", "vibe": true, "orgAgentJob": map[string]any{"sessionId": "s1", "kind": "job"},
		"teammateIds": []any{"t1", "t2"}, "skillsUsed": []any{"Go"}, "costUsd": 1.25,
	})
	raw, _ := json.Marshal(meta)
	first := parseSession(t, string(raw))
	encoded, err := json.Marshal(first)
	if err != nil {
		t.Fatal(err)
	}
	var out map[string]any
	if err := json.Unmarshal(encoded, &out); err != nil {
		t.Fatal(err)
	}
	if _, ok := out["directCheckout"]; ok {
		t.Fatal("directCheckout false must be omitted like toJson")
	}
	if out["held"] != true || out["heldBy"] != "x" || out["vibe"] != true {
		t.Fatalf("held/vibe %v %v %v", out["held"], out["heldBy"], out["vibe"])
	}
	if !reflect.DeepEqual(out["futureField"], map[string]any{"a": []any{1.0, 2.0}}) {
		t.Fatalf("unknown field lost: %v", out["futureField"])
	}
	if out["webSessionId"] != "s1" {
		t.Fatalf("webSessionId %v", out["webSessionId"])
	}
	second := parseSession(t, string(encoded))
	if !reflect.DeepEqual(first, second) {
		t.Fatalf("round trip differs:\n%+v\n%+v", first, second)
	}
	var expected map[string]any
	raw, _ = json.Marshal(meta)
	_ = json.Unmarshal(raw, &expected)
	expected["webSessionId"] = "s1"
	delete(expected, "directCheckout")
	if !reflect.DeepEqual(expected, out) {
		for key := range expected {
			if !reflect.DeepEqual(expected[key], out[key]) {
				t.Errorf("%s: %v != %v", key, expected[key], out[key])
			}
		}
		for key := range out {
			if _, ok := expected[key]; !ok {
				t.Errorf("extra key %s", key)
			}
		}
	}
}

func TestToJSONSanitizes(t *testing.T) {
	s := NewSession()
	s.Priority = 400
	s.TaskGrade = -1
	s.ChatConversation = "  " + strings.Repeat("c", 200)
	s.TeammateIDs = []string{"x", " x ", ""}
	s.Provider = "codex"
	obj := s.Object()
	if obj["priority"] != 99 || obj["taskGrade"] != 0 {
		t.Fatalf("bounds %v %v", obj["priority"], obj["taskGrade"])
	}
	if len(obj["chatConversation"].(string)) != 160 {
		t.Fatalf("chatConversation not trimmed to 160")
	}
	if !reflect.DeepEqual(obj["teammateIds"], []string{"x"}) {
		t.Fatalf("teammates %v", obj["teammateIds"])
	}
	if obj["runtimeId"] != RuntimeCodexAppServer {
		t.Fatalf("runtime %v", obj["runtimeId"])
	}
	if _, held := obj["held"]; held {
		t.Fatal("held false must be omitted")
	}
}

func TestRepoKey(t *testing.T) {
	cases := [][3]string{
		{"alice", "repo", "alice-repo"},
		{"my org", "repo.name", "my_org-repo.name"},
		{"élan", "a/b", "élan-a_b"},
		{"x", "\U0001F600", "x-__"},
	}
	for _, c := range cases {
		if got := sanitizeRepoKey(c[0], c[1]); got != c[2] {
			t.Errorf("%q %q -> %q, want %q", c[0], c[1], got, c[2])
		}
	}
}

func TestLeftCountsUTF16(t *testing.T) {
	if got := left("a\U0001F600b", 3); got != "a\U0001F600" {
		t.Fatalf("left %q", got)
	}
	if got := left("a\U0001F600b", 2); got != "a" {
		t.Fatalf("left split surrogate %q", got)
	}
	if utf16Len("a\U0001F600") != 3 {
		t.Fatal("utf16Len")
	}
}
