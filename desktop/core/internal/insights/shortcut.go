package insights

import (
	"bufio"
	"context"
	"encoding/base64"
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unicode/utf8"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/phonefamily"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/qvariant"
	"forkmesh.com/desktop-core/internal/rpc"
	"forkmesh.com/desktop-core/internal/stats"
)

const (
	ShortcutUsageSetting    = "shortcuts/usageCounts"
	ShortcutRunStatsSetting = "shortcuts/runStats"
	maxShortcutOutputChars  = 96 * 1024
	maxShortcutChunkChars   = 16 * 1024
	shortcutStopGrace       = 3 * time.Second
	shortcutPipeGrace       = 2 * time.Second
	settingsRetryEvery      = 2 * time.Second
	settingsWriteTimeout    = 10 * time.Second
	maxShortcutBytes        = 64 * 1024
)

type ShortcutRun struct {
	ID          int
	Repo        string
	File        string
	Name        string
	Output      string
	Running     bool
	OK          bool
	Message     string
	StartedAtMs int64
}

func (r ShortcutRun) Row(withOutput bool) map[string]any {
	row := map[string]any{"id": r.ID, "repo": r.Repo, "file": r.File, "name": r.Name, "running": r.Running,
		"ok": r.OK, "message": r.Message, "startedAtMs": float64(r.StartedAtMs)}
	if withOutput {
		row["output"] = r.Output
	}
	return row
}

func AppendShortcutOutput(output, chunk string, maxChars int) string {
	joined := output + chunk
	if maxChars <= 0 || u16len(joined) <= maxChars {
		return joined
	}
	joined = right16(joined, maxChars)
	if newline := strings.IndexByte(joined, '\n'); newline >= 0 && newline+1 < len(joined) {
		return joined[newline+1:]
	}
	return joined
}

var ansiEscape = regexp.MustCompile("\x1b\\[[0-9;?]*[A-Za-z]")

func LastOutputLine(output string, maxChars int) string {
	lines := strings.Split(output, "\n")
	for i := len(lines) - 1; i >= 0; i-- {
		line := lines[i]
		if cr := strings.LastIndexByte(line, '\r'); cr >= 0 {
			line = line[cr+1:]
		}
		line = trimmed(ansiEscape.ReplaceAllString(line, ""))
		if line != "" {
			if maxChars > 0 && u16len(line) > maxChars {
				return left16(line, maxChars)
			}
			return line
		}
	}
	return ""
}

type statsOp struct {
	key        string
	usage      bool
	atMs       int64
	result     bool
	recorded   bool
	ok         bool
	durationMs int64
	lastOutput string
}

type shortcutRunner struct {
	m       *Module
	mu      sync.Mutex
	run     ShortcutRun
	cmd     *exec.Cmd
	stopped bool
	ops     []statsOp
	wake    chan struct{}
	writing sync.Mutex
}

func (m *Module) registerShortcuts(core *daemon.Core) {
	m.shortcuts = &shortcutRunner{m: m, wake: make(chan struct{}, 1)}
	core.Registry.Handle("core.insights.shortcut.start", m.Name(), localOnly(m.handleShortcutStart))
	core.Registry.Handle("core.insights.shortcut.stop", m.Name(), localOnly(m.handleShortcutStop))
	core.Registry.Handle("core.insights.shortcut.state", m.Name(), localOnly(m.handleShortcutState))
	if statsModule, ok := core.Module("stats").(*stats.Module); ok && statsModule.ShortcutState == nil {
		statsModule.ShortcutState = m.shortcutStateHook(statsModule)
	}
	go m.shortcuts.settingsLoop()
}

func (m *Module) shortcutStateHook(statsModule *stats.Module) func(ctx context.Context) stats.DesktopShortcutState {
	return func(ctx context.Context) stats.DesktopShortcutState {
		r := m.shortcuts
		r.mu.Lock()
		active := r.cmd != nil
		state := stats.DesktopShortcutState{Active: active, Running: r.run.Running, Repo: r.run.Repo, File: r.run.File,
			LastOutput: LastOutputLine(r.run.Output, 200)}
		r.mu.Unlock()
		if active {
			return state
		}
		return m.desktopShortcutState(ctx)
	}
}

