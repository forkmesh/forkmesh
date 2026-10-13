package stats

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode"
)

const (
	historyDays          = 30
	statsSkip            = ".forkmesh/stats/"
	generatedStatsPath   = ".forkmesh/stats/repository.json"
	ratchetEnabledKey    = "forkmesh.ratchet.enabled"
	repositoryJSONKey    = "forkmesh.stats.repositoryJson"
	ratchetCeilingDayKey = "forkmesh.ratchet.ceilingDay"
	ratchetCeilingKey    = "forkmesh.ratchet.ceilingBytes"
	gitTimeout           = 30 * time.Second
	gitOutputTimeout     = 3 * time.Second
	configWriteError     = "Could not write the repository's local Git config."
	maxTreeCache         = 4096
)

type repoSample struct {
	Day   string
	Bytes int64
	Lines int64
	Files int64
}

type ratchetReading struct {
	Enabled      bool
	CeilingBytes int64
	CeilingDay   string
}

type fileMeasure struct {
	size    int64
	modTime int64
	bytes   int64
	lines   int64
}

type historyKey struct {
	dir   string
	head  string
	first string
}

type history struct {
	commits   []commitDay
	lineDelta map[string]int64
}

type commitDay struct {
	day string
	sha string
}

type repoStatsStore struct {
	git string
	now func() time.Time

	mu       sync.Mutex
	files    map[string]map[string]fileMeasure
	trees    map[string]repoSample
	treeKeys []string
	history  map[historyKey]history
	counts   map[string]countEntry
	locks    map[string]*sync.Mutex
}

type countEntry struct {
	head  string
	count int64
}

func newRepoStatsStore(git string, now func() time.Time) *repoStatsStore {
	return &repoStatsStore{git: git, now: now, files: map[string]map[string]fileMeasure{},
		trees: map[string]repoSample{}, history: map[historyKey]history{}, counts: map[string]countEntry{},
		locks: map[string]*sync.Mutex{}}
}

func (s *repoStatsStore) lock(dir string) func() {
	s.mu.Lock()
	lock := s.locks[dir]
	if lock == nil {
		lock = &sync.Mutex{}
		s.locks[dir] = lock
	}
	s.mu.Unlock()
	lock.Lock()
	return lock.Unlock
}

func (s *repoStatsStore) run(dir string, timeout time.Duration, args ...string) ([]byte, bool) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, s.git, args...)
	cmd.Dir = dir
	var stdout bytes.Buffer
	cmd.Stdout = &stdout
	if err := cmd.Run(); err != nil {
		return nil, false
	}
	return stdout.Bytes(), true
}

func (s *repoStatsStore) today() string {
	return s.now().Format("2006-01-02")
}

func statsPath(dir string) string {
	return filepath.Join(dir, filepath.FromSlash(generatedStatsPath))
}

func readDocument(path string) map[string]any {
	data, err := os.ReadFile(path)
	if err != nil {
		return map[string]any{}
	}
	var out map[string]any
	if json.Unmarshal(data, &out) != nil || out == nil {
		return map[string]any{}
	}
	return out
}

func compactJSON(value any) ([]byte, error) {
	var buf bytes.Buffer
	encoder := json.NewEncoder(&buf)
	encoder.SetEscapeHTML(false)
	if err := encoder.Encode(value); err != nil {
		return nil, err
	}
	return bytes.TrimRight(buf.Bytes(), "\n"), nil
}

func writeDocument(path string, document map[string]any) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	data, err := compactJSON(document)
	if err != nil {
		return err
	}
	mode := os.FileMode(0o644)
	if info, err := os.Stat(path); err == nil {
		mode = info.Mode().Perm()
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), ".repository.json.*")
	if err != nil {
		return err
	}
	name := tmp.Name()
	_, werr := tmp.Write(data)
	cerr := tmp.Close()
	if werr == nil {
		werr = cerr
	}
	if werr == nil {
		werr = os.Chmod(name, mode)
	}
	if werr == nil {
		werr = os.Rename(name, path)
	}
	if werr != nil {
		_ = os.Remove(name)
	}
	return werr
}

