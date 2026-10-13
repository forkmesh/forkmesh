package agents

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/safedelete"
)

var (
	ErrUnsupportedSchema = errors.New("unsupported_schema")
	ErrRetired           = errors.New("retired")
	ErrBadSession        = errors.New("bad_session")
)

type taskKind int

const (
	taskAppend taskKind = iota
	taskRemove
	taskBarrier
)

type laneTask struct {
	kind     taskKind
	path     string
	data     []byte
	done     func()
	sequence uint64
}

type lane struct {
	mu        sync.Mutex
	ready     *sync.Cond
	drained   *sync.Cond
	tasks     []laneTask
	next      uint64
	completed uint64
	lastPath  map[string]uint64
	running   bool
}

func newLane() *lane {
	l := &lane{lastPath: map[string]uint64{}}
	l.ready = sync.NewCond(&l.mu)
	l.drained = sync.NewCond(&l.mu)
	return l
}

func (l *lane) enqueue(task laneTask) uint64 {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.next++
	task.sequence = l.next
	if task.path != "" {
		l.lastPath[task.path] = task.sequence
	}
	l.tasks = append(l.tasks, task)
	if !l.running {
		l.running = true
		go l.run()
	}
	l.ready.Signal()
	return task.sequence
}

func (l *lane) run() {
	for {
		l.mu.Lock()
		for len(l.tasks) == 0 {
			l.running = false
			l.mu.Unlock()
			return
		}
		task := l.tasks[0]
		l.tasks[0] = laneTask{}
		l.tasks = l.tasks[1:]
		l.mu.Unlock()
		perform(task)
		l.mu.Lock()
		l.completed = task.sequence
		if l.lastPath[task.path] == task.sequence {
			delete(l.lastPath, task.path)
		}
		l.drained.Broadcast()
		l.mu.Unlock()
	}
}

func perform(task laneTask) {
	switch task.kind {
	case taskRemove:
		_ = os.Remove(task.path)
	case taskBarrier:
		if task.done != nil {
			task.done()
		}
	case taskAppend:
		if err := os.MkdirAll(filepath.Dir(task.path), 0o777); err != nil {
			return
		}
		file, err := os.OpenFile(task.path, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0o666)
		if err != nil {
			return
		}
		_, _ = file.Write(task.data)
		_ = file.Close()
	}
}

func (l *lane) waitFor(target uint64) {
	l.mu.Lock()
	for l.completed < target {
		l.drained.Wait()
	}
	l.mu.Unlock()
}

func (l *lane) flush() {
	l.mu.Lock()
	target := l.next
	l.mu.Unlock()
	l.waitFor(target)
}

func (l *lane) flushPath(path string) {
	l.mu.Lock()
	target := l.lastPath[path]
	l.mu.Unlock()
	l.waitFor(target)
}

type writtenMeta struct {
	bytes []byte
	stamp string
}

type Writer struct {
	store   *Store
	events  *lane
	logs    *lane
	now     func() time.Time
	OnLog   func(id int)
	OnEvent func(id int)

	mu      sync.Mutex
	retired map[string]bool
	written map[string]writtenMeta
	create  sync.Mutex
	syncs   chan string

	observers observerList
}

func NewWriter(store *Store) *Writer {
	_ = os.MkdirAll(store.SessionsDir(), 0o777)
	return &Writer{store: store, events: newLane(), logs: newLane(), now: time.Now,
		retired: map[string]bool{}, written: map[string]writtenMeta{}}
}

func (w *Writer) Store() *Store { return w.store }

func (w *Writer) SetClock(now func() time.Time) { w.now = now }

func (w *Writer) nowMs() int64 { return w.now().UnixMilli() }

func cleanKey(dir string) string {
	abs, err := filepath.Abs(dir)
	if err != nil {
		abs = dir
	}
	return filepath.Clean(abs)
}

func (w *Writer) Retired(dir string) bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	if len(w.retired) == 0 {
		return false
	}
	return w.retired[cleanKey(dir)]
}

func (w *Writer) retire(dir string) {
	w.mu.Lock()
	w.retired[cleanKey(dir)] = true
	w.mu.Unlock()
}

func (w *Writer) revive(dir string) {
	w.mu.Lock()
	delete(w.retired, cleanKey(dir))
	w.mu.Unlock()
}

var repoKeyPattern = regexp.MustCompile(`^[^/\\]+$`)

func ValidRepoKey(key string) bool {
	return key != "" && key != "." && key != ".." && repoKeyPattern.MatchString(key) && !strings.ContainsRune(key, 0)
}

