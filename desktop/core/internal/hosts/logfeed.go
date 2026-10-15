package hosts

import (
	"bytes"
	"context"
	"fmt"
	"os/exec"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"

	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	feedLinesBatch    = 100 * time.Millisecond
	feedRetryBaseMs   = 30 * 1000
	feedRetryMaxMs    = 10 * 60 * 1000
	feedHealthyMs     = 3 * 60 * 1000
	feedTailCap       = 64 * 1024
	feedRecyclingHow  = "recycling"
	feedSSHNoStartHow = "SSH could not start"
)

var (
	feedSilenceCheck = 60 * time.Second
	feedSilenceLimit = 120 * time.Second
	feedRegistry     sync.Map
)

type feedLine struct {
	Source int    `json:"source"`
	Text   string `json:"text"`
}

type hostFeed struct {
	key      string
	node     string
	ip       string
	user     string
	pass     string
	identity string
	ctx      context.Context
	cancel   context.CancelFunc
	retarget chan struct{}

	mu         sync.Mutex
	sshUntilMs int64
	running    bool
	gotData    bool
	recycling  bool
	failed     bool
	failures   int
	startedMs  int64
	lastReadMs int64
	lastHow    string
	stdoutTail []byte
	stderrTail []byte
	pending    []feedLine
	flushArmed bool
	flushMu    sync.Mutex
}

type feedTable struct {
	mu    sync.Mutex
	feeds map[string]*hostFeed
	seen  *forwardedHostLines
}

func (m *Module) feedTable() *feedTable {
	if value, ok := feedRegistry.Load(m); ok {
		return value.(*feedTable)
	}
	value, _ := feedRegistry.LoadOrStore(m, &feedTable{feeds: map[string]*hostFeed{}, seen: newForwardedHostLines(forwardedLinesPerHost)})
	return value.(*feedTable)
}

func (m *Module) registerFeedMethods(desktop func(string, rpc.Handler)) {
	desktop("core.hosts.feed.start", m.handleFeedStart)
	desktop("core.hosts.feed.stop", m.handleFeedStop)
	desktop("core.hosts.feeds", m.handleFeeds)
}

func (m *Module) startFeeds(ctx context.Context) {
	m.feedTable()
	go func() {
		<-ctx.Done()
		table := m.feedTable()
		table.mu.Lock()
		for key, feed := range table.feeds {
			feed.cancel()
			delete(table.feeds, key)
		}
		table.mu.Unlock()
	}()
}

func (f *hostFeed) snapshot() map[string]any {
	f.mu.Lock()
	defer f.mu.Unlock()
	return map[string]any{"key": f.key, "node": f.node, "ip": f.ip, "user": f.user, "identityFile": f.identity,
		"running": f.running, "gotData": f.gotData, "failed": f.failed, "failures": f.failures, "sshUntilMs": f.sshUntilMs,
		"startedAt": f.startedMs, "lastReadAt": f.lastReadMs, "lastEnd": f.lastHow}
}