func (s *repoStatsStore) readLocalConfig(dir, key string) (string, bool) {
	out, ok := s.run(dir, gitTimeout, "config", "--local", "--get", key)
	if !ok {
		return "", false
	}
	return strings.TrimSpace(string(out)), true
}

func (s *repoStatsStore) writeLocalConfig(dir, key, value string) error {
	if _, ok := s.run(dir, gitTimeout, "config", "--local", key, value); !ok {
		return errors.New(configWriteError)
	}
	return nil
}

func (s *repoStatsStore) localConfigBool(dir, key string) (bool, bool) {
	value, found := s.readLocalConfig(dir, key)
	if !found {
		return false, false
	}
	switch strings.ToLower(value) {
	case "true", "yes", "on", "1":
		return true, true
	}
	return false, true
}

func (s *repoStatsStore) localConfigInteger(dir, key string, fallback int64) int64 {
	value, found := s.readLocalConfig(dir, key)
	if !found {
		return fallback
	}
	parsed, err := strconv.ParseInt(value, 10, 64)
	if err != nil {
		return fallback
	}
	return parsed
}

func (s *repoStatsStore) writeRatchetCeiling(dir string, sample repoSample) error {
	if err := s.writeLocalConfig(dir, ratchetCeilingDayKey, sample.Day); err != nil {
		return err
	}
	return s.writeLocalConfig(dir, ratchetCeilingKey, strconv.FormatInt(sample.Bytes, 10))
}

func (s *repoStatsStore) repositoryJSONEnabled(dir string) bool {
	enabled, _ := s.localConfigBool(dir, repositoryJSONKey)
	return enabled
}

func (s *repoStatsStore) ratchetEnabled(dir string) bool {
	if enabled, found := s.localConfigBool(dir, ratchetEnabledKey); found {
		return enabled
	}
	enabled, _ := readDocument(statsPath(dir))["ratchet"].(bool)
	return enabled
}

func (s *repoStatsStore) ratchetReading(dir string) ratchetReading {
	reading := ratchetReading{Enabled: s.ratchetEnabled(dir)}
	if !reading.Enabled {
		return reading
	}
	reading.CeilingBytes = s.localConfigInteger(dir, ratchetCeilingKey, 0)
	reading.CeilingDay, _ = s.readLocalConfig(dir, ratchetCeilingDayKey)
	return reading
}

func (s *repoStatsStore) ceilingBytes(dir string, now repoSample, refreshStaleDay bool) int64 {
	stored := s.localConfigInteger(dir, ratchetCeilingKey, 0)
	if day, ok := s.readLocalConfig(dir, ratchetCeilingDayKey); ok && day == now.Day && stored > 0 {
		return stored
	}
	if !refreshStaleDay {
		return stored
	}
	_ = s.writeRatchetCeiling(dir, now)
	return now.Bytes
}

func (s *repoStatsStore) setRatchetEnabled(dir string, enabled bool) error {
	if enabled {
		now, err := s.measure(dir)
		if now.Day == "" {
			return err
		}
		if err := s.writeRatchetCeiling(dir, now); err != nil {
			return err
		}
	} else {
		s.run(dir, gitTimeout, "config", "--local", "--unset", ratchetCeilingDayKey)
		s.run(dir, gitTimeout, "config", "--local", "--unset", ratchetCeilingKey)
	}
	value := "false"
	if enabled {
		value = "true"
	}
	return s.writeLocalConfig(dir, ratchetEnabledKey, value)
}

func countLines(data []byte) int64 {
	if bytes.IndexByte(data, 0) >= 0 {
		return 0
	}
	lines := int64(bytes.Count(data, []byte{'\n'}))
	if len(data) > 0 && data[len(data)-1] != '\n' {
		lines++
	}
	return lines
}