func (m *Module) desktopShortcutState(ctx context.Context) stats.DesktopShortcutState {
	var state stats.DesktopShortcutState
	if err := m.callDesktop(ctx, "ui.insights.shortcutState", map[string]any{}, &state); err != nil {
		return stats.DesktopShortcutState{}
	}
	return state
}

func (r *shortcutRunner) busy() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.cmd != nil
}

func (r *shortcutRunner) snapshot() ShortcutRun {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.run
}

type shortcutStart struct {
	Path     string `json:"path"`
	Repo     string `json:"repo"`
	Workdir  string `json:"workdir"`
	File     string `json:"file"`
	Name     string `json:"name"`
	UsageKey string `json:"usageKey"`
}

var errShortcutRunning = errors.New("shortcut_running")

func (r *shortcutRunner) start(spec shortcutStart) (ShortcutRun, error) {
	r.mu.Lock()
	if r.cmd != nil {
		r.mu.Unlock()
		return ShortcutRun{}, errShortcutRunning
	}
	startedAtMs := nowMs()
	if spec.UsageKey != "" {
		r.ops = append(r.ops, statsOp{key: spec.UsageKey, usage: true, atMs: startedAtMs})
	}
	command := "$ bash " + spec.Path
	r.run = ShortcutRun{ID: r.run.ID + 1, Repo: spec.Repo, File: spec.File, Name: spec.Name, Running: true,
		StartedAtMs: startedAtMs}
	r.stopped = false
	cmd := exec.Command("bash", spec.Path)
	cmd.Dir = spec.Workdir
	cmd.WaitDelay = shortcutPipeGrace
	reader, writer := io.Pipe()
	cmd.Stdout = writer
	cmd.Stderr = writer
	r.cmd = cmd
	started := r.run.Row(false)
	r.mu.Unlock()
	r.kickSettings()
	if core := r.m.core; core != nil {
		core.BroadcastUI("core.insights.shortcut.started", map[string]any{"run": started, "path": spec.Path})
	}
	r.output(command+"\n", cmd)
	if err := cmd.Start(); err != nil {
		writer.Close()
		reader.Close()
		r.finish(cmd, spec, "failed to start bash", false, false, startedAtMs)
		return r.snapshot(), nil
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		r.pump(reader, cmd)
	}()
	go func() {
		err := cmd.Wait()
		writer.Close()
		<-done
		code, ok := exitResult(cmd, err)
		message := "done (exit 0)"
		if !ok {
			message = "failed (exit " + strconv.Itoa(code) + ")"
		}
		r.finish(cmd, spec, message, ok, true, startedAtMs)
	}()
	return r.snapshot(), nil
}

func exitResult(cmd *exec.Cmd, err error) (int, bool) {
	state := cmd.ProcessState
	if state == nil {
		return -1, false
	}
	if status, ok := state.Sys().(syscall.WaitStatus); ok && status.Signaled() {
		return int(status.Signal()), false
	}
	code := state.ExitCode()
	if errors.Is(err, exec.ErrWaitDelay) {
		err = nil
	}
	return code, err == nil && code == 0
}

func (r *shortcutRunner) pump(reader io.Reader, cmd *exec.Cmd) {
	buffered := bufio.NewReaderSize(reader, 32*1024)
	buf := make([]byte, 32*1024)
	var carry []byte
	for {
		n, err := buffered.Read(buf)
		if n > 0 {
			data := append(carry, buf[:n]...)
			cut := len(data)
			for back := 1; back <= 3 && back <= len(data); back++ {
				b := data[len(data)-back]
				if b&0xC0 != 0x80 {
					if b >= 0xC0 && !utf8.FullRune(data[len(data)-back:]) {
						cut = len(data) - back
					}
					break
				}
			}
			carry = append([]byte(nil), data[cut:]...)
			if cut > 0 {
				r.output(strings.ToValidUTF8(string(data[:cut]), "�"), cmd)
			}
		}
		if err != nil {
			if len(carry) > 0 {
				r.output(strings.ToValidUTF8(string(carry), "�"), cmd)
			}
			return
		}
	}
}

