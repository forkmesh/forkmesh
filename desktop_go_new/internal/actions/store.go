package actions

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode"

	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/safedelete"
	"forkmesh.com/desktop-core/internal/vault"
)

const (
	StatusAwaitingApproval = "awaiting-approval"
	StatusQueued           = "queued"
	StatusRunning          = "running"
	StatusSuccess          = "success"
	StatusFailed           = "failed"
	StatusRejected         = "rejected"
	StatusCancelled        = "cancelled"
	StatusSkipped          = "skipped"

	StepPending   = "pending"
	StepQueued    = "queued"
	StepRunning   = "running"
	StepSuccess   = "success"
	StepFailed    = "failed"
	StepCancelled = "cancelled"
	StepSkipped   = "skipped"
)

type Run struct {
	ID                  int      `json:"id"`
	Owner               string   `json:"owner"`
	Name                string   `json:"name"`
	WorkflowPath        string   `json:"workflowPath"`
	WorkflowName        string   `json:"workflowName"`
	WorkflowContent     string   `json:"workflowContent"`
	Commit              string   `json:"commit"`
	Ref                 string   `json:"ref"`
	RepositoryTree      string   `json:"repositoryTree"`
	ExecutionDigest     string   `json:"executionDigest"`
	Status              string   `json:"status"`
	StepStatuses        []string `json:"stepStatuses"`
	CurrentStepIndex    int      `json:"currentStepIndex"`
	PullNumber          int      `json:"pullNumber,omitempty"`
	PullAuthor          string   `json:"pullAuthor,omitempty"`
	FirstTimePullAuthor bool     `json:"firstTimePullAuthor,omitempty"`
	CreatedAtMs         int64    `json:"createdAtMs"`
	StartedAtMs         int64    `json:"startedAtMs"`
	FinishedAtMs        int64    `json:"finishedAtMs"`
}

func NewRun() Run {
	return Run{Status: StatusQueued, CurrentStepIndex: -1}
}

func (r *Run) RequeueAfterDesktopInterruption() bool {
	if r.Status != StatusRunning {
		return false
	}
	r.Status = StatusQueued
	r.StartedAtMs = 0
	r.FinishedAtMs = 0
	r.StepStatuses = nil
	r.CurrentStepIndex = -1
	return true
}

func CanonicalRepositoryKey(owner, name string) string {
	material := []byte("forkmesh-actions-repository-v2\n")
	material = append(material, strconv.Itoa(len(owner))...)
	material = append(material, ':')
	material = append(material, owner...)
	material = append(material, strconv.Itoa(len(name))...)
	material = append(material, ':')
	material = append(material, name...)
	sum := sha256.Sum256(material)
	return "v2-" + hex.EncodeToString(sum[:])
}

func LegacyRepositoryKey(owner, name string) string {
	var out strings.Builder
	for _, r := range owner + "-" + name {
		if r > 0xFFFF {
			out.WriteString("__")
			continue
		}
		if unicode.IsLetter(r) || unicode.IsNumber(r) || r == '-' || r == '.' {
			out.WriteRune(r)
		} else {
			out.WriteByte('_')
		}
	}
	return out.String()
}

func (r Run) RepoKey() string       { return CanonicalRepositoryKey(r.Owner, r.Name) }
func (r Run) LegacyRepoKey() string { return LegacyRepositoryKey(r.Owner, r.Name) }

func (r Run) jsonObject() map[string]any {
	steps := r.StepStatuses
	if steps == nil {
		steps = []string{}
	}
	out := map[string]any{
		"id":               r.ID,
		"owner":            r.Owner,
		"name":             r.Name,
		"workflowPath":     r.WorkflowPath,
		"workflowName":     r.WorkflowName,
		"workflowContent":  r.WorkflowContent,
		"commit":           r.Commit,
		"ref":              r.Ref,
		"repositoryTree":   r.RepositoryTree,
		"executionDigest":  r.ExecutionDigest,
		"status":           r.Status,
		"stepStatuses":     steps,
		"currentStepIndex": r.CurrentStepIndex,
		"createdAtMs":      r.CreatedAtMs,
		"startedAtMs":      r.StartedAtMs,
		"finishedAtMs":     r.FinishedAtMs,
	}
	if r.PullNumber > 0 {
		out["pullNumber"] = r.PullNumber
		out["pullAuthor"] = r.PullAuthor
		out["firstTimePullAuthor"] = r.FirstTimePullAuthor
	}
	return out
}

