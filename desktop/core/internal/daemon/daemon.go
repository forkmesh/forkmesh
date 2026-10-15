package daemon

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
	"time"

	"forkmesh.com/desktop-core/internal/autostart"
	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/ipc"
	"forkmesh.com/desktop-core/internal/paths"
	"forkmesh.com/desktop-core/internal/qsettings"
	"forkmesh.com/desktop-core/internal/rpc"
)

type Module interface {
	Name() string
	Start(ctx context.Context, core *Core) error
}

type Busy interface {
	Busy() bool
}

type Stopper interface {
	Stop()
}

type Options struct {
	Project       string
	Version       string
	Linger        time.Duration
	Env           paths.Env
	Home          string
	StateDir      string
	LogWriter     io.Writer
	Background    bool
	Executable    string
	Autostart     autostart.Manager
	VerifyBinary  func(binary string) bool
	SuperviseTick time.Duration
}

type Core struct {
	Layout   paths.Layout
	Identity *identity.Identity
	Registry *rpc.Registry
	Hub      *rpc.Hub
	IPC      *ipc.Server
	Logger   *log.Logger
	Settings *qsettings.Cache
	Version  string
	Started  time.Time

	mu       sync.Mutex
	modules  []Module
	cancel   context.CancelFunc
	lastUI   time.Time
	uiOnline bool
	values   map[string]any
	life     lifecycle
}

func (c *Core) Set(key string, value any) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.values[key] = value
}

func (c *Core) Get(key string) any {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.values[key]
}

func (c *Core) UI() *ipc.Conn { return c.IPC.UI() }

func (c *Core) CallUI(ctx context.Context, method string, params any) ([]byte, error) {
	ui := c.UI()
	if ui == nil {
		return nil, rpc.Fail("desktop_unavailable")
	}
	return ui.Call(ctx, method, params)
}

func (c *Core) NotifyUI(method string, params any) bool {
	ui := c.UI()
	if ui == nil {
		return false
	}
	return ui.Notify(method, params)
}

type SettingWrite struct {
	Key    string `json:"key"`
	Type   string `json:"type"`
	Value  any    `json:"value,omitempty"`
	Remove bool   `json:"remove,omitempty"`
}

func (c *Core) WriteSettings(ctx context.Context, writes []SettingWrite) error {
	if len(writes) == 0 {
		return nil
	}
	_, err := c.CallUI(ctx, "ui.settings.write", map[string]any{"writes": writes})
	return err
}

func (c *Core) LogUI(format string, args ...any) {
	text := fmt.Sprintf(format, args...)
	c.Logger.Print(text)
	c.NotifyUI("ui.log", map[string]any{"text": text})
}

func (c *Core) BroadcastUI(event string, data any) {
	c.Hub.Broadcast(event, data, func(peer rpc.Peer) bool { return peer.Transport() == rpc.UI })
}

func (c *Core) Shutdown() {
	c.mu.Lock()
	cancel := c.cancel
	c.mu.Unlock()
	if cancel != nil {
		cancel()
	}
}

func (c *Core) BusyModules() []string {
	out := []string{}
	for _, module := range c.modules {
		if b, ok := module.(Busy); ok && b.Busy() {
			out = append(out, module.Name())
		}
	}
	return out
}

func (c *Core) busy() bool {
	for _, module := range c.modules {
		if busy, ok := module.(Busy); ok && busy.Busy() {
			return true
		}
	}
	return false
}

func openLog(dir string) (io.Writer, error) {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, err
	}
	path := filepath.Join(dir, "forkmesh-core.log")
	if info, err := os.Stat(path); err == nil && info.Size() > 8<<20 {
		_ = os.Rename(path, path+".1")
	}
	return os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
}

func NewCore(layout paths.Layout, version string, logger *log.Logger) *Core {
	core := &Core{Layout: layout, Registry: rpc.NewRegistry(), Hub: rpc.NewHub(), Logger: logger,
		Version: version, Started: time.Now(), values: map[string]any{}, lastUI: time.Now(),
		Settings: &qsettings.Cache{Path: layout.SettingsFile()}}
	core.IPC = ipc.NewServer(core.Registry, core.Hub)
	core.IPC.Version = version
	core.IPC.Project = layout.Project
	core.IPC.Logger = logger
	core.initLifecycle(Options{})
	core.registerBuiltins()
	core.registerLifecycle()
	return core
}

func (c *Core) SetModules(modules []Module) { c.modules = modules }

func (c *Core) Module(name string) Module {
	for _, module := range c.modules {
		if module.Name() == name {
			return module
		}
	}
	return nil
}