func (r *shortcutRunner) output(chunk string, cmd *exec.Cmd) {
	r.mu.Lock()
	if r.cmd != cmd {
		r.mu.Unlock()
		return
	}
	r.run.Output = AppendShortcutOutput(r.run.Output, chunk, maxShortcutOutputChars)
	row := r.run.Row(false)
	r.mu.Unlock()
	row["chunk"] = right16(chunk, maxShortcutChunkChars)
	if core := r.m.core; core != nil {
		phonefamily.BroadcastPhones(core, shortcutOutputEvent, row)
		core.BroadcastUI("core.insights.shortcut.output", row)
	}
}

func (r *shortcutRunner) finish(cmd *exec.Cmd, spec shortcutStart, message string, ok, started bool, startedAtMs int64) {
	r.mu.Lock()
	if r.cmd != cmd {
		r.mu.Unlock()
		return
	}
	lastLine := LastOutputLine(r.run.Output, 200)
	r.run.Running = false
	r.run.OK = ok
	r.run.Message = message
	stopped := r.stopped
	r.mu.Unlock()
	r.output(message+"\n", cmd)
	r.mu.Lock()
	r.cmd = nil
	if spec.UsageKey != "" {
		lastOutput := lastLine
		if lastOutput == "" {
			lastOutput = message
		}
		duration := int64(-1)
		if started {
			duration = nowMs() - startedAtMs
		}
		r.ops = append(r.ops, statsOp{key: spec.UsageKey, result: true, recorded: !stopped, ok: ok,
			durationMs: duration, lastOutput: lastOutput})
	}
	row := r.run.Row(false)
	r.mu.Unlock()
	r.kickSettings()
	if core := r.m.core; core != nil {
		row["stopped"] = stopped
		core.BroadcastUI("core.insights.shortcut.finished", row)
		phonefamily.BroadcastPhones(core, insightsChanged, nil)
	}
}

func (r *shortcutRunner) stop() bool {
	r.mu.Lock()
	cmd := r.cmd
	if cmd == nil || cmd.Process == nil {
		r.mu.Unlock()
		return false
	}
	r.stopped = true
	r.mu.Unlock()
	terminate(cmd.Process)
	time.AfterFunc(shortcutStopGrace, func() {
		r.mu.Lock()
		still := r.cmd == cmd
		r.mu.Unlock()
		if still {
			_ = cmd.Process.Kill()
		}
	})
	return true
}

func (r *shortcutRunner) stopAll() {
	r.stop()
	r.flushSettings()
}

func (r *shortcutRunner) kickSettings() {
	select {
	case r.wake <- struct{}{}:
	default:
	}
}

func (r *shortcutRunner) settingsLoop() {
	ticker := time.NewTicker(settingsRetryEvery)
	defer ticker.Stop()
	for {
		select {
		case <-r.wake:
		case <-ticker.C:
		}
		r.mu.Lock()
		pending := len(r.ops) > 0
		r.mu.Unlock()
		if pending {
			r.flushSettings()
		}
	}
}

func variantMapOf(s *qsettings.Settings, key string) map[string]any {
	value, ok := s.Value(key)
	if !ok {
		return map[string]any{}
	}
	decoded, err := qvariant.FromSettingsRaw(value.Raw)
	if err != nil {
		return map[string]any{}
	}
	if out, ok := decoded.(map[string]any); ok {
		return out
	}
	return map[string]any{}
}

type runStatsRow struct {
	lastRunAtMs int64
	finished    int32
	succeeded   int32
	timed       int32
	totalMs     int64
	lastMs      int64
	lastOk      bool
	lastOutput  string
}

func runStatsRowOf(value any) runStatsRow {
	row, _ := value.(map[string]any)
	out := runStatsRow{lastMs: -1}
	if row == nil {
		return out
	}
	out.lastRunAtMs = qvariant.Int64(row["lastRunAtMs"])
	out.finished = int32(qvariant.Int64(row["finished"]))
	out.succeeded = int32(qvariant.Int64(row["succeeded"]))
	out.timed = int32(qvariant.Int64(row["timed"]))
	out.totalMs = qvariant.Int64(row["totalMs"])
	if raw, ok := row["lastMs"]; ok {
		out.lastMs = qvariant.Int64(raw)
	}
	out.lastOk = qvariant.Bool(row["lastOk"])
	out.lastOutput = qvariant.String(row["lastOutput"])
	return out
}