func (w *Writer) DirFor(key string, id int) (string, bool) {
	if !ValidRepoKey(key) || id <= 0 {
		return "", false
	}
	return filepath.Join(w.store.SessionsDir(), key, strconv.Itoa(id)), true
}

func (w *Writer) AppendLog(dir string, id int, text string) {
	if w.Retired(dir) {
		return
	}
	data := []byte(text)
	if !strings.HasSuffix(text, "\n") {
		data = append(data, '\n')
	}
	w.logs.enqueue(laneTask{kind: taskAppend, path: filepath.Join(dir, "transcript.txt"), data: data})
	if w.OnLog != nil {
		w.OnLog(id)
	}
	w.observers.notify(ChangeLog, id)
}

func (w *Writer) AppendEventLine(dir string, id int, compact []byte) {
	if w.Retired(dir) {
		return
	}
	data := make([]byte, 0, len(compact)+1)
	data = append(data, compact...)
	data = append(data, '\n')
	w.events.enqueue(laneTask{kind: taskAppend, path: filepath.Join(dir, "events.jsonl"), data: data})
	if w.OnEvent != nil {
		w.OnEvent(id)
	}
	w.observers.notify(ChangeEvent, id)
}

func (w *Writer) AppendEvent(dir string, id int, event qtjson.Value) {
	w.AppendEventLine(dir, id, event.Compact())
}

func (w *Writer) ClearEvents(dir string) {
	w.events.enqueue(laneTask{kind: taskRemove, path: filepath.Join(dir, "events.jsonl")})
}

func (w *Writer) Flush() {
	w.events.flush()
	w.logs.flush()
}

func (w *Writer) FlushDir(dir string) {
	w.events.flushPath(filepath.Join(dir, "events.jsonl"))
	w.logs.flushPath(filepath.Join(dir, "transcript.txt"))
}

func (w *Writer) WhenDrained(done func()) {
	var once sync.WaitGroup
	once.Add(2)
	go func() {
		once.Wait()
		done()
	}()
	w.events.enqueue(laneTask{kind: taskBarrier, done: once.Done})
	w.logs.enqueue(laneTask{kind: taskBarrier, done: once.Done})
}

func recordObject(session qtjson.Value) (string, int, bool) {
	if !session.IsObject() {
		return "", 0, false
	}
	owner := session.Get("owner").Str()
	name := session.Get("name").Str()
	id := session.Get("id").IntOr(0)
	return sanitizeRepoKey(owner, name), id, true
}

func schemaSupported(session qtjson.Value) bool {
	version := session.Get("schemaVersion").IntOr(0)
	return version >= SchemaMinimum && version <= SchemaCurrent
}

func (w *Writer) SessionDirOf(session qtjson.Value) (string, bool) {
	key, id, ok := recordObject(session)
	if !ok {
		return "", false
	}
	return filepath.Join(w.store.SessionsDir(), key, strconv.Itoa(id)), true
}

func (w *Writer) SaveRecord(session qtjson.Value) (bool, error) {
	if !schemaSupported(session) {
		return false, ErrUnsupportedSchema
	}
	dir, ok := w.SessionDirOf(session)
	if !ok {
		return false, ErrBadSession
	}
	if w.Retired(dir) {
		return false, ErrRetired
	}
	path := filepath.Join(dir, "meta.json")
	data := session.Indented()
	w.mu.Lock()
	last, seen := w.written[path]
	w.mu.Unlock()
	if seen && bytes.Equal(last.bytes, data) && last.stamp == FileStamp(path) {
		return true, nil
	}
	if err := os.MkdirAll(dir, 0o777); err != nil {
		return false, err
	}
	err := replaceFile(path, data, 0o600, w.syncLater)
	w.store.forget(path)
	w.mu.Lock()
	if err == nil {
		w.written[path] = writtenMeta{bytes: data, stamp: FileStamp(path)}
	} else {
		delete(w.written, path)
	}
	w.mu.Unlock()
	if err == nil {
		w.observers.notify(ChangeRecord, session.Get("id").IntOr(0))
	}
	return err == nil, err
}

func (w *Writer) syncLater(path string) {
	w.mu.Lock()
	if w.syncs == nil {
		w.syncs = make(chan string, 256)
		go func(ch chan string) {
			for name := range ch {
				if file, err := os.Open(name); err == nil {
					_ = file.Sync()
					_ = file.Close()
				}
			}
		}(w.syncs)
	}
	ch := w.syncs
	w.mu.Unlock()
	select {
	case ch <- path:
	default:
	}
}