func (r Run) Encode() []byte {
	return qtJSONIndented(r.jsonObject())
}

func (r Run) Record() map[string]any {
	out := r.jsonObject()
	if r.StepStatuses == nil {
		out["stepStatuses"] = []string{}
	}
	return out
}

func RunFromObject(o map[string]any) Run {
	r := Run{}
	r.ID = jsonInt(o, "id", 0)
	r.Owner = jsonString(o, "owner")
	r.Name = jsonString(o, "name")
	r.WorkflowPath = jsonString(o, "workflowPath")
	r.WorkflowName = jsonString(o, "workflowName")
	r.WorkflowContent = jsonString(o, "workflowContent")
	r.Commit = jsonString(o, "commit")
	r.Ref = jsonString(o, "ref")
	r.RepositoryTree = jsonString(o, "repositoryTree")
	r.ExecutionDigest = jsonString(o, "executionDigest")
	r.Status = jsonStringOr(o, "status", StatusQueued)
	for _, value := range jsonArray(o, "stepStatuses") {
		text, _ := value.(string)
		if text = qtTrim(text); text != "" {
			r.StepStatuses = append(r.StepStatuses, text)
		}
	}
	r.CurrentStepIndex = jsonInt(o, "currentStepIndex", -1)
	r.PullNumber = jsonInt(o, "pullNumber", 0)
	r.PullAuthor = jsonString(o, "pullAuthor")
	r.FirstTimePullAuthor = jsonBool(o, "firstTimePullAuthor")
	r.CreatedAtMs = jsonLongLong(o, "createdAtMs")
	r.StartedAtMs = jsonLongLong(o, "startedAtMs")
	r.FinishedAtMs = jsonLongLong(o, "finishedAtMs")
	return r
}

func DecodeRun(data []byte) (Run, bool) {
	object, ok := decodeJSONObject(data)
	if !ok {
		return Run{}, false
	}
	return RunFromObject(object), true
}

type cachedRun struct {
	run     Run
	modTime time.Time
	size    int64
}

type Store struct {
	Root string

	mu       sync.Mutex
	migrated bool
	cache    map[string]cachedRun
	nextID   int
}

func NewStore(root string) *Store {
	store := &Store{Root: root, cache: map[string]cachedRun{}}
	_ = os.MkdirAll(store.RunsDir(), 0o700)
	_ = os.MkdirAll(store.SpoolDir(), 0o700)
	return store
}

func (s *Store) RunsDir() string  { return filepath.Join(s.Root, "runs") }
func (s *Store) SpoolDir() string { return filepath.Join(s.Root, "spool") }

func (s *Store) ensure(path string) string {
	_ = os.MkdirAll(path, 0o700)
	return path
}

func (s *Store) ArtifactsDir() string { return s.ensure(filepath.Join(s.Root, "artifacts")) }
func (s *Store) SandboxDir() string   { return s.ensure(filepath.Join(s.Root, "sandbox")) }
func (s *Store) ToolchainBinDir() string {
	return s.ensure(filepath.Join(s.Root, "toolchain", "bin"))
}
func (s *Store) BuildCacheDir(run Run) string {
	return s.ensure(filepath.Join(s.Root, "cache", run.RepoKey()))
}

func (s *Store) RunDir(run Run) string {
	return s.RunsDir() + "/" + run.RepoKey() + "/" + strconv.Itoa(run.ID)
}

func (s *Store) legacyRunDir(run Run) string {
	return s.RunsDir() + "/" + run.LegacyRepoKey() + "/" + strconv.Itoa(run.ID)
}

func runDirectoryMatches(directory string, run Run) bool {
	data, err := os.ReadFile(filepath.Join(directory, "meta.json"))
	if err != nil {
		return false
	}
	stored, ok := DecodeRun(data)
	return ok && stored.ID == run.ID && stored.Owner == run.Owner && stored.Name == run.Name
}

func (s *Store) ExistingRunDir(run Run) string {
	canonical := s.RunDir(run)
	if _, err := os.Stat(canonical); err == nil {
		return canonical
	}
	legacy := s.legacyRunDir(run)
	if runDirectoryMatches(legacy, run) {
		return legacy
	}
	return canonical
}