func (row runStatsRow) variant() map[string]any {
	return map[string]any{
		"lastRunAtMs": row.lastRunAtMs,
		"finished":    row.finished,
		"succeeded":   row.succeeded,
		"timed":       row.timed,
		"totalMs":     row.totalMs,
		"lastMs":      row.lastMs,
		"lastOk":      row.lastOk,
		"lastOutput":  nullable(row.lastOutput),
	}
}

func nullable(text string) any {
	if text == "" {
		return qvariant.NullString{}
	}
	return text
}

func ApplyShortcutOps(usageStored, statsStored map[string]any, ops []statsOp) (map[string]any, map[string]any, bool, bool) {
	usage := map[string]int32{}
	for key, value := range usageStored {
		usage[key] = int32(qvariant.Int64(value))
	}
	rows := map[string]runStatsRow{}
	for key, value := range statsStored {
		rows[key] = runStatsRowOf(value)
	}
	usageChanged, statsChanged := false, false
	for _, op := range ops {
		row, ok := rows[op.key]
		if !ok {
			row = runStatsRow{lastMs: -1}
		}
		if op.usage {
			usage[op.key]++
			usageChanged = true
			row.lastRunAtMs = op.atMs
		}
		if op.result {
			row.lastOutput = op.lastOutput
			if op.recorded {
				row.finished++
				row.lastOk = op.ok
				if op.ok {
					row.succeeded++
				}
				if op.durationMs >= 0 {
					row.timed++
					row.totalMs += op.durationMs
					row.lastMs = op.durationMs
				}
			}
		}
		rows[op.key] = row
		statsChanged = true
	}
	usageOut := map[string]any{}
	for key, count := range usage {
		usageOut[key] = count
	}
	statsOut := map[string]any{}
	for key, row := range rows {
		statsOut[key] = row.variant()
	}
	return usageOut, statsOut, usageChanged, statsChanged
}

func variantWrite(key string, value any) daemon.SettingWrite {
	return daemon.SettingWrite{Key: key, Type: "variant", Value: base64.StdEncoding.EncodeToString(qvariant.Encode(value))}
}

func (r *shortcutRunner) flushSettings() bool {
	r.writing.Lock()
	defer r.writing.Unlock()
	r.mu.Lock()
	ops := append([]statsOp(nil), r.ops...)
	r.mu.Unlock()
	if len(ops) == 0 {
		return true
	}
	core := r.m.core
	if core == nil || core.Settings == nil || !r.m.uiAttached() {
		return false
	}
	s := core.Settings.Get()
	usage, runStats, usageChanged, statsChanged := ApplyShortcutOps(variantMapOf(s, ShortcutUsageSetting),
		variantMapOf(s, ShortcutRunStatsSetting), ops)
	writes := []daemon.SettingWrite{}
	if usageChanged {
		writes = append(writes, variantWrite(ShortcutUsageSetting, usage))
	}
	if statsChanged {
		writes = append(writes, variantWrite(ShortcutRunStatsSetting, runStats))
	}
	ctx, cancel := context.WithTimeout(context.Background(), settingsWriteTimeout)
	_, err := r.m.callUI(ctx, "ui.settings.write", map[string]any{"writes": writes})
	cancel()
	if err != nil {
		if !phonefamily.DesktopUnavailable(err) {
			r.m.logf("insights: shortcut stats write failed: %v", err)
		}
		return false
	}
	r.mu.Lock()
	r.ops = r.ops[len(ops):]
	r.mu.Unlock()
	core.BroadcastUI("core.insights.shortcut.stats", map[string]any{"keys": []string{ShortcutUsageSetting, ShortcutRunStatsSetting}})
	return true
}

func (m *Module) handleShortcutStart(ctx context.Context, req *rpc.Request) (any, error) {
	var spec shortcutStart
	if err := req.Bind(&spec); err != nil {
		return nil, err
	}
	if spec.Path == "" {
		return nil, rpc.Fail("bad_request")
	}
	run, err := m.shortcuts.start(spec)
	if errors.Is(err, errShortcutRunning) {
		return nil, rpc.Fail("shortcut_running")
	}
	return map[string]any{"run": run.Row(false)}, nil
}

func (m *Module) handleShortcutStop(ctx context.Context, req *rpc.Request) (any, error) {
	return map[string]any{"stopping": m.shortcuts.stop()}, nil
}