func createTemp(path string, perm os.FileMode) (*os.File, error) {
	for attempt := 0; attempt < 100; attempt++ {
		name := path + "." + strconv.FormatInt(time.Now().UnixNano()%1_000_000_000+int64(attempt), 36)
		file, err := os.OpenFile(name, os.O_WRONLY|os.O_CREATE|os.O_EXCL, perm)
		if err == nil || !os.IsExist(err) {
			return file, err
		}
	}
	return nil, os.ErrExist
}

func replaceFile(path string, data []byte, perm os.FileMode, after func(string)) error {
	temp, err := createTemp(path, perm)
	if err != nil {
		return err
	}
	name := temp.Name()
	if _, err := temp.Write(data); err != nil {
		temp.Close()
		_ = os.Remove(name)
		return err
	}
	if err := temp.Close(); err != nil {
		_ = os.Remove(name)
		return err
	}
	if err := os.Rename(name, path); err != nil {
		_ = os.Remove(name)
		return err
	}
	if after != nil {
		after(path)
	}
	return nil
}

func numericName(name string) int {
	n := 0
	for i := 0; i < len(name); i++ {
		c := name[i]
		if c < '0' || c > '9' {
			if i == 0 && (c == '+' || c == '-') && len(name) > 1 {
				continue
			}
			return 0
		}
		n = n*10 + int(c-'0')
		if n > 1<<31-1 {
			return 0
		}
	}
	if strings.HasPrefix(name, "-") {
		return -n
	}
	return n
}

func (w *Writer) NextID() int {
	maxID := 0
	for _, tree := range []string{w.store.SessionsDir(), w.store.RecycleBinDir()} {
		for _, repo := range listDirs(tree) {
			for _, id := range listDirs(filepath.Join(tree, repo)) {
				maxID = max(maxID, numericName(id))
			}
		}
	}
	return maxID + 1
}

func (w *Writer) CreateRecord(session qtjson.Value) (qtjson.Value, error) {
	if !session.IsObject() {
		return qtjson.Value{}, ErrBadSession
	}
	w.create.Lock()
	defer w.create.Unlock()
	record := session.Clone()
	record.Set("id", qtjson.NewInt(int64(w.NextID())))
	record.Set("createdAtMs", qtjson.NewInt(w.nowMs()))
	dir, _ := w.SessionDirOf(record)
	w.revive(dir)
	_ = os.MkdirAll(dir, 0o777)
	_, _ = w.SaveRecord(record)
	return record, nil
}

func (w *Writer) DeleteRecord(session qtjson.Value) (bool, error) {
	dir, ok := w.SessionDirOf(session)
	if !ok {
		return false, ErrBadSession
	}
	w.FlushDir(dir)
	w.retire(dir)
	if _, err := os.Stat(dir); err != nil {
		return true, nil
	}
	if err := safedelete.RemoveTreeUnder(w.store.SessionsDir(), dir); err != nil {
		w.revive(dir)
		return false, err
	}
	w.recordRetired(session)
	w.observers.notify(ChangeRemoved, session.Get("id").IntOr(0))
	return true, nil
}

func (w *Writer) RecycleRecord(session qtjson.Value) (bool, error) {
	key, id, ok := recordObject(session)
	if !ok {
		return false, ErrBadSession
	}
	dir := filepath.Join(w.store.SessionsDir(), key, strconv.Itoa(id))
	w.FlushDir(dir)
	w.retire(dir)
	if _, err := os.Stat(dir); err != nil {
		return true, nil
	}
	target := filepath.Join(w.store.RecycleBinDir(), key, strconv.Itoa(id))
	if err := os.MkdirAll(filepath.Dir(target), 0o777); err != nil {
		w.revive(dir)
		return false, err
	}
	if _, err := os.Stat(target); err == nil {
		_ = safedelete.RemoveTreeUnder(w.store.RecycleBinDir(), target)
	}
	if err := os.Rename(dir, target); err != nil {
		w.revive(dir)
		return false, err
	}
	stamp := qtjson.ObjectOf("deletedAtMs", qtjson.NewInt(w.nowMs()))
	_ = os.WriteFile(filepath.Join(target, "deleted-at.json"), stamp.Compact(), 0o666)
	w.recordRetired(session)
	w.observers.notify(ChangeRemoved, id)
	return true, nil
}

func (w *Writer) RestoreRecycled(id int) (bool, error) {
	ok, _, err := w.RestoreRecycledPath(id)
	return ok, err
}

