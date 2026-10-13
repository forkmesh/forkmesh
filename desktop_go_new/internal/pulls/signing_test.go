package pulls

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/identity"
)

func testIdentity(t *testing.T) *identity.Identity {
	t.Helper()
	seed := make([]byte, 32)
	for i := range seed {
		seed[i] = byte(i + 1)
	}
	id, err := identity.FromSeed(seed)
	if err != nil {
		t.Fatal(err)
	}
	return id
}

func TestCanonicalStringsMatchTheDesktop(t *testing.T) {
	ev := Event{Type: "thread-comment", ThreadID: "t1", Path: "a.txt", Side: "new", LineStart: 3, LineEnd: 4,
		Body: "fix", SuggestionPatch: "+x", Author: "AUTH", Ts: 1700000000123, Patchset: "abc", AnchorLines: "two\nthree"}
	content := ContentForSigning(&ev)
	if content != "t1\x00a.txt\x00new\x003\x004\x00fix\x00+x\x00abc\x00two\nthree" {
		t.Fatalf("content %q", content)
	}
	got := string(EventCanonical(7, &ev, "alice/demo"))
	want := "forkmesh-pull-comment-v2\nalice/demo\nthread-comment\n7\nAUTH\n1700000000123\n" + sha256Hex(content)
	if got != want {
		t.Fatalf("canonical %q", got)
	}
	plain := Event{Type: "line-comment", Path: "p", Side: "old", Line: 9, Body: "b"}
	if ContentForSigning(&plain) != "p\x00old\x009\x00b" {
		t.Fatalf("an event without an anchor signs the pre-anchor bytes: %q", ContentForSigning(&plain))
	}
	if !strings.HasPrefix(string(BranchEventCanonical("feature", &ev, "")), "forkmesh-branch-comment-v1\nthread-comment\nfeature\nAUTH\n") {
		t.Fatal("branch canonical prefix")
	}
	pr := Pull{Title: "T", Base: "main", Head: "f", Patch: "P", Commits: "C", Author: "A", Ts: 5}
	if string(PullCanonical(&pr, "")) != "forkmesh-pull-event-v1\nA\n5\n"+sha256Hex("T\x00main\x00f\x00P\x00C") {
		t.Fatal("pull canonical")
	}
	if string(LegacyPullCanonical(&pr)) != "forkmesh-pull-event-v1\nA\n5\n"+sha256Hex("T\x00main\x00f\x00P") {
		t.Fatal("legacy pull canonical")
	}
	if RepoKey(" Alice ", "Demo") != "alice/demo" || RepoKey("a/b", "c") != "" || RepoKey("", "c") != "" {
		t.Fatal("repo key")
	}
}