func (m *Module) handleShortcutState(ctx context.Context, req *rpc.Request) (any, error) {
	run := m.shortcuts.snapshot()
	out := map[string]any{"active": m.shortcuts.busy()}
	if run.ID > 0 {
		out["run"] = run.Row(true)
	}
	return out, nil
}

func (m *Module) handleShortcutRun(ctx context.Context, req *rpc.Request) (any, error) {
	run := m.shortcuts.snapshot()
	if run.ID > 0 {
		return map[string]any{"run": run.Row(true)}, nil
	}
	if state, ok := m.desktopShortcutRun(ctx); ok {
		return state, nil
	}
	return map[string]any{}, nil
}

func (m *Module) desktopShortcutRun(ctx context.Context) (map[string]any, bool) {
	var reply map[string]any
	if err := m.callDesktop(ctx, "ui.insights.shortcutRun", map[string]any{}, &reply); err != nil || reply == nil {
		return nil, false
	}
	return reply, true
}

type repoRecord struct {
	Owner     string
	Name      string
	LocalPath string
}

var ownerSegmentPattern = regexp.MustCompile(`[^A-Za-z0-9._-]+`)

func loadRepoRecords(s *qsettings.Settings) []repoRecord {
	count := int(s.Int(repositoriesArray+"/size", 0))
	out := []repoRecord{}
	for i := 1; i <= count; i++ {
		prefix := repositoriesArray + "/" + strconv.Itoa(i) + "/"
		get := func(key string) string { return s.String(prefix+key, "") }
		record := repoRecord{Owner: get("owner"), Name: get("name"), LocalPath: get("localPath")}
		source := trimmed(record.LocalPath)
		if source == "" {
			source = trimmed(get("cloneUrl"))
		}
		if record.Name == "" || (source == "" && !opaqueIDPattern.MatchString(trimmed(get("publicArchiveId"))) &&
			!opaqueIDPattern.MatchString(trimmed(get("privateReplicaId")))) {
			continue
		}
		out = append(out, record)
	}
	return out
}

func (m *Module) dataRoot() string {
	if m.core == nil {
		return ""
	}
	return m.core.Layout.Data
}

func (m *Module) shortcutDir(records []repoRecord, repoKey, scope string) (string, int) {
	if scope == "app" {
		return filepath.Join(m.dataRoot(), "skills"), -1
	}
	for i, repo := range records {
		if !strings.EqualFold(repo.Owner+"/"+repo.Name, repoKey) {
			continue
		}
		if repo.LocalPath == "" {
			return "", -1
		}
		switch scope {
		case "shortcut":
			return repo.LocalPath + "/.forkmesh/shortcuts", i
		case "repository":
			return filepath.Join(repo.LocalPath, ".forkmesh", "skills"), i
		case "organization":
			owner := ownerSegmentPattern.ReplaceAllString(trimmed(repo.Owner), "-")
			if owner == "" || owner == "." || owner == ".." {
				owner = "personal"
			}
			return filepath.Join(m.dataRoot(), "organization-skills", owner), i
		}
		return "", i
	}
	return "", -1
}

func readShortcutText(path string) string {
	file, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer file.Close()
	data, _ := io.ReadAll(io.LimitReader(file, maxShortcutBytes))
	return strings.ReplaceAll(strings.ToValidUTF8(string(data), "�"), "\r\n", "\n")
}

func shortcutName(fileName, text string) string {
	name := fileName
	lines := strings.Split(text, "\n")
	for i := 0; i < 20 && i < len(lines); i++ {
		line := trimmed(lines[i])
		if strings.HasPrefix(line, "# name:") {
			name = trimmed(line[7:])
		}
	}
	return name
}

func fileSuffix(name string) string {
	base := filepath.Base(name)
	if dot := strings.LastIndexByte(base, '.'); dot >= 0 {
		return base[dot+1:]
	}
	return ""
}

func ShortcutKind(fileName string, executable bool) string {
	switch suffix := strings.ToLower(fileSuffix(fileName)); {
	case suffix == "sh" || suffix == "bash" || (suffix == "" && executable):
		return "script"
	case suffix == "skill":
		return "skill"
	case suffix == "md" || suffix == "txt" || suffix == "prompt":
		return "prompt"
	}
	return "file"
}