func Run(ctx context.Context, options Options, modules []Module) error {
	layout := paths.Resolve(options.Project, options.Env, options.Home)
	stateDir := options.StateDir
	if stateDir == "" {
		stateDir = layout.CoreState()
	}
	if err := os.MkdirAll(stateDir, 0o700); err != nil {
		return err
	}
	writer := options.LogWriter
	if writer == nil {
		file, err := openLog(layout.CoreLogs())
		if err != nil {
			writer = os.Stderr
		} else {
			writer = file
		}
	}
	logger := log.New(writer, "", log.LstdFlags|log.Lmicroseconds)

	lock, err := acquireLock(filepath.Join(stateDir, "core.lock"))
	if err != nil {
		return fmt.Errorf("%w: %v", ErrAlreadyRunning, err)
	}
	defer lock.Close()

	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	core := NewCore(layout, options.Version, logger)
	core.cancel = cancel
	core.initLifecycle(options)
	core.recordStart(stateDir)
	core.recallOwners()
	if id, err := identity.Load(layout.IdentityKey()); err == nil {
		core.Identity = id
	} else {
		logger.Printf("identity: %v", err)
	}
	core.IPC.HelloExtra = func() map[string]any {
		return map[string]any{"busy": core.BusyModules()}
	}
	core.IPC.OnUI = func(conn *ipc.Conn, attached bool) {
		core.mu.Lock()
		core.uiOnline = core.IPC.UI() != nil
		core.lastUI = time.Now()
		core.mu.Unlock()
		logger.Printf("ui %s %s (pid %d)", conn.ID(), map[bool]string{true: "attached", false: "detached"}[attached], conn.PID())
	}

	core.modules = modules
	for _, module := range modules {
		if err := module.Start(ctx, core); err != nil {
			logger.Printf("module %s: %v", module.Name(), err)
		}
	}

	endpoint, err := core.IPC.Listen(stateDir)
	if err != nil {
		return err
	}
	logger.Printf("forkmesh-core %s serving project %s on %s %s", options.Version, layout.Project, endpoint.Unix, endpoint.TCP)

	tick := options.SuperviseTick
	if tick <= 0 {
		tick = time.Second
	}
	go core.supervise(ctx, options.Linger, tick)
	core.startTray(options)
	core.IPC.Serve(ctx)
	for i := len(modules) - 1; i >= 0; i-- {
		if stopper, ok := modules[i].(Stopper); ok {
			stopper.Stop()
		}
	}
	_ = os.Remove(ipc.EndpointPath(stateDir))
	if endpoint.Unix != "" {
		_ = os.Remove(endpoint.Unix)
	}
	core.recordStop(stateDir)
	if core.RestartRequested() {
		logger.Printf("forkmesh-core stopped for a restart")
		return ErrRestart
	}
	logger.Printf("forkmesh-core stopped")
	return nil
}

func (c *Core) registerBuiltins() {
	c.Registry.Handle("core.ping", "core", func(ctx context.Context, req *rpc.Request) (any, error) {
		return map[string]any{"pong": time.Now().UnixMilli()}, nil
	})
	c.Registry.Handle("core.status", "core", func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail("forbidden")
		}
		return c.Status(), nil
	})
	c.Registry.Handle("core.routes", "core", func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail("forbidden")
		}
		return map[string]any{"routes": c.Registry.Routes()}, nil
	})
	c.Registry.Handle("core.owners", "core", func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail("forbidden")
		}
		var params struct {
			Names []string `json:"names"`
		}
		if err := req.Bind(&params); err != nil {
			return nil, err
		}
		names := []string{}
		for _, name := range params.Names {
			if trimmed := strings.TrimSpace(name); trimmed != "" {
				names = append(names, trimmed)
			}
		}
		c.Set("control.owners", names)
		if err := c.Remember(ownersFile, names); err != nil {
			c.Logger.Printf("could not save the node owners: %v", err)
		}
		return map[string]any{"owners": names}, nil
	})
	c.Registry.Handle("core.shutdown", "core", func(ctx context.Context, req *rpc.Request) (any, error) {
		if req.Peer == nil || rpc.Remote(req.Peer) {
			return nil, rpc.Fail("forbidden")
		}
		var params struct {
			Hard bool `json:"hard"`
		}
		_ = req.Bind(&params)
		if params.Hard {
			c.Set("core.shutdown.hard", true)
		}
		go func() {
			time.Sleep(50 * time.Millisecond)
			c.Shutdown()
		}()
		return map[string]any{"stopping": true, "hard": params.Hard}, nil
	})
}

func (c *Core) Status() map[string]any {
	var memory runtime.MemStats
	runtime.ReadMemStats(&memory)
	names := make([]string, 0, len(c.modules))
	busy := []string{}
	for _, module := range c.modules {
		names = append(names, module.Name())
		if b, ok := module.(Busy); ok && b.Busy() {
			busy = append(busy, module.Name())
		}
	}
	sort.Strings(names)
	peers := map[string]int{}
	for _, peer := range c.Hub.Peers(nil) {
		peers[string(peer.Transport())]++
	}
	key := ""
	if c.Identity != nil {
		key = c.Identity.PublicKey()
	}
	return map[string]any{
		"version":    c.Version,
		"project":    c.Layout.Project,
		"pid":        os.Getpid(),
		"startedMs":  c.Started.UnixMilli(),
		"uptimeMs":   time.Since(c.Started).Milliseconds(),
		"goroutines": runtime.NumGoroutine(),
		"heapBytes":  memory.HeapAlloc,
		"sysBytes":   memory.Sys,
		"modules":    names,
		"busy":       busy,
		"peers":      peers,
		"identity":   key,
		"data":       c.Layout.Data,
		"ui":         c.IPC.UI() != nil,
		"background": c.Background(),
		"lastExit":   c.life.lastExit,
	}
}

var ErrNoIdentity = errors.New("core: no identity key")

func (c *Core) RequireIdentity() (*identity.Identity, error) {
	if c.Identity != nil {
		return c.Identity, nil
	}
	id, err := identity.Load(c.Layout.IdentityKey())
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrNoIdentity, err)
	}
	c.mu.Lock()
	c.Identity = id
	c.mu.Unlock()
	return id, nil
}