func (w *Writer) RestoreRecycledPath(id int) (bool, string, error) {
	binDir := w.store.FindRecycledSessionDir(id)
	if binDir == "" {
		return false, "", nil
	}
	data, err := os.ReadFile(filepath.Join(binDir, "meta.json"))
	if err != nil {
		return false, "", nil
	}
	obj, ok := qtjson.ParseObject(data)
	if !ok {
		return false, "", nil
	}
	session := SessionFromQt(obj)
	target := w.store.SessionDir(session)
	if _, err := os.Stat(target); err == nil {
		return false, "", nil
	}
	if err := os.MkdirAll(filepath.Dir(target), 0o777); err != nil {
		return false, "", err
	}
	if err := os.Rename(binDir, target); err != nil {
		return false, "", err
	}
	_ = os.Remove(filepath.Join(target, "deleted-at.json"))
	w.revive(target)
	if session.Status == StatusQueued || session.Status == StatusRunning || session.Status == StatusWaiting {
		session.Status = StatusStopped
		_, _ = w.SaveRecord(session.ToQt())
	}
	w.observers.notify(ChangeRestored, session.ID)
	return true, "sessions/" + session.RepoKey() + "/" + strconv.Itoa(session.ID), nil
}

func (w *Writer) PurgeRecycled(id int) (bool, error) {
	binDir := w.store.FindRecycledSessionDir(id)
	if binDir == "" {
		return false, nil
	}
	if err := safedelete.RemoveTreeUnder(w.store.RecycleBinDir(), binDir); err != nil {
		return false, err
	}
	w.observers.notify(ChangeBin, id)
	return true, nil
}

func (w *Writer) PurgeExpired(retentionMs, nowMs int64) int {
	if retentionMs <= 0 {
		return 0
	}
	purged := 0
	root := w.store.RecycleBinDir()
	for _, repo := range listDirs(root) {
		repoDir := filepath.Join(root, repo)
		for _, id := range listDirs(repoDir) {
			dir := filepath.Join(repoDir, id)
			deletedAt := readDeletedAt(filepath.Join(dir, "deleted-at.json"))
			if deletedAt <= 0 {
				if info, err := os.Stat(dir); err == nil {
					deletedAt = info.ModTime().UnixMilli()
				}
			}
			if nowMs-deletedAt < retentionMs {
				continue
			}
			if safedelete.RemoveTreeUnder(root, dir) == nil {
				purged++
				w.observers.notify(ChangeBin, numericName(id))
			}
		}
		if len(listDirs(repoDir)) == 0 {
			_ = safedelete.RemoveTreeUnder(root, repoDir)
		}
	}
	return purged
}

func (w *Writer) WritePatch(dir, patch string) bool {
	if w.Retired(dir) {
		return false
	}
	if err := os.MkdirAll(dir, 0o777); err != nil {
		return false
	}
	return os.WriteFile(filepath.Join(dir, "changes.patch"), []byte(patch), 0o666) == nil
}

func (w *Writer) recordRetired(session qtjson.Value) {
	provider := session.Get("provider").Str()
	model := session.Get("model").Str()
	merged := session.Get("merged").BoolValue()
	key := ModelOutcomeKey(provider, model)
	if key == "" {
		return
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	path := w.store.ModelOutcomesPath()
	models := qtjson.NewArray()
	if data, err := os.ReadFile(path); err == nil {
		if obj, ok := qtjson.ParseObject(data); ok && obj.Get("models").IsArray() {
			models = obj.Get("models")
		}
	}
	mergedCount := int64(0)
	if merged {
		mergedCount = 1
	}
	credited := false
	items := models.Items()
	for i := range items {
		entry := items[i]
		if ModelOutcomeKey(entry.Get("provider").Str(), entry.Get("model").Str()) != key {
			continue
		}
		if !entry.IsObject() {
			entry = qtjson.NewObject()
		}
		entry.Set("runs", qtjson.NewInt(int64(max(0, entry.Get("runs").Int())+1)))
		entry.Set("merged", qtjson.NewInt(int64(max(0, entry.Get("merged").Int()))+mergedCount))
		items[i] = entry
		credited = true
		break
	}
	if !credited {
		models.Append(qtjson.ObjectOf("provider", provider, "model", strings.ToLower(trimmed(model)),
			"runs", 1, "merged", mergedCount))
	}
	doc := qtjson.ObjectOf("version", 1, "models", models)
	_ = os.MkdirAll(w.store.Root, 0o777)
	_ = replaceFile(path, doc.Indented(), 0o666, nil)
}
