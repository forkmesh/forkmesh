package agents

import (
	"bufio"
	"bytes"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
)

const (
	TranscriptSearchTailBytes = 512 * 1024
	AttachmentScanBytes       = 128 * 1024

	DefaultRecycleRetentionHours = 24
	MinRecycleRetentionHours     = 1
	MaxRecycleRetentionHours     = 24 * 30
)

type Store struct {
	Root string

	mu    sync.Mutex
	cache map[string]cachedMeta
}

type cachedMeta struct {
	stamp   string
	session *Session
}

type ModelOutcome struct {
	Provider string `json:"provider"`
	Model    string `json:"model"`
	Runs     int    `json:"runs"`
	Merged   int    `json:"merged"`
}

func NewStore(root string) *Store {
	return &Store{Root: root, cache: map[string]cachedMeta{}}
}

func (s *Store) SessionsDir() string { return filepath.Join(s.Root, "sessions") }

func (s *Store) RecycleBinDir() string { return filepath.Join(s.Root, "recycle-bin") }

func (s *Store) ModelOutcomesPath() string { return filepath.Join(s.Root, "model-outcomes.json") }

func (s *Store) SessionDir(session Session) string {
	return filepath.Join(s.SessionsDir(), session.RepoKey(), strconv.Itoa(session.ID))
}

func (s *Store) RecycledSessionDir(session Session) string {
	return filepath.Join(s.RecycleBinDir(), session.RepoKey(), strconv.Itoa(session.ID))
}

func (s *Store) MetaPath(session Session) string {
	return filepath.Join(s.SessionDir(session), "meta.json")
}

func (s *Store) LogPath(session Session) string {
	return filepath.Join(s.SessionDir(session), "transcript.txt")
}

func (s *Store) EventsPath(session Session) string {
	return filepath.Join(s.SessionDir(session), "events.jsonl")
}

func (s *Store) PatchPath(session Session) string {
	return filepath.Join(s.SessionDir(session), "changes.patch")
}

func (s *Store) FindRecycledSessionDir(id int) string {
	if id <= 0 {
		return ""
	}
	wanted := strconv.Itoa(id)
	for _, repo := range listDirs(s.RecycleBinDir()) {
		candidate := filepath.Join(s.RecycleBinDir(), repo, wanted)
		if _, err := os.Stat(candidate); err == nil {
			return candidate
		}
	}
	return ""
}

func listDirs(dir string) []string {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	out := make([]string, 0, len(entries))
	for _, entry := range entries {
		name := entry.Name()
		if strings.HasPrefix(name, ".") {
			continue
		}
		if entry.IsDir() {
			out = append(out, name)
			continue
		}
		if entry.Type()&os.ModeSymlink != 0 {
			if info, err := os.Stat(filepath.Join(dir, name)); err == nil && info.IsDir() {
				out = append(out, name)
			}
		}
	}
	return out
}

func statStamp(path string) string {
	info, err := os.Stat(path)
	if err != nil {
		return "-"
	}
	return strconv.FormatInt(info.Size(), 10) + ":" + strconv.FormatInt(info.ModTime().UnixNano(), 10)
}

func FileStamp(path string) string {
	info, err := os.Stat(path)
	if err != nil {
		return "-"
	}
	return strconv.FormatInt(info.Size(), 10) + ":" + strconv.FormatInt(info.ModTime().UnixMilli(), 10)
}

func readDeletedAt(path string) int64 {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0
	}
	obj, ok := parseObject(data)
	if !ok {
		return 0
	}
	return qtLongLong(decodeValue(obj["deletedAtMs"]))
}

func (s *Store) loadMeta(dir string, recycled bool, seen map[string]bool) *Session {
	path := filepath.Join(dir, "meta.json")
	stampPath := filepath.Join(dir, "deleted-at.json")
	seen[path] = true
	stamp := statStamp(path)
	if recycled {
		stamp += "|" + statStamp(stampPath)
	}
	s.mu.Lock()
	if s.cache == nil {
		s.cache = map[string]cachedMeta{}
	}
	if hit, ok := s.cache[path]; ok && hit.stamp == stamp {
		s.mu.Unlock()
		return hit.session
	}
	s.mu.Unlock()
	data, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	var session *Session
	if obj, ok := parseObject(data); ok {
		parsed := SessionFromObject(obj)
		if recycled {
			parsed.DeletedAtMs = readDeletedAt(stampPath)
		}
		session = &parsed
	}
	s.mu.Lock()
	s.cache[path] = cachedMeta{stamp: stamp, session: session}
	s.mu.Unlock()
	return session
}