func TestSignedEventsVerifyWithTheRelayVerifier(t *testing.T) {
	python, err := exec.LookPath("python3")
	if err != nil {
		t.Skip("python3 not installed")
	}
	if exec.Command(python, "-c", "import cryptography").Run() != nil {
		t.Skip("python cryptography not installed")
	}
	events, _ := filepath.Abs("../../../app/src/events.py")
	if _, err := os.Stat(events); err != nil {
		t.Skip("app/src/events.py not present")
	}
	id := testIdentity(t)
	clock := func() time.Time { return time.UnixMilli(1790000000000) }
	signer := &Signer{Identity: id, AuthorName: "Alice", RepoKey: "alice/demo", Now: clock}
	legacy := &Signer{Identity: id, AuthorName: "Alice", Now: clock}
	cases := []map[string]any{}
	addPull := func(s *Signer, number int, ev Event) {
		signed := s.SignEvent(number, ev)
		cases = append(cases, map[string]any{"kind": "pull-comment", "number": number, "repoKey": s.RepoKey, "event": signed.ToJSON()})
	}
	addPull(signer, 3, Event{Type: "comment", Body: "hello"})
	addPull(signer, 3, Event{Type: "review", State: "approved", Body: "ship it"})
	addPull(signer, 3, Event{Type: "line-comment", Path: "a.txt", Side: "new", Line: 2, Body: "nit", Patchset: "0123", AnchorLines: "two"})
	addPull(signer, 3, Event{Type: "thread-comment", Path: "a.txt", Side: "new", LineStart: 1, LineEnd: 2, Body: "b", SuggestionPatch: "+y"})
	addPull(signer, 3, Event{Type: "thread-reply", ThreadID: "t", ParentID: "p", Body: "reply"})
	addPull(signer, 3, Event{Type: "thread-state", ThreadID: "t", State: "resolved"})
	addPull(signer, 3, Event{Type: "suggestion-state", ThreadID: "t", State: "applied", AppliedCommit: "abc", Body: "done"})
	addPull(legacy, 4, Event{Type: "comment", Body: "unicode é ✓"})
	branch := signer.SignBranchEvent("feature/x", Event{Type: "thread-comment", Path: "b.txt", Side: "old", LineStart: 4, LineEnd: 4, Body: "why"})
	cases = append(cases, map[string]any{"kind": "branch-comment", "branch": "feature/x", "repoKey": "alice/demo", "event": branch.ToJSON()})
	tampered := signer.SignEvent(5, Event{Type: "comment", Body: "original"})
	tampered.Body = "changed"
	cases = append(cases, map[string]any{"kind": "pull-comment", "number": 5, "repoKey": "alice/demo", "event": tampered.ToJSON(), "reject": true})
	pull := signer.SignPull(Pull{Title: "T", Base: "main", Head: "f", Patch: "diff --git a/x b/x\n", Commits: "From 0\n"})
	cases = append(cases, map[string]any{"kind": "pull-event", "repoKey": "alice/demo", "pull": pull.WireJSON()})
	dir := t.TempDir()
	fixture := filepath.Join(dir, "cases.json")
	raw, _ := json.Marshal(cases)
	if err := os.WriteFile(fixture, raw, 0o600); err != nil {
		t.Fatal(err)
	}
	script := filepath.Join(dir, "verify.py")
	if err := os.WriteFile(script, []byte(relayVerifyScript), 0o600); err != nil {
		t.Fatal(err)
	}
	out, err := exec.Command(python, script, events, fixture).CombinedOutput()
	if err != nil {
		t.Fatalf("relay verifier rejected a core signature: %v\n%s", err, out)
	}
	if strings.TrimSpace(string(out)) != "ok "+strconv.Itoa(len(cases)) {
		t.Fatalf("verifier output %q", out)
	}
}

const relayVerifyScript = `import ast, asyncio, base64, hashlib, json, sys, time
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

def dec(value):
    value = value + "=" * (-len(value) % 4)
    return base64.urlsafe_b64decode(value.encode())

async def verify(pub, sig, data):
    try:
        Ed25519PublicKey.from_public_bytes(dec(pub)).verify(dec(sig), data)
        return True
    except Exception:
        return False

async def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()

source = open(sys.argv[1], encoding="utf-8").read()
tree = ast.parse(source)
bound = {"to_js", "_to_js_bytes", "ed25519_verify", "sha256_hex", "verify_release_manifest"}
body = [n for n in tree.body if (isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name not in bound) or isinstance(n, ast.Assign)]
ns = {"time": time, "ed25519_verify": verify, "sha256_hex": sha, "clean_string": lambda v, l: str(v or "")[:l]}
exec(compile(ast.fix_missing_locations(ast.Module(body=body, type_ignores=[])), "events.py", "exec"), ns)
cases = json.load(open(sys.argv[2], encoding="utf-8"))
async def run():
    for case in cases:
        keys = (case["repoKey"],) if case["repoKey"] else ()
        allow_v1 = not case["repoKey"]
        if case["kind"] == "pull-comment":
            ok = await ns["verify_pull_comment_event"](case["number"], case["event"], keys, allow_v1)
        elif case["kind"] == "branch-comment":
            ok = await ns["verify_branch_comment_event"](case["branch"], case["event"], keys)
        else:
            ok = await ns["verify_pull_event"](case["pull"], keys, allow_v1)
        if ok == bool(case.get("reject")):
            print("unexpected", ok, json.dumps(case))
            sys.exit(1)
    print("ok", len(cases))
asyncio.run(run())
`