func (s *Store) SaveRun(run Run) error {
	directory := s.ExistingRunDir(run)
	_ = os.MkdirAll(directory, 0o700)
	path := filepath.Join(directory, "meta.json")
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
	if err != nil {
		return err
	}
	_, werr := file.Write(run.Encode())
	cerr := file.Close()
	if werr != nil {
		return werr
	}
	if cerr != nil {
		return cerr
	}
	if info, err := os.Stat(path); err == nil {
		s.mu.Lock()
		s.cache[path] = cachedRun{run: run, modTime: info.ModTime(), size: info.Size()}
		s.mu.Unlock()
	}
	return nil
}

func (s *Store) AppendLog(run Run, text string) {
	directory := s.ExistingRunDir(run)
	_ = os.MkdirAll(directory, 0o700)
	file, err := os.OpenFile(filepath.Join(directory, "log.txt"), os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644)
	if err != nil {
		return
	}
	defer file.Close()
	data := []byte(text)
	if !strings.HasSuffix(text, "\n") {
		data = append(data, '\n')
	}
	_, _ = file.Write(data)
}

func (s *Store) LogPath(run Run) string {
	return filepath.Join(s.ExistingRunDir(run), "log.txt")
}

func (s *Store) ReadLog(run Run) string {
	data, err := os.ReadFile(s.LogPath(run))
	if err != nil {
		return ""
	}
	return qtFromUtf8(stripCarriageReturns(data))
}

func (s *Store) readMeta(path string) (Run, bool) {
	info, err := os.Stat(path)
	if err != nil {
		return Run{}, false
	}
	s.mu.Lock()
	cached, ok := s.cache[path]
	s.mu.Unlock()
	if ok && cached.modTime.Equal(info.ModTime()) && cached.size == info.Size() {
		return cached.run, true
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return Run{}, false
	}
	run, ok := DecodeRun(data)
	if !ok {
		return Run{}, false
	}
	s.mu.Lock()
	s.cache[path] = cachedRun{run: run, modTime: info.ModTime(), size: info.Size()}
	s.mu.Unlock()
	return run, true
}

func (s *Store) runsIn(repoDir string) []Run {
	entries, err := os.ReadDir(repoDir)
	if err != nil {
		return nil
	}
	var out []Run
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		run, ok := s.readMeta(filepath.Join(repoDir, entry.Name(), "meta.json"))
		if !ok || run.ID <= 0 {
			continue
		}
		out = append(out, run)
	}
	return out
}

func sortNewestFirst(runs []Run) {
	sort.SliceStable(runs, func(i, j int) bool {
		if runs[i].CreatedAtMs != runs[j].CreatedAtMs {
			return runs[i].CreatedAtMs > runs[j].CreatedAtMs
		}
		return runs[i].ID > runs[j].ID
	})
}

func (s *Store) LoadAllRuns() []Run {
	s.Migrate()
	entries, err := os.ReadDir(s.RunsDir())
	if err != nil {
		return nil
	}
	byID := map[int]Run{}
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		for _, run := range s.runsIn(filepath.Join(s.RunsDir(), entry.Name())) {
			byID[run.ID] = run
		}
	}
	out := make([]Run, 0, len(byID))
	for _, run := range byID {
		out = append(out, run)
	}
	sortNewestFirst(out)
	return out
}

func (s *Store) RepoRuns(owner, name string) []Run {
	wanted := strings.ToLower(owner + "/" + name)
	var out []Run
	for _, run := range s.LoadAllRuns() {
		if strings.ToLower(run.Owner+"/"+run.Name) == wanted {
			out = append(out, run)
		}
	}
	return out
}

func (s *Store) FindRun(id int) (Run, bool) {
	for _, run := range s.LoadAllRuns() {
		if run.ID == id {
			return run, true
		}
	}
	return Run{}, false
}

func (s *Store) highestRunID() int {
	highest := 0
	repos, _ := os.ReadDir(s.RunsDir())
	for _, repo := range repos {
		if !repo.IsDir() {
			continue
		}
		ids, _ := os.ReadDir(filepath.Join(s.RunsDir(), repo.Name()))
		for _, id := range ids {
			if value, err := strconv.Atoi(id.Name()); err == nil && id.IsDir() && value > highest {
				highest = value
			}
		}
	}
	for _, run := range s.LoadAllRuns() {
		if run.ID > highest {
			highest = run.ID
		}
	}
	return highest
}

