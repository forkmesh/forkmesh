package sfu

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"sync"
	"syscall"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/ice"
	"forkmesh.com/mirror-node/rtc/signal"
)

type callNode struct {
	server *Server
	mux    *ice.Mux
	web    *httptest.Server
	cancel context.CancelFunc
	mu     sync.Mutex
	down   bool
}

func startCallNode(t *testing.T, secret []byte) *callNode {
	t.Helper()
	mux, err := ice.Listen("127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go mux.Serve()
	server, err := NewServer(Config{
		Secret:      secret,
		Addresses:   []netip.Addr{netip.MustParseAddr("127.0.0.1")},
		AllowOrigin: func(string) bool { return true },
	}, mux)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	go server.Run(ctx)
	node := &callNode{server: server, mux: mux, cancel: cancel}
	node.web = httptest.NewServer(server.Handler())
	t.Cleanup(node.kill)
	return node
}

func (n *callNode) kill() {
	n.mu.Lock()
	defer n.mu.Unlock()
	if n.down {
		return
	}
	n.down = true
	n.server.Close()
	n.cancel()
	n.mux.Close()
	n.web.Close()
}

func (n *callNode) alive() bool {
	n.mu.Lock()
	defer n.mu.Unlock()
	return !n.down
}

func TestAGroupCallMovesToASurvivingNodeWhenItsNodeDies(t *testing.T) {
	chromium := headlessChromium()
	if chromium == "" {
		t.Skip("no headless Chromium; set FORKMESH_TEST_CHROMIUM")
	}
	public, err := filepath.Abs(filepath.Join("..", "..", "..", "app", "public"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(public, "chat-group-calls.js")); err != nil {
		t.Skip("app/public is not beside the server module")
	}
	secret := []byte("browser-test-secret-0123456789abcdef")
	first := startCallNode(t, secret)
	second := startCallNode(t, secret)
	room := signal.RoomID(secret, "fa11fa11fa11fa11fa11fa11fa11fa11")
	var mu sync.Mutex
	reports := map[string]browserReport{}
	var relay []json.RawMessage
	routes := http.NewServeMux()
	routes.Handle("/public/", http.StripPrefix("/public/", http.FileServer(http.Dir(public))))
	routes.HandleFunc("/page", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, groupCallPage)
	})
	routes.HandleFunc("/ticket", func(w http.ResponseWriter, r *http.Request) {
		node := first
		if !first.alive() {
			node = second
		}
		json.NewEncoder(w).Encode(map[string]string{
			"ticket": signal.MintTicket(secret, signal.Ticket{
				Room: room, Peer: signal.NewPeerID(), Exp: time.Now().Add(signal.TicketLifetime).Unix(),
			}),
			"url": "ws" + node.web.URL[len("http"):] + "/calls/ws",
		})
	})
	routes.HandleFunc("/relay", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		if r.Method == http.MethodPost {
			body, _ := io.ReadAll(r.Body)
			relay = append(relay, json.RawMessage(body))
			return
		}
		since, _ := strconv.Atoi(r.URL.Query().Get("since"))
		if since < 0 || since > len(relay) {
			since = len(relay)
		}
		frames := append([]json.RawMessage{}, relay[since:]...)
		json.NewEncoder(w).Encode(map[string]any{"frames": frames, "next": len(relay)})
	})
	routes.HandleFunc("/report", func(w http.ResponseWriter, r *http.Request) {
		var report browserReport
		json.NewDecoder(r.Body).Decode(&report)
		mu.Lock()
		reports[r.URL.Query().Get("name")] = report
		mu.Unlock()
	})
	web := httptest.NewServer(routes)
	defer web.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	names := []string{"ana", "ben", "cai"}
	until := time.Now().Add(26 * time.Second).UnixMilli()
	var processes []*exec.Cmd
	for _, name := range names {
		command := exec.CommandContext(ctx, chromium,
			"--headless", "--no-sandbox", "--disable-gpu", "--no-first-run",
			"--user-data-dir="+t.TempDir(),
			"--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream",
			"--allow-loopback-in-peer-connection",
			"--disable-features=WebRtcHideLocalIpsWithMdns",
			"--autoplay-policy=no-user-gesture-required",
			web.URL+"/page?name="+name+"&keyless=0&until="+strconv.FormatInt(until, 10))
		command.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
		if err := command.Start(); err != nil {
			t.Fatal(err)
		}
		processes = append(processes, command)
		time.Sleep(1500 * time.Millisecond)
	}
	defer func() {
		for _, process := range processes {
			syscall.Kill(-process.Process.Pid, syscall.SIGKILL)
			process.Wait()
		}
	}()
	time.Sleep(6 * time.Second)
	if first.server.Rooms() != 1 || second.server.Rooms() != 0 {
		t.Fatalf("before the failure: first node %d rooms, second %d", first.server.Rooms(), second.server.Rooms())
	}
	first.kill()
	deadline := time.Now().Add(40 * time.Second)
	for time.Now().Before(deadline) {
		mu.Lock()
		done := len(reports) == len(names)
		mu.Unlock()
		if done {
			break
		}
		time.Sleep(200 * time.Millisecond)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(reports) != len(names) {
		t.Fatalf("only %d of %d browsers reported: %+v", len(reports), len(names), reports)
	}
	if second.server.Rooms() != 1 {
		t.Fatalf("the call did not move to the surviving node: %d rooms", second.server.Rooms())
	}
	for _, name := range names {
		report := reports[name]
		if report.Error != "" {
			t.Fatalf("%s failed: %s", name, report.Error)
		}
		decoded := 0
		for _, stream := range report.Inbound {
			if stream.Kind == "video" && stream.Frames > 10 {
				decoded++
			}
		}
		t.Logf("%s: state %s, tiles %v, inbound %+v", name, report.State, report.Streams, report.Inbound)
		if report.State != "live" || decoded != 2 || len(report.Streams) != 2 {
			t.Errorf("%s after failover: state %s, %d decoded video, tiles %v", name, report.State, decoded, report.Streams)
		}
	}
}
