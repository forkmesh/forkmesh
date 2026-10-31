package netsim

import (
	"bytes"
	"errors"
	"net"
	"net/netip"
	"sync"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/stun"
)

type echoServer struct {
	conn *net.UDPConn
	mu   sync.Mutex
	seen map[netip.AddrPort]int
}

func startEcho(t *testing.T) *echoServer {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	echo := &echoServer{conn: conn, seen: map[netip.AddrPort]int{}}
	t.Cleanup(func() { conn.Close() })
	go func() {
		buffer := make([]byte, 2048)
		for {
			n, from, err := conn.ReadFromUDPAddrPort(buffer)
			if err != nil {
				if errors.Is(err, net.ErrClosed) {
					return
				}
				continue
			}
			echo.mu.Lock()
			echo.seen[from]++
			echo.mu.Unlock()
			conn.WriteToUDPAddrPort(buffer[:n], from)
		}
	}()
	return echo
}

func (e *echoServer) addr() netip.AddrPort {
	return e.conn.LocalAddr().(*net.UDPAddr).AddrPort()
}

func (e *echoServer) sources() []netip.AddrPort {
	e.mu.Lock()
	defer e.mu.Unlock()
	var out []netip.AddrPort
	for addr := range e.seen {
		out = append(out, addr)
	}
	return out
}

func bindingRequest(sender string) []byte {
	message := &stun.Message{Type: stun.BindingRequest}
	copy(message.TransactionID[:], "abcdefghijkl")
	message.Add(stun.AttrUsername, []byte("sfu1:"+sender))
	return message.Encode([]byte("password"), true)
}

func dialRelay(t *testing.T, relay *Relay) *net.UDPConn {
	t.Helper()
	conn, err := net.DialUDP("udp", nil, net.UDPAddrFromAddrPort(relay.Addr()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

func roundTrip(t *testing.T, conn *net.UDPConn, packet []byte) time.Duration {
	t.Helper()
	start := time.Now()
	if _, err := conn.Write(packet); err != nil {
		t.Fatal(err)
	}
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buffer := make([]byte, 2048)
	n, err := conn.Read(buffer)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(buffer[:n], packet) {
		t.Fatalf("echo changed the packet")
	}
	return time.Since(start)
}

func TestTheICEUfragKeyIsTheSendersUfrag(t *testing.T) {
	key, ok := ICEUfragKey(bindingRequest("browser9"))
	if !ok || key != "browser9" {
		t.Fatalf("key %q %v", key, ok)
	}
	if _, ok := ICEUfragKey([]byte{0x80, 0x60, 0, 1}); ok {
		t.Fatal("an RTP packet has no ufrag")
	}
	response := &stun.Message{Type: stun.BindingSuccess}
	if _, ok := ICEUfragKey(response.Encode(nil, true)); ok {
		t.Fatal("a response is not keyed")
	}
}

func TestTheRelayShapesEachClientInBothDirections(t *testing.T) {
	echo := startEcho(t)
	relay, err := Listen("127.0.0.1:0", echo.addr(), Config{Key: ICEUfragKey, Seed: 1})
	if err != nil {
		t.Fatal(err)
	}
	defer relay.Close()
	now := time.Now()
	relay.Shape("slow", now, []Step{{Profile: Profile{Delay: 40 * time.Millisecond}}}, []Step{{Profile: Profile{Delay: 60 * time.Millisecond}}})
	slow := dialRelay(t, relay)
	fast := dialRelay(t, relay)
	if rtt := roundTrip(t, slow, bindingRequest("slow")); rtt < 100*time.Millisecond || rtt > 400*time.Millisecond {
		t.Errorf("shaped round trip %v, want about 100 ms", rtt)
	}
	if rtt := roundTrip(t, fast, bindingRequest("fast")); rtt > 50*time.Millisecond {
		t.Errorf("unshaped round trip %v", rtt)
	}
	media := []byte{0x80, 0x60, 0, 1, 1, 2, 3, 4}
	if rtt := roundTrip(t, slow, media); rtt < 100*time.Millisecond {
		t.Errorf("later packets of a shaped flow are shaped too: %v", rtt)
	}
	stats, ok := relay.Client("slow")
	if !ok || stats.Flows != 1 || stats.Up.Delivered != 2 || stats.Down.Delivered != 2 {
		t.Errorf("stats %+v", stats)
	}
	if len(echo.sources()) != 2 {
		t.Errorf("the target saw %d sources, want one per client flow", len(echo.sources()))
	}
}

func TestTheRelayDropsWhatALossyLinkLoses(t *testing.T) {
	echo := startEcho(t)
	relay, err := Listen("127.0.0.1:0", echo.addr(), Config{Key: ICEUfragKey, Seed: 2})
	if err != nil {
		t.Fatal(err)
	}
	defer relay.Close()
	relay.Shape("lossy", time.Now(), []Step{{Profile: Profile{Loss: 0.5}}}, nil)
	conn := dialRelay(t, relay)
	conn.Write(bindingRequest("lossy"))
	for range 400 {
		conn.Write([]byte{0x80, 0x60, 0, 1})
	}
	time.Sleep(200 * time.Millisecond)
	stats, _ := relay.Client("lossy")
	if stats.Up.Packets != 401 || stats.Up.Lost < 150 || stats.Up.Lost > 250 {
		t.Errorf("stats %+v", stats.Up)
	}
}

func TestRebindingMovesAClientToANewSourcePort(t *testing.T) {
	echo := startEcho(t)
	relay, err := Listen("127.0.0.1:0", echo.addr(), Config{Key: ICEUfragKey})
	if err != nil {
		t.Fatal(err)
	}
	defer relay.Close()
	relay.Shape("phone", time.Now(), []Step{{At: 300 * time.Millisecond, Rebind: true, Profile: Profile{Delay: 30 * time.Millisecond}}}, nil)
	conn := dialRelay(t, relay)
	roundTrip(t, conn, bindingRequest("phone"))
	time.Sleep(500 * time.Millisecond)
	if rtt := roundTrip(t, conn, []byte{0x80, 0x60, 0, 2}); rtt < 30*time.Millisecond {
		t.Errorf("the new path should carry the new delay: %v", rtt)
	}
	if sources := echo.sources(); len(sources) != 2 {
		t.Errorf("the target saw %v, want the old and the new source", sources)
	}
	if stats, _ := relay.Client("phone"); stats.Rebinds != 1 {
		t.Errorf("stats %+v", stats)
	}
}

func TestTheSchedulerKeepsOrderForEqualTimes(t *testing.T) {
	s := newScheduler()
	defer s.stop()
	var mu sync.Mutex
	var got []int
	at := time.Now().Add(20 * time.Millisecond)
	for i := range 100 {
		s.at(at, func() {
			mu.Lock()
			got = append(got, i)
			mu.Unlock()
		})
	}
	s.at(time.Now(), func() {
		mu.Lock()
		got = append(got, -1)
		mu.Unlock()
	})
	deadline := time.Now().Add(2 * time.Second)
	for s.pending() > 0 && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	time.Sleep(10 * time.Millisecond)
	mu.Lock()
	defer mu.Unlock()
	if len(got) != 101 || got[0] != -1 {
		t.Fatalf("got %v", got)
	}
	for i := 1; i < len(got); i++ {
		if got[i] != i-1 {
			t.Fatalf("out of order at %d: %v", i, got)
		}
	}
}
