package mirrornode

import (
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"testing"
	"time"
)

func TestCatalogPublisherSignsTheWorkerNormalizedRecord(t *testing.T) {
	source, bare := makeBareRepo(t)
	if err := os.MkdirAll(filepath.Join(source, ".forkmesh/issues/open/7"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(source, ".forkmesh/issues/open/7/issue-7.json"), []byte("{}"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(source, ".forkmesh/issues/open/8"), 0700); err != nil {
		t.Fatal(err)
	}
	tombstone := `{"events":[{"type":"open","author":"alice"},{"type":"delete","target":"self","author":"alice"}]}`
	if err := os.WriteFile(filepath.Join(source, ".forkmesh/issues/open/8/issue-8.json"), []byte(tombstone), 0600); err != nil {
		t.Fatal(err)
	}
	// The catalog hashes with ASCII escaping and without HTML escaping, so
	// the description carries both kinds of character (and one outside the BMP).
	if err := os.WriteFile(filepath.Join(source, ".forkmesh/info.json"),
		[]byte(`{"about":"Mirror <and> share — café & more 🚀"}`), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", ".")
	run(t, source, "git", "commit", "-m", "issue")
	for _, number := range []string{"1", "2"} {
		dir := filepath.Join(source, ".forkmesh", "pulls", number)
		if err := os.MkdirAll(dir, 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "pull.md"), []byte("---\nstatus: open\n---\n"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	run(t, source, "git", "add", ".")
	run(t, source, "git", "commit", "-m", "historical pulls")
	run(t, source, "git", "checkout", "-b", "forkmesh/pulls")
	run(t, source, "git", "rm", "-r", ".forkmesh/pulls/2")
	run(t, source, "git", "commit", "-m", "retain open pulls only")
	run(t, source, "git", "checkout", "main")
	run(t, source, "git", "checkout", "-b", "activity-merge")
	if err := os.WriteFile(filepath.Join(source, "merged.txt"), []byte("merged\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "merged.txt")
	run(t, source, "git", "commit", "-m", "merge fixture")
	run(t, source, "git", "checkout", "main")
	run(t, source, "git", "merge", "--no-ff", "activity-merge", "-m", "merge activity")
	run(t, ".", "git", "--git-dir="+bare, "fetch", source, "refs/heads/main:refs/heads/main")
	run(t, ".", "git", "--git-dir="+bare, "fetch", source, "refs/heads/forkmesh/pulls:refs/heads/forkmesh/pulls")
	identity, _ := testIdentity(t)
	stateHash, err := refsSHA256(context.Background(), bare)
	if err != nil {
		t.Fatal(err)
	}
	var received map[string]any
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.Header.Get("Content-Type") != "application/json" {
			t.Errorf("bad request: %s %s", r.Method, r.Header.Get("Content-Type"))
		}
		if err := json.NewDecoder(r.Body).Decode(&received); err != nil {
			t.Error(err)
		}
		w.WriteHeader(http.StatusCreated)
	}))
	defer server.Close()
	repo := Repository{Owner: "forkmesh", Name: "forkmesh", ForkedFrom: "alice/forkmesh", Visibility: "public", Enabled: true, GitDir: bare}
	publisher := &CatalogPublisher{
		URL: server.URL, Owner: "mirror9", Node: "mirror9", Version: "test",
		Identity: identity, Client: server.Client(),
		MirrorURL: "https://git.mirror9.example.test",
		LookPath: func(binary string) (string, error) {
			if binary == "claude" || binary == "codex" {
				return "/usr/local/bin/" + binary, nil
			}
			return "", exec.ErrNotFound
		},
	}
	if err := publisher.Publish(context.Background(), repo, stateHash); err != nil {
		t.Fatal(err)
	}
	if received["stateHash"] != stateHash || received["issueCount"] != "1" ||
		received["pullCount"] != "2" || received["mergeCount"] != "1" ||
		received["runtimeMode"] != "headless" ||
		received["mirrorUrl"] != "https://git.mirror9.example.test/forkmesh/forkmesh.git" ||
		received["forkedFrom"] != "alice/forkmesh" ||
		received["description"] != "Mirror <and> share — café & more \U0001F680" {
		t.Fatalf("unexpected record: %+v", received)
	}
	agentProviders, agentProvidersOK := received["agentProviders"].([]any)
	if !agentProvidersOK || len(agentProviders) != 2 ||
		agentProviders[0] != "claude-code" || agentProviders[1] != "codex" {
		t.Fatalf("unexpected agent providers: %v", received["agentProviders"])
	}
	commitWeeks, commitWeeksOK := received["activityWeeks"].([]any)
	mergeWeeks, mergeWeeksOK := received["mergeActivityWeeks"].([]any)
	if !commitWeeksOK || !mergeWeeksOK || len(commitWeeks) != 52 ||
		len(mergeWeeks) != 52 || sumJSONNumbers(mergeWeeks) != 1 ||
		sumJSONNumbers(commitWeeks) < 1 {
		t.Fatalf("unexpected activity series: commits=%v merges=%v",
			commitWeeks, mergeWeeks)
	}
	statePayload := "forkmesh-repostate-v1\nmirror9\nforkmesh\n" + stateHash + "\n" + received["updatedAt"].(string)
	stateSig, _ := base64.RawURLEncoding.DecodeString(received["stateSig"].(string))
	if !ed25519.Verify(identity.public, []byte(statePayload), stateSig) {
		t.Fatal("bad state signature")
	}
	catalogSig, _ := base64.RawURLEncoding.DecodeString(received["catalogSig"].(string))
	delete(received, "catalogSig")
	delete(received, "catalogSigVersion")
	delete(received, "signature")
	canonical, err := catalogCanonicalJSON(received)
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(canonical)
	payload := []byte("forkmesh-catalog-v2\n" + hex.EncodeToString(digest[:]))
	if !ed25519.Verify(identity.public, payload, catalogSig) {
		t.Fatalf("bad catalog signature over %s", canonical)
	}
}

func TestCatalogPublisherOmitsUnavailableAgentProviders(t *testing.T) {
	identity, _ := testIdentity(t)
	publisher := &CatalogPublisher{
		Identity: identity,
		LookPath: func(string) (string, error) { return "", exec.ErrNotFound },
	}
	if providers := publisher.agentProviders(); len(providers) != 0 {
		t.Fatalf("unexpected unavailable providers: %v", providers)
	}
}

func sumJSONNumbers(values []any) int {
	total := 0
	for _, value := range values {
		if number, ok := value.(float64); ok {
			total += int(number)
		}
	}
	return total
}

func TestCatalogPublisherSurfacesHTTPFailure(t *testing.T) {
	_, bare := makeBareRepo(t)
	identity, _ := testIdentity(t)
	stateHash, _ := refsSHA256(context.Background(), bare)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) { http.Error(w, "no", http.StatusUnauthorized) }))
	defer server.Close()
	p := &CatalogPublisher{URL: server.URL, Owner: "mirror9", Node: "mirror9", Version: "test", Identity: identity, Client: server.Client()}
	if err := p.Publish(context.Background(), Repository{Owner: "mirror9", Name: "forkmesh", GitDir: bare}, stateHash); err == nil || !strings.Contains(err.Error(), "401") {
		t.Fatal("HTTP failure ignored")
	}
}

func TestEndpointPublisherRenewsSignedActiveLease(t *testing.T) {
	identity, _ := testIdentity(t)
	var received map[string]any
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/mirrors/https" || r.Method != http.MethodPost {
			t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
		}
		if err := json.NewDecoder(r.Body).Decode(&received); err != nil {
			t.Error(err)
		}
		_ = json.NewEncoder(w).Encode(map[string]any{
			"ok": true, "node": "mirror9", "baseUrl": "https://mirror9.example.test",
			"health": "active",
		})
	}))
	defer server.Close()
	publisher := &EndpointPublisher{
		CatalogURL: server.URL + "/api/repositories?ignored=true",
		Node:       "mirror9", BaseURL: "https://mirror9.example.test",
		Version: "test", Identity: identity, Client: server.Client(),
	}
	active, err := publisher.Publish(context.Background())
	if err != nil || !active {
		t.Fatal(err)
	}
	issuedAt := int64(received["issuedAt"].(float64))
	message := "forkmesh-https-endpoint-v1\nmirror9\nhttps://mirror9.example.test\n" +
		identity.PublicKey() + "\n" + strconv.FormatInt(issuedAt, 10)
	signature, err := base64.RawURLEncoding.DecodeString(received["signature"].(string))
	if err != nil || !ed25519.Verify(identity.public, []byte(message), signature) {
		t.Fatal("endpoint registration signature is invalid")
	}
}

