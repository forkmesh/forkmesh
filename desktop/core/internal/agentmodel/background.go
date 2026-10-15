package agentmodel

import (
	"context"
	"fmt"
	"strconv"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/agents"
	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qtjson"
	"forkmesh.com/desktop-core/internal/rpc"
)

const (
	completionFastPolls = 20
	completionFastPoll  = 250 * time.Millisecond
	completionSlowPoll  = 1000 * time.Millisecond
	completionIdleGrace = int64(8000)
	completionMaxWait   = int64(15 * 60 * 1000)
	completionKillGrace = int64(10000)
	shownSubprocesses   = 4
	backgroundEventName = "core.agents.background"
)

type processEntry struct {
	parent   int64
	command  string
	zombie   bool
	cpuTicks int64
}

type processTable struct {
	entries  map[int64]processEntry
	children map[int64][]int64
}

type descendant struct {
	pid      int64
	parent   int64
	command  string
	cpuTicks int64
}

func (t processTable) descendants(root int64) []descendant {
	out := []descendant{}
	if root <= 0 {
		return out
	}
	queue := []int64{root}
	seen := map[int64]bool{root: true}
	for len(queue) > 0 {
		pid := queue[len(queue)-1]
		queue = queue[:len(queue)-1]
		if entry, ok := t.entries[pid]; ok && pid != root && !entry.zombie {
			out = append(out, descendant{pid: pid, parent: entry.parent, command: entry.command, cpuTicks: entry.cpuTicks})
		}
		for _, child := range t.children[pid] {
			if !seen[child] {
				seen[child] = true
				queue = append(queue, child)
			}
		}
	}
	return out
}

func subprocessLabels(processes []descendant) []string {
	labels := []string{}
	for i := 0; i < len(processes) && i < shownSubprocesses; i++ {
		command := fromU16(qtDecodeUTF8([]byte(processes[i].command)))
		if command == "" {
			command = "process"
		}
		labels = append(labels, command+" (PID "+strconv.FormatInt(processes[i].pid, 10)+")")
	}
	if len(processes) > shownSubprocesses {
		labels = append(labels, "+"+strconv.Itoa(len(processes)-shownSubprocesses)+" more")
	}
	return labels
}

type backgroundWork struct {
	pid        int64
	count      int
	busy       bool
	summary    string
	cpuTicks   int64
	startedAt  int64
	lastBusyAt int64
	killedAt   int64
	polls      int
	announced  bool
	shown      string
	timer      *time.Timer
	generation int
}

type backgroundTracker struct {
	m     *Module
	mu    sync.Mutex
	works map[int]*backgroundWork
	scan  func() processTable
	kill  func([]int64)
	gen   int
}

func newBackgroundTracker(m *Module) *backgroundTracker {
	return &backgroundTracker{m: m, works: map[int]*backgroundWork{}, scan: readProcessTable, kill: killStragglers}
}

func plural(count int, one, many string) string {
	if count == 1 {
		return one
	}
	return many
}

func (b *backgroundTracker) watch(id int, pid int64, announced bool) map[string]any {
	b.mu.Lock()
	work := b.works[id]
	adopted := work != nil && work.pid == pid
	if !adopted {
		if work != nil && work.timer != nil {
			work.timer.Stop()
		}
		b.gen++
		work = &backgroundWork{pid: pid, generation: b.gen, announced: announced}
		b.works[id] = work
	}
	state := work.state(id)
	b.mu.Unlock()
	if !adopted {
		go b.poll(id, work.generation)
	}
	state["adopted"] = adopted
	return state
}

func (w *backgroundWork) state(id int) map[string]any {
	return map[string]any{"id": id, "pid": w.pid, "count": w.count, "busy": w.busy, "summary": w.summary,
		"polls": w.polls, "killed": w.killedAt > 0}
}

func (b *backgroundTracker) cancel(id int) bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	work := b.works[id]
	if work == nil {
		return false
	}
	if work.timer != nil {
		work.timer.Stop()
	}
	delete(b.works, id)
	return true
}

func (b *backgroundTracker) list() []map[string]any {
	b.mu.Lock()
	defer b.mu.Unlock()
	out := []map[string]any{}
	for id, work := range b.works {
		out = append(out, work.state(id))
	}
	return out
}

func (b *backgroundTracker) stopAll() {
	b.mu.Lock()
	defer b.mu.Unlock()
	for id, work := range b.works {
		if work.timer != nil {
			work.timer.Stop()
		}
		delete(b.works, id)
	}
}

func (b *backgroundTracker) current(id, generation int) *backgroundWork {
	work := b.works[id]
	if work == nil || work.generation != generation {
		return nil
	}
	return work
}