var (
	shortcutFilePattern = regexp.MustCompile(`^[A-Za-z0-9_][A-Za-z0-9._ -]{0,127}$`)
	familyIDPattern     = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{0,63}$`)
	repoKeyPattern      = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}/[A-Za-z0-9._][A-Za-z0-9._-]{0,99}$`)
	shortcutScopes      = map[string]bool{"shortcut": true, "repository": true, "app": true, "organization": true}
)

func validShortcutFile(name string) bool {
	return shortcutFilePattern.MatchString(name) && !strings.Contains(name, "..")
}

func subjectFor(action string, params map[string]any) (string, string) {
	switch action {
	case "toggleFamily":
		family, _ := params["family"].(string)
		family = trimmed(family)
		enabled, isBool := params["enabled"].(bool)
		if !familyIDPattern.MatchString(family) || !isBool {
			return "", "bad_family"
		}
		if enabled {
			return family + "|on", ""
		}
		return family + "|off", ""
	case "runShortcut":
		repo, _ := params["repo"].(string)
		scope, _ := params["scope"].(string)
		file, _ := params["file"].(string)
		repo, scope = trimmed(repo), trimmed(scope)
		if !shortcutScopes[scope] || !validShortcutFile(file) {
			return "", "bad_shortcut"
		}
		if !repoKeyPattern.MatchString(repo) || strings.Contains(repo, "..") {
			return "", "bad_repo"
		}
		return repo + "|" + scope + "|" + file, ""
	}
	return "", "unsupported_action"
}

func section(subject string, index int) string {
	parts := strings.Split(subject, "|")
	if index < len(parts) {
		return parts[index]
	}
	return ""
}

func sectionFrom(subject string, index int) string {
	parts := strings.Split(subject, "|")
	if index < len(parts) {
		return strings.Join(parts[index:], "|")
	}
	return ""
}

func actionSummary(action, subject, label string, script bool) string {
	switch {
	case action == "toggleFamily":
		name := label
		if name == "" {
			name = section(subject, 0)
		}
		if strings.HasSuffix(subject, "|on") {
			return "Switch the " + name + " tool family back on for agents. Running agents get it from their next launch or resume."
		}
		return "Switch the " + name + " tool family off for agents. Running agents keep what they launched with until their next launch or resume."
	case action == "runShortcut":
		name := label
		if name == "" {
			name = section(subject, 2)
		}
		if script {
			return "Run the \"" + name + "\" script on the desktop in " + section(subject, 0) + ". Its output streams here while it runs."
		}
		return "Start an agent on the desktop with the \"" + name + "\" shortcut in " + section(subject, 0) + "."
	}
	return ""
}

func isScriptFile(path string) bool {
	info, err := os.Stat(path)
	if err != nil || !info.Mode().IsRegular() {
		return false
	}
	return ShortcutKind(filepath.Base(path), executableFile(path, info)) == "script"
}

func (m *Module) settings() *qsettings.Settings {
	if m.core == nil || m.core.Settings == nil {
		return qsettings.Parse(nil)
	}
	return m.core.Settings.Get()
}

func (m *Module) handlePrepare(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	action, _ := params["action"].(string)
	subject, failure := subjectFor(action, params)
	if subject == "" {
		return nil, rpc.Fail(failure)
	}
	label := ""
	script := false
	if action == "toggleFamily" {
		family := FamilyByID(section(subject, 0))
		if family == nil {
			return nil, rpc.Fail("bad_family")
		}
		label = family.DisplayName
	} else {
		label, _ = params["file"].(string)
		scope := section(subject, 1)
		dir, _ := m.shortcutDir(loadRepoRecords(m.settings()), section(subject, 0), scope)
		if dir != "" && scope == "shortcut" {
			script = isScriptFile(filepath.Join(dir, label))
		}
	}
	token := m.confirmations.Issue(phonefamily.DeviceOf(req).Key, action, subject, nowMs())
	return map[string]any{
		"token":       token,
		"action":      action,
		"subject":     subject,
		"summary":     actionSummary(action, subject, label, script),
		"expiresInMs": float64(phonefamily.ConfirmationLifetimeMs),
	}, nil
}

