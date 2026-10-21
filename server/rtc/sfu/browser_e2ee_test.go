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
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"forkmesh.com/mirror-node/rtc/ice"
	"forkmesh.com/mirror-node/rtc/signal"
)

const groupCallPage = `<!doctype html><meta charset="utf-8"><body><script type="module">
import { createGroupCalls, groupCallEncryptionMode } from "/public/chat-group-calls.js";
const params = new URLSearchParams(location.search);
const name = params.get("name");
const keyless = params.get("keyless") === "1";
const report = (body) => fetch("/report?name=" + name, {method: "POST", body: JSON.stringify(body)});
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
try {
  const calls = createGroupCalls({
    sendFrame: async (channelKey, type, extra) => {
      if (keyless) return false;
      await fetch("/relay", {method: "POST", body: JSON.stringify({type, sender: name, ...extra})});
      return true;
    },
    fetchTicket: async () => (await fetch("/ticket")).json(),
    accountName: () => name,
    workerUrl: "/public/chat-group-call-crypto.js",
  });
  let since = 0;
  (async () => {
    while (!keyless) {
      try {
        const response = await fetch("/relay?since=" + since);
        const batch = await response.json();
        for (const frame of batch.frames || []) calls.handleFrame(frame, "#team");
        since = batch.next;
      } catch (_) {}
      await sleep(150);
    }
  })();
  const joined = await calls.join("#team", "c".repeat(32), "video");
  if (joined.error) throw new Error(joined.error);
  await sleep(Math.max(0, Number(params.get("until")) - Date.now()));
  const active = calls.active;
  const stats = await active.pc.getStats();
  const inbound = [];
  stats.forEach((entry) => {
    if (entry.type === "inbound-rtp") {
      inbound.push({kind: entry.kind, ssrc: entry.ssrc, bytes: entry.bytesReceived || 0,
        frames: entry.framesDecoded || 0, packets: entry.packetsReceived || 0});
    }
  });
  const names = [...active.tiles.values()].map((tile) => tile.name).sort();
  await report({self: active.peer, inbound, streams: names, state: active.state, mode: groupCallEncryptionMode()});
} catch (error) {
  await report({error: String(error && error.stack || error)});
}
</script></body>`

func TestThreeBrowsersHoldAnEncryptedGroupCall(t *testing.T) {
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
	mux, err := ice.Listen("127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer mux.Close()
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
	defer cancel()
	go server.Run(ctx)
	room := signal.RoomID(secret, "c0ffee00c0ffee00c0ffee00c0ffee00")
	var mu sync.Mutex
	reports := map[string]browserReport{}
	var relay []json.RawMessage
	routes := http.NewServeMux()
	routes.Handle("/calls/", server.Handler())
	routes.Handle("/public/", http.StripPrefix("/public/", http.FileServer(http.Dir(public))))
	routes.HandleFunc("/page", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, groupCallPage)
	})
	routes.HandleFunc("/ticket", func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode(map[string]string{
			"ticket": signal.MintTicket(secret, signal.Ticket{
				Room: room, Peer: signal.NewPeerID(), Acct: signal.NewPeerID(), Exp: time.Now().Add(signal.TicketLifetime).Unix(),
			}),
			"url": "ws://" + r.Host + "/calls/ws",
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
	names := []string{"ana", "ben", "cai", "eve"}
	until := time.Now().Add(18 * time.Second).UnixMilli()
	var processes []*exec.Cmd
	for _, name := range names {
		command := exec.CommandContext(ctx, chromium,
			"--headless", "--no-sandbox", "--disable-gpu", "--no-first-run",
			"--user-data-dir="+t.TempDir(),
			"--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream",
			"--allow-loopback-in-peer-connection",
			"--disable-features=WebRtcHideLocalIpsWithMdns",
			"--autoplay-policy=no-user-gesture-required",
			web.URL+"/page?name="+name+"&keyless="+map[bool]string{true: "1", false: "0"}[name == "eve"]+"&until="+strconv.FormatInt(until, 10))
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
	deadline := time.Now().Add(45 * time.Second)
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
	for _, name := range names {
		report := reports[name]
		if report.Error != "" {
			t.Fatalf("%s failed: %s", name, report.Error)
		}
		audio, decoded, carried := 0, 0, 0
		for _, stream := range report.Inbound {
			if stream.Kind == "audio" && stream.Bytes > 1000 {
				audio++
			}
			if stream.Kind == "video" && stream.Bytes > 20000 {
				carried++
			}
			if stream.Kind == "video" && stream.Frames > 10 {
				decoded++
			}
		}
		t.Logf("%s: state %s, tiles %v, inbound %+v", name, report.State, report.Streams, report.Inbound)
		if name == "eve" {
			if carried != 3 || decoded != 0 {
				t.Errorf("eve has no keys yet decoded %d of %d video streams", decoded, carried)
			}
			continue
		}
		want := []string{"Someone"}
		for _, other := range names[:3] {
			if other != name {
				want = append(want, other)
			}
		}
		sort.Strings(want)
		if audio != 3 || carried != 3 || decoded != 2 || strings.Join(report.Streams, ",") != strings.Join(want, ",") {
			t.Errorf("%s: %d audio, %d of %d video decoded, tiles %v (want %v)", name, audio, decoded, carried, report.Streams, want)
		}
	}
}