func (b *backgroundTracker) poll(id, generation int) {
	b.mu.Lock()
	work := b.current(id, generation)
	if work == nil {
		b.mu.Unlock()
		return
	}
	pid := work.pid
	b.mu.Unlock()
	if !b.m.sessionRunning(id) {
		b.cancel(id)
		b.m.emitBackground(qtjson.ObjectOf("id", id, "cleared", true))
		return
	}
	subprocesses := b.scan().descendants(pid)
	now := b.m.nowMs()
	notice := ""
	b.mu.Lock()
	work = b.current(id, generation)
	if work == nil {
		b.mu.Unlock()
		return
	}
	if len(subprocesses) > 0 {
		if work.startedAt == 0 {
			work.startedAt = now
			work.lastBusyAt = now
		}
		work.polls++
		ticks := int64(0)
		for _, process := range subprocesses {
			ticks += process.cpuTicks
		}
		work.busy = work.polls == 1 || ticks > work.cpuTicks
		work.cpuTicks = ticks
		if work.busy {
			work.lastBusyAt = now
		}
		work.count = len(subprocesses)
		work.summary = strings.Join(subprocessLabels(subprocesses), ", ")
		idle := now-work.lastBusyAt >= completionIdleGrace
		overdue := now-work.startedAt >= completionMaxWait
		killed := work.killedAt > 0
		what := work.summary
		count := work.count
		announce := !work.announced
		work.announced = true
		signature := fmt.Sprintf("%d|%d|%s", count, map[bool]int{true: 1, false: 0}[work.busy], what)
		changed := work.shown != signature
		work.shown = signature
		var pids []int64
		if !killed && (idle || overdue) {
			work.killedAt = now
			for _, process := range subprocesses {
				pids = append(pids, process.pid)
			}
			why := "after 15 minutes"
			if idle {
				why = "without using any CPU"
			}
			notice = "Stopping " + strconv.Itoa(count) + " background task" + plural(count, "", "s") +
				" that outlived the turn " + why + ": " + what + "."
		} else if announce {
			notice = "The turn is over but " + strconv.Itoa(count) + " background task" + plural(count, "", "s") +
				" started here " + plural(count, "is", "are") + " still running (" + what + "). This session stays Running until " +
				plural(count, "it", "they") + " finish."
		}
		progress := qtjson.ObjectOf("id", id, "count", count, "busy", work.busy, "summary", what, "changed", changed,
			"notice", notice)
		killTimedOut := work.killedAt > 0 && now-work.killedAt >= completionKillGrace
		interval := completionSlowPoll
		if work.polls < completionFastPolls {
			interval = completionFastPoll
		}
		if !killTimedOut {
			work.timer = time.AfterFunc(interval, func() { b.poll(id, generation) })
		}
		b.mu.Unlock()
		if len(pids) > 0 {
			b.kill(pids)
		}
		b.m.backgroundProgress(id, progress, notice)
		if !killTimedOut {
			return
		}
		notice = strconv.Itoa(count) + " background task" + plural(count, "", "s") + " ignored the stop signal and " +
			plural(count, "is", "are") + " still running (" + what + "); completing this session anyway."
		b.mu.Lock()
		if b.current(id, generation) == nil {
			b.mu.Unlock()
			return
		}
	}
	delete(b.works, id)
	b.mu.Unlock()
	b.m.backgroundDone(id, notice)
}

func (m *Module) sessionRunning(id int) bool {
	session, _, ok := m.ownSessionAny(id)
	return ok && session.Status == agents.StatusRunning
}

func (m *Module) emitBackground(data qtjson.Value) {
	if m.core != nil && m.core.Hub != nil {
		m.core.BroadcastUI(backgroundEventName, raw(data))
	}
}

func (m *Module) backgroundProgress(id int, progress qtjson.Value, notice string) {
	if m.desktopAttached() {
		m.emitBackground(progress)
		return
	}
	if notice != "" {
		m.appendNotice(id, notice)
	}
}

func (m *Module) backgroundDone(id int, notice string) {
	if m.desktopAttached() {
		m.emitBackground(qtjson.ObjectOf("id", id, "done", true, "notice", notice))
		return
	}
	if notice != "" {
		m.appendNotice(id, notice)
	}
	session, store, ok := m.ownSessionAny(id)
	if !ok || session.Status != agents.StatusRunning {
		return
	}
	record, found := m.rawRecord(store, session)
	if !found {
		return
	}
	record.Set("status", qtjson.NewString(agents.StatusSuccess))
	record.Set("finishedAtMs", qtjson.NewInt(m.nowMs()))
	record.Set("lastError", qtjson.NewString(""))
	if writer := m.writer(); writer != nil {
		_, _ = writer.SaveRecord(record)
	}
	m.logUI("Agent %d finished its background work while the desktop was closed.", id)
}

func (m *Module) appendNotice(id int, notice string) {
	session, store, ok := m.ownSessionAny(id)
	writer := m.writer()
	if !ok || writer == nil {
		return
	}
	writer.AppendLog(store.SessionDir(session), id, "\n==> "+notice+"\n")
}

func (m *Module) registerBackground(core *daemon.Core) {
	core.Registry.Handle("core.agents.background.watch", m.Name(), m.local(func(ctx context.Context, req *rpc.Request) (any, error) {
		params := paramsObject(req)
		id := params.Get("id").Int()
		pid := params.Get("pid").Int64()
		if id <= 0 {
			return nil, rpc.Fail("bad_request")
		}
		return m.background.watch(id, pid, params.Get("announced").BoolValue()), nil
	}))
	core.Registry.Handle("core.agents.background.cancel", m.Name(), m.local(func(ctx context.Context, req *rpc.Request) (any, error) {
		return map[string]any{"cancelled": m.background.cancel(paramsObject(req).Get("id").Int())}, nil
	}))
	core.Registry.Handle("core.agents.background.list", m.Name(), m.local(func(ctx context.Context, req *rpc.Request) (any, error) {
		return map[string]any{"checks": m.background.list()}, nil
	}))
}

func (b *backgroundTracker) active() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return len(b.works) > 0
}

func (m *Module) Busy() bool {
	return m.background != nil && m.background.active()
}