func (m *Module) handleRun(ctx context.Context, req *rpc.Request) (any, error) {
	params := req.Object()
	action, _ := params["action"].(string)
	subject, failure := subjectFor(action, params)
	token, _ := params["token"].(string)
	verdict := m.confirmations.Consume(token, phonefamily.DeviceOf(req).Key, action, subject, nowMs())
	if subject == "" || verdict != phonefamily.Ok {
		why := failure
		if subject != "" {
			why = phonefamily.ConfirmationError(verdict)
		}
		if m.core != nil {
			m.core.LogUI("Refused an insights %s from %s: %s.", action, phonefamily.DeviceLabel(req), why)
		}
		return nil, rpc.Fail(why)
	}
	who := phonefamily.DeviceLabel(req)
	switch action {
	case "toggleFamily":
		return m.runToggleFamily(ctx, subject, who)
	case "runShortcut":
		return m.runShortcutAction(ctx, subject, who)
	}
	return nil, rpc.Fail("unsupported_action")
}

func (m *Module) runToggleFamily(ctx context.Context, subject, who string) (any, error) {
	family := section(subject, 0)
	enabled := strings.HasSuffix(subject, "|on")
	if FamilyByID(family) == nil {
		return nil, rpc.Fail("bad_family")
	}
	if err := m.callDesktop(ctx, "ui.insights.toggleFamily", map[string]any{"family": family, "enabled": enabled}, nil); err != nil {
		if phonefamily.DesktopUnavailable(err) {
			return nil, rpc.Fail("desktop_unavailable")
		}
		return nil, err
	}
	state := "off"
	message := "Switched off."
	if enabled {
		state = "on"
		message = "Switched on."
	}
	if m.core != nil {
		m.core.LogUI("Tool family %s switched %s by %s.", family, state, who)
	}
	phonefamily.BroadcastPhones(m.core, insightsChanged, nil)
	return map[string]any{"started": true, "action": "toggleFamily", "subject": subject, "message": message}, nil
}

func (m *Module) runShortcutAction(ctx context.Context, subject, who string) (any, error) {
	repo, scope, file := section(subject, 0), section(subject, 1), sectionFrom(subject, 2)
	records := loadRepoRecords(m.settings())
	dir, repoIndex := m.shortcutDir(records, repo, scope)
	path := ""
	if dir != "" {
		path = filepath.Join(dir, file)
	}
	info, err := os.Stat(path)
	if repoIndex < 0 || path == "" || err != nil || !info.Mode().IsRegular() {
		return nil, rpc.Fail("unknown_shortcut")
	}
	if scope == "shortcut" && ShortcutKind(file, executableFile(path, info)) == "script" {
		record := records[repoIndex]
		repoKey := record.Owner + "/" + record.Name
		spec := shortcutStart{Path: path, Repo: repoKey, Workdir: record.LocalPath, File: file,
			Name: shortcutName(file, readShortcutText(path)), UsageKey: "shortcut:" + repoKey + ":" + file}
		run, err := m.shortcuts.start(spec)
		if errors.Is(err, errShortcutRunning) {
			return nil, rpc.Fail("shortcut_running")
		}
		if m.core != nil {
			m.core.LogUI("Shortcut %s in %s started on the desktop for %s.", file, repo, who)
		}
		phonefamily.BroadcastPhones(m.core, insightsChanged, nil)
		return map[string]any{"started": true, "action": "runShortcut", "subject": subject, "run": run.ID,
			"message": "Running " + run.Name + " on the desktop."}, nil
	}
	var reply struct {
		Session int    `json:"session"`
		Name    string `json:"name"`
	}
	if err := m.callDesktop(ctx, "ui.insights.runShortcutAgent", map[string]any{"repo": repo, "scope": scope, "file": file}, &reply); err != nil {
		if phonefamily.DesktopUnavailable(err) {
			return nil, rpc.Fail("desktop_unavailable")
		}
		return nil, err
	}
	if m.core != nil {
		m.core.LogUI("Shortcut %s in %s started agent #%d for %s.", file, repo, reply.Session, who)
	}
	phonefamily.BroadcastPhones(m.core, insightsChanged, nil)
	phonefamily.BroadcastPhones(m.core, "agents.changed", nil)
	return map[string]any{"started": true, "action": "runShortcut", "subject": subject, "session": reply.Session,
		"message": "Started agent #" + strconv.Itoa(reply.Session) + " for " + reply.Name + "."}, nil
}