func (s *Store) forget(path string) {
	s.mu.Lock()
	delete(s.cache, path)
	s.mu.Unlock()
}

func (s *Store) prune(root string, seen map[string]bool) {
	prefix := root + string(filepath.Separator)
	s.mu.Lock()
	defer s.mu.Unlock()
	for path := range s.cache {
		if strings.HasPrefix(path, prefix) && !seen[path] {
			delete(s.cache, path)
		}
	}
}

func (s *Store) loadTree(root string, recycled bool) []Session {
	seen := map[string]bool{}
	sessions := []Session{}
	for _, repo := range listDirs(root) {
		repoDir := filepath.Join(root, repo)
		for _, id := range listDirs(repoDir) {
			if session := s.loadMeta(filepath.Join(repoDir, id), recycled, seen); session != nil {
				clone := *session
				clone.TeammateIDs = append([]string(nil), session.TeammateIDs...)
				clone.SkillsUsed = append([]string(nil), session.SkillsUsed...)
				sessions = append(sessions, clone)
			}
		}
	}
	s.prune(root, seen)
	return sessions
}

func (s *Store) LoadAllSessions() []Session {
	sessions := s.loadTree(s.SessionsDir(), false)
	sort.SliceStable(sessions, func(i, j int) bool {
		a, b := sessions[i].UpdatedAtMs(), sessions[j].UpdatedAtMs()
		if a != b {
			return a > b
		}
		return sessions[i].ID > sessions[j].ID
	})
	return sessions
}

func (s *Store) LoadRecycledSessions() []Session {
	sessions := s.loadTree(s.RecycleBinDir(), true)
	sort.SliceStable(sessions, func(i, j int) bool {
		if sessions[i].DeletedAtMs != sessions[j].DeletedAtMs {
			return sessions[i].DeletedAtMs > sessions[j].DeletedAtMs
		}
		return sessions[i].ID > sessions[j].ID
	})
	return sessions
}

func (s *Store) FindSession(id int) (Session, bool) {
	if id <= 0 {
		return Session{}, false
	}
	for _, session := range s.LoadAllSessions() {
		if session.ID == id {
			return session, true
		}
	}
	return Session{}, false
}

func QueuedSessionIDsOldestFirst(sessions []Session) []int {
	queued := []Session{}
	for _, session := range sessions {
		if session.Status == StatusQueued && !session.Held {
			queued = append(queued, session)
		}
	}
	sort.SliceStable(queued, func(i, j int) bool {
		a, b := queued[i], queued[j]
		if a.Priority != b.Priority {
			return a.Priority < b.Priority
		}
		if a.CreatedAtMs != b.CreatedAtMs {
			return a.CreatedAtMs < b.CreatedAtMs
		}
		return a.ID < b.ID
	})
	ids := make([]int, len(queued))
	for i, session := range queued {
		ids[i] = session.ID
	}
	return ids
}

func ModelOutcomeKey(provider, model string) string {
	id := strings.ToLower(trimmed(model))
	if id == "" {
		return ""
	}
	return provider + "\x1f" + id
}

func (s *Store) RetiredModelOutcomes() map[string]ModelOutcome {
	outcomes := map[string]ModelOutcome{}
	data, err := os.ReadFile(s.ModelOutcomesPath())
	if err != nil {
		return outcomes
	}
	obj, ok := parseObject(data)
	if !ok {
		return outcomes
	}
	for _, entry := range qtArray(decodeValue(obj["models"])) {
		record, _ := entry.(map[string]any)
		provider := qtString(record["provider"], "")
		model := qtString(record["model"], "")
		key := ModelOutcomeKey(provider, model)
		if key == "" {
			continue
		}
		outcome, exists := outcomes[key]
		if !exists {
			outcome = ModelOutcome{Provider: provider, Model: strings.ToLower(trimmed(model))}
		}
		outcome.Runs += max(0, qtInt(record["runs"], 0))
		outcome.Merged += max(0, qtInt(record["merged"], 0))
		outcomes[key] = outcome
	}
	return outcomes
}