func TestEndpointPublisherRetriesPendingLeaseWithoutFailingSync(t *testing.T) {
	identity, _ := testIdentity(t)
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"ok": true, "node": "mirror9", "baseUrl": "https://mirror9.example.test",
			"health": "pending",
		})
	}))
	defer server.Close()
	publisher := &EndpointPublisher{
		CatalogURL: server.URL + "/api/repositories", Node: "mirror9",
		BaseURL: "https://mirror9.example.test", Version: "test",
		Identity: identity, Client: server.Client(),
	}
	active, err := publisher.Publish(context.Background())
	if err != nil || active {
		t.Fatalf("pending lease result: active=%v err=%v", active, err)
	}
}

func TestEndpointPublisherSignsReplaceableDirectMirrorURL(t *testing.T) {
	identity, _ := testIdentity(t)
	var received map[string]any
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if err := json.NewDecoder(r.Body).Decode(&received); err != nil {
			t.Error(err)
		}
		_ = json.NewEncoder(w).Encode(map[string]any{
			"ok": true, "node": "mirror9",
			"baseUrl":   "https://tunnel.example.test",
			"mirrorUrl": "https://git.example.test", "health": "active",
		})
	}))
	defer server.Close()
	publisher := &EndpointPublisher{
		CatalogURL: server.URL, Node: "mirror9",
		BaseURL:   "https://tunnel.example.test",
		MirrorURL: "https://git.example.test",
		Version:   "test", Identity: identity, Client: server.Client(),
	}
	active, err := publisher.Publish(context.Background())
	if err != nil || !active {
		t.Fatal(err)
	}
	issuedAt := int64(received["issuedAt"].(float64))
	message := "forkmesh-https-endpoint-v2\nmirror9\n" +
		"https://tunnel.example.test\nhttps://git.example.test\n" +
		identity.PublicKey() + "\n" + strconv.FormatInt(issuedAt, 10)
	signature, err := base64.RawURLEncoding.DecodeString(received["signature"].(string))
	if err != nil || !ed25519.Verify(identity.public, []byte(message), signature) {
		t.Fatal("direct endpoint registration signature is invalid")
	}
}

