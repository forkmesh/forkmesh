package fmmserver

import (
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
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

	"forkmesh.com/mirror-node/rtc/fmmcall"
	"forkmesh.com/mirror-node/rtc/quic"
)

const browserPage = `<!doctype html><meta charset="utf-8"><body><script>
const params = new URLSearchParams(location.search);
const report = (body) => fetch("/report", {method: "POST", body: JSON.stringify(body)});
const timeout = (p, ms) => Promise.race([p, new Promise((_, no) => setTimeout(() => no(new Error("timeout")), ms))]);
const enc = new TextEncoder(), dec = new TextDecoder();
function message(type, body) {
  const out = new Uint8Array(3 + body.length);
  out[0] = type; out[1] = body.length >> 8; out[2] = body.length & 255; out.set(body, 3);
  return out;
}
function str(s) {
  const b = enc.encode(s), out = new Uint8Array(2 + b.length);
  out[0] = b.length >> 8; out[1] = b.length & 255; out.set(b, 2);
  return out;
}
(async () => {
  const result = {joined: "", tracks: 0, media: 0, feedback: 0, sent: 0, error: ""};
  try {
    const hashes = params.get("hashes").split(",").map((h) => ({algorithm: "sha-256", value: Uint8Array.from(atob(h), (c) => c.charCodeAt(0))}));
    const wt = new WebTransport(params.get("url"), {serverCertificateHashes: hashes});
    await timeout(wt.ready, 10000);
    const stream = await wt.createBidirectionalStream();
    const writer = stream.writable.getWriter();
    await writer.write(message(0x01, str(params.get("ticket"))));
    await writer.write(message(0x03, new Uint8Array([1, 1, 1, 0, 0])));
    const reader = stream.readable.getReader();
    let buf = new Uint8Array(0);
    const control = (async () => {
      for (;;) {
        const {value, done} = await reader.read();
        if (done) return;
        const joined = new Uint8Array(buf.length + value.length);
        joined.set(buf); joined.set(value, buf.length); buf = joined;
        while (buf.length >= 3) {
          const n = (buf[1] << 8) | buf[2];
          if (buf.length < 3 + n) break;
          const type = buf[0], body = buf.slice(3, 3 + n);
          buf = buf.slice(3 + n);
          if (type === 0x02) result.joined = dec.decode(body.slice(2, 2 + ((body[0] << 8) | body[1])));
          if (type === 0x0d) result.error = "server error " + ((body[0] << 8) | body[1]);
          if (type === 0x05) {
            result.tracks++;
            await writer.write(message(0x07, new Uint8Array([body[0], 0])));
          }
        }
      }
    })();
    const dr = wt.datagrams.readable.getReader();
    const receiving = (async () => {
      for (;;) {
        const {value, done} = await dr.read();
        if (done) return;
        if (value[0] === 0x10) result.media++;
        if (value[0] === 0x13) result.feedback++;
      }
    })();
    const dw = wt.datagrams.writable.getWriter();
    const start = performance.now();
    for (let i = 0; i < 120; i++) {
      const d = new Uint8Array(16 + 40);
      const ts = Math.floor((performance.now() - start) * 1000) >>> 0;
      d[0] = 0x10; d[1] = 1;
      d[2] = i >> 8; d[3] = i & 255; d[4] = i >> 8; d[5] = i & 255; d[6] = i >> 8; d[7] = i & 255;
      d[8] = 0; d[9] = 1; d[10] = 0; d[11] = 0;
      d[12] = ts >>> 24; d[13] = (ts >>> 16) & 255; d[14] = (ts >>> 8) & 255; d[15] = ts & 255;
      d[16] = 9; d[17] = 0; d[18] = 0; d[19] = i >> 8; d[20] = i & 255;
      await dw.write(d);
      result.sent++;
      await new Promise((r) => setTimeout(r, 20));
    }
    await new Promise((r) => setTimeout(r, 300));
    await writer.write(message(0x0c, str("done")));
    wt.close({closeCode: 0, reason: "done"});
  } catch (error) {
    result.error = String(error && error.stack || error);
  }
  await report(result);
})();
</script></body>`

