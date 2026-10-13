package coretest

import (
	"context"
	"encoding/json"
	"io"
	"log"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"forkmesh.com/desktop-core/internal/daemon"
	"forkmesh.com/desktop-core/internal/paths"
	"forkmesh.com/desktop-core/internal/rpc"
)

func Env(home string) paths.Env {
	values := map[string]string{
		"HOME":            home,
		"XDG_DATA_HOME":   filepath.Join(home, ".local/share"),
		"XDG_CONFIG_HOME": filepath.Join(home, ".config"),
		"XDG_CACHE_HOME":  filepath.Join(home, ".cache"),
	}
	return func(name string) string { return values[name] }
}

func New(t testing.TB) *daemon.Core {
	t.Helper()
	home := t.TempDir()
	layout := paths.Resolve("", Env(home), home)
	return daemon.NewCore(layout, "test", log.New(io.Discard, "", 0))
}

func Start(t testing.TB, core *daemon.Core, modules ...daemon.Module) context.Context {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(func() {
		cancel()
		for i := len(modules) - 1; i >= 0; i-- {
			if stopper, ok := modules[i].(daemon.Stopper); ok {
				stopper.Stop()
			}
		}
	})
	core.SetModules(modules)
	for _, module := range modules {
		if err := module.Start(ctx, core); err != nil {
			t.Fatalf("start %s: %v", module.Name(), err)
		}
	}
	return ctx
}

type Event struct {
	Name string
	Data json.RawMessage
}

type Peer struct {
	Name      string
	Kind      rpc.Transport
	Dev       rpc.Device
	Events    chan Event
	done      chan struct{}
	closeOnce sync.Once
}

func NewPeer(id string, transport rpc.Transport) *Peer {
	return &Peer{Name: id, Kind: transport, Events: make(chan Event, 1024), done: make(chan struct{}),
		Dev: rpc.Device{Key: "test-key-" + id, Name: id, Platform: "test"}}
}

func (p *Peer) ID() string               { return p.Name }
func (p *Peer) Transport() rpc.Transport { return p.Kind }
func (p *Peer) Device() rpc.Device       { return p.Dev }
func (p *Peer) Done() <-chan struct{}    { return p.done }
func (p *Peer) Close()                   { p.closeOnce.Do(func() { close(p.done) }) }

func (p *Peer) Send(event string, data any) bool {
	raw, err := json.Marshal(data)
	if err != nil {
		return false
	}
	select {
	case <-p.done:
		return false
	case p.Events <- Event{Name: event, Data: raw}:
		return true
	default:
		return false
	}
}

func (p *Peer) Next(t testing.TB, event string, timeout time.Duration) json.RawMessage {
	t.Helper()
	deadline := time.After(timeout)
	for {
		select {
		case got := <-p.Events:
			if got.Name == event {
				return got.Data
			}
		case <-deadline:
			t.Fatalf("no %s event within %s", event, timeout)
			return nil
		}
	}
}

func Call(t testing.TB, core *daemon.Core, peer rpc.Peer, method string, params any) (json.RawMessage, error) {
	t.Helper()
	raw, err := json.Marshal(params)
	if err != nil {
		t.Fatalf("params: %v", err)
	}
	if params == nil {
		raw = []byte("{}")
	}
	result, err := core.Registry.Dispatch(context.Background(), &rpc.Request{Peer: peer, ID: 1, Method: method, Params: raw})
	if err != nil {
		return nil, err
	}
	encoded, err := json.Marshal(result)
	if err != nil {
		t.Fatalf("result: %v", err)
	}
	return encoded, nil
}

func MustCall(t testing.TB, core *daemon.Core, peer rpc.Peer, method string, params any, into any) {
	t.Helper()
	raw, err := Call(t, core, peer, method, params)
	if err != nil {
		t.Fatalf("%s: %v", method, err)
	}
	if into != nil {
		if err := json.Unmarshal(raw, into); err != nil {
			t.Fatalf("%s decode: %v (%s)", method, err, raw)
		}
	}
}