func stripCR(data []byte) []byte {
	if bytes.IndexByte(data, '\r') < 0 {
		return data
	}
	out := make([]byte, 0, len(data))
	for _, c := range data {
		if c != '\r' {
			out = append(out, c)
		}
	}
	return out
}

func readTextFile(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return string(stripCR(data))
}

func (s *Store) ReadLog(session Session) string { return readTextFile(s.LogPath(session)) }

func (s *Store) ReadPatch(session Session) string { return readTextFile(s.PatchPath(session)) }

func (s *Store) LoadEvents(session Session) []json.RawMessage {
	events, _, _ := readEvents(s.EventsPath(session), 0, 0)
	return events
}

func readEvents(path string, offset, limit int64) ([]json.RawMessage, int64, bool) {
	events := []json.RawMessage{}
	file, err := os.Open(path)
	if err != nil {
		return events, 0, false
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return events, 0, false
	}
	size := info.Size()
	if offset < 0 || offset > size {
		offset = 0
	}
	if _, err := file.Seek(offset, io.SeekStart); err != nil {
		return events, offset, false
	}
	reader := bufio.NewReaderSize(file, 256<<10)
	next := offset
	for {
		if limit > 0 && next-offset >= limit {
			return events, next, next < size
		}
		line, err := reader.ReadBytes('\n')
		complete := err == nil
		if len(line) > 0 {
			body := bytes.TrimSpace(line)
			parsed := false
			if len(body) > 0 && body[0] == '{' && json.Valid(body) {
				events = append(events, append(json.RawMessage(nil), body...))
				parsed = true
			}
			if complete || parsed || len(body) == 0 {
				next += int64(len(line))
			}
		}
		if err != nil {
			return events, next, false
		}
	}
}

func readTranscriptTail(path string, tailBytes int64) string {
	file, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return ""
	}
	size := info.Size()
	if size > tailBytes {
		if _, err := file.Seek(size-tailBytes, io.SeekStart); err != nil {
			return ""
		}
		reader := bufio.NewReader(file)
		if _, err := reader.ReadBytes('\n'); err != nil {
			return ""
		}
		rest, _ := io.ReadAll(reader)
		return string(rest)
	}
	data, _ := io.ReadAll(file)
	return string(data)
}

func readTranscriptEnds(path string, endBytes int64) string {
	file, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return ""
	}
	size := info.Size()
	if size <= 2*endBytes {
		data, _ := io.ReadAll(file)
		return string(data)
	}
	head := make([]byte, endBytes)
	n, _ := io.ReadFull(file, head)
	head = head[:n]
	if cut := bytes.LastIndexByte(head, '\n'); cut >= 0 {
		head = head[:cut+1]
	} else {
		head = head[:0]
	}
	if _, err := file.Seek(size-endBytes, io.SeekStart); err != nil {
		return string(head)
	}
	reader := bufio.NewReader(file)
	if _, err := reader.ReadBytes('\n'); err != nil {
		return string(head)
	}
	rest, _ := io.ReadAll(reader)
	return string(head) + string(rest)
}

func (s *Store) TranscriptStamp(session Session) string {
	return FileStamp(s.LogPath(session)) + "|" + FileStamp(s.EventsPath(session))
}

func (s *Store) AttachmentPaths(session Session) []string {
	paths := AttachmentPathsIn(session.Prompt)
	for _, name := range []string{s.LogPath(session), s.EventsPath(session)} {
		for _, path := range AttachmentPathsIn(readTranscriptEnds(name, AttachmentScanBytes)) {
			if !containsString(paths, path, false) {
				paths = append(paths, path)
			}
		}
	}
	return paths
}