type browserResult struct {
	Joined   string `json:"joined"`
	Tracks   int    `json:"tracks"`
	Media    int    `json:"media"`
	Feedback int    `json:"feedback"`
	Sent     int    `json:"sent"`
	Error    string `json:"error"`
}

func headlessChromium() string {
	if path := os.Getenv("FORKMESH_TEST_CHROMIUM"); path != "" {
		return path
	}
	home, _ := os.UserHomeDir()
	matches, _ := filepath.Glob(filepath.Join(home, ".cache", "ms-playwright", "chromium_headless_shell-*", "chrome-headless-shell-linux64", "chrome-headless-shell"))
	sort.Strings(matches)
	if len(matches) == 0 {
		return ""
	}
	return matches[len(matches)-1]
}

func TestChromiumJoinsOverWebTransport(t *testing.T) {
	chromium := headlessChromium()
	if chromium == "" {
		t.Skip("no headless Chromium; set FORKMESH_TEST_CHROMIUM")
	}
	results := make(chan browserResult, 1)
	routes := http.NewServeMux()
	routes.HandleFunc("/page", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, browserPage)
	})
	routes.HandleFunc("/report", func(w http.ResponseWriter, r *http.Request) {
		var res browserResult
		json.NewDecoder(r.Body).Decode(&res)
		select {
		case results <- res:
		default:
		}
	})
	web := httptest.NewServer(routes)
	defer web.Close()

	certs, err := NewCertStore(t.TempDir(), nil)
	if err != nil {
		t.Fatal(err)
	}
	var mu sync.Mutex
	var origins []string
	srv, err := New(Config{Secret: testSecret, Certs: certs, AllowOrigin: func(o string) bool {
		mu.Lock()
		defer mu.Unlock()
		origins = append(origins, o)
		return o == web.URL
	}})
	if err != nil {
		t.Fatal(err)
	}
	ep, err := quic.Listen("udp4", "127.0.0.1:0", srv.QUICConfig())
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go srv.Serve(ctx, ep)
	defer func() { srv.Close(); ep.Close() }()
	port := ep.LocalAddr().(*net.UDPAddr).Port

	room := testRoom(9)
	native := newParty(t, 1, false)
	nativeEnd := fmmcall.Endpoint{Addr: ep.LocalAddr().String(), Hashes: certs.Hashes(), Ticket: ticketFor(room, native.peer, native.acct(), false)}
	jctx, jcancel := context.WithTimeout(ctx, 5*time.Second)
	defer jcancel()
	if err := native.join(jctx, fmmcall.Access{Endpoint: nativeEnd}); err != nil {
		t.Fatal(err)
	}
	defer native.leave()

	browserPeer := "b10b10b10b10b10b10b10b10b10b10b1"
	target := "https://127.0.0.1:" + strconv.Itoa(port) + Path
	page := web.URL + "/page?" + url.Values{
		"url":    {target},
		"hashes": {strings.Join(certs.Hashes(), ",")},
		"ticket": {ticketFor(room, browserPeer, "b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0", false)},
	}.Encode()
	cmd := exec.CommandContext(ctx, chromium, "--headless", "--no-sandbox", "--disable-gpu", "--no-first-run",
		"--user-data-dir="+t.TempDir(), page)
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		cmd.Wait()
	}()
	start := time.Now()
	var res browserResult
	select {
	case res = <-results:
	case <-time.After(60 * time.Second):
		t.Fatal("browser did not report")
	}
	heard := len(native.playsFrom(9, start))
	mu.Lock()
	defer mu.Unlock()
	t.Logf("browser %+v; native peer played %d browser frames; origins %v", res, heard, origins)
	if res.Error != "" {
		t.Fatalf("browser: %s", res.Error)
	}
	if res.Joined != browserPeer {
		t.Fatalf("browser joined as %q", res.Joined)
	}
	if res.Tracks < 1 || res.Media < 30 {
		t.Fatalf("browser got %d tracks and %d media datagrams", res.Tracks, res.Media)
	}
	if heard < 40 {
		t.Fatalf("native peer played %d of %d browser frames", heard, res.Sent)
	}
	if len(origins) == 0 || origins[0] != web.URL {
		t.Fatalf("origin check saw %v", origins)
	}
}
