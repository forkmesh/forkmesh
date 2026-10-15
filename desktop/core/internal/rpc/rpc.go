package rpc

import (
	"context"
	"encoding/json"
	"errors"
	"sort"
	"strings"
	"sync"
)

type Transport string

const (
	UI    Transport = "ui"
	LAN   Transport = "lan"
	Relay Transport = "relay"
	CLI   Transport = "cli"
)

type Device struct {
	Key      string `json:"key"`
	Name     string `json:"name"`
	User     string `json:"user"`
	Platform string `json:"platform"`
}

type Peer interface {
	ID() string
	Transport() Transport
	Device() Device
	Send(event string, data any) bool
	Done() <-chan struct{}
}

type Request struct {
	Peer     Peer
	ID       int64
	Method   string
	Params   json.RawMessage
	progress func(data any) bool
}

const ProgressEvent = "core.progress"

func (r *Request) SetProgress(send func(data any) bool) { r.progress = send }

func (r *Request) Progress(data any) bool {
	if r.progress != nil {
		return r.progress(data)
	}
	if r.Peer == nil || r.ID <= 0 {
		return false
	}
	return r.Peer.Send(ProgressEvent, map[string]any{"id": r.ID, "method": r.Method, "progress": data})
}

func (r *Request) Bind(target any) error {
	if len(r.Params) == 0 || string(r.Params) == "null" {
		return nil
	}
	if err := json.Unmarshal(r.Params, target); err != nil {
		return Fail("bad_params")
	}
	return nil
}

func (r *Request) Object() map[string]any {
	out := map[string]any{}
	if len(r.Params) > 0 {
		_ = json.Unmarshal(r.Params, &out)
	}
	return out
}

type Handler func(ctx context.Context, req *Request) (any, error)

type Error struct {
	Code string
}

func (e *Error) Error() string { return e.Code }

func Fail(code string) error { return &Error{Code: code} }

func Code(err error) string {
	var rpcErr *Error
	if errors.As(err, &rpcErr) {
		return rpcErr.Code
	}
	if err == nil {
		return ""
	}
	return err.Error()
}

var ErrUnsupported = Fail("unsupported")

var ErrDelegate = Fail("delegate")

type Delegation struct {
	Params json.RawMessage
}

func (d *Delegation) Error() string { return "delegate" }

func DelegateWith(params json.RawMessage) error { return &Delegation{Params: params} }

func DelegatedParams(err error) (json.RawMessage, bool) {
	var delegation *Delegation
	if errors.As(err, &delegation) && delegation != nil {
		return delegation.Params, true
	}
	return nil, false
}

func Delegated(err error) bool {
	if err == ErrDelegate {
		return true
	}
	_, ok := DelegatedParams(err)
	return ok
}

type Registry struct {
	mu       sync.RWMutex
	exact    map[string]Handler
	families map[string]Handler
	fallback Handler
	owners   map[string]string
}

func NewRegistry() *Registry {
	return &Registry{exact: map[string]Handler{}, families: map[string]Handler{}, owners: map[string]string{}}
}

func (r *Registry) Handle(method string, owner string, handler Handler) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.exact[method] = handler
	r.owners[method] = owner
}

func (r *Registry) HandleFamily(prefix string, owner string, handler Handler) bool {
	if len(prefix) < 2 || !strings.HasSuffix(prefix, ".") || handler == nil {
		return false
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.families[prefix] = handler
	r.owners[prefix] = owner
	return true
}

func (r *Registry) SetFallback(handler Handler) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.fallback = handler
}

func (r *Registry) FamilyFor(method string) string {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.familyLocked(method)
}

func (r *Registry) familyLocked(method string) string {
	best := ""
	for prefix := range r.families {
		if strings.HasPrefix(method, prefix) && len(prefix) > len(best) {
			best = prefix
		}
	}
	return best
}

func (r *Registry) Resolve(method string) (Handler, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	if handler, ok := r.exact[method]; ok {
		return handler, true
	}
	if family := r.familyLocked(method); family != "" {
		return r.families[family], true
	}
	return nil, false
}

func (r *Registry) Native(method string) bool {
	_, ok := r.Resolve(method)
	return ok
}

func (r *Registry) Dispatch(ctx context.Context, req *Request) (any, error) {
	r.mu.RLock()
	fallback := r.fallback
	r.mu.RUnlock()
	if handler, ok := r.Resolve(req.Method); ok {
		result, err := handler(ctx, req)
		if !Delegated(err) {
			return result, err
		}
		if params, ok := DelegatedParams(err); ok {
			forwarded := *req
			forwarded.Params = params
			req = &forwarded
		}
	}
	if fallback != nil {
		return fallback(ctx, req)
	}
	return nil, ErrUnsupported
}

type Route struct {
	Pattern string `json:"pattern"`
	Owner   string `json:"owner"`
	Family  bool   `json:"family"`
}

func (r *Registry) Routes() []Route {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]Route, 0, len(r.exact)+len(r.families))
	for method := range r.exact {
		out = append(out, Route{Pattern: method, Owner: r.owners[method]})
	}
	for prefix := range r.families {
		out = append(out, Route{Pattern: prefix, Owner: r.owners[prefix], Family: true})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Pattern < out[j].Pattern })
	return out
}

type Hub struct {
	mu    sync.RWMutex
	peers map[string]Peer
}

func NewHub() *Hub { return &Hub{peers: map[string]Peer{}} }

func (h *Hub) Add(peer Peer) {
	h.mu.Lock()
	h.peers[peer.ID()] = peer
	h.mu.Unlock()
}

func (h *Hub) Remove(peer Peer) {
	h.mu.Lock()
	if current, ok := h.peers[peer.ID()]; ok && current == peer {
		delete(h.peers, peer.ID())
	}
	h.mu.Unlock()
}

func (h *Hub) Get(id string) Peer {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return h.peers[id]
}

func (h *Hub) Peers(filter func(Peer) bool) []Peer {
	h.mu.RLock()
	defer h.mu.RUnlock()
	out := make([]Peer, 0, len(h.peers))
	for _, peer := range h.peers {
		if filter == nil || filter(peer) {
			out = append(out, peer)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID() < out[j].ID() })
	return out
}

func (h *Hub) Broadcast(event string, data any, filter func(Peer) bool) int {
	sent := 0
	for _, peer := range h.Peers(filter) {
		if peer.Send(event, data) {
			sent++
		}
	}
	return sent
}

func Remote(peer Peer) bool {
	return peer.Transport() == LAN || peer.Transport() == Relay
}

type Message struct {
	ID     int64           `json:"id,omitempty"`
	Method string          `json:"method,omitempty"`
	Params json.RawMessage `json:"params,omitempty"`
	OK     *bool           `json:"ok,omitempty"`
	Result json.RawMessage `json:"result,omitempty"`
	Error  string          `json:"error,omitempty"`
	Event  string          `json:"event,omitempty"`
	Data   json.RawMessage `json:"data,omitempty"`
}

func (m *Message) IsRequest() bool  { return m.Method != "" && m.ID > 0 }
func (m *Message) IsResponse() bool { return m.OK != nil && m.ID > 0 }
func (m *Message) IsEvent() bool    { return m.Event != "" }

func Response(id int64, result any, err error) map[string]any {
	if err != nil {
		return map[string]any{"id": id, "ok": false, "error": Code(err)}
	}
	if result == nil {
		result = map[string]any{}
	}
	return map[string]any{"id": id, "ok": true, "result": result}
}

func EventFrame(event string, data any) map[string]any {
	if data == nil {
		data = map[string]any{}
	}
	return map[string]any{"event": event, "data": data}
}