func (s *repoStatsStore) measure(dir string) (repoSample, error) {
	sample := repoSample{Day: s.today()}
	listing, ok := s.run(dir, gitTimeout, "ls-files", "-z")
	if !ok {
		return repoSample{}, errors.New("git ls-files failed")
	}
	var paths []string
	for _, encoded := range bytes.Split(listing, []byte{0}) {
		if len(encoded) == 0 || bytes.HasPrefix(encoded, []byte(statsSkip)) {
			continue
		}
		paths = append(paths, string(encoded))
	}
	s.mu.Lock()
	previous := s.files[dir]
	s.mu.Unlock()
	results := make([]fileMeasure, len(paths))
	present := make([]bool, len(paths))
	workers := runtime.NumCPU()
	if workers > 8 {
		workers = 8
	}
	if workers < 1 {
		workers = 1
	}
	var wg sync.WaitGroup
	next := make(chan int)
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range next {
				full := filepath.Join(dir, filepath.FromSlash(paths[i]))
				info, err := os.Stat(full)
				if err != nil || info.IsDir() {
					continue
				}
				stamp := info.ModTime().UnixNano()
				if cached, ok := previous[paths[i]]; ok && cached.size == info.Size() && cached.modTime == stamp {
					results[i] = cached
					present[i] = true
					continue
				}
				data, err := os.ReadFile(full)
				if err != nil {
					continue
				}
				results[i] = fileMeasure{size: info.Size(), modTime: stamp, bytes: int64(len(data)), lines: countLines(data)}
				present[i] = true
			}
		}()
	}
	for i := range paths {
		next <- i
	}
	close(next)
	wg.Wait()
	fresh := make(map[string]fileMeasure, len(paths))
	for i, path := range paths {
		if !present[i] {
			continue
		}
		fresh[path] = results[i]
		sample.Bytes += results[i].bytes
		sample.Lines += results[i].lines
		sample.Files++
	}
	s.mu.Lock()
	s.files[dir] = fresh
	s.mu.Unlock()
	return sample, nil
}

func simplifiedFields(text string) []string {
	return strings.FieldsFunc(text, func(r rune) bool {
		return r == ' ' || r == '\t' || r == '\n' || r == '\r' || r == '\v' || r == '\f'
	})
}

func (s *repoStatsStore) treeSample(dir, sha string) repoSample {
	s.mu.Lock()
	cached, ok := s.trees[sha]
	s.mu.Unlock()
	if ok {
		return cached
	}
	var sample repoSample
	listing, ok := s.run(dir, gitTimeout, "ls-tree", "-r", "-l", sha)
	if !ok {
		return sample
	}
	for _, line := range bytes.Split(listing, []byte{'\n'}) {
		tab := bytes.IndexByte(line, '\t')
		if tab < 0 || bytes.HasPrefix(line[tab+1:], []byte(statsSkip)) {
			continue
		}
		fields := simplifiedFields(string(line[:tab]))
		if len(fields) < 4 || fields[1] != "blob" {
			continue
		}
		size, _ := strconv.ParseInt(fields[3], 10, 64)
		sample.Bytes += size
		sample.Files++
	}
	s.mu.Lock()
	if _, exists := s.trees[sha]; !exists {
		if len(s.treeKeys) >= maxTreeCache {
			delete(s.trees, s.treeKeys[0])
			s.treeKeys = s.treeKeys[1:]
		}
		s.trees[sha] = sample
		s.treeKeys = append(s.treeKeys, sha)
	}
	s.mu.Unlock()
	return sample
}

func validISODate(text string) (time.Time, bool) {
	if len(text) != 10 {
		return time.Time{}, false
	}
	day, err := time.ParseInLocation("2006-01-02", text, time.UTC)
	return day, err == nil
}