func TestFirstLineMatchesWorkerRootCommitNormalization(t *testing.T) {
	if got := firstLine("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"); got != "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" {
		t.Fatalf("got %q", got)
	}
	if got := firstLine(strings.Repeat("x", 80)); len(got) != 64 {
		t.Fatalf("got %d chars", len(got))
	}
}

func TestCatalogPathCountsUseCurrentGitLayouts(t *testing.T) {
	issues, maxIssue, pulls, discussions, artifacts := catalogPathCounts([]string{
		".forkmesh/issues/open/7/issue-7.json",
		".forkmesh/issues/closed/91/issue-91.json",
		".forkmesh/pulls/11/pull.md",
		".forkmesh/pulls/11/changes.patch",
		".forkmesh/pulls/open/pull-11.json",
		".forkmesh/pulls/not-a-number/pull.md",
		".forkmesh/discussions/2/discussion.md",
		".forkmesh/discussions/2/0002-comment.md",
		".forkmesh/artifacts/sha256/file",
	})
	if issues != 1 || maxIssue != 91 || pulls != 1 || discussions != 1 || artifacts != 1 {
		t.Fatalf("got issues=%d max=%d pulls=%d discussions=%d artifacts=%d",
			issues, maxIssue, pulls, discussions, artifacts)
	}
}