func (m *Module) handleFeedStart(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Key          string  `json:"key"`
		Node         string  `json:"node"`
		IP           string  `json:"ip"`
		User         string  `json:"user"`
		Password     string  `json:"password"`
		IdentityFile string  `json:"identityFile"`
		SSHUntilMs   float64 `json:"sshUntilMs"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	if params.IP == "" || params.User == "" {
		return nil, rpc.Fail("bad_params")
	}
	key := params.Key
	if key == "" {
		key = credentialKey(params.Node, params.IP, params.User)
	}
	identity := params.IdentityFile
	if identity == "" {
		identity = savedHostIdentityFile(m.savedHosts(), m.sshDir(), params.Node, params.IP, params.User)
	}
	node := params.Node
	if node == "" {
		node = params.IP
	}
	feed, started := m.startFeed(key, node, params.IP, params.User, params.Password, identity, int64(params.SSHUntilMs))
	return map[string]any{"started": started, "kept": !started, "feed": feed.snapshot()}, nil
}

func (m *Module) startFeed(key, node, ip, user, pass, identity string, sshUntilMs int64) (*hostFeed, bool) {
	table := m.feedTable()
	table.mu.Lock()
	defer table.mu.Unlock()
	if existing := table.feeds[key]; existing != nil {
		if existing.pass == pass && existing.identity == identity && existing.ip == ip && existing.user == user && existing.node == node {
			existing.mu.Lock()
			changed := existing.sshUntilMs != sshUntilMs
			existing.sshUntilMs = sshUntilMs
			existing.mu.Unlock()
			if changed {
				select {
				case existing.retarget <- struct{}{}:
				default:
				}
			}
			return existing, false
		}
		existing.cancel()
		delete(table.feeds, key)
	}
	base := m.ctx
	if base == nil {
		base = context.Background()
	}
	feedCtx, cancel := context.WithCancel(base)
	feed := &hostFeed{key: key, node: node, ip: ip, user: user, pass: pass, identity: identity, ctx: feedCtx, cancel: cancel,
		retarget: make(chan struct{}, 1), sshUntilMs: sshUntilMs}
	table.feeds[key] = feed
	go m.runFeed(feed)
	return feed, true
}

func (m *Module) handleFeedStop(ctx context.Context, req *rpc.Request) (any, error) {
	var params struct {
		Key string `json:"key"`
	}
	if err := req.Bind(&params); err != nil {
		return nil, err
	}
	table := m.feedTable()
	table.mu.Lock()
	feed := table.feeds[params.Key]
	delete(table.feeds, params.Key)
	table.mu.Unlock()
	if feed != nil {
		feed.cancel()
	}
	return map[string]any{"stopped": feed != nil}, nil
}

func (m *Module) handleFeeds(ctx context.Context, req *rpc.Request) (any, error) {
	table := m.feedTable()
	table.mu.Lock()
	feeds := make([]*hostFeed, 0, len(table.feeds))
	for _, feed := range table.feeds {
		feeds = append(feeds, feed)
	}
	table.mu.Unlock()
	sort.Slice(feeds, func(i, j int) bool { return feeds[i].key < feeds[j].key })
	out := make([]map[string]any, 0, len(feeds))
	for _, feed := range feeds {
		out = append(out, feed.snapshot())
	}
	return map[string]any{"feeds": out}, nil
}

func (m *Module) dropFeed(feed *hostFeed) {
	table := m.feedTable()
	table.mu.Lock()
	if table.feeds[feed.key] == feed {
		delete(table.feeds, feed.key)
	}
	table.mu.Unlock()
	feed.cancel()
}

func (m *Module) feedRecycleDue(feed *hostFeed) bool {
	feed.mu.Lock()
	until := feed.sshUntilMs
	feed.mu.Unlock()
	return until > 0 && m.nowMs() >= until
}

func (m *Module) feedEnded(feed *hostFeed, how string, gotData, recycling bool) {
	m.flushFeedLines(feed)
	m.broadcastUI("core.hosts.feedEnded", map[string]any{"key": feed.key, "node": feed.node, "how": how, "gotData": gotData, "recycling": recycling})
}

func (m *Module) runFeed(feed *hostFeed) {
	for {
		if feed.ctx.Err() != nil {
			return
		}
		if m.feedRecycleDue(feed) && m.desktopAttached() {
			m.feedEnded(feed, feedRecyclingHow, false, true)
			m.dropFeed(feed)
			return
		}
		how, connected := m.feedConnect(feed)
		if !connected || feed.ctx.Err() != nil {
			return
		}
		now := m.nowMs()
		feed.mu.Lock()
		recycling := feed.recycling
		gotData := feed.gotData
		feed.lastHow = how
		if !recycling {
			if now-feed.startedMs >= feedHealthyMs {
				feed.failures = 0
			} else {
				feed.failures++
			}
		}
		failures := feed.failures
		feed.mu.Unlock()
		if recycling {
			m.feedEnded(feed, how, gotData, true)
			m.dropFeed(feed)
			return
		}
		shift := failures
		if shift > 5 {
			shift = 5
		}
		delay := int64(feedRetryBaseMs) << shift
		if delay > feedRetryMaxMs {
			delay = feedRetryMaxMs
		}
		m.feedEnded(feed, how, gotData, false)
		m.feedForward(feed, fallbackLogSource, fmt.Sprintf("[log feed] SSH connection closed (%s); reconnecting in %d s", how, delay/1000))
		if !m.sleep(feed.ctx, time.Duration(delay)*time.Millisecond) {
			return
		}
	}
}

func feedRemoteCommand() string {
	return "sh -lc '" + strings.ReplaceAll(mainLogFeedScript, "'", `'\''`) + "'"
}

func (m *Module) feedConnect(feed *hostFeed) (string, bool) {
	now := m.nowMs()
	feed.mu.Lock()
	feed.stdoutTail = nil
	feed.stderrTail = nil
	feed.gotData = false
	feed.recycling = false
	feed.startedMs = now
	feed.lastReadMs = now
	feed.mu.Unlock()
	command, err := buildHostSSHCommand(m.sshDir(), feed.ip, feed.user, feed.pass, feedRemoteCommand(), feed.identity)
	if err != nil {
		feed.mu.Lock()
		feed.failed = true
		feed.lastHow = err.Error()
		feed.mu.Unlock()
		m.feedForward(feed, fallbackLogSource, "[error] Cannot follow this host's logs: "+err.Error())
		m.flushFeedLines(feed)
		return "", false
	}
	cmd := exec.Command(command.Program, command.Args...)
	cmd.Env = command.Env
	cmd.Stdin = nil
	serial := &sync.Mutex{}
	cmd.Stdout = &chunkWriter{mu: serial, emit: func(chunk []byte) { m.feedConsume(feed, chunk, false) }}
	cmd.Stderr = &chunkWriter{mu: serial, emit: func(chunk []byte) { m.feedConsume(feed, chunk, true) }}
	cmd.WaitDelay = time.Second
	if err := cmd.Start(); err != nil {
		return feedSSHNoStartHow, true
	}
	feed.mu.Lock()
	feed.running = true
	feed.mu.Unlock()
	exited := make(chan struct{})
	watched := make(chan struct{})
	go func() {
		m.feedWatch(feed, cmd, exited)
		close(watched)
	}()
	_ = cmd.Wait()
	close(exited)
	<-watched
	feed.mu.Lock()
	feed.running = false
	feed.mu.Unlock()
	code := -1
	if state := cmd.ProcessState; state != nil {
		code = state.ExitCode()
		if code < 0 {
			code = signalNumber(state)
		}
	}
	return fmt.Sprintf("exit %d", code), true
}

func terminateProcess(cmd *exec.Cmd) {
	if cmd.Process == nil {
		return
	}
	if cmd.Process.Signal(syscall.SIGTERM) != nil {
		_ = cmd.Process.Kill()
	}
}

func (m *Module) feedWatch(feed *hostFeed, cmd *exec.Cmd, exited <-chan struct{}) {
	ticker := time.NewTicker(feedSilenceCheck)
	defer ticker.Stop()
	var timer *time.Timer
	var recycle <-chan time.Time
	arm := func() {
		if timer != nil {
			timer.Stop()
		}
		timer, recycle = nil, nil
		feed.mu.Lock()
		until := feed.sshUntilMs
		feed.mu.Unlock()
		if delay := until - m.nowMs(); until > 0 && delay > 0 {
			timer = time.NewTimer(time.Duration(delay) * time.Millisecond)
			recycle = timer.C
		}
	}
	defer func() {
		if timer != nil {
			timer.Stop()
		}
	}()
	startRecycle := func() {
		if !m.desktopAttached() {
			return
		}
		feed.mu.Lock()
		already := feed.recycling
		feed.recycling = true
		feed.mu.Unlock()
		if !already {
			terminateProcess(cmd)
		}
	}
	arm()
	for {
		select {
		case <-exited:
			return
		case <-feed.ctx.Done():
			_ = cmd.Process.Kill()
			<-exited
			return
		case <-feed.retarget:
			arm()
		case <-recycle:
			recycle = nil
			startRecycle()
		case <-ticker.C:
			feed.mu.Lock()
			silent := m.nowMs()-feed.lastReadMs > feedSilenceLimit.Milliseconds()
			feed.mu.Unlock()
			if silent {
				_ = cmd.Process.Kill()
			} else if m.feedRecycleDue(feed) {
				startRecycle()
			}
		}
	}
}

func (m *Module) feedConsume(feed *hostFeed, chunk []byte, fromStderr bool) {
	feed.mu.Lock()
	feed.lastReadMs = m.nowMs()
	tail := &feed.stdoutTail
	if fromStderr {
		tail = &feed.stderrTail
	}
	*tail = append(*tail, chunk...)
	lines := []feedLine{}
	start := 0
	for {
		newline := bytes.IndexByte((*tail)[start:], '\n')
		if newline < 0 {
			break
		}
		line := strings.ToValidUTF8(string((*tail)[start:start+newline]), "�")
		start += newline + 1
		line = strings.TrimSuffix(line, "\r")
		if fromStderr {
			if strings.TrimSpace(line) != "" {
				lines = append(lines, feedLine{Source: fallbackLogSource, Text: "[ssh] " + line})
			}
			continue
		}
		feed.gotData = true
		if source, text := feedLogLine(line); source >= 0 {
			lines = append(lines, feedLine{Source: source, Text: text})
		}
	}
	*tail = append([]byte(nil), (*tail)[start:]...)
	if len(*tail) > feedTailCap {
		*tail = nil
	}
	feed.mu.Unlock()
	for _, line := range lines {
		m.feedForward(feed, line.Source, line.Text)
	}
}

func (m *Module) feedForward(feed *hostFeed, source int, text string) {
	if strings.TrimSpace(text) == "" {
		return
	}
	host := lowerHost(feed.node)
	if m.feedTable().seen.firstSighting(host, forwardedLineKey(source, text)) {
		m.pushLines([]incomingLine{{Node: host, Source: logSourceID(source), Text: text}})
	}
	feed.mu.Lock()
	feed.pending = append(feed.pending, feedLine{Source: source, Text: text})
	arm := !feed.flushArmed
	feed.flushArmed = true
	feed.mu.Unlock()
	if arm {
		time.AfterFunc(feedLinesBatch, func() { m.flushFeedLines(feed) })
	}
}

func (m *Module) flushFeedLines(feed *hostFeed) {
	feed.flushMu.Lock()
	defer feed.flushMu.Unlock()
	feed.mu.Lock()
	lines := feed.pending
	feed.pending = nil
	feed.flushArmed = false
	feed.mu.Unlock()
	if len(lines) == 0 {
		return
	}
	m.broadcastUI("core.hosts.feedLines", map[string]any{"key": feed.key, "node": feed.node, "lines": lines})
}

func (m *Module) skipFeedEchoes(lines []incomingLine) []incomingLine {
	seen := m.feedTable().seen
	out := make([]incomingLine, 0, len(lines))
	for _, line := range lines {
		if strings.TrimSpace(line.Text) == "" {
			out = append(out, line)
			continue
		}
		source := logSourceIndex(knownLogSource(line.Source))
		if seen.desktopSighting(lowerHost(line.Node), forwardedLineKey(source, line.Text)) {
			out = append(out, line)
		}
	}
	return out
}