func (s *Store) NoteRunID(id int) {
	s.mu.Lock()
	if id >= s.nextID {
		s.nextID = id + 1
	}
	s.mu.Unlock()
}

func (s *Store) CreateRun(run Run) (Run, error) {
	highest := s.highestRunID()
	s.mu.Lock()
	id := s.nextID
	s.mu.Unlock()
	if id <= highest {
		id = highest + 1
	}
	for attempt := 0; attempt < 1000; attempt++ {
		run.ID = id
		dir := s.RunDir(run)
		if err := os.MkdirAll(filepath.Dir(dir), 0o700); err != nil {
			return Run{}, err
		}
		if err := os.Mkdir(dir, 0o700); err == nil {
			break
		} else if !os.IsExist(err) {
			return Run{}, err
		}
		id++
	}
	s.NoteRunID(run.ID)
	run.CreatedAtMs = nowMs()
	if err := s.SaveRun(run); err != nil {
		return Run{}, err
	}
	return run, nil
}

func (s *Store) DeleteRun(run Run) bool {
	removed := true
	canonical := s.RunDir(run)
	if pathExists(canonical) {
		removed = safedelete.RemoveTreeUnder(filepath.Dir(canonical), canonical) == nil
	}
	legacy := s.legacyRunDir(run)
	if runDirectoryMatches(legacy, run) {
		removed = safedelete.RemoveTreeUnder(filepath.Dir(legacy), legacy) == nil && removed
	}
	if removed {
		s.mu.Lock()
		delete(s.cache, filepath.Join(canonical, "meta.json"))
		delete(s.cache, filepath.Join(legacy, "meta.json"))
		s.mu.Unlock()
	}
	return removed
}

func (s *Store) Migrate() {
	s.mu.Lock()
	if s.migrated {
		s.mu.Unlock()
		return
	}
	s.migrated = true
	s.mu.Unlock()
	root := s.RunsDir()
	repos, err := os.ReadDir(root)
	if err != nil {
		return
	}
	for _, repo := range repos {
		if !repo.IsDir() {
			continue
		}
		repoDir := filepath.Join(root, repo.Name())
		ids, _ := os.ReadDir(repoDir)
		for _, id := range ids {
			if !id.IsDir() {
				continue
			}
			data, err := os.ReadFile(filepath.Join(repoDir, id.Name(), "meta.json"))
			if err != nil {
				continue
			}
			run, ok := DecodeRun(data)
			if !ok || run.ID <= 0 || strconv.Itoa(run.ID) != id.Name() || run.Owner == "" || run.Name == "" {
				continue
			}
			if repo.Name() == run.RepoKey() {
				continue
			}
			destinationParent := filepath.Join(root, run.RepoKey())
			destination := filepath.Join(destinationParent, id.Name())
			if _, err := os.Stat(destination); os.IsNotExist(err) && os.MkdirAll(destinationParent, 0o700) == nil {
				_ = os.Rename(filepath.Join(repoDir, id.Name()), destination)
			}
			oldArtifact := filepath.Join(s.Root, "artifacts", run.LegacyRepoKey(), id.Name())
			newParent := filepath.Join(s.Root, "artifacts", run.RepoKey())
			newArtifact := filepath.Join(newParent, id.Name())
			if _, err := os.Stat(oldArtifact); err == nil {
				if _, err := os.Stat(newArtifact); os.IsNotExist(err) && os.MkdirAll(newParent, 0o700) == nil {
					_ = os.Rename(oldArtifact, newArtifact)
				}
			}
		}
		if left, err := os.ReadDir(repoDir); err == nil && len(left) == 0 {
			_ = os.Remove(repoDir)
		}
	}
}

var (
	objectIDPattern = regexp.MustCompile(`^(?:[0-9a-f]{40}|[0-9a-f]{64})$`)
	fieldPattern    = regexp.MustCompile(`^[^\t\n\v\f\r ]+$`)
)