func (s *repoStatsStore) loadHistory(dir, first string) history {
	head := ""
	if out, ok := s.run(dir, gitOutputTimeout, "rev-parse", "--verify", "-q", "HEAD"); ok {
		head = strings.TrimSpace(string(out))
	}
	key := historyKey{dir: dir, head: head, first: first}
	if head != "" {
		s.mu.Lock()
		cached, ok := s.history[key]
		s.mu.Unlock()
		if ok {
			return cached
		}
	}
	var result history
	var numstat []byte
	numstatOK := false
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		numstat, numstatOK = s.run(dir, gitTimeout, "log", "--first-parent", "-m", "--numstat", "--format=%x01%cs", "--since", first)
	}()
	logOut, logOK := s.run(dir, gitTimeout, "log", "--first-parent", "--format=%H %cs")
	wg.Wait()
	for _, line := range bytes.Split(logOut, []byte{'\n'}) {
		space := bytes.IndexByte(line, ' ')
		if space <= 0 {
			continue
		}
		rest := line[space+1:]
		if len(rest) > 10 {
			rest = rest[:10]
		}
		if _, ok := validISODate(string(rest)); ok {
			result.commits = append(result.commits, commitDay{day: string(rest), sha: string(line[:space])})
		}
	}
	result.lineDelta = map[string]int64{}
	if len(result.commits) > 0 {
		cursor := ""
		for _, line := range bytes.Split(numstat, []byte{'\n'}) {
			if len(line) > 0 && line[0] == 0x01 {
				rest := line[1:]
				if len(rest) > 10 {
					rest = rest[:10]
				}
				cursor = string(rest)
				continue
			}
			fields := bytes.Split(line, []byte{'\t'})
			if len(fields) < 3 || string(fields[0]) == "-" {
				continue
			}
			if bytes.HasPrefix(fields[2], []byte(statsSkip)) {
				continue
			}
			added, _ := strconv.ParseInt(string(fields[0]), 10, 64)
			deleted, _ := strconv.ParseInt(string(fields[1]), 10, 64)
			result.lineDelta[cursor] += added - deleted
		}
	}
	if head != "" && logOK && numstatOK {
		s.mu.Lock()
		for existing := range s.history {
			if existing.dir == dir {
				delete(s.history, existing)
			}
		}
		s.history[key] = result
		s.mu.Unlock()
	}
	return result
}

func (s *repoStatsStore) backfill(dir string, today repoSample) []repoSample {
	last, ok := validISODate(today.Day)
	if !ok {
		return []repoSample{today}
	}
	first := last.AddDate(0, 0, -(historyDays - 1))
	hist := s.loadHistory(dir, first.Format("2006-01-02"))
	if len(hist.commits) == 0 {
		return []repoSample{today}
	}
	shaFor := func(key string) string {
		for _, commit := range hist.commits {
			if commit.day <= key {
				return commit.sha
			}
		}
		return ""
	}
	s.prefetchTrees(dir, last, first, today.Day, shaFor)
	var days []repoSample
	lines := today.Lines
	for day := last; !day.Before(first); day = day.AddDate(0, 0, -1) {
		key := day.Format("2006-01-02")
		if key == today.Day {
			days = append(days, today)
		} else {
			sha := shaFor(key)
			if sha == "" {
				break
			}
			sample := s.treeSample(dir, sha)
			sample.Day = key
			if lines < 0 {
				sample.Lines = 0
			} else {
				sample.Lines = lines
			}
			days = append(days, sample)
		}
		lines -= hist.lineDelta[key]
	}
	for i, j := 0, len(days)-1; i < j; i, j = i+1, j-1 {
		days[i], days[j] = days[j], days[i]
	}
	return days
}

func (s *repoStatsStore) prefetchTrees(dir string, last, first time.Time, today string, shaFor func(string) string) {
	seen := map[string]bool{}
	var wanted []string
	for day := last; !day.Before(first); day = day.AddDate(0, 0, -1) {
		key := day.Format("2006-01-02")
		if key == today {
			continue
		}
		sha := shaFor(key)
		if sha == "" {
			break
		}
		s.mu.Lock()
		_, cached := s.trees[sha]
		s.mu.Unlock()
		if !cached && !seen[sha] {
			seen[sha] = true
			wanted = append(wanted, sha)
		}
	}
	if len(wanted) < 2 {
		return
	}
	next := make(chan string)
	var wg sync.WaitGroup
	for w := 0; w < 4 && w < len(wanted); w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for sha := range next {
				s.treeSample(dir, sha)
			}
		}()
	}
	for _, sha := range wanted {
		next <- sha
	}
	close(next)
	wg.Wait()
}

