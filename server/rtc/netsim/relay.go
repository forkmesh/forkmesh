package netsim

import (
	"errors"
	"hash/fnv"
	"net"
	"net/netip"
	"strings"
	"sync"
	"time"

	"forkmesh.com/mirror-node/rtc/stun"
)

const maxDatagram = 2048

type Config struct {
	Key  func(packet []byte) (string, bool)
	Seed uint64
}

type Relay struct {
	conn    *net.UDPConn
	target  netip.AddrPort
	cfg     Config
	sched   *scheduler
	mu      sync.Mutex
	flows   map[netip.AddrPort]*flow
	clients map[string]*Client
	timers  []*time.Timer
	closed  bool
	wg      sync.WaitGroup
}

type Client struct {
	Key     string
	Up      *Link
	Down    *Link
	flows   []*flow
	rebinds int
}

type ClientStats struct {
	Key     string    `json:"key"`
	Flows   int       `json:"flows"`
	Rebinds int       `json:"rebinds"`
	Up      LinkStats `json:"up"`
	Down    LinkStats `json:"down"`
}

type flow struct {
	from     netip.AddrPort
	client   *Client
	upstream *net.UDPConn
}

func Listen(address string, target netip.AddrPort, cfg Config) (*Relay, error) {
	addr, err := net.ResolveUDPAddr("udp", address)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		return nil, err
	}
	_ = conn.SetReadBuffer(4 << 20)
	_ = conn.SetWriteBuffer(4 << 20)
	relay := &Relay{
		conn:    conn,
		target:  target,
		cfg:     cfg,
		sched:   newScheduler(),
		flows:   map[netip.AddrPort]*flow{},
		clients: map[string]*Client{},
	}
	relay.wg.Add(1)
	go relay.serve()
	return relay, nil
}

func (r *Relay) Addr() netip.AddrPort {
	return r.conn.LocalAddr().(*net.UDPAddr).AddrPort()
}

func (r *Relay) Target() netip.AddrPort {
	return r.target
}

func seedFor(base uint64, key string) uint64 {
	h := fnv.New64a()
	h.Write([]byte(key))
	return base ^ h.Sum64()
}

func (r *Relay) clientLocked(key string) *Client {
	client := r.clients[key]
	if client == nil {
		seed := seedFor(r.cfg.Seed, key)
		client = &Client{
			Key:  key,
			Up:   NewLink(seed, time.Now()),
			Down: NewLink(seed^0xd0d0d0d0d0d0d0d0, time.Now()),
		}
		r.clients[key] = client
	}
	return client
}

func (r *Relay) Shape(key string, start time.Time, up, down []Step) *Client {
	r.mu.Lock()
	defer r.mu.Unlock()
	client := r.clientLocked(key)
	client.Up.Schedule(start, up...)
	client.Down.Schedule(start, down...)
	seen := map[time.Duration]bool{}
	for _, step := range append(append([]Step(nil), up...), down...) {
		if !step.Rebind || seen[step.At] || r.closed {
			continue
		}
		seen[step.At] = true
		r.timers = append(r.timers, time.AfterFunc(time.Until(start.Add(step.At)), func() { r.Rebind(key) }))
	}
	return client
}

func (r *Relay) Rebind(key string) int {
	r.mu.Lock()
	defer r.mu.Unlock()
	client := r.clients[key]
	if client == nil || r.closed {
		return 0
	}
	moved := 0
	for _, f := range client.flows {
		upstream, err := r.dialLocked()
		if err != nil {
			continue
		}
		old := f.upstream
		f.upstream = upstream
		r.readDown(f, upstream)
		_ = old.Close()
		moved++
	}
	client.rebinds++
	return moved
}

func (r *Relay) Clients() []ClientStats {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]ClientStats, 0, len(r.clients))
	for _, client := range r.clients {
		out = append(out, ClientStats{
			Key: client.Key, Flows: len(client.flows), Rebinds: client.rebinds,
			Up: client.Up.Stats(), Down: client.Down.Stats(),
		})
	}
	return out
}

func (r *Relay) Client(key string) (ClientStats, bool) {
	for _, stats := range r.Clients() {
		if stats.Key == key {
			return stats, true
		}
	}
	return ClientStats{}, false
}

func (r *Relay) Close() error {
	r.mu.Lock()
	if r.closed {
		r.mu.Unlock()
		return nil
	}
	r.closed = true
	for _, timer := range r.timers {
		timer.Stop()
	}
	err := r.conn.Close()
	for _, f := range r.flows {
		_ = f.upstream.Close()
	}
	r.mu.Unlock()
	r.wg.Wait()
	r.sched.stop()
	return err
}

func (r *Relay) dialLocked() (*net.UDPConn, error) {
	upstream, err := net.DialUDP("udp", nil, net.UDPAddrFromAddrPort(r.target))
	if err != nil {
		return nil, err
	}
	_ = upstream.SetReadBuffer(4 << 20)
	_ = upstream.SetWriteBuffer(4 << 20)
	return upstream, nil
}

func (r *Relay) flowFor(packet []byte, from netip.AddrPort) *flow {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.closed {
		return nil
	}
	if f := r.flows[from]; f != nil {
		return f
	}
	key := ""
	if r.cfg.Key != nil {
		if found, ok := r.cfg.Key(packet); ok {
			key = found
		}
	}
	if key == "" {
		key = "addr:" + from.String()
	}
	upstream, err := r.dialLocked()
	if err != nil {
		return nil
	}
	client := r.clientLocked(key)
	f := &flow{from: from, client: client, upstream: upstream}
	client.flows = append(client.flows, f)
	r.flows[from] = f
	r.readDown(f, upstream)
	return f
}

func (r *Relay) upstreamOf(f *flow) *net.UDPConn {
	r.mu.Lock()
	defer r.mu.Unlock()
	return f.upstream
}

func (r *Relay) serve() {
	defer r.wg.Done()
	buffer := make([]byte, maxDatagram)
	for {
		n, from, err := r.conn.ReadFromUDPAddrPort(buffer)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		from = netip.AddrPortFrom(from.Addr().Unmap(), from.Port())
		packet := append([]byte(nil), buffer[:n]...)
		f := r.flowFor(packet, from)
		if f == nil {
			continue
		}
		for _, at := range f.client.Up.Plan(time.Now(), n) {
			r.sched.at(at, func() { _, _ = r.upstreamOf(f).Write(packet) })
		}
	}
}

func (r *Relay) readDown(f *flow, upstream *net.UDPConn) {
	r.wg.Add(1)
	go func() {
		defer r.wg.Done()
		buffer := make([]byte, maxDatagram)
		for {
			n, err := upstream.Read(buffer)
			if err != nil {
				if errors.Is(err, net.ErrClosed) {
					return
				}
				continue
			}
			packet := append([]byte(nil), buffer[:n]...)
			for _, at := range f.client.Down.Plan(time.Now(), n) {
				r.sched.at(at, func() { _, _ = r.conn.WriteToUDPAddrPort(packet, f.from) })
			}
		}
	}()
}

func ICEUfragKey(packet []byte) (string, bool) {
	if !stun.IsMessage(packet) {
		return "", false
	}
	message, err := stun.Parse(packet)
	if err != nil || message.Type != stun.BindingRequest {
		return "", false
	}
	username, ok := message.Get(stun.AttrUsername)
	if !ok {
		return "", false
	}
	_, sender, found := strings.Cut(string(username), ":")
	if !found || sender == "" {
		return "", false
	}
	return sender, true
}