func (s *Store) SpoolPushEvent(owner, name, commit, ref, before string, pid int) bool {
	lowered := strings.ToLower(qtTrim(commit))
	if !objectIDPattern.MatchString(lowered) || !strings.HasPrefix(ref, "refs/heads/") ||
		!fieldPattern.MatchString(ref) || !fieldPattern.MatchString(owner) || !fieldPattern.MatchString(name) {
		return false
	}
	dir := s.SpoolDir()
	if os.MkdirAll(dir, 0o700) != nil {
		return false
	}
	base := fmt.Sprintf("%d-%d-%s.push", time.Now().Unix(), pid, lowered[:12])
	loweredBefore := strings.ToLower(qtTrim(before))
	oldValue := strings.Repeat("0", len(lowered))
	if objectIDPattern.MatchString(loweredBefore) && len(loweredBefore) == len(lowered) {
		oldValue = loweredBefore
	}
	text := fmt.Sprintf("owner %s\nname %s\nref %s %s %s\n", owner, name, oldValue, lowered, ref)
	temp, err := os.CreateTemp(dir, "."+base+".*")
	if err != nil {
		return false
	}
	_, werr := temp.WriteString(text)
	cerr := temp.Close()
	if werr != nil || cerr != nil || os.Rename(temp.Name(), filepath.Join(dir, base)) != nil {
		_ = os.Remove(temp.Name())
		return false
	}
	return true
}

func approvalKey(repoKey string) string { return "actions/approved/" + repoKey }

func settingsBytes(settings *qsettings.Settings, key string) []byte {
	if settings == nil {
		return nil
	}
	value, ok := settings.Value(key)
	if !ok {
		return nil
	}
	if value.Kind == "bytes" {
		return value.Bytes
	}
	return []byte(value.String())
}

func storedApprovals(settings *qsettings.Settings, repoKey string) map[string]any {
	object, ok := decodeJSONObject(settingsBytes(settings, approvalKey(repoKey)))
	if !ok {
		return map[string]any{}
	}
	return object
}

func IsApproved(settings *qsettings.Settings, repoKey, workflowPath, content, repositoryTree, executionDigest string) bool {
	if repositoryTree == "" || executionDigest == "" {
		return false
	}
	approval, ok := storedApprovals(settings, repoKey)[workflowPath].(map[string]any)
	if !ok {
		return false
	}
	return jsonInt(approval, "schema", 0) == 2 &&
		jsonString(approval, "workflowContent") == content &&
		jsonString(approval, "repositoryTree") == repositoryTree &&
		jsonString(approval, "executionDigest") == executionDigest
}

func LastApprovedContent(settings *qsettings.Settings, repoKey, workflowPath string) string {
	switch value := storedApprovals(settings, repoKey)[workflowPath].(type) {
	case map[string]any:
		return jsonString(value, "workflowContent")
	case string:
		return value
	}
	return ""
}

func Variables(settings *qsettings.Settings) map[string]string {
	return vault.ActionVariablesOr(settings)
}

func NeedsMatches(token, workflowName, workflowPath string) bool {
	wanted := qtTrim(token)
	if wanted == "" {
		return false
	}
	aliases := []string{qtTrim(workflowName), qtTrim(workflowPath), qtFileName(workflowPath), qtCompleteBaseName(workflowPath)}
	for _, alias := range aliases {
		if alias != "" && equalFoldQt(alias, wanted) {
			return true
		}
	}
	return false
}

type NeedsState int

const (
	NeedsReady NeedsState = iota
	NeedsWaiting
	NeedsBlocked
)

func ResolveNeeds(run Run, needs []string, history []Run) (NeedsState, string) {
	state := NeedsReady
	detail := ""
	for _, token := range needs {
		var dependency *Run
		for index := range history {
			candidate := &history[index]
			if candidate.ID == run.ID || candidate.Owner != run.Owner || candidate.Name != run.Name || candidate.Commit != run.Commit {
				continue
			}
			if !NeedsMatches(token, candidate.WorkflowName, candidate.WorkflowPath) {
				continue
			}
			dependency = candidate
			break
		}
		if dependency == nil || dependency.Status == StatusSuccess {
			continue
		}
		pending := dependency.Status == StatusQueued || dependency.Status == StatusRunning || dependency.Status == StatusAwaitingApproval
		if !pending {
			return NeedsBlocked, fmt.Sprintf("\"%s\" %s", dependency.WorkflowName, dependency.Status)
		}
		if state == NeedsReady {
			state = NeedsWaiting
			detail = fmt.Sprintf("\"%s\" is %s", dependency.WorkflowName, dependency.Status)
		}
	}
	return state, detail
}