func (s *repoStatsStore) load(dir string) []repoSample {
	var out []repoSample
	values, _ := readDocument(statsPath(dir))["days"].([]any)
	for _, value := range values {
		item, _ := value.(map[string]any)
		sample := repoSample{}
		sample.Day, _ = item["day"].(string)
		sample.Bytes = int64(number(item["bytes"]))
		sample.Lines = int64(number(item["lines"]))
		sample.Files = int64(number(item["files"]))
		if sample.Day != "" {
			out = append(out, sample)
		}
	}
	if len(out) > historyDays {
		out = out[len(out)-historyDays:]
	}
	return out
}

func number(value any) float64 {
	n, _ := value.(float64)
	return n
}

func (s *repoStatsStore) captureDaily(dir string) ([]repoSample, error) {
	days := s.load(dir)
	today := s.today()
	if len(days) > 0 && days[len(days)-1].Day == today {
		if s.ratchetEnabled(dir) {
			s.ceilingBytes(dir, days[len(days)-1], true)
		}
		return days, nil
	}
	current, err := s.measure(dir)
	if current.Day == "" {
		return days, err
	}
	if len(days) == 0 {
		days = s.backfill(dir, current)
	} else {
		days = append(days, current)
	}
	for len(days) > historyDays {
		days = days[1:]
	}
	root := readDocument(statsPath(dir))
	encoded := make([]any, 0, len(days))
	for _, sample := range days {
		encoded = append(encoded, map[string]any{"day": sample.Day, "bytes": float64(sample.Bytes),
			"lines": float64(sample.Lines), "files": float64(sample.Files)})
	}
	root["days"] = encoded
	if s.ratchetEnabled(dir) {
		if err := s.writeRatchetCeiling(dir, current); err != nil {
			return nil, err
		}
	}
	if s.repositoryJSONEnabled(dir) {
		if err := writeDocument(statsPath(dir), root); err != nil {
			return nil, err
		}
	}
	return days, nil
}

func (s *repoStatsStore) commitCount(dir string) int64 {
	head := ""
	if out, ok := s.run(dir, gitOutputTimeout, "rev-parse", "--verify", "-q", "HEAD"); ok {
		head = strings.TrimSpace(string(out))
	}
	if head != "" {
		s.mu.Lock()
		cached, ok := s.counts[dir]
		s.mu.Unlock()
		if ok && cached.head == head {
			return cached.count
		}
	}
	out, ok := s.run(dir, gitOutputTimeout, "rev-list", "--count", "HEAD")
	if !ok {
		return 0
	}
	count, _ := strconv.ParseInt(strings.TrimSpace(string(out)), 10, 64)
	if head != "" {
		s.mu.Lock()
		s.counts[dir] = countEntry{head: head, count: count}
		s.mu.Unlock()
	}
	return count
}

type repoFacts struct {
	Commits        int64
	Files          int64
	Lines          int64
	Bytes          int64
	Day            string
	RatchetEnabled bool
	CeilingBytes   int64
	CeilingDay     string
}

func (s *repoStatsStore) readRepoFacts(dir string) (repoFacts, string) {
	var facts repoFacts
	commits := make(chan int64, 1)
	go func() { commits <- s.commitCount(dir) }()
	days, err := s.captureDaily(dir)
	if len(days) > 0 {
		latest := days[len(days)-1]
		facts.Files = latest.Files
		facts.Lines = latest.Lines
		facts.Bytes = latest.Bytes
		facts.Day = latest.Day
	}
	reading := s.ratchetReading(dir)
	facts.RatchetEnabled = reading.Enabled
	facts.CeilingBytes = reading.CeilingBytes
	facts.CeilingDay = reading.CeilingDay
	facts.Commits = <-commits
	if len(days) == 0 && err != nil && err.Error() != "" {
		return facts, "stats_unavailable"
	}
	return facts, ""
}

func isLetterOrNumber(r rune) bool {
	return unicode.IsLetter(r) || unicode.IsNumber(r)
}
