package stats

import (
	"context"
	"net/http"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
)

type Module struct {
	Owners       func() []string
	Now          func() time.Time
	HTTPClient   *http.Client
	CodemagicURL string
	Git          string

	ShortcutState func(ctx context.Context) DesktopShortcutState

	core          *daemon.Core
	confirmations *ratchetConfirmations
	repoStore     *repoStatsStore
	codemagic     codemagicState

	mu             sync.Mutex
	ratchetFlights map[string]bool
	active         int
}

func New() *Module {
	return &Module{
		confirmations:  newRatchetConfirmations(),
		ratchetFlights: map[string]bool{},
	}
}

func (m *Module) Name() string { return "stats" }

func (m *Module) Start(ctx context.Context, core *daemon.Core) error {
	m.core = core
	if m.confirmations == nil {
		m.confirmations = newRatchetConfirmations()
	}
	if m.ratchetFlights == nil {
		m.ratchetFlights = map[string]bool{}
	}
	if m.repoStore == nil {
		m.repoStore = newRepoStatsStore(m.gitBinary(), m.now)
	}
	register := func(method string, handler rpc.Handler) {
		core.Registry.Handle(method, m.Name(), m.gate(handler))
	}
	register("stats.repo", m.handleRepo)
	register("stats.ratchet.prepare", m.handleRatchetPrepare)
	register("stats.ratchet.run", m.handleRatchetRun)
	register("stats.codemagic", m.handleCodemagic)
	register("insights.shortcut", m.handleShortcut)
	register("insights.shortcuts", m.handleShortcuts)
	return nil
}

func (m *Module) Busy() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.active > 0
}

func (m *Module) track() func() {
	m.mu.Lock()
	m.active++
	m.mu.Unlock()
	return func() {
		m.mu.Lock()
		m.active--
		m.mu.Unlock()
	}
}

func (m *Module) now() time.Time {
	if m.Now != nil {
		return m.Now()
	}
	return time.Now()
}

func (m *Module) gitBinary() string {
	if m.Git != "" {
		return m.Git
	}
	return "git"
}

func (m *Module) settings() *qsettings.Settings {
	if m.core == nil || m.core.Settings == nil {
		return qsettings.Parse(nil)
	}
	return m.core.Settings.Get()
}

func (m *Module) logf(format string, args ...any) {
	if m.core != nil && m.core.Logger != nil {
		m.core.Logger.Printf("stats: "+format, args...)
	}
}

func (m *Module) ownerNames() []string {
	if m.Owners != nil {
		return m.Owners()
	}
	var names []string
	if m.core != nil {
		switch value := m.core.Get("control.owners").(type) {
		case []string:
			names = append(names, value...)
		case []any:
			for _, item := range value {
				if text, ok := item.(string); ok {
					names = append(names, text)
				}
			}
		case string:
			names = append(names, value)
		}
		switch info := m.core.Get("control.info").(type) {
		case map[string]any:
			if user, ok := info["user"].(string); ok {
				names = append(names, user)
			}
		case map[string]string:
			names = append(names, info["user"])
		}
	}
	names = append(names, accountOwner(m.settings()))
	return names
}

func ownerMayManage(deviceUser, account string) bool {
	device := strings.TrimSpace(deviceUser)
	account = strings.TrimSpace(account)
	return device != "" && account != "" && strings.EqualFold(device, account)
}

func (m *Module) isOwner(device rpc.Device) bool {
	if device.Key == "" {
		return false
	}
	for _, name := range m.ownerNames() {
		if ownerMayManage(device.User, name) {
			return true
		}
	}
	return false
}

func (m *Module) gate(handler rpc.Handler) rpc.Handler {
	return func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer != nil && rpc.Remote(req.Peer) && !m.isOwner(req.Peer.Device()) {
			return nil, rpc.Fail("not_owner")
		}
		return handler(ctx, req)
	}
}

func peerDevice(req *rpc.Request) rpc.Device {
	if req.Peer == nil {
		return rpc.Device{}
	}
	return req.Peer.Device()
}

func deviceLabel(req *rpc.Request) string {
	if req.Peer == nil {
		return "a phone"
	}
	device := req.Peer.Device()
	label := device.Name
	if label == "" {
		label = "Phone"
	}
	if device.User != "" {
		label += " (" + device.User + ")"
	}
	if req.Peer.Transport() == rpc.Relay {
		label += ", through the relay"
	} else {
		label += ", on the local network"
	}
	return label
}

func (m *Module) broadcastPhones(event string, data any) {
	if m.core == nil || m.core.Hub == nil {
		return
	}
	m.core.Hub.Broadcast(event, data, rpc.Remote)
}