func TestReleaseArtifactCountUsesPhysicalCasBlobs(t *testing.T) {
	store := t.TempDir()
	valid := strings.Repeat("a", 64)
	validData := filepath.Join(store, "sha256", "aa", valid, "data")
	if err := os.MkdirAll(filepath.Dir(validData), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(validData, []byte("release"), 0600); err != nil {
		t.Fatal(err)
	}

	wrongShard := filepath.Join(store, "sha256", "bb", valid, "data")
	if err := os.MkdirAll(filepath.Dir(wrongShard), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(wrongShard, []byte("wrong shard"), 0600); err != nil {
		t.Fatal(err)
	}
	incomplete := filepath.Join(store, "sha256", "cc", strings.Repeat("c", 64))
	if err := os.MkdirAll(incomplete, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(validData, filepath.Join(incomplete, "data")); err != nil {
		t.Fatal(err)
	}

	if got := releaseArtifactCount(store); got != 1 {
		t.Fatalf("artifact count = %d, want 1", got)
	}
}

func TestCPUPercentUsesDeltaAndBoundsInput(t *testing.T) {
	oldRead := readCPUStat
	oldPrevious := cpuPrevious
	t.Cleanup(func() {
		readCPUStat = oldRead
		cpuPrevious = oldPrevious
	})
	samples := [][]byte{
		[]byte("cpu  100 0 50 800 50 0 0 0\n"),
		[]byte("cpu  150 0 70 860 60 0 0 0\n"),
	}
	readCPUStat = func() ([]byte, error) {
		value := samples[0]
		samples = samples[1:]
		return value, nil
	}
	cpuPrevious = cpuTimes{}
	if got := cpuPercent(); got != 0 {
		t.Fatalf("first sample = %d", got)
	}
	if got := cpuPercent(); got != 50 {
		t.Fatalf("second sample = %d", got)
	}
	if _, ok := parseCPUTimes([]byte("intr 1 2 3\n")); ok {
		t.Fatal("accepted non-CPU stat line")
	}
}

func TestMemoryUsageExcludesReclaimableLinuxCache(t *testing.T) {
	oldRead := readMemInfo
	t.Cleanup(func() { readMemInfo = oldRead })
	readMemInfo = func() ([]byte, error) {
		return []byte("MemTotal:       1000000 kB\nMemFree:         100000 kB\nMemAvailable:    650000 kB\nCached:          500000 kB\n"), nil
	}
	used, total := memoryUsage()
	if total != 1000000*1024 || used != 350000*1024 {
		t.Fatalf("used=%d total=%d", used, total)
	}
}

func TestCatalogActivityWeeksMatchesGitLogInOnePass(t *testing.T) {
	source, bare := makeBareRepo(t)
	run(t, source, "git", "checkout", "-b", "side")
	if err := os.WriteFile(filepath.Join(source, "side.txt"), []byte("side\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "side.txt")
	run(t, source, "git", "commit", "-m", "side")
	run(t, source, "git", "checkout", "main")
	if err := os.WriteFile(filepath.Join(source, "main.txt"), []byte("main\n"), 0600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", "main.txt")
	run(t, source, "git", "commit", "-m", "main")
	run(t, source, "git", "merge", "--no-ff", "side", "-m", "merge side")
	run(t, ".", "git", "--git-dir="+bare, "fetch", source, "+refs/heads/main:refs/heads/main")
	commits, merges := catalogActivityWeeks(context.Background(), bare)
	all := countLines(gitValue(context.Background(), bare, "log", "--since=52 weeks ago", "--format=%ct", "HEAD", "--"))
	merged := countLines(gitValue(context.Background(), bare, "log", "--since=52 weeks ago", "--format=%ct", "--merges", "HEAD", "--"))
	sum := func(values []int) int {
		total := 0
		for _, value := range values {
			total += value
		}
		return total
	}
	if len(commits) != 52 || len(merges) != 52 || sum(commits) != all || sum(merges) != merged || merged != 1 {
		t.Fatalf("activity commits=%d/%d merges=%d/%d", sum(commits), all, sum(merges), merged)
	}
}

func TestCatalogDescriptionComesFromTheRepositoryAbout(t *testing.T) {
	source, bare := makeBareRepo(t)
	ctx := context.Background()
	if got := catalogDescription(ctx, bare); got != "" {
		t.Fatalf("description without an about file = %q", got)
	}
	cases := []struct {
		about string
		want  string
	}{
		{`{"about": "  Mirror,\n preserve\tand share  ", "description": "x"}`, "Mirror, preserve and share"},
		{`{"about": "", "description": "Fallback text"}`, "Fallback text"},
		{`{"about": 7}`, ""},
		{`not json`, ""},
		{`{"about": "` + strings.Repeat("é", 300) + `"}`, strings.Repeat("é", 240)},
	}
	if err := os.MkdirAll(filepath.Join(source, ".forkmesh"), 0o700); err != nil {
		t.Fatal(err)
	}
	for index, test := range cases {
		if err := os.WriteFile(filepath.Join(source, ".forkmesh", "info.json"), []byte(test.about), 0o600); err != nil {
			t.Fatal(err)
		}
		run(t, source, "git", "add", ".forkmesh/info.json")
		run(t, source, "git", "commit", "-q", "-m", "about "+strconv.Itoa(index))
		run(t, source, "git", "push", "-q", bare, "HEAD:main")
		if got := catalogDescription(ctx, bare); got != test.want {
			t.Fatalf("case %d: description = %q, want %q", index, got, test.want)
		}
	}
}

func TestCatalogCanonicalJSONMatchesTheCatalogHashInput(t *testing.T) {
	// Expected values are Python's json.dumps(value, sort_keys=True,
	// separators=(",", ":")), which the catalog hashes for catalog-v2.
	cases := []struct {
		value any
		want  string
	}{
		{map[string]any{"b": 1, "a": []string{"x"}, "c": map[string]int64{}}, `{"a":["x"],"b":1,"c":{}}`},
		{"hosts\u2014without", `"hosts\u2014without"`},
		{"caf\u00e9", `"caf\u00e9"`},
		{"a <b> & c", `"a <b> & c"`},
		{"rocket \U0001F680", `"rocket \ud83d\ude80"`},
		{"del\x7f nul\x00 esc\x1b", `"del\u007f nul\u0000 esc\u001b"`},
		{"\b\f\n\r\t\"\\/", `"\b\f\n\r\t\"\\/"`},
		{"line\u2028para\u2029", `"line\u2028para\u2029"`},
		{"bad\xffbyte", `"bad\ufffdbyte"`},
		{[]int{0, -3, 1 << 50}, `[0,-3,1125899906842624]`},
		{map[string]any{"t": true, "f": false, "n": nil}, `{"f":false,"n":null,"t":true}`},
	}
	for _, test := range cases {
		got, err := catalogCanonicalJSON(test.value)
		if err != nil {
			t.Fatal(err)
		}
		if string(got) != test.want {
			t.Errorf("catalogCanonicalJSON(%q) = %s, want %s", test.value, got, test.want)
		}
	}
}

func TestCatalogCanonicalJSONMatchesTheProtocolVector(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join("..", "conformance", "protocol", "v1", "vectors", "signatures.json"))
	if err != nil {
		t.Fatal(err)
	}
	var vectors struct {
		CatalogV2Record  map[string]any `json:"catalogV2Record"`
		CanonicalStrings []struct {
			Scheme string     `json:"scheme"`
			Fields [][]string `json:"fields"`
		} `json:"canonicalStrings"`
	}
	if err := json.Unmarshal(raw, &vectors); err != nil {
		t.Fatal(err)
	}
	record := vectors.CatalogV2Record
	if len(record) == 0 {
		t.Fatal("no catalogV2Record vector")
	}
	delete(record, "signature")
	canonical, err := catalogCanonicalJSON(record)
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(canonical)
	for _, vector := range vectors.CanonicalStrings {
		if vector.Scheme != "forkmesh-catalog-v2" {
			continue
		}
		for _, field := range vector.Fields {
			if len(field) == 2 && field[0] == "recordSha256" {
				if field[1] != hex.EncodeToString(digest[:]) {
					t.Fatalf("record hash %x, vector %s, over %s", digest, field[1], canonical)
				}
				return
			}
		}
	}
	t.Fatal("no forkmesh-catalog-v2 vector")
}

// catalogFixturePath is a node-signed record that the app's test suite
// (app/tests/test_catalog_node_signature.py) verifies the way the catalog does.
// Regenerate it with FORKMESH_UPDATE_CATALOG_FIXTURE=1 go test -run CatalogRecordFixture.
const catalogFixturePath = "testdata/catalog-v2-record.json"

func TestCatalogRecordFixtureForTheApp(t *testing.T) {
	// The fixture must carry every field a real publication does, so build
	// one with every optional extension switched on and compare key sets.
	source, bare := makeBareRepo(t)
	if err := os.MkdirAll(filepath.Join(source, ".forkmesh"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(source, ".forkmesh", "info.json"), []byte(`{"about":"x"}`), 0o600); err != nil {
		t.Fatal(err)
	}
	run(t, source, "git", "add", ".forkmesh/info.json")
	run(t, source, "git", "commit", "-q", "-m", "about")
	run(t, source, "git", "push", "-q", bare, "HEAD:main")
	identity, _ := testIdentity(t)
	stateHash, err := refsSHA256(context.Background(), bare)
	if err != nil {
		t.Fatal(err)
	}
	publisher := &CatalogPublisher{
		Owner: "mirror9", Node: "mirror9", Version: "test", Identity: identity,
		MirrorURL: "https://git.mirror9.example.test", MirrorOf: "https://upstream.example.test/forkmesh/forkmesh",
		LastSyncedAt: time.UnixMilli(1784840000000),
		LookPath:     func(string) (string, error) { return "/usr/bin/true", nil },
	}
	live, err := publisher.record(context.Background(), Repository{
		Owner: "forkmesh", Name: "forkmesh", ForkedFrom: "alice/forkmesh", GitDir: bare,
	}, stateHash)
	if err != nil {
		t.Fatal(err)
	}

	seed := sha256.Sum256([]byte("forkmesh catalog fixture key"))
	private := ed25519.NewKeyFromSeed(seed[:])
	fixed := &Identity{private: private, public: private.Public().(ed25519.PublicKey)}
	updatedAt := "1784840000002"
	commit := strings.Repeat("ab", 20)
	state := strings.Repeat("cd", 32)
	weeks := make([]int, 52)
	weeks[51] = 3
	merges := make([]int, 52)
	merges[51] = 1
	record := map[string]any{
		"owner": "mirror9", "name": "forkmesh", "visibility": "public",
		"mirrorEncryption": "", "opaqueRepoId": "", "keyEpoch": 0,
		"encryptedManifestHash": "", "encryptedManifestSig": "",
		"sizeBytes": 123456,
		// Non-ASCII (BMP and astral), HTML-sensitive and DEL characters: the
		// ones encoding/json writes differently from the catalog's hash input.
		"description":  "Mirror <and> share — café & more \U0001F680 del\x7f",
		"logoMetadata": map[string]any{"description": "", "languages": map[string]int64{}, "topics": []string{}, "fileStructure": []string{}, "frameworks": []string{}, "projectCategory": ""},
		"cloneUrl":     "", "solana": "", "channel": "#mirror9-forkmesh",
		"hostedSince": "", "lastSync": updatedAt, "updatedAt": updatedAt,
		"rootCommit": commit, "source": "remote-clone", "commit": commit, "branch": "main",
		"issueCount": "1", "issueMaxNumber": "8", "commitCount": "3", "mergeCount": "1",
		"branchCount": "1", "pullCount": "2", "discussionCount": "0",
		"activityWeeks": weeks, "mergeActivityWeeks": merges,
		"worktreeCount": "0", "artifactCount": "0",
		"platform": "linux", "version": "test", "nodeId": "",
		"clonesServed": "0", "websiteServed": "0", "maintainer": fixed.PublicKey(),
		"signature": "", "stateHash": state,
		"cpuPercent": 7, "memUsedBytes": int64(300), "memTotalBytes": int64(1000),
		"diskUsedBytes": int64(800), "diskTotalBytes": int64(2000),
		"machineName": "mirror9", "runtimeMode": "headless",
		"mirrorUrl":      "https://git.mirror9.example.test/forkmesh/forkmesh.git",
		"agentProviders": []string{"claude-code", "codex", "local-llama"},
		"mirrorOf":       "https://upstream.example.test/forkmesh/forkmesh",
		"upstreamCommit": commit, "lastSyncedAt": "1784840000000",
		"forkedFrom": "alice/forkmesh",
	}
	record["stateSig"] = fixed.Sign([]byte("forkmesh-repostate-v1\nmirror9\nforkmesh\n" + state + "\n" + updatedAt))
	if err := signCatalogRecord(fixed, record); err != nil {
		t.Fatal(err)
	}
	var liveKeys, fixtureKeys []string
	for key := range live {
		liveKeys = append(liveKeys, key)
	}
	for key := range record {
		fixtureKeys = append(fixtureKeys, key)
	}
	slices.Sort(liveKeys)
	slices.Sort(fixtureKeys)
	if !slices.Equal(liveKeys, fixtureKeys) {
		t.Fatalf("fixture keys %v\nlive record keys %v", fixtureKeys, liveKeys)
	}

	// Byte for byte the request body Publish sends.
	encoded, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	encoded = append(encoded, '\n')
	if os.Getenv("FORKMESH_UPDATE_CATALOG_FIXTURE") == "1" {
		if err := os.MkdirAll(filepath.Dir(catalogFixturePath), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(catalogFixturePath, encoded, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	committed, err := os.ReadFile(catalogFixturePath)
	if err != nil {
		t.Fatalf("%v (regenerate with FORKMESH_UPDATE_CATALOG_FIXTURE=1)", err)
	}
	if string(committed) != string(encoded) {
		t.Fatalf("%s is stale; regenerate with FORKMESH_UPDATE_CATALOG_FIXTURE=1", catalogFixturePath)
	}
}
